/*
 * NanoKVM LT6911 adapter for Sophgo's sample VI/ISP helpers.
 *
 * Recent Sophgo releases maintain the sensor list independently from cvi_mpi.
 * The generic sample_common_sensor.c still contains cases for sensors removed
 * from that list, so it is not a stable product API.  Keep the small set of
 * callbacks needed by this device local to the backend instead.
 */

#include <string.h>

#include "sample_comm.h"

extern ISP_SNS_OBJ_S stSnsLT6911_Obj;

static CVI_U32 g_active_width = 1920;
static CVI_U32 g_active_height = 1080;
static CVI_S32 (*g_vendor_get_rx_attr)(VI_PIPE, SNS_COMBO_DEV_ATTR_S *);
static CVI_S32 (*g_vendor_get_sns_regs_info[VI_MAX_PIPE_NUM])(
	VI_PIPE, ISP_SNS_SYNC_INFO_S *);

/*
 * The vendor LT6911 sensor module keeps its only mode at 1920x1080.  Updating
 * the MIPI receiver and VI attributes is therefore not enough: ISP obtains
 * the sensor/WDR size from pfn_cmos_get_sns_reg_info and forwards that fixed
 * value to VI_IOCTL_SET_SNR_INFO.  The kernel then programs the CSI bridge
 * from that sensor info, which leaves csibdg_width/height at 1920x1080.
 *
 * CMake links the vendor sensor source into this DSO, so --wrap lets us amend
 * just the LT6911 callback at its registration boundary without forking the
 * complete vendor sensor driver.
 */
static CVI_S32 onekvm_lt6911_get_sns_regs_info(
	VI_PIPE pipe, ISP_SNS_SYNC_INFO_S *sync_info)
{
	ISP_WDR_SIZE_S *size;
	CVI_S32 result;

	if (pipe < 0 || pipe >= VI_MAX_PIPE_NUM || sync_info == CVI_NULL ||
	    g_vendor_get_sns_regs_info[pipe] == CVI_NULL)
		return CVI_FAILURE;

	result = g_vendor_get_sns_regs_info[pipe](pipe, sync_info);
	if (result != CVI_SUCCESS)
		return result;

	sync_info->ispCfg.frm_num = 1;
	sync_info->ispCfg.need_update = CVI_TRUE;
	size = &sync_info->ispCfg.img_size[0];
	size->stWndRect.s32X = 0;
	size->stWndRect.s32Y = 0;
	size->stWndRect.u32Width = g_active_width;
	size->stWndRect.u32Height = g_active_height;
	size->stSnsSize.u32Width = g_active_width;
	size->stSnsSize.u32Height = g_active_height;
	size->stMaxSize.u32Width = g_active_width;
	size->stMaxSize.u32Height = g_active_height;

	return CVI_SUCCESS;
}

CVI_S32 __real_CVI_ISP_SensorRegCallBack(
	VI_PIPE pipe, ISP_SNS_ATTR_INFO_S *sensor_info,
	ISP_SENSOR_REGISTER_S *registration);

CVI_S32 __wrap_CVI_ISP_SensorRegCallBack(
	VI_PIPE pipe, ISP_SNS_ATTR_INFO_S *sensor_info,
	ISP_SENSOR_REGISTER_S *registration)
{
	ISP_SENSOR_REGISTER_S dynamic_registration;

	if (pipe < 0 || pipe >= VI_MAX_PIPE_NUM || sensor_info == CVI_NULL ||
	    registration == CVI_NULL || sensor_info->eSensorId != 6911)
		return __real_CVI_ISP_SensorRegCallBack(
			pipe, sensor_info, registration);

	dynamic_registration = *registration;
	g_vendor_get_sns_regs_info[pipe] =
		dynamic_registration.stSnsExp.pfn_cmos_get_sns_reg_info;
	if (g_vendor_get_sns_regs_info[pipe] != CVI_NULL)
		dynamic_registration.stSnsExp.pfn_cmos_get_sns_reg_info =
			onekvm_lt6911_get_sns_regs_info;

	return __real_CVI_ISP_SensorRegCallBack(
		pipe, sensor_info, &dynamic_registration);
}

static CVI_S32 onekvm_lt6911_get_rx_attr(VI_PIPE pipe,
					 SNS_COMBO_DEV_ATTR_S *attr)
{
	CVI_S32 result;

	if (attr == CVI_NULL || g_vendor_get_rx_attr == CVI_NULL)
		return CVI_FAILURE;
	result = g_vendor_get_rx_attr(pipe, attr);
	if (result != CVI_SUCCESS)
		return result;
	attr->img_size.width = g_active_width;
	attr->img_size.height = g_active_height;
	return CVI_SUCCESS;
}

CVI_S32 onekvm_lt6911_set_active_size(CVI_U32 width, CVI_U32 height)
{
	if (width == 0 || height == 0 || width > 4096 || height > 2160)
		return CVI_FAILURE;
	if ((width & 1u) != 0 || (height & 1u) != 0)
		return CVI_FAILURE;

	g_active_width = width;
	g_active_height = height;
	if (g_vendor_get_rx_attr == CVI_NULL) {
		g_vendor_get_rx_attr = stSnsLT6911_Obj.pfnGetRxAttr;
		stSnsLT6911_Obj.pfnGetRxAttr = onekvm_lt6911_get_rx_attr;
	}
	return CVI_SUCCESS;
}

void onekvm_lt6911_get_active_size(CVI_U32 *width, CVI_U32 *height)
{
	if (width != CVI_NULL)
		*width = g_active_width;
	if (height != CVI_NULL)
		*height = g_active_height;
}

static const VI_DEV_ATTR_S kLt6911DevAttr = {
	/* LT6911 sends packed YUV422 over CSI-2, not Bayer RAW.  The interface
	 * mode is consumed independently from enInputDataType by the new VI
	 * driver, so VI_MODE_MIPI leaves the preraw state machine waiting for RAW
	 * events and makes cvitask_vpss spin after the first channel is enabled. */
	VI_MODE_MIPI_YUV422,
	VI_WORK_MODE_1Multiplex,
	VI_SCAN_PROGRESSIVE,
	{-1, -1, -1, -1},
	VI_DATA_SEQ_UYVY,
	{
		VI_VSYNC_PULSE, VI_VSYNC_NEG_LOW,
		VI_HSYNC_VALID_SINGNAL, VI_HSYNC_NEG_HIGH,
		VI_VSYNC_VALID_SIGNAL, VI_VSYNC_VALID_NEG_HIGH,
		{0, 1920, 0, 0, 1080, 0, 0, 0, 0}
	},
	VI_DATA_TYPE_YUV,
	{1920, 1080},
	{WDR_MODE_NONE, 1080, 0},
	.enBayerFormat = BAYER_FORMAT_BG,
};

static const ISP_PUB_ATTR_S kLt6911IspAttr = {
	{0, 0, 1920, 1080},
	{1920, 1080},
	60,
	BAYER_BGGR,
	WDR_MODE_NONE,
	0,
};

CVI_CHAR *SAMPLE_COMM_SNS_GetSnsrTypeName(void)
{
	static CVI_CHAR name[] = "LONTIUM_LT6911_2M_60FPS_8BIT";
	return name;
}

CVI_S32 SAMPLE_COMM_SNS_GetSize(SAMPLE_SNS_TYPE_E enMode, PIC_SIZE_E *penSize)
{
	if (enMode != LONTIUM_LT6911_2M_60FPS_8BIT || penSize == CVI_NULL)
		return CVI_FAILURE;

	*penSize = PIC_CUSTOMIZE;
	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_COMM_SNS_GetPicSize(PIC_SIZE_E enPicSize, SIZE_S *pstSize)
{
	if (enPicSize != PIC_CUSTOMIZE || pstSize == CVI_NULL)
		return CVI_FAILURE;

	pstSize->u32Width = g_active_width;
	pstSize->u32Height = g_active_height;
	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_COMM_SNS_GetDevAttr(SAMPLE_SNS_TYPE_E enSnsType,
				   VI_DEV_ATTR_S *pstViDevAttr)
{
	if (enSnsType != LONTIUM_LT6911_2M_60FPS_8BIT ||
	    pstViDevAttr == CVI_NULL)
		return CVI_FAILURE;

	memcpy(pstViDevAttr, &kLt6911DevAttr, sizeof(*pstViDevAttr));
	pstViDevAttr->stSynCfg.stTimingBlank.u32HsyncAct = g_active_width;
	pstViDevAttr->stSynCfg.stTimingBlank.u32VsyncVact = g_active_height;
	pstViDevAttr->stSize.u32Width = g_active_width;
	pstViDevAttr->stSize.u32Height = g_active_height;
	pstViDevAttr->stWDRAttr.u32CacheLine = g_active_height;
	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_COMM_SNS_GetYuvBypassSts(SAMPLE_SNS_TYPE_E enSnsType)
{
	/* LT6911 delivers YUV422.  Marking it as a Bayer/ISP path makes the new
	 * OSDRV run DCI against an unallocated ISP statistics buffer. */
	return enSnsType == LONTIUM_LT6911_2M_60FPS_8BIT ? 1 : CVI_FAILURE;
}

CVI_S32 SAMPLE_COMM_SNS_GetIspAttrBySns(SAMPLE_SNS_TYPE_E enSnsType,
					ISP_PUB_ATTR_S *pstPubAttr)
{
	if (enSnsType != LONTIUM_LT6911_2M_60FPS_8BIT ||
	    pstPubAttr == CVI_NULL)
		return CVI_FAILURE;

	memcpy(pstPubAttr, &kLt6911IspAttr, sizeof(*pstPubAttr));
	pstPubAttr->stWndRect.u32Width = g_active_width;
	pstPubAttr->stWndRect.u32Height = g_active_height;
	pstPubAttr->stSnsSize.u32Width = g_active_width;
	pstPubAttr->stSnsSize.u32Height = g_active_height;
	return CVI_SUCCESS;
}

CVI_VOID *SAMPLE_COMM_SNS_GetSnsObj(SAMPLE_SNS_TYPE_E enSnsType)
{
	if (enSnsType != LONTIUM_LT6911_2M_60FPS_8BIT)
		return CVI_NULL;

	return &stSnsLT6911_Obj;
}

CVI_S32 SAMPLE_COMM_SNS_SetIniPath(const CVI_CHAR *iniPath)
{
	(void)iniPath;
	return CVI_FAILURE;
}

CVI_S32 SAMPLE_COMM_SNS_ParseIni(SAMPLE_INI_CFG_S *pstIniCfg)
{
	static const CVI_S16 lanes[5] = {2, 4, 3, 1, 0};

	if (pstIniCfg == CVI_NULL)
		return CVI_FAILURE;

	memset(pstIniCfg, 0, sizeof(*pstIniCfg));
	pstIniCfg->enSource = VI_PIPE_FRAME_SOURCE_DEV;
	pstIniCfg->devNum = 1;
	pstIniCfg->enSnsType[0] = LONTIUM_LT6911_2M_60FPS_8BIT;
	pstIniCfg->enWDRMode[0] = WDR_MODE_NONE;
	pstIniCfg->s32BusId[0] = 4;
	pstIniCfg->s32SnsI2cAddr[0] = 0x2b;
	pstIniCfg->MipiDev[0] = 0;
	memcpy(pstIniCfg->as16LaneId[0], lanes, sizeof(lanes));
	return CVI_SUCCESS;
}

/* LT6911 uses the sensor defaults and does not need an ISP parameter bin. */
CVI_S32 SAMPLE_COMM_BIN_ReadParaFrombin(void)
{
	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_COMM_BIN_ReadBlockParaFrombin(enum CVI_BIN_SECTION_ID id)
{
	(void)id;
	return CVI_SUCCESS;
}
