#include "mmf_internal.hpp"
#include "input_resolution_tracker.hpp"

extern "C" {
/* libsys keeps CVI_VB_Exit behind a process-local `vb_inited` guard.  A new
 * process therefore cannot drop the kernel reference leaked by a SIGKILL via
 * the public function.  These two exported vendor functions issue the same
 * VB_IOCTL_EXIT without relying on that stale userspace flag. */
CVI_S32 get_base_fd(CVI_VOID);
CVI_S32 vb_ioctl_exit(CVI_S32 fd);
}

namespace onekvm::mmf {

RuntimeState g_runtime{};
CaptureOptions g_capture_options{};
static int module_loaded(const char *module_name) {
	FILE *fp = fopen("/proc/modules", "r");
	if (fp == NULL)
		return 0;

	char buffer[256];
	int found = 0;
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char mod_name[256];
		if (sscanf(buffer, "%255s", mod_name) != 1)
			continue;
		if (strcmp(mod_name, module_name) == 0) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	return found;
}

static int module_in_use(const char *module_name) {
	FILE *fp = fopen("/proc/modules", "r");
	if (fp == NULL)
		return -1;

	char buffer[256];
	int usage_count = 0;
	int found = 0;
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		char mod_name[256];
		if (sscanf(buffer, "%255s %*s %d", mod_name, &usage_count) != 2)
			continue;
		if (strcmp(mod_name, module_name) == 0) {
			found = 1;
			break;
		}
	}
	fclose(fp);
	if (!found)
		return 0;
	return usage_count > 0;
}

static int soph_media_modules_ready(void) {
	return module_loaded("soph_sys") && module_loaded("soph_base") &&
	       module_loaded("soph_vi") && module_loaded("soph_vpss");
}

static int count_vendor_buffer_pools(void)
{
    FILE *file;
    char line[1024];
    int poolIdCount = 0;

    file = fopen("/proc/cvitek/vb", "r");
    if (file == NULL) {
        perror("can not open /proc/cvitek/vb");
        return 0;
    }

    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, "PoolId(") != NULL) {
            poolIdCount++;
        }
    }

    fclose(file);
    return poolIdCount;
}

int _create_vb_pool(const char *name, uint32_t size, uint32_t max_num)
{
	uint32_t pool_id = -1;
	VB_POOL_CONFIG_S stVbPoolCfg;
	memset(&stVbPoolCfg, 0, sizeof(stVbPoolCfg));
	stVbPoolCfg.u32BlkCnt = max_num;
	stVbPoolCfg.u32BlkSize = size;
	stVbPoolCfg.enRemapMode = VB_REMAP_MODE_CACHED;
	if (name == NULL || max_num == 0 || size == 0) {
		return -1;
	}
	snprintf(stVbPoolCfg.acName, sizeof(stVbPoolCfg.acName), "%s", name);

	pool_id = CVI_VB_CreatePool(&stVbPoolCfg);
	if (pool_id == VB_INVALID_POOLID || pool_id >= VB_MAX_COMM_POOLS) {
		return -2;
	}

	BufferPool *info = (BufferPool *)&g_runtime.vb_pool[pool_id];
	info->pool_id = pool_id;
	snprintf(info->name, sizeof(info->name), "%s", name);
	info->size = size;
	info->max_num = max_num;
	info->is_used = 1;

	return pool_id;
}

int _destroy_vb_pool(uint32_t pool_id)
{
	if (pool_id >= VB_MAX_COMM_POOLS)
		return 0;
	CVI_S32 s32Ret;
	BufferPool *info = (BufferPool *)&g_runtime.vb_pool[pool_id];
	if (info->is_used) {
		s32Ret =  CVI_VB_DestroyPool(pool_id);
		if (s32Ret != CVI_SUCCESS) {
			printf("CVI_VB_DestroyPool : %d fail!\n", pool_id);
			return -1;
		}
		memset(info, 0, sizeof(BufferPool));
	}

	return 0;
}

__attribute__((unused)) static void _list_vb_pool(void)
{
	printf("====== VB POOL =======\r\n");
	for (int pool_id = 0; pool_id < VB_MAX_COMM_POOLS; pool_id ++) {
		BufferPool *info = (BufferPool *)&g_runtime.vb_pool[pool_id];
		if (info->is_used) {
			printf("[%d] name:%s size:%d num:%d\r\n", pool_id, info->name, info->size, info->max_num);
		}
	}
	printf("\r\n");
}

static SAMPLE_VI_CONFIG_S g_stViConfig;
static SAMPLE_INI_CFG_S g_stIniCfg;
CVI_S32 destroy_vpss_group(VPSS_GRP VpssGrp);

static int stale_buffer_pools_all_free(void)
{
	FILE *fp = fopen("/proc/cvitek/vb", "r");
	if (fp == NULL)
		return -1;

	char line[256];
	int block_count = -1;
	int pool_count = 0;
	bool all_free = true;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strstr(line, "BlkCnt    :")) {
			if (sscanf(line, "%*s    : %d", &block_count) != 1)
				block_count = -1;
		} else if (strstr(line, "Free      :")) {
			int free_count = -1;
			if (sscanf(line, "%*s      : %d", &free_count) == 1 &&
			    block_count >= 0) {
				++pool_count;
				if (free_count != block_count)
					all_free = false;
			}
			block_count = -1;
		}
	}
	fclose(fp);
	return pool_count > 0 && all_free ? 1 : 0;
}

static bool foreign_vendor_device_owner_exists(void)
{
	DIR *proc = opendir("/proc");
	if (proc == NULL)
		return true;

	const pid_t self = getpid();
	bool found = false;
	struct dirent *process_entry;
	while (!found && (process_entry = readdir(proc)) != NULL) {
		char *end = NULL;
		const long pid = strtol(process_entry->d_name, &end, 10);
		if (end == process_entry->d_name || *end != '\0' || pid <= 0 ||
		    pid == self)
			continue;

		char fd_dir_path[64];
		snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%ld/fd", pid);
		DIR *fd_dir = opendir(fd_dir_path);
		if (fd_dir == NULL)
			continue;
		struct dirent *fd_entry;
		while ((fd_entry = readdir(fd_dir)) != NULL) {
			if (fd_entry->d_name[0] == '.')
				continue;
			char fd_path[PATH_MAX];
			char target[PATH_MAX];
			snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir_path,
				 fd_entry->d_name);
			const ssize_t length = readlink(fd_path, target,
						       sizeof(target) - 1);
			if (length < 0)
				continue;
			target[length] = '\0';
			if (strncmp(target, "/dev/cvi", 8) == 0 ||
			    strcmp(target, "/dev/ion") == 0) {
				fprintf(stderr,
					"OneKVM: vendor multimedia device still owned by pid %ld (%s)\n",
					pid, target);
				found = true;
				break;
			}
		}
		closedir(fd_dir);
	}
	closedir(proc);
	return found;
}

static bool vendor_encoder_channel_exists(int channel)
{
	FILE *fp = fopen("/proc/cvitek/venc", "r");
	if (fp == NULL)
		return false;

	char line[256];
	bool found = false;
	while (fgets(line, sizeof(line), fp) != NULL) {
		int id = -1;
		if (sscanf(line, "ID: %d", &id) == 1 && id == channel) {
			found = true;
			break;
		}
	}
	fclose(fp);
	return found;
}

static int reclaim_stale_vendor_encoders(void)
{
	for (int channel = 0; channel < VENC_MAX_CHN_NUM; ++channel) {
		if (!vendor_encoder_channel_exists(channel))
			continue;

		char path[64];
		snprintf(path, sizeof(path), "/dev/%s%d",
			 CVI_VC_DRV_ENCODER_DEV_NAME, channel);
		const int fd = open(path, O_RDWR | O_DSYNC | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr,
				"OneKVM: open stale VENC channel %d failed: %s\n",
				channel, strerror(errno));
			return -1;
		}

		/* libvenc only sends this ioctl through a process-local channel FD.
		 * After SIGKILL a replacement process therefore has to open the
		 * channel device itself before it can release the kernel resources.
		 * Do not send STOP/RESET first: a stale bound worker can block those
		 * ioctls forever while waiting for a producer that no longer exists. */
		const int result = ioctl(fd, CVI_VC_VENC_DESTROY_CHN);
		const int saved_errno = errno;
		close(fd);
		if (result != CVI_SUCCESS) {
			fprintf(stderr,
				"OneKVM: destroy stale VENC channel %d failed: %s\n",
				channel, strerror(saved_errno));
			return -1;
		}
		fprintf(stderr, "OneKVM: reclaimed stale VENC channel %d\n",
			channel);
	}
	return 0;
}

static int reclaim_stale_vendor_vpss(void)
{
	for (int group = 0; group < VPSS_MAX_GRP_NUM; ++group) {
		VPSS_GRP_ATTR_S attributes{};
		if (CVI_VPSS_GetGrpAttr(group, &attributes) != CVI_SUCCESS)
			continue;
		for (int channel = 0; channel < VPSS_MAX_CHN_NUM; ++channel)
			(void)CVI_VPSS_DisableChn(group, channel);
		(void)CVI_VPSS_StopGrp(group);
		if (CVI_VPSS_DestroyGrp(group) != CVI_SUCCESS) {
			fprintf(stderr,
				"OneKVM: destroy stale VPSS group %d failed\n",
				group);
			return -1;
		}
		fprintf(stderr, "OneKVM: reclaimed stale VPSS group %d\n", group);
	}
	return 0;
}

static int release_stale_buffer_blocks(void)
{
	FILE *fp = fopen("/proc/cvitek/vb", "r");
	if (fp == NULL)
		return -1;

	char line[256];
	uint64_t physical_address = 0;
	int block_size = 0;
	int block_count = 0;
	int free_count = 0;
	int result = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strstr(line, "PhysAddr  :")) {
			(void)sscanf(line, "%*s  : %" SCNx64, &physical_address);
		} else if (strstr(line, "BlkSz     :")) {
			(void)sscanf(line, "%*s     : %d", &block_size);
		} else if (strstr(line, "BlkCnt    :")) {
			(void)sscanf(line, "%*s    : %d", &block_count);
		} else if (strstr(line, "Free      :")) {
			(void)sscanf(line, "%*s      : %d", &free_count);
			if (free_count == block_count)
				continue;
			for (int index = 0; index < block_count; ++index) {
				const uint64_t address = physical_address +
					(uint64_t)index * (uint64_t)block_size;
				const VB_BLK block = CVI_VB_PhysAddr2Handle(address);
				if (block == VB_INVALID_HANDLE)
					continue;
				CVI_U32 users = 0;
				if (CVI_VB_InquireUserCnt(block, &users) != CVI_SUCCESS)
					continue;
				while (users-- > 0) {
					if (CVI_VB_ReleaseBlock(block) != CVI_SUCCESS) {
						fprintf(stderr,
							"OneKVM: release stale VB block at %#" PRIx64 " failed\n",
							address);
						result = -1;
						break;
					}
				}
			}
		}
	}
	fclose(fp);
	return result;
}

static int reclaim_stale_buffer_pools(void)
{
	int pool_count = count_vendor_buffer_pools();
	if (pool_count == 0)
		return 0;

	/* Systemd does not overlap OneKVM processes, but refuse destructive stale
	 * cleanup if any other process still owns a vendor multimedia device. */
	if (foreign_vendor_device_owner_exists()) {
		fprintf(stderr,
			"OneKVM: refusing to reclaim %d VB pools with a live owner\n",
			pool_count);
		return -1;
	}
	if (reclaim_stale_vendor_encoders() != 0)
		return -1;
	if (reclaim_stale_vendor_vpss() != 0)
		return -1;
	if (release_stale_buffer_blocks() != 0)
		return -1;
	if (stale_buffer_pools_all_free() != 1) {
		fprintf(stderr,
			"OneKVM: refusing to drop references for %d busy VB pools\n",
			pool_count);
		return -1;
	}

	const CVI_S32 fd = get_base_fd();
	if (fd < 0) {
		fprintf(stderr, "OneKVM: open base device for stale VB reclaim failed\n");
		return -1;
	}
	int exits = 0;
	while (pool_count > 0 && exits < VB_MAX_COMM_POOLS) {
		const CVI_S32 result = vb_ioctl_exit(fd);
		if (result != CVI_SUCCESS) {
			fprintf(stderr,
				"OneKVM: stale VB exit %d failed with %#x\n",
				exits + 1, result);
			return -1;
		}
		++exits;
		pool_count = count_vendor_buffer_pools();
	}
	if (pool_count != 0) {
		fprintf(stderr,
			"OneKVM: %d stale VB pools remain after %d exits\n",
			pool_count, exits);
		return -1;
	}
	fprintf(stderr,
		"OneKVM: reclaimed stale VB pools through MMF (%d exits)\n",
		exits);
	return 0;
}

static inline CVI_VOID VENC_GetPicBufferConfig2(CVI_U32 u32Width, CVI_U32 u32Height,
	PIXEL_FORMAT_E enPixelFormat, DATA_BITWIDTH_E enBitWidth, COMPRESS_MODE_E enCmpMode,
	VB_CAL_CONFIG_S *pstVbCfg)
{
	CVI_U32 u32AlignWidth = ALIGN(u32Width, VENC_ALIGN_W);
	CVI_U32 u32AlignHeight = u32Height;
	CVI_U32 u32Align = VENC_ALIGN_W;

	COMMON_GetPicBufferConfig(u32AlignWidth, u32AlignHeight, enPixelFormat,
		enBitWidth, enCmpMode, u32Align, pstVbCfg);
}

CVI_U32 frame_buffer_size(const VIDEO_FRAME_S *frame)
{
	CVI_U64 end = frame->u64PhyAddr[0] + frame->u32Length[0];
	for (int plane = 1; plane < 3; ++plane) {
		if (frame->u64PhyAddr[plane] == 0 || frame->u32Length[plane] == 0)
			continue;
		CVI_U64 plane_end = frame->u64PhyAddr[plane] + frame->u32Length[plane];
		if (plane_end > end)
			end = plane_end;
	}
	return end > frame->u64PhyAddr[0] ? (CVI_U32)(end - frame->u64PhyAddr[0]) : 0;
}

VIDEO_FRAME_INFO_S *allocate_frame(int id, SIZE_S stSize, PIXEL_FORMAT_E enPixelFormat)
{
	VIDEO_FRAME_INFO_S *pstVideoFrame;
	VIDEO_FRAME_S *pstVFrame;
	VB_BLK blk;
	VB_CAL_CONFIG_S stVbCfg;

	pstVideoFrame = (VIDEO_FRAME_INFO_S *)calloc(1, sizeof(*pstVideoFrame));
	if (pstVideoFrame == NULL) {
		SAMPLE_PRT("Failed to allocate VIDEO_FRAME_INFO_S\n");
		return NULL;
	}

	memset(&stVbCfg, 0, sizeof(stVbCfg));
	VENC_GetPicBufferConfig2(stSize.u32Width,
				stSize.u32Height,
				enPixelFormat,
				DATA_BITWIDTH_8,
				COMPRESS_MODE_NONE,
				&stVbCfg);

	pstVFrame = &pstVideoFrame->stVFrame;

	pstVFrame->enCompressMode = COMPRESS_MODE_NONE;
	pstVFrame->enPixelFormat = enPixelFormat;
	pstVFrame->enVideoFormat = VIDEO_FORMAT_LINEAR;
	pstVFrame->enColorGamut = COLOR_GAMUT_BT709;
	pstVFrame->u32Width = stSize.u32Width;
	pstVFrame->u32Height = stSize.u32Height;
	pstVFrame->u32TimeRef = 0;
	pstVFrame->u64PTS = 0;
	pstVFrame->enDynamicRange = DYNAMIC_RANGE_SDR8;

	blk = CVI_VB_GetBlock(id, stVbCfg.u32VBSize);
	if (blk == VB_INVALID_HANDLE) {
		SAMPLE_PRT("Can't acquire vb block. id: %d size:%d\n", id, stVbCfg.u32VBSize);
		free(pstVideoFrame);
		return NULL;
	}

	pstVideoFrame->u32PoolId = CVI_VB_Handle2PoolId(blk);
	pstVFrame->u64PhyAddr[0] = CVI_VB_Handle2PhysAddr(blk);
	pstVFrame->u32Stride[0] = stVbCfg.u32MainStride;
	pstVFrame->u32Length[0] = stVbCfg.u32MainYSize;
	pstVFrame->pu8VirAddr[0] = (CVI_U8 *)CVI_SYS_MmapCache(pstVFrame->u64PhyAddr[0], stVbCfg.u32VBSize);

	if (stVbCfg.plane_num > 1) {
		pstVFrame->u64PhyAddr[1] = ALIGN(pstVFrame->u64PhyAddr[0] + stVbCfg.u32MainYSize, stVbCfg.u16AddrAlign);
		pstVFrame->u32Stride[1] = stVbCfg.u32CStride;
		pstVFrame->u32Length[1] = stVbCfg.u32MainCSize;
		pstVFrame->pu8VirAddr[1] = (CVI_U8 *)pstVFrame->pu8VirAddr[0]
			+ (pstVFrame->u64PhyAddr[1] - pstVFrame->u64PhyAddr[0]);
	}

	if (stVbCfg.plane_num > 2) {
		pstVFrame->u64PhyAddr[2] = ALIGN(pstVFrame->u64PhyAddr[1] + stVbCfg.u32MainCSize, stVbCfg.u16AddrAlign);
		pstVFrame->u32Stride[2] = stVbCfg.u32CStride;
		pstVFrame->u32Length[2] = stVbCfg.u32MainCSize;
		pstVFrame->pu8VirAddr[2] = (CVI_U8 *)pstVFrame->pu8VirAddr[0]
			+ (pstVFrame->u64PhyAddr[2] - pstVFrame->u64PhyAddr[0]);
	}

	// CVI_VENC_TRACE("phy addr(%#llx, %#llx, %#llx), Size %x\n", (long long)pstVFrame->u64PhyAddr[0]
	// 	, (long long)pstVFrame->u64PhyAddr[1], (long long)pstVFrame->u64PhyAddr[2], stVbCfg.u32VBSize);
	// CVI_VENC_TRACE("vir addr(%p, %p, %p), Size %x\n", pstVFrame->pu8VirAddr[0]
	// 	, pstVFrame->pu8VirAddr[1], pstVFrame->pu8VirAddr[2], stVbCfg.u32MainSize);

	return pstVideoFrame;
}

CVI_S32 free_frame(VIDEO_FRAME_INFO_S *pstVideoFrame)
{
	if (pstVideoFrame == NULL)
		return CVI_SUCCESS;

	VIDEO_FRAME_S *pstVFrame = &pstVideoFrame->stVFrame;
	VB_BLK blk;

	CVI_U32 mapped_size = frame_buffer_size(pstVFrame);
	if (pstVFrame->pu8VirAddr[0] && mapped_size > 0)
		CVI_SYS_Munmap((CVI_VOID *)pstVFrame->pu8VirAddr[0], mapped_size);

	blk = CVI_VB_PhysAddr2Handle(pstVFrame->u64PhyAddr[0]);
	if (blk != VB_INVALID_HANDLE) {
		CVI_VB_ReleaseBlock(blk);
	}

	free(pstVideoFrame);

	return CVI_SUCCESS;
}

static void _nanokvm_sensor_config(SAMPLE_INI_CFG_S *config)
{
	static const CVI_S16 lanes[5] = {2, 4, 3, 1, 0};

	memset(config, 0, sizeof(*config));
	config->enSource = VI_PIPE_FRAME_SOURCE_DEV;
	config->devNum = 1;
	config->enSnsType[0] = LONTIUM_LT6911_2M_60FPS_8BIT;
	config->enWDRMode[0] = WDR_MODE_NONE;
	config->s32BusId[0] = 4;
	config->s32SnsI2cAddr[0] = 0x2b;
	config->MipiDev[0] = 0;
	memcpy(config->as16LaneId[0], lanes, sizeof(lanes));
}

static int release_stale_vendor_system(void)
{
	CVI_S32 s32Ret = CVI_FAILURE;
	SAMPLE_INI_CFG_S	   	stIniCfg;
	SAMPLE_VI_CONFIG_S 		stViConfig;
	_nanokvm_sensor_config(&stIniCfg);

	s32Ret = CVI_VI_SetDevNum(stIniCfg.devNum);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("VI_SetDevNum failed with %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VI_IniToViCfg failed with %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VI_DestroyIsp(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VI_DestroyIsp failed with %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VI_DestroyVi(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VI_DestroyVi failed with %#x\n", s32Ret);
		return s32Ret;
	}

	SAMPLE_COMM_SYS_Exit();
	return s32Ret;
}

int reset_capture_channels(void)
{
	CVI_S32 s32Ret = CVI_FAILURE;
	s32Ret = close_all_capture_channels();
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("close_all_capture_channels failed with %#x\n", s32Ret);
		return s32Ret;
	}
	return s32Ret;
}

static void shutdown_vendor_system(void)
{
	if (g_stViConfig.s32WorkingViNum != 0) {
		SAMPLE_COMM_VI_DestroyIsp(&g_stViConfig);
		SAMPLE_COMM_VI_DestroyVi(&g_stViConfig);
	}
	SAMPLE_COMM_SYS_Exit();
}

static void drain_vendor_vb(void)
{
	/* SIGKILL and failed VI EnableChn leave CVI_VB_Init refs. One Exit
	   then makes VB_SetConfig a no-op ("vb has already inited"), so the
	   previous 1080p common pool (4.18 MiB) is reused for 1440p. */
	for (int i = 0; i < 16; ++i) {
		CVI_SYS_Exit();
		CVI_VB_Exit();
	}
}

static CVI_S32 initialize_vendor_system(SIZE_S stSize)
{
	VB_CONFIG_S	   stVbConf;
	CVI_U32        u32BlkSize, u32BlkRotSize;
	CVI_S32 s32Ret = CVI_SUCCESS;
	COMPRESS_MODE_E    enCompressMode   = COMPRESS_MODE_NONE;

	drain_vendor_vb();
	memset(&stVbConf, 0, sizeof(VB_CONFIG_S));
	memcpy(&stVbConf, &g_runtime.vb_conf, sizeof(VB_CONFIG_S));

	// vi
	u32BlkSize = COMMON_GetPicBufferSize(stSize.u32Width, stSize.u32Height, PIXEL_FORMAT_UYVY,
		DATA_BITWIDTH_8, enCompressMode, DEFAULT_ALIGN);
	u32BlkRotSize = COMMON_GetPicBufferSize(stSize.u32Height, stSize.u32Width, PIXEL_FORMAT_UYVY,
		DATA_BITWIDTH_8, enCompressMode, DEFAULT_ALIGN);
	u32BlkSize = MAX(u32BlkSize, u32BlkRotSize);
	stVbConf.astCommPool[MMF_VB_VI_ID].u32BlkSize	= u32BlkSize;
	/* Two UYVY blocks: VI fill + VPSS consume. A third block occupied
	   VPSS waitq=1 and added 1/fps to capture. WAVE4/VPSS private pools
	   still fit the 64 MiB carveout at 2880. */
	stVbConf.astCommPool[MMF_VB_VI_ID].u32BlkCnt =
		vi_common_pool_blocks(stSize.u32Width, stSize.u32Height);
	stVbConf.astCommPool[MMF_VB_VI_ID].enRemapMode	= VB_REMAP_MODE_CACHED;
	stVbConf.u32MaxPoolCnt = 1;
	fprintf(stderr, "OneKVM: common VB %ux%u blk=%u count=%u\n",
		stSize.u32Width, stSize.u32Height, u32BlkSize,
		stVbConf.astCommPool[MMF_VB_VI_ID].u32BlkCnt);

	s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("system init failed with %#x\n", s32Ret);
		goto error;
	}

	return s32Ret;
error:
	shutdown_vendor_system();
	return s32Ret;
}

CVI_S32 _mmf_vpss_deinit(VPSS_GRP VpssGrp, VPSS_CHN VpssChn)
{
	CVI_BOOL           abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};
	CVI_S32 s32Ret = CVI_SUCCESS;

	/*start vpss*/
	abChnEnable[VpssChn] = CVI_TRUE;
	s32Ret = SAMPLE_COMM_VPSS_Stop(VpssGrp, abChnEnable);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("init vpss group failed. s32Ret: 0x%x !\n", s32Ret);
	}

	return s32Ret;
}

CVI_S32 destroy_vpss_group(VPSS_GRP VpssGrp)
{
	CVI_S32 s32Ret = CVI_SUCCESS;

	s32Ret = CVI_VPSS_StopGrp(VpssGrp);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("Vpss Stop Grp %d failed! Please check param\n", VpssGrp);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VPSS_DestroyGrp(VpssGrp);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("Vpss Destroy Grp %d failed! Please check\n", VpssGrp);
		return CVI_FAILURE;
	}

	return s32Ret;
}

// fit = 0, width to new width, height to new height, may be stretch
// fit = 1, keep aspect ratio, fill blank area with black color
// fit = other, keep aspect ratio, crop image to fit new size
CVI_S32 _mmf_vpss_init(VPSS_GRP VpssGrp, VPSS_CHN VpssChn, SIZE_S stSizeIn, SIZE_S stSizeOut, PIXEL_FORMAT_E formatIn, PIXEL_FORMAT_E formatOut,
int fps, int depth, bool mirror, bool flip, int fit)
{
	VPSS_GRP_ATTR_S    stVpssGrpAttr;
	VPSS_CROP_INFO_S   stGrpCropInfo;
	CVI_BOOL           abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};
	VPSS_CHN_ATTR_S    astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM];
	CVI_S32 s32Ret = CVI_SUCCESS;

	memset(&stVpssGrpAttr, 0, sizeof(VPSS_GRP_ATTR_S));
	stVpssGrpAttr.stFrameRate.s32SrcFrameRate    = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate    = -1;
	stVpssGrpAttr.enPixelFormat                  = formatIn;
	stVpssGrpAttr.u32MaxW                        = stSizeIn.u32Width;
	stVpssGrpAttr.u32MaxH                        = stSizeIn.u32Height;
	stVpssGrpAttr.u8VpssDev                      = 0;

	CVI_FLOAT corp_scale_w = (CVI_FLOAT)stSizeIn.u32Width / stSizeOut.u32Width;
	CVI_FLOAT corp_scale_h = (CVI_FLOAT)stSizeIn.u32Height / stSizeOut.u32Height;
	CVI_U32 crop_w = -1, crop_h = -1;
	if (fit == 0) {
		memset(astVpssChnAttr, 0, sizeof(VPSS_CHN_ATTR_S) * VPSS_MAX_PHY_CHN_NUM);
		astVpssChnAttr[VpssChn].u32Width                    = stSizeOut.u32Width;
		astVpssChnAttr[VpssChn].u32Height                   = stSizeOut.u32Height;
		astVpssChnAttr[VpssChn].enVideoFormat               = VIDEO_FORMAT_LINEAR;
		astVpssChnAttr[VpssChn].enPixelFormat               = formatOut;
		astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = fps;
		astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = fps;
		astVpssChnAttr[VpssChn].u32Depth                    = depth;
		astVpssChnAttr[VpssChn].bMirror                     = mirror;
		astVpssChnAttr[VpssChn].bFlip                       = flip;
		astVpssChnAttr[VpssChn].stAspectRatio.enMode        = ASPECT_RATIO_MANUAL;
		astVpssChnAttr[VpssChn].stAspectRatio.stVideoRect.s32X       = 0;
		astVpssChnAttr[VpssChn].stAspectRatio.stVideoRect.s32Y       = 0;
		astVpssChnAttr[VpssChn].stAspectRatio.stVideoRect.u32Width   = stSizeOut.u32Width;
		astVpssChnAttr[VpssChn].stAspectRatio.stVideoRect.u32Height  = stSizeOut.u32Height;
		astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
		astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
		astVpssChnAttr[VpssChn].stNormalize.bEnable         = CVI_FALSE;

		stGrpCropInfo.bEnable = false;
	} else if (fit == 1) {
		memset(astVpssChnAttr, 0, sizeof(VPSS_CHN_ATTR_S) * VPSS_MAX_PHY_CHN_NUM);
		astVpssChnAttr[VpssChn].u32Width                    = stSizeOut.u32Width;
		astVpssChnAttr[VpssChn].u32Height                   = stSizeOut.u32Height;
		astVpssChnAttr[VpssChn].enVideoFormat               = VIDEO_FORMAT_LINEAR;
		astVpssChnAttr[VpssChn].enPixelFormat               = formatOut;
		astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = fps;
		astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = fps;
		astVpssChnAttr[VpssChn].u32Depth                    = depth;
		astVpssChnAttr[VpssChn].bMirror                     = mirror;
		astVpssChnAttr[VpssChn].bFlip                       = flip;
		astVpssChnAttr[VpssChn].stAspectRatio.enMode        = ASPECT_RATIO_AUTO;
		astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
		astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
		astVpssChnAttr[VpssChn].stNormalize.bEnable         = CVI_FALSE;

		stGrpCropInfo.bEnable = false;
	} else {
		memset(astVpssChnAttr, 0, sizeof(VPSS_CHN_ATTR_S) * VPSS_MAX_PHY_CHN_NUM);
		astVpssChnAttr[VpssChn].u32Width                    = stSizeOut.u32Width;
		astVpssChnAttr[VpssChn].u32Height                   = stSizeOut.u32Height;
		astVpssChnAttr[VpssChn].enVideoFormat               = VIDEO_FORMAT_LINEAR;
		astVpssChnAttr[VpssChn].enPixelFormat               = formatOut;
		astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = fps;
		astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = fps;
		astVpssChnAttr[VpssChn].u32Depth                    = depth;
		astVpssChnAttr[VpssChn].bMirror                     = mirror;
		astVpssChnAttr[VpssChn].bFlip                       = flip;
		astVpssChnAttr[VpssChn].stAspectRatio.enMode        = ASPECT_RATIO_AUTO;
		astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
		astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
		astVpssChnAttr[VpssChn].stNormalize.bEnable         = CVI_FALSE;

		crop_w = corp_scale_w < corp_scale_h ? stSizeOut.u32Width * corp_scale_w: stSizeOut.u32Width * corp_scale_h;
		crop_h = corp_scale_w < corp_scale_h ? stSizeOut.u32Height * corp_scale_w: stSizeOut.u32Height * corp_scale_h;
		if (corp_scale_h < 0 || corp_scale_w < 0) {
			SAMPLE_PRT("crop scale error. corp_scale_w: %f, corp_scale_h: %f\n", corp_scale_w, corp_scale_h);
			goto error;
		}

		stGrpCropInfo.bEnable = true;
		stGrpCropInfo.stCropRect.s32X = (stSizeIn.u32Width - crop_w) / 2;
		stGrpCropInfo.stCropRect.s32Y = (stSizeIn.u32Height - crop_h) / 2;
		stGrpCropInfo.stCropRect.u32Width = crop_w;
		stGrpCropInfo.stCropRect.u32Height = crop_h;
	}

	/*start vpss*/
	abChnEnable[0] = CVI_TRUE;
	s32Ret = SAMPLE_COMM_VPSS_Init(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("init vpss group failed. s32Ret: 0x%x ! retry!!!\n", s32Ret);
		s32Ret = SAMPLE_COMM_VPSS_Stop(VpssGrp, abChnEnable);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("stop vpss group failed. s32Ret: 0x%x !\n", s32Ret);
		}
		s32Ret = SAMPLE_COMM_VPSS_Init(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("retry to init vpss group failed. s32Ret: 0x%x !\n", s32Ret);
			return s32Ret;
		} else {
			SAMPLE_PRT("retry to init vpss group ok!\n");
		}
	}

	if (crop_w != 0 && crop_h != 0) {
		s32Ret = CVI_VPSS_SetChnCrop(VpssGrp, VpssChn, &stGrpCropInfo);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("set vpss group crop failed. s32Ret: 0x%x !\n", s32Ret);
			goto error;
		}
	}

	s32Ret = SAMPLE_COMM_VPSS_Start(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("start vpss group failed. s32Ret: 0x%x !\n", s32Ret);
		goto error;
	}

	return s32Ret;
error:
	_mmf_vpss_deinit(VpssGrp, VpssChn);
	return s32Ret;
}

static CVI_S32 initialize_runtime(void)
{
	MMF_VERSION_S stVersion;
	SAMPLE_INI_CFG_S	   stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;

	PIC_SIZE_E enPicSize;
	SIZE_S stSize;
	CVI_S32 s32Ret = CVI_SUCCESS;
	LOG_LEVEL_CONF_S log_conf;

	int old_pool_cnt = count_vendor_buffer_pools();
	if (old_pool_cnt > 0) {
		if (module_in_use("soph_vi") == 0) {
			/* Kernel module lifetime belongs to the systemd module service.
			 * The vendor implementation used to rmmod half of the media stack
			 * here and reload it from /mnt/system/ko, which breaks online
			 * restarts and leaves missing device nodes. Stale VB pools are
			 * reclaimed below through the MMF APIs instead. */
			printf("found %d stale VB pools; reclaiming without reloading modules\n",
			       old_pool_cnt);
		} else {
			printf("You may have repeatedly initialized OneKVM MMF!\n");
		}
	}

	CVI_SYS_GetVersion(&stVersion);
	SAMPLE_PRT("OneKVM MMF version:%s\n", stVersion.version);

	log_conf.enModId = CVI_ID_LOG;
	log_conf.s32Level = CVI_DBG_DEBUG;
	CVI_LOG_SetLevelConf(&log_conf);

	_nanokvm_sensor_config(&stIniCfg);

	//Set sensor number
	CVI_VI_SetDevNum(stIniCfg.devNum);

	/************************************************
	 * step1:  Config VI
	 ************************************************/
	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));
	memcpy(&g_stIniCfg, &stIniCfg, sizeof(SAMPLE_INI_CFG_S));

	/************************************************
	 * step2:  Get input size
	 ************************************************/
	s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed with %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed with %#x\n", s32Ret);
		return s32Ret;
	}
	/* PIC_1080P is the only enum exposed for the LT6911 sensor, but it is a
	 * maximum/capability value rather than the current HDMI timing. */
	onekvm_lt6911_get_active_size(&stSize.u32Width, &stSize.u32Height);

	/************************************************
	 * step3:  Init modules
	 ************************************************/
	if (reclaim_stale_buffer_pools() != 0) {
		SAMPLE_PRT("reclaim stale VB pools failed\n");
		return CVI_FAILURE;
	}

	/* Common VB must cover 1080p VPSS output and the current HDMI frame.
	 * 1440p VI EnableChn asks for 2560x1440 NV21 (5.53 MiB); a 1080p UYVY
	 * pool (4.18 MiB) fails with "No valid pool for size(5529600)". */
	const auto pool = onekvm::common_vb_pool_size(
		{stSize.u32Width, stSize.u32Height});
	SIZE_S stPoolSize = {pool.width, pool.height};
	SAMPLE_PRT("common VB pool %ux%u (HDMI %ux%u)\n",
		   stPoolSize.u32Width, stPoolSize.u32Height,
		   stSize.u32Width, stSize.u32Height);
	s32Ret = initialize_vendor_system(stPoolSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("sys init failed. s32Ret: 0x%x !\n", s32Ret);
		goto _need_exit_sys_and_deinit_vi;
	}

	s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi init failed. s32Ret: 0x%x !\n", s32Ret);
		SAMPLE_PRT("Please try to check if the camera is working.\n");
		goto _need_exit_sys_and_deinit_vi;
	}

	g_runtime.vi_dma_running = true;
	g_runtime.vi_chn_attr_valid =
		CVI_VI_GetChnAttr(0, 0, &g_runtime.vi_chn_attr) == CVI_SUCCESS;
	g_runtime.vi_size.u32Width = stSize.u32Width;
	g_runtime.vi_size.u32Height = stSize.u32Height;

	return s32Ret;

_need_exit_sys_and_deinit_vi:
	shutdown_vendor_system();

	return s32Ret;
}

static void destroy_runtime(void)
{
	/* Bound H.26x channels consume VPSS output directly. Disconnect and stop
	 * those workers before destroying their producer channels during an HDMI
	 * input-size rebuild. */
	close_all_h26x_encoders();
	close_jpeg_encoder(0);
	close_all_capture_channels();
	stop_capture_pipeline();
	shutdown_vendor_system();
}

static int find_unused_capture_channel() {
	for (int i = 0; i < MMF_VI_MAX_CHN; i++) {
		if (g_runtime.vi_chn_is_inited[i] == false) {
			return i;
		}
	}
	return -1;
}

int initialize(void)
{
    if (g_runtime.reference_count) {
		g_runtime.reference_count ++;
        // printf("OneKVM MMF already inited(cnt:%d)\n", g_runtime.reference_count);
        return 0;
    }

	if (!soph_media_modules_ready()) {
		printf("OneKVM MMF: soph_* modules not loaded yet\n");
		return -1;
	}

	if (release_stale_vendor_system() != CVI_SUCCESS) {
		printf("try release sys failed\n");
		return -1;
	} else {
		printf("try release sys ok\n");
	}

    if (initialize_runtime() != CVI_SUCCESS) {
        printf("OneKVM MMF init failed\n");
        return -1;
    } else {
		printf("OneKVM MMF init ok\n");
	}

	g_runtime.reference_count = 1;

	if (reset_capture_channels() != CVI_SUCCESS) {
		printf("try release vio failed\n");
		return -1;
	} else {
		printf("try release vio ok\n");
	}

    return 0;
}

int shutdown(void) {
	if (!g_runtime.reference_count) {
		return 0;
	}

	if (--g_runtime.reference_count > 0)
		return 0;

	printf("OneKVM MMF runtime stopped.\n");
	destroy_runtime();
	return 0;
}

int find_free_capture_channel(void) {
	return find_unused_capture_channel();
}

} // namespace onekvm::mmf
