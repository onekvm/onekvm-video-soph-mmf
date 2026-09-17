#include "mmf_internal.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <stdio.h>

namespace onekvm::mmf {

static void release_vpss_user_frames();

void set_capture_mirror(int channel, bool enabled)
{
	if (channel < 0 || channel >= MMF_VI_MAX_CHN)
		return;
	g_capture_options.mirror[channel] = enabled;
}

void set_capture_flip(int channel, bool enabled)
{
	if (channel < 0 || channel >= MMF_VI_MAX_CHN)
		return;
	g_capture_options.flip[channel] = enabled;
}

static CVI_S32 configure_vpss_channel(VPSS_GRP VpssGrp, VPSS_CHN VpssChn, int width, int height, PIXEL_FORMAT_E format, int fps, int depth, bool mirror, bool flip, int fit)
{
#if 1
	VPSS_GRP_ATTR_S stGrpAttr;
	VPSS_CROP_INFO_S   stChnCropInfo{};
	VPSS_CHN_ATTR_S chn_attr{};
	CVI_S32 s32Ret = CVI_SUCCESS;

	s32Ret = CVI_VPSS_GetGrpAttr(VpssGrp, &stGrpAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_GetGrpAttr failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}
	CVI_FLOAT corp_scale_w = (CVI_FLOAT)stGrpAttr.u32MaxW / width;
	CVI_FLOAT corp_scale_h = (CVI_FLOAT)stGrpAttr.u32MaxH / height;
	CVI_U32 crop_w = -1, crop_h = -1;
	if (fit == 0) {
		chn_attr.u32Width                    = width;
		chn_attr.u32Height                   = height;
		chn_attr.enVideoFormat               = VIDEO_FORMAT_LINEAR;
		chn_attr.enPixelFormat               = format;
		chn_attr.stFrameRate.s32SrcFrameRate = fps;
		chn_attr.stFrameRate.s32DstFrameRate = fps;
		chn_attr.u32Depth                    = depth;
		chn_attr.bMirror                     = mirror;
		chn_attr.bFlip                       = flip;
		chn_attr.stAspectRatio.enMode        = ASPECT_RATIO_MANUAL;
		chn_attr.stAspectRatio.stVideoRect.s32X       = 0;
		chn_attr.stAspectRatio.stVideoRect.s32Y       = 0;
		chn_attr.stAspectRatio.stVideoRect.u32Width   = width;
		chn_attr.stAspectRatio.stVideoRect.u32Height  = height;
		chn_attr.stAspectRatio.bEnableBgColor = CVI_TRUE;
		chn_attr.stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
		chn_attr.stNormalize.bEnable         = CVI_FALSE;

		stChnCropInfo.bEnable = false;
	} else if (fit == 1) {
		chn_attr.u32Width                    = width;
		chn_attr.u32Height                   = height;
		chn_attr.enVideoFormat               = VIDEO_FORMAT_LINEAR;
		chn_attr.enPixelFormat               = format;
		chn_attr.stFrameRate.s32SrcFrameRate = fps;
		chn_attr.stFrameRate.s32DstFrameRate = fps;
		chn_attr.u32Depth                    = depth;
		chn_attr.bMirror                     = mirror;
		chn_attr.bFlip                       = flip;
		chn_attr.stAspectRatio.enMode        = ASPECT_RATIO_AUTO;
		chn_attr.stAspectRatio.bEnableBgColor = CVI_TRUE;
		chn_attr.stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
		chn_attr.stNormalize.bEnable         = CVI_FALSE;

		stChnCropInfo.bEnable = false;
	} else {
		chn_attr.u32Width                    = width;
		chn_attr.u32Height                   = height;
		chn_attr.enVideoFormat               = VIDEO_FORMAT_LINEAR;
		chn_attr.enPixelFormat               = format;
		chn_attr.stFrameRate.s32SrcFrameRate = fps;
		chn_attr.stFrameRate.s32DstFrameRate = fps;
		chn_attr.u32Depth                    = depth;
		chn_attr.bMirror                     = mirror;
		chn_attr.bFlip                       = flip;
		chn_attr.stAspectRatio.enMode        = ASPECT_RATIO_AUTO;
		chn_attr.stAspectRatio.bEnableBgColor = CVI_TRUE;
		chn_attr.stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
		chn_attr.stNormalize.bEnable         = CVI_FALSE;

		crop_w = corp_scale_w < corp_scale_h ? width * corp_scale_w: width * corp_scale_h;
		crop_h = corp_scale_w < corp_scale_h ? height * corp_scale_w: height * corp_scale_h;
		if (corp_scale_h < 0 || corp_scale_w < 0) {
			SAMPLE_PRT("crop scale error. corp_scale_w: %f, corp_scale_h: %f\n", corp_scale_w, corp_scale_h);
			return -1;
		}

		stChnCropInfo.bEnable = true;
		stChnCropInfo.enCropCoordinate = VPSS_CROP_ABS_COOR;
		stChnCropInfo.stCropRect.s32X = (stGrpAttr.u32MaxW - crop_w) / 2;
		stChnCropInfo.stCropRect.s32Y = (stGrpAttr.u32MaxH - crop_h) / 2;
		stChnCropInfo.stCropRect.u32Width = crop_w;
		stChnCropInfo.stCropRect.u32Height = crop_h;
	}

	if (stChnCropInfo.bEnable) {
		s32Ret = CVI_VPSS_SetChnCrop(VpssGrp, VpssChn, &stChnCropInfo);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("set vpss group crop failed. s32Ret: 0x%x !\n", s32Ret);
			return -1;
		}
	}

	s32Ret = CVI_VPSS_SetChnAttr(VpssGrp, VpssChn, &chn_attr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_SetChnAttr failed with %#x\n", s32Ret);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VPSS_EnableChn(VpssGrp, VpssChn);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_EnableChn failed with %#x\n", s32Ret);
		return CVI_FAILURE;
	}

	return s32Ret;
#else
	CVI_S32 s32Ret;
	VPSS_CHN_ATTR_S chn_attr = {0};
	chn_attr.u32Width                    = width;
	chn_attr.u32Height                   = height;
	chn_attr.enVideoFormat               = VIDEO_FORMAT_LINEAR;
	chn_attr.enPixelFormat               = format;
	chn_attr.stFrameRate.s32SrcFrameRate = fps;
	chn_attr.stFrameRate.s32DstFrameRate = fps;
	chn_attr.u32Depth                    = depth;
	chn_attr.bMirror                     = mirror;
	chn_attr.bFlip                       = flip;
	chn_attr.stAspectRatio.enMode        = ASPECT_RATIO_MANUAL;
	chn_attr.stAspectRatio.stVideoRect.s32X       = 0;
	chn_attr.stAspectRatio.stVideoRect.s32Y       = 0;
	chn_attr.stAspectRatio.stVideoRect.u32Width   = width;
	chn_attr.stAspectRatio.stVideoRect.u32Height  = height;
	chn_attr.stAspectRatio.bEnableBgColor = CVI_TRUE;
	chn_attr.stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
	chn_attr.stNormalize.bEnable         = CVI_FALSE;

	s32Ret = CVI_VPSS_SetChnAttr(VpssGrp, VpssChn, &chn_attr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_SetChnAttr failed with %#x\n", s32Ret);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VPSS_EnableChn(VpssGrp, VpssChn);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_EnableChn failed with %#x\n", s32Ret);
		return CVI_FAILURE;
	}

	return CVI_SUCCESS;
#endif
}

static CVI_S32 disable_vpss_channel(VPSS_GRP VpssGrp, VPSS_CHN VpssChn)
{
	return CVI_VPSS_DisableChn(VpssGrp, VpssChn);
}

static CVI_S32 create_vpss_group(VPSS_GRP VpssGrp, CVI_U32 width, CVI_U32 height, PIXEL_FORMAT_E format)
{
	VPSS_GRP_ATTR_S    stVpssGrpAttr;
	CVI_S32 s32Ret = CVI_SUCCESS;

	memset(&stVpssGrpAttr, 0, sizeof(VPSS_GRP_ATTR_S));
	stVpssGrpAttr.stFrameRate.s32SrcFrameRate    = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate    = -1;
	stVpssGrpAttr.enPixelFormat                  = format;
	stVpssGrpAttr.u32MaxW                        = width;
	stVpssGrpAttr.u32MaxH                        = height;
	/* Leave 0: the driver remaps YUV groups onto input_mem (dev 1).
	   Forcing 1 makes CVI_VPSS_CreateGrp fail in single mode. */
	stVpssGrpAttr.u8VpssDev                      = 0;

	/* SIGKILL leaves grp started. DestroyGrp without StopGrp fails, and the
	   next CreateGrp then attaches to a WAVE4/VPSS zombie (VI RecvPic runs,
	   EncodedFrame stays 0). Always Stop then Destroy before creating. */
	(void)CVI_VPSS_StopGrp(VpssGrp);
	(void)CVI_VPSS_DestroyGrp(VpssGrp);

	s32Ret = CVI_VPSS_CreateGrp(VpssGrp, &stVpssGrpAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_CreateGrp(grp:%d) retry(%#x)!\n", VpssGrp, s32Ret);
		(void)CVI_VPSS_StopGrp(VpssGrp);
		(void)CVI_VPSS_DestroyGrp(VpssGrp);

		s32Ret = CVI_VPSS_CreateGrp(VpssGrp, &stVpssGrpAttr);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("CVI_VPSS_CreateGrp(grp:%d) failed with %#x!\n", VpssGrp, s32Ret);
			return CVI_FAILURE;
		}
	}

	s32Ret = CVI_VPSS_ResetGrp(VpssGrp);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_ResetGrp(grp:%d) failed with %#x!%d\n", VpssGrp, s32Ret, CVI_ERR_VPSS_ILLEGAL_PARAM);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VPSS_StartGrp(VpssGrp);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_StartGrp failed with %#x\n", s32Ret);
		return CVI_FAILURE;
	}
	return s32Ret;
}

int start_capture_pipeline(void)
{
	if (g_runtime.vi_is_inited) {
		return 0;
	}

	CVI_S32 s32Ret = CVI_SUCCESS;
	s32Ret = create_vpss_group(0, g_runtime.vi_size.u32Width, g_runtime.vi_size.u32Height, PIXEL_FORMAT_UYVY);		// PIXEL_FORMAT_UYVY  PIXEL_FORMAT_NV21
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("create_vpss_group failed. s32Ret: 0x%x !\n", s32Ret);
		return s32Ret;
	}

	g_runtime.vi_is_inited = true;

	return s32Ret;
}

int stop_capture_pipeline(void)
{
	if (!g_runtime.vi_is_inited) {
		return 0;
	}

	CVI_S32 s32Ret = CVI_SUCCESS;
	s32Ret = destroy_vpss_group(0);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("destroy_vpss_group failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	g_runtime.vi_is_inited = false;
	release_vpss_user_frames();

	return s32Ret;
}

static int create_capture_channel(int ch, int width, int height, int format, int fps) {
	uint32_t pool_size_out = 0;
	int pool_id = -1;
	if (ch < 0 || ch >= MMF_VI_MAX_CHN) {
		printf("invalid vi channel:%d\n", ch);
		return -1;
	}

	if (!g_runtime.reference_count) {
		printf("%s: OneKVM MMF or vi not inited\n", __func__);
		return -1;
	}

	if (!g_runtime.vi_is_inited) {
		if (0 != start_capture_pipeline()) {
			printf("start_capture_pipeline failed!\r\n");
			return -1;
		}
	}

	if (width <= 0 || height <= 0) {
		printf("invalid width or height\n");
		return -1;
	}
	if (fps <= 0 || fps > 120) {
		printf("invalid vi fps:%d\n", fps);
		return -1;
	}

	if (format != PIXEL_FORMAT_NV21
		&& format != PIXEL_FORMAT_RGB_888) {
		printf("invalid format\n");
		return -1;
	}

	// if ((format == PIXEL_FORMAT_RGB_888 && width * height * 3 > 640 * 640 * 3)
	// 	|| (format == PIXEL_FORMAT_RGB_888 && width * height * 3 / 2 > 2560 * 1440 * 3 / 2)) {
	// 	printf("camera size is too large, for NV21, maximum resolution 2560x1440, for RGB888, maximum resolution 640x640!\n");
	// 	return -1;
	// }

	if (capture_channel_open(ch)) {
		printf("vi ch %d already open\n", ch);
		return -1;
	}

	CVI_S32 s32Ret = CVI_SUCCESS;
	/* A depth of one prevents VPSS from retaining an extra completed frame.
	 * At 60 Hz that stale frame costs about 16.7 ms before encoding starts. */
	const int depth = MMF_VPSS_LOW_LATENCY_DEPTH;
	/* Keep the logical picture size. DEFAULT_ALIGN only pads the VB stride. */
	PIXEL_FORMAT_E format_out = (PIXEL_FORMAT_E)format;
	const bool mirror = g_capture_options.mirror[ch];
	const bool flip = g_capture_options.flip[ch];
	std::fprintf(stderr, "OneKVM: VPSS phy chn %d (%s) %dx%d\n",
		     ch, ch == 1 ? "sc_v1" : "sc_d", width, height);
	s32Ret = disable_vpss_channel(0, ch);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("disable_vpss_channel failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	/* fit=2 enables an identity crop. At 2560x1440 that crop path
	   produces a zero NV21 while the UYVY VI pool still has pixels. */
	s32Ret = configure_vpss_channel(0, ch, width, height, format_out, fps, depth, mirror, flip, 0);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("configure_vpss_channel failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	char name[20];
	snprintf(name, 20, "vi_vpss%.1d", ch);
	pool_size_out = COMMON_GetPicBufferSize(width, height, format_out, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	/* Three independent owners can overlap at 1080p60:
	 *
	 *   1 queued VPSS frame (`depth`)
	 *   1 frame borrowed by Core and the synchronous zero-copy VENC call
	 *   1 block being filled by the VPSS worker
	 *
	 * Allocating only depth + producer (two blocks) starves VPSS whenever VENC
	 * crosses a 16.7 ms frame boundary.  The worker then retries in a tight
	 * loop, floods dmesg with "Can't acquire VB BLK", and collapses the stream
	 * to roughly 20-35 FPS. The third block absorbs that bounded overlap while
	 * u32Depth remains one, so it adds capacity without adding queue latency. */
	const int pool_blocks = capture_pool_blocks(
		static_cast<int>(g_runtime.vi_size.u32Width),
		static_cast<int>(g_runtime.vi_size.u32Height));
	pool_id = _create_vb_pool(name, pool_size_out, pool_blocks);
	if (pool_id < 0) {
		printf("[%s][%d]_create_vb_pool failed, id %d\n", __func__, __LINE__, pool_id);
		goto _need_deinit_vpss_chn;
	}

	s32Ret = CVI_VPSS_AttachVbPool(0, ch, pool_id);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_AttachVbPool failed. s32Ret: 0x%x !\n", s32Ret);
		goto _need_destroy_vb_pool;
	}

	/* The newest OSDRV runs the VPSS worker as a real-time FIFO thread.  Bind
	 * VI only after the output pool is usable; otherwise the first VI frame
	 * enters an unbuffered channel and the worker retries forever, starving
	 * the userspace thread before it can reach AttachVbPool(). */
	/* VI only has chn 0. `ch` is the VPSS physical scaler (0=sc_d, 1=sc_v1). */
	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, 0);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi bind vpss failed. s32Ret: 0x%x !\n", s32Ret);
		goto _need_detach_vb_pool;
	}
	g_runtime.vi_bound_to_vpss = true;

	// VIDEO_FRAME_INFO_S frame;
	// if ((s32Ret = CVI_VPSS_GetChnFrame(0, ch, &frame, 3000)) != CVI_SUCCESS) {
	// 	SAMPLE_PRT("vi get frame failed: 0x%x !\n", s32Ret);
	// 	if ((s32Ret = SAMPLE_COMM_VI_UnBind_VPSS(0, ch, 0)) != CVI_SUCCESS) {
	// 		SAMPLE_PRT("vi unbind vpss failed. s32Ret: 0x%x !\n", s32Ret);
	// 	}
	// 	goto _need_deinit_vpss_chn;
	// }
	// CVI_VPSS_ReleaseChnFrame(0, ch, &frame);

	g_runtime.vi_chn_pool_id[ch] = pool_id;
	g_runtime.vi_chn_is_inited[ch] = true;
	g_runtime.vi_chn_running[ch] = true;
	g_runtime.vi_dma_running = true;
	/* HDMI presence is LT6911 CSI timing. Stop scaler and VI DMA until a
	   bound encoder or snapshot actually consumes frames. */
	if (pause_vpss_channel(ch) != 0)
		SAMPLE_PRT("pause VPSS chn %d after open failed\n", ch);
	if (pause_vi_dma() != 0)
		SAMPLE_PRT("pause VI DMA after open failed\n");

	return 0;
_need_detach_vb_pool:
	CVI_VPSS_DetachVbPool(0, ch);
_need_destroy_vb_pool:
	_destroy_vb_pool(pool_id);
_need_deinit_vpss_chn:
	disable_vpss_channel(0, ch);
	return -1;
}

int open_capture_channel(int ch, int width, int height, int format, int fps) {
	return create_capture_channel(ch, width, height, format, fps);
}

static void clear_capture_mappings(int ch)
{
	if (ch < 0 || ch >= MMF_VI_MAX_CHN)
		return;
	for (int index = 0; index < MMF_VI_MAP_CACHE_SIZE; ++index) {
		CaptureMapping *mapping = &g_runtime.vi_mappings[ch][index];
		if (mapping->vir_addr != NULL && mapping->size > 0)
			CVI_SYS_Munmap(mapping->vir_addr, mapping->size);
		memset(mapping, 0, sizeof(*mapping));
	}
	g_runtime.vi_map_next[ch] = 0;
	memset(&g_runtime.vi_frame[ch], 0, sizeof(g_runtime.vi_frame[ch]));
}

static CVI_VOID *map_capture_frame(int ch, CVI_U64 phy_addr, CVI_U32 size)
{
	CaptureMapping *slot = NULL;
	for (int index = 0; index < MMF_VI_MAP_CACHE_SIZE; ++index) {
		CaptureMapping *mapping = &g_runtime.vi_mappings[ch][index];
		if (mapping->vir_addr != NULL && mapping->phy_addr == phy_addr && mapping->size == size)
			return mapping->vir_addr;
		if (slot == NULL && mapping->vir_addr == NULL)
			slot = mapping;
	}

	if (slot == NULL) {
		slot = &g_runtime.vi_mappings[ch][g_runtime.vi_map_next[ch] % MMF_VI_MAP_CACHE_SIZE];
		g_runtime.vi_map_next[ch] = (g_runtime.vi_map_next[ch] + 1) % MMF_VI_MAP_CACHE_SIZE;
		if (slot->vir_addr != NULL && slot->size > 0)
			CVI_SYS_Munmap(slot->vir_addr, slot->size);
	}

	CVI_VOID *vir_addr = CVI_SYS_MmapCache(phy_addr, size);
	if (vir_addr == NULL) {
		memset(slot, 0, sizeof(*slot));
		return NULL;
	}
	slot->phy_addr = phy_addr;
	slot->vir_addr = vir_addr;
	slot->size = size;
	return vir_addr;
}

int close_capture_channel(int ch) {
	if (ch < 0 || ch >= MMF_VI_MAX_CHN) {
		printf("[%d] invalid ch %d\n", __LINE__, ch);
		return -1;
	}

	if (g_runtime.vi_chn_is_inited[ch] == false) {
		return 0;
	}

	CVI_S32 s32Ret = CVI_SUCCESS;
	/* Teardown needs the scaler and VI DMA enabled; DisableChn while idle
	   leaves VI Stop* hanging. */
	(void)resume_vpss_channel(ch);
	(void)resume_vi_dma();
	s32Ret = SAMPLE_COMM_VI_UnBind_VPSS(0, 0, 0);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi unbind vpss failed. s32Ret: 0x%x !\n", s32Ret);
		// return -1; // continue to deinit vpss
	}
	g_runtime.vi_bound_to_vpss = false;

	if (0 != disable_vpss_channel(0, ch)) {
		SAMPLE_PRT("disable_vpss_channel failed. s32Ret: 0x%x !\n", s32Ret);
	}

	clear_capture_mappings(ch);
	CVI_VPSS_DetachVbPool(0, ch);
	_destroy_vb_pool(g_runtime.vi_chn_pool_id[ch]);

	g_runtime.vi_chn_pool_id[ch] = -1;
	g_runtime.vi_chn_is_inited[ch] = false;
	g_runtime.vi_chn_running[ch] = false;
	return s32Ret;
}

int close_all_capture_channels() {
	for (int i = 0; i < MMF_VI_MAX_CHN; i++) {
		if (g_runtime.vi_chn_is_inited[i] == true) {
			close_capture_channel(i);
		}
	}
	return 0;
}

bool capture_channel_open(int ch) {
	if (ch < 0 || ch >= MMF_VI_MAX_CHN) {
		return false;
	}

	return g_runtime.vi_chn_is_inited[ch];
}

int pause_vpss_channel(int ch)
{
	if (!capture_channel_open(ch) || !g_runtime.vi_chn_running[ch])
		return 0;
	const CVI_S32 ret = CVI_VPSS_DisableChn(0, ch);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_DisableChn(%d) failed with %#x\n", ch, ret);
		return ret;
	}
	g_runtime.vi_chn_running[ch] = false;
	std::fprintf(stderr, "OneKVM: VPSS chn %d paused\n", ch);
	return 0;
}

int resume_vpss_channel(int ch)
{
	if (!capture_channel_open(ch))
		return -1;
	if (g_runtime.vi_chn_running[ch])
		return 0;
	const CVI_S32 ret = CVI_VPSS_EnableChn(0, ch);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_EnableChn(%d) failed with %#x\n", ch, ret);
		return ret;
	}
	g_runtime.vi_chn_running[ch] = true;
	std::fprintf(stderr, "OneKVM: VPSS chn %d resumed\n", ch);
	return 0;
}

int enable_vi_dma(void)
{
	if (g_runtime.vi_dma_running)
		return 0;
	/* DisableChn drops LT6911 CSI TX to 0x0. EnableChn alone does not
	   restore it; the same 805a/8010/D283 sequence as open_source must
	   run before the SoC RX starts again. StartViChn also SetChnAttr
	   first: Disable clears the proc channel, so Enable without attr
	   leaves VI CHN ATTR empty and Preraw at 0. */
	if (lt6911_start_csi() != 0)
		SAMPLE_PRT("LT6911 CSI re-arm before EnableChn failed\n");
	if (g_runtime.vi_chn_attr_valid) {
		const CVI_S32 attr = CVI_VI_SetChnAttr(0, 0, &g_runtime.vi_chn_attr);
		if (attr != CVI_SUCCESS) {
			SAMPLE_PRT("CVI_VI_SetChnAttr failed with %#x\n", attr);
			return attr;
		}
	}
	const CVI_S32 ret = CVI_VI_EnableChn(0, 0);
	if (ret != CVI_SUCCESS && ret != CVI_ERR_VI_FAILED_NOT_DISABLED) {
		SAMPLE_PRT("CVI_VI_EnableChn failed with %#x\n", ret);
		return ret;
	}
	g_runtime.vi_dma_running = true;
	std::fprintf(stderr, "OneKVM: VI DMA resumed\n");
	return 0;
}

int pause_vi_dma(void)
{
	if (!g_runtime.vi_dma_running)
		return 0;
	/* Do not CVI_VI_DisableChn and do not UnBind VI→VPSS. On SG2002 HDMI
	   both stop Preraw: IntCnt dies, EnableChn/SetChnAttr do not bring
	   CSIBDG back, and the encoder falls through to the no-signal still.
	   Idle parks the VPSS scaler only. */
	return 0;
}

int resume_vi_dma(void)
{
	if (enable_vi_dma() != 0)
		return -1;
	return capture_use_vi_frames();
}

bool vi_dma_running(void)
{
	return g_runtime.vi_dma_running;
}

void park_unbound_vpss_channels()
{
	for (int ch = 0; ch < MMF_VI_MAX_CHN; ++ch) {
		if (!capture_channel_open(ch) || !g_runtime.vi_chn_running[ch])
			continue;
		bool bound = false;
		for (int venc = 0; venc < MMF_VENC_MAX_CHN; ++venc) {
			const H26xEncoderState *info = &g_runtime.h26x_encoders[venc];
			if (info->initialized && info->bound_to_capture &&
			    info->capture_channel == ch) {
				bound = true;
				break;
			}
		}
		if (bound)
			continue;
		(void)pause_vpss_channel(ch);
	}
}

int reset_capture_channel(int ch, int width, int height, int format, int fps)
{
	const int out_ch = vpss_phy_channel(width);
	if (ch != out_ch)
		close_capture_channel(ch);
	close_capture_channel(out_ch);
	return open_capture_channel(out_ch, width, height, format, fps);
}

int acquire_capture_frame_timeout(int ch, void **data, int *len, int *width,
	int *height, int *format, int timeout_ms) {
	if (ch < 0 || ch >= MMF_VI_MAX_CHN) {
        printf("[%d] invalid ch %d\n", __LINE__, ch);
        return -1;
    }
	if (!g_runtime.vi_chn_is_inited[ch]) {
        // printf("vi ch %d not open\n", ch);
        return -1;
    }
    if (data == NULL || len == NULL || width == NULL || height == NULL || format == NULL) {
        printf("invalid param\n");
        return -1;
    }

	int ret = -1;
	if (resume_vi_dma() != 0)
		return -1;
	if (resume_vpss_channel(ch) != 0)
		return -1;
	VIDEO_FRAME_INFO_S *frame = &g_runtime.vi_frame[ch];
	if (CVI_VPSS_GetChnFrame(0, ch, frame, timeout_ms) == 0) {
        int image_size = frame->stVFrame.u32Length[0]
                        + frame->stVFrame.u32Length[1]
				        + frame->stVFrame.u32Length[2];
		CVI_VOID *vir_addr = map_capture_frame(ch, frame->stVFrame.u64PhyAddr[0], image_size);
		if (vir_addr == NULL) {
			CVI_VPSS_ReleaseChnFrame(0, ch, frame);
			return -1;
		}
        CVI_SYS_IonInvalidateCache(frame->stVFrame.u64PhyAddr[0], vir_addr, image_size);

		frame->stVFrame.pu8VirAddr[0] = (CVI_U8 *)vir_addr;
		// printf("width: %d, height: %d, total_buf_length: %d, phy:%#lx  vir:%p\n",
		// 	   frame->stVFrame.u32Width,
		// 	   frame->stVFrame.u32Height, image_size,
        //        frame->stVFrame.u64PhyAddr[0], vir_addr);

		*data = vir_addr;
        *len = image_size;
        *width = frame->stVFrame.u32Width;
        *height = frame->stVFrame.u32Height;
        *format = frame->stVFrame.enPixelFormat;
		return 0;
    }
	return ret;
}

int acquire_capture_frame(int ch, void **data, int *len, int *width, int *height, int *format) {
	return acquire_capture_frame_timeout(ch, data, len, width, height, format, 1000);
}

void release_capture_frame(int ch) {
	if (ch < 0 || ch >= MMF_VI_MAX_CHN || !g_runtime.vi_chn_is_inited[ch])
		return;
	VIDEO_FRAME_INFO_S *frame = &g_runtime.vi_frame[ch];
	if (CVI_VPSS_ReleaseChnFrame(0, ch, frame) != 0) {
		SAMPLE_PRT("CVI_VI_ReleaseChnFrame NG\n");
	}
	frame->stVFrame.pu8VirAddr[0] = NULL;
}

static void release_vpss_user_frames()
{
	for (int i = 0; i < 2; ++i) {
		if (g_runtime.vpss_user_frame[i] != nullptr) {
			free_frame(g_runtime.vpss_user_frame[i]);
			g_runtime.vpss_user_frame[i] = nullptr;
		}
	}
	if (g_runtime.vpss_user_pool_id >= 0)
		_destroy_vb_pool(static_cast<uint32_t>(g_runtime.vpss_user_pool_id));
	g_runtime.vpss_user_pool_id = -1;
	g_runtime.vpss_user_index = 0;
	g_runtime.vpss_user_nv21_source = nullptr;
	g_runtime.vpss_user_prepared_mask = 0;
}

int vpss_input_width()
{
	return static_cast<int>(g_runtime.vi_size.u32Width);
}

int vpss_input_height()
{
	return static_cast<int>(g_runtime.vi_size.u32Height);
}

int capture_use_user_frames()
{
	if (!g_runtime.vi_bound_to_vpss)
		return 0;
	const CVI_S32 ret = SAMPLE_COMM_VI_UnBind_VPSS(0, 0, 0);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi unbind vpss for placeholder failed. s32Ret: 0x%x\n", ret);
		return ret;
	}
	g_runtime.vi_bound_to_vpss = false;
	std::fprintf(stderr, "OneKVM: VPSS input switched to user frames\n");
	return 0;
}

int capture_use_vi_frames()
{
	if (g_runtime.vi_bound_to_vpss)
		return 0;
	const CVI_S32 ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, 0);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi bind vpss after placeholder failed. s32Ret: 0x%x\n", ret);
		return ret;
	}
	g_runtime.vi_bound_to_vpss = true;
	std::fprintf(stderr, "OneKVM: VPSS input switched to VI\n");
	return 0;
}

static void nv21_to_uyvy(const uint8_t *nv21, int width, int height,
			 uint8_t *uyvy, int stride)
{
	const uint8_t *luma = nv21;
	const uint8_t *chroma = nv21 + width * height;
	for (int y = 0; y < height; ++y) {
		uint8_t *dst = uyvy + static_cast<size_t>(y) * stride;
		const uint8_t *ys = luma + static_cast<size_t>(y) * width;
		const uint8_t *cs = chroma + static_cast<size_t>(y / 2) * width;
		for (int x = 0; x < width; x += 2) {
			dst[0] = cs[x + 1];
			dst[1] = ys[x];
			dst[2] = cs[x];
			dst[3] = ys[x + 1];
			dst += 4;
		}
	}
}

static int ensure_vpss_user_frames()
{
	const int width = vpss_input_width();
	const int height = vpss_input_height();
	if (width <= 0 || height <= 0)
		return -1;
	if (g_runtime.vpss_user_frame[0] != nullptr &&
	    static_cast<int>(g_runtime.vpss_user_frame[0]->stVFrame.u32Width) == width &&
	    static_cast<int>(g_runtime.vpss_user_frame[0]->stVFrame.u32Height) == height)
		return 0;

	release_vpss_user_frames();
	const uint32_t size = COMMON_GetPicBufferSize(
		width, height, PIXEL_FORMAT_UYVY, DATA_BITWIDTH_8,
		COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	static auto last_pool_fail = std::chrono::steady_clock::time_point{};
	static bool logged_pool_fail = false;
	const auto now = std::chrono::steady_clock::now();
	if (last_pool_fail.time_since_epoch().count() != 0 &&
	    now - last_pool_fail < std::chrono::seconds(2))
		return -1;
	const int pool = _create_vb_pool("vpss_user", size, 2);
	if (pool < 0) {
		last_pool_fail = now;
		if (!logged_pool_fail) {
			SAMPLE_PRT("vpss_user VB pool %u bytes failed; backing off\n", size);
			logged_pool_fail = true;
		}
		return -1;
	}
	logged_pool_fail = false;
	g_runtime.vpss_user_pool_id = pool;
	const SIZE_S st{static_cast<CVI_U32>(width), static_cast<CVI_U32>(height)};
	for (int i = 0; i < 2; ++i) {
		g_runtime.vpss_user_frame[i] = allocate_frame(pool, st, PIXEL_FORMAT_UYVY);
		if (g_runtime.vpss_user_frame[i] == nullptr) {
			release_vpss_user_frames();
			return -1;
		}
	}
	return 0;
}

int submit_vpss_nv21(const uint8_t *nv21, int width, int height)
{
	if (nv21 == nullptr || width <= 0 || height <= 0 || (width & 1) != 0)
		return -1;
	if (width != vpss_input_width() || height != vpss_input_height())
		return -1;
	if (ensure_vpss_user_frames() != 0)
		return -1;

	const int index = g_runtime.vpss_user_index & 1;
	VIDEO_FRAME_INFO_S *frame = g_runtime.vpss_user_frame[index];
	VIDEO_FRAME_S *vf = &frame->stVFrame;
	/* The no-signal NV21 asset is immutable until its source buffer or
	 * geometry changes. Preparing both DMA buffers once avoids converting and
	 * flushing the same multi-megabyte still image at the stream frame rate. */
	if (g_runtime.vpss_user_nv21_source != nv21) {
		g_runtime.vpss_user_nv21_source = nv21;
		g_runtime.vpss_user_prepared_mask = 0;
	}
	const uint8_t prepared_bit = static_cast<uint8_t>(1u << index);
	if ((g_runtime.vpss_user_prepared_mask & prepared_bit) == 0) {
		nv21_to_uyvy(nv21, width, height, vf->pu8VirAddr[0],
			     static_cast<int>(vf->u32Stride[0]));
		const CVI_U32 bytes = frame_buffer_size(vf);
		CVI_SYS_IonFlushCache(vf->u64PhyAddr[0], vf->pu8VirAddr[0], bytes);
		g_runtime.vpss_user_prepared_mask |= prepared_bit;
	}
	vf->u32TimeRef += 2;
	vf->u64PTS += 1;
	const CVI_S32 ret = CVI_VPSS_SendFrame(0, frame, 1000);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_SendFrame failed with %#x\n", ret);
		return ret;
	}
	g_runtime.vpss_user_index = index ^ 1;
	return 0;
}

} // namespace onekvm::mmf
