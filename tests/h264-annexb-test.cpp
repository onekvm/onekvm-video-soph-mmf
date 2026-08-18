#include "../src/h264_annexb.hpp"

int main()
{
	const uint8_t idr_long[] = {0, 0, 0, 1, 0x65, 1, 2};
	const uint8_t idr_short[] = {0, 0, 1, 0x65, 9};
	const uint8_t sps_idr[] = {0, 0, 0, 1, 0x67, 1, 0, 0, 0, 1, 0x65, 2};
	const uint8_t p_only[] = {0, 0, 0, 1, 0x41, 1, 2, 3};
	const uint8_t empty[] = {0, 0, 0};
	if (!annexb_has_idr(idr_long, sizeof(idr_long))) return 1;
	if (!annexb_has_idr(idr_short, sizeof(idr_short))) return 2;
	if (!annexb_has_idr(sps_idr, sizeof(sps_idr))) return 3;
	if (annexb_has_idr(p_only, sizeof(p_only))) return 4;
	if (annexb_has_idr(empty, sizeof(empty))) return 5;
	if (annexb_has_idr(nullptr, 8)) return 6;
	return 0;
}
