#include "../src/hw_latency_parser.hpp"

int main()
{
	uint32_t cost = 0;
	if (!parse_vpss_grp_cost_us(
		    "       # 0    155942         0                   0         Y                6249               12484                6007                8176\n",
		    0, &cost) ||
		cost != 6249)
		return 1;
	if (parse_vpss_grp_cost_us(
		    "       # 0    155942         0                   0         Y                6249               12484                6007                8176\n",
		    1, &cost))
		return 2;
	if (parse_vpss_grp_cost_us(
		    "     GrpID   RecvCnt   LostCnt        StartFailCnt    bStart        CostTime(us)     MaxCostTime(us)      HwCostTime(us)   HwMaxCostTime(us)\n",
		    0, &cost))
		return 3;

	uint32_t hwenc = 0;
	if (!parse_venc_hwenc_us(
		    "ID: 1 No.SendFramePerSec: 59 No.EncFramePerSec: 59 HwEncTime: 9188 us MaxHwEncTime: 9835 us EncodedFrame: 112014\n",
		    1, &hwenc) ||
		hwenc != 9188)
		return 4;
	if (parse_venc_hwenc_us(
		    "ID: 1 No.SendFramePerSec: 59 No.EncFramePerSec: 59 HwEncTime: 9188 us MaxHwEncTime: 9835 us EncodedFrame: 112014\n",
		    0, &hwenc))
		return 5;
	if (parse_venc_hwenc_us("ID: 1 RcvFirstFrmPts: 0 RcvFrmPts: 6615308629\n",
			       1, &hwenc))
		return 6;

	if (bound_capture_us(6249, 60) != 6249 + 1000000u / 60)
		return 7;
	if (bound_capture_us(6249, 0) != 6249)
		return 8;
	if (bound_capture_us(0, 60) != 1000000u / 60)
		return 9;
	return 0;
}
