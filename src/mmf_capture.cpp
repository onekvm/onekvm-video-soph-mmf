#include "mmf_internal.hpp"

namespace onekvm::mmf {
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
	VPSS_CROP_INFO_S   stChnCropInfo;
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
		stChnCropInfo.stCropRect.s32X = (stGrpAttr.u32MaxW - crop_w) / 2;
		stChnCropInfo.stCropRect.s32Y = (stGrpAttr.u32MaxH - crop_h) / 2;
		stChnCropInfo.stCropRect.u32Width = crop_w;
		stChnCropInfo.stCropRect.u32Height = crop_h;
	}

	if (crop_w != 0 && crop_h != 0) {
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
	stVpssGrpAttr.u8VpssDev                      = 0;

	s32Ret = CVI_VPSS_CreateGrp(VpssGrp, &stVpssGrpAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VPSS_CreateGrp(grp:%d) retry(%#x)!\n", VpssGrp, s32Ret);
		CVI_VPSS_DestroyGrp(VpssGrp);

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
	int width_out = ALIGN(width, DEFAULT_ALIGN);
	int height_out = height;
	PIXEL_FORMAT_E format_out = (PIXEL_FORMAT_E)format;
	const bool mirror = g_capture_options.mirror[ch];
	const bool flip = g_capture_options.flip[ch];
	s32Ret = disable_vpss_channel(0, ch);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("disable_vpss_channel failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	s32Ret = configure_vpss_channel(0, ch, width_out, height_out, format_out, fps, depth, mirror, flip, 2);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("configure_vpss_channel failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	char name[20];
	snprintf(name, 20, "vi_vpss%.1d", ch);
	pool_size_out = COMMON_GetPicBufferSize(width_out, height_out, format_out, DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	/* Three independent owners can overlap at 1080p60:
	 *
	 *   1 queued VPSS frame (`depth`)
	 *   1 frame borrowed by Core and the synchronous zero-copy VENC call
	 *   1 block being filled by the VPSS worker
	 *
	 * Allocating only depth + producer (two blocks) starves VPSS whenever VENC
	 * crosses a 16.7 ms frame boundary.  The worker then retries in a tight
	 * loop, floods dmesg with "Can't acquire VB BLK", and collapses the stream
	 * to roughly 20-35 FPS.  The third block absorbs that bounded overlap while
	 * u32Depth remains one, so it adds capacity without adding queue latency. */
	const int pool_blocks = depth + MMF_VPSS_BORROWED_BLOCKS
		+ MMF_VPSS_PRODUCER_BLOCKS;
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
	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(0, ch, 0);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi bind vpss failed. s32Ret: 0x%x !\n", s32Ret);
		goto _need_detach_vb_pool;
	}

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
	s32Ret = SAMPLE_COMM_VI_UnBind_VPSS(0, ch, 0);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi unbind vpss failed. s32Ret: 0x%x !\n", s32Ret);
		// return -1; // continue to deinit vpss
	}

	if (0 != disable_vpss_channel(0, ch)) {
		SAMPLE_PRT("disable_vpss_channel failed. s32Ret: 0x%x !\n", s32Ret);
	}

	clear_capture_mappings(ch);
	CVI_VPSS_DetachVbPool(0, ch);
	_destroy_vb_pool(g_runtime.vi_chn_pool_id[ch]);

	g_runtime.vi_chn_pool_id[ch] = -1;
	g_runtime.vi_chn_is_inited[ch] = false;
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

int reset_capture_channel(int ch, int width, int height, int format, int fps)
{
	close_capture_channel(ch);
	return open_capture_channel(ch, width, height, format, fps);
}

int acquire_capture_frame(int ch, void **data, int *len, int *width, int *height, int *format) {
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
	VIDEO_FRAME_INFO_S *frame = &g_runtime.vi_frame[ch];
	if (CVI_VPSS_GetChnFrame(0, ch, frame, 1000) == 0) {
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

void release_capture_frame(int ch) {
	if (ch < 0 || ch >= MMF_VI_MAX_CHN || !g_runtime.vi_chn_is_inited[ch])
		return;
	VIDEO_FRAME_INFO_S *frame = &g_runtime.vi_frame[ch];
	if (CVI_VPSS_ReleaseChnFrame(0, ch, frame) != 0) {
		SAMPLE_PRT("CVI_VI_ReleaseChnFrame NG\n");
	}
	frame->stVFrame.pu8VirAddr[0] = NULL;
}

} // namespace onekvm::mmf
