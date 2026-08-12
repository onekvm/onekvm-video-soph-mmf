/* Device-specific subset of Sophgo's sample platform helper. */

#include <string.h>

#include "sample_comm.h"

extern void onekvm_lt6911_get_active_size(CVI_U32 *width, CVI_U32 *height);

static void cleanup_vi(SAMPLE_VI_CONFIG_S *pstViConfig)
{
	SAMPLE_COMM_VI_DestroyIsp(pstViConfig);
	SAMPLE_COMM_VI_DestroyVi(pstViConfig);
	SAMPLE_COMM_SYS_Exit();
}

CVI_S32 SAMPLE_PLAT_SYS_INIT(SIZE_S stSize)
{
	VB_CONFIG_S vbConfig;
	ION_MEM_STATE_S memoryState;
	CVI_U32 normalSize;
	CVI_U32 rotatedSize;

	memset(&vbConfig, 0, sizeof(vbConfig));
	memset(&memoryState, 0, sizeof(memoryState));
	vbConfig.u32MaxPoolCnt = 1;
	normalSize = COMMON_GetPicBufferSize(
		stSize.u32Width, stSize.u32Height, SAMPLE_PIXEL_FORMAT,
		DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	rotatedSize = COMMON_GetPicBufferSize(
		stSize.u32Height, stSize.u32Width, SAMPLE_PIXEL_FORMAT,
		DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	vbConfig.astCommPool[0].u32BlkSize =
		normalSize > rotatedSize ? normalSize : rotatedSize;

	if (CVI_SYS_IonGetMemoryState(&memoryState) != CVI_SUCCESS)
		return CVI_FAILURE;
	vbConfig.astCommPool[0].u32BlkCnt =
		memoryState.total_size > 0x4000000 ? 8 : 3;
	vbConfig.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;

	return SAMPLE_COMM_SYS_Init(&vbConfig);
}

CVI_S32 SAMPLE_PLAT_VI_INIT(SAMPLE_VI_CONFIG_S *pstViConfig)
{
	PIC_SIZE_E enPicSize;
	SIZE_S stSize;
	VI_PIPE_ATTR_S stPipeAttr;
	CVI_S32 s32Ret;
	CVI_S32 i;
	CVI_S32 j;

	if (pstViConfig == CVI_NULL)
		return CVI_FAILURE;

	s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(
		pstViConfig->astViInfo[0].stSnsInfo.enSnsType, &enPicSize);
	if (s32Ret != CVI_SUCCESS)
		goto error;

	s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (s32Ret != CVI_SUCCESS)
		goto error;
	/* LT6911 uses PIC_1080P as its maximum mode enum.  Configure the VI pipe
	 * from the bridge's current HDMI active size instead of that enum. */
	onekvm_lt6911_get_active_size(&stSize.u32Width, &stSize.u32Height);

#if USE_USER_SEN_DRIVER
	s32Ret = SAMPLE_COMM_VI_StartSensor(pstViConfig);
	if (s32Ret != CVI_SUCCESS)
		goto error;
#endif

	for (i = 0; i < pstViConfig->s32WorkingViNum; ++i) {
		CVI_S32 dev = pstViConfig->as32WorkingViId[i];
		s32Ret = SAMPLE_COMM_VI_StartDev(&pstViConfig->astViInfo[dev]);
		if (s32Ret != CVI_SUCCESS)
			goto error;
	}

#if USE_USER_SEN_DRIVER
	s32Ret = SAMPLE_COMM_VI_StartMIPI(pstViConfig);
	if (s32Ret != CVI_SUCCESS)
		goto error;

	/* Some middleware releases replace the patched LT6911 sensor object while
	 * registering its ISP callbacks, which restores the vendor's fixed 1080p
	 * MIPI word count.  Program the receiver geometry explicitly at the end of
	 * StartMIPI while it is still being configured. */
	{
		SAMPLE_VI_INFO_S *info = &pstViConfig->astViInfo[0];
		VI_PIPE pipe = info->stPipeInfo.aPipe[0];
		CVI_U32 sensor_id = info->stSnsInfo.s32SnsId;
		ISP_SNS_OBJ_S *sensor =
			(ISP_SNS_OBJ_S *)SAMPLE_COMM_ISP_GetSnsObj(sensor_id);
		SNS_COMBO_DEV_ATTR_S rx_attr;

		if (sensor == CVI_NULL || sensor->pfnGetRxAttr == CVI_NULL) {
			s32Ret = CVI_FAILURE;
			goto error;
		}
		s32Ret = sensor->pfnGetRxAttr(pipe, &rx_attr);
		if (s32Ret != CVI_SUCCESS)
			goto error;
		rx_attr.img_size.width = stSize.u32Width;
		rx_attr.img_size.height = stSize.u32Height;
		s32Ret = CVI_MIPI_SetMipiAttr(pipe, &rx_attr);
		if (s32Ret != CVI_SUCCESS)
			goto error;
	}
	s32Ret = SAMPLE_COMM_VI_SensorProbe(pstViConfig);
	if (s32Ret != CVI_SUCCESS)
		goto error;
#endif

	memset(&stPipeAttr, 0, sizeof(stPipeAttr));
	stPipeAttr.bYuvSkip = CVI_FALSE;
	stPipeAttr.u32MaxW = stSize.u32Width;
	stPipeAttr.u32MaxH = stSize.u32Height;
	stPipeAttr.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP;
	stPipeAttr.enBitWidth = DATA_BITWIDTH_12;
	stPipeAttr.stFrameRate.s32SrcFrameRate = -1;
	stPipeAttr.stFrameRate.s32DstFrameRate = -1;
	stPipeAttr.bNrEn = CVI_TRUE;
	stPipeAttr.enCompressMode =
		pstViConfig->astViInfo[0].stChnInfo.enCompressMode;

	for (i = 0; i < pstViConfig->s32WorkingViNum; ++i) {
		CVI_S32 dev = pstViConfig->as32WorkingViId[i];
		SAMPLE_VI_INFO_S *info = &pstViConfig->astViInfo[dev];
		stPipeAttr.bYuvBypassPath = SAMPLE_COMM_VI_GetYuvBypassSts(
			info->stSnsInfo.enSnsType);

		for (j = 0; j < WDR_MAX_PIPE_NUM; ++j) {
			VI_PIPE pipe = info->stPipeInfo.aPipe[j];
			if (pipe < 0 || pipe >= VI_MAX_PIPE_NUM)
				continue;

			s32Ret = CVI_VI_CreatePipe(pipe, &stPipeAttr);
			if (s32Ret != CVI_SUCCESS)
				goto error;
			s32Ret = CVI_VI_StartPipe(pipe);
			if (s32Ret != CVI_SUCCESS)
				goto error;
		}
	}

	s32Ret = SAMPLE_COMM_VI_CreateIsp(pstViConfig);
	if (s32Ret != CVI_SUCCESS)
		goto error;

	s32Ret = SAMPLE_COMM_VI_StartViChn(pstViConfig);
	if (s32Ret != CVI_SUCCESS)
		goto error;

	return CVI_SUCCESS;

error:
	cleanup_vi(pstViConfig);
	return s32Ret;
}
