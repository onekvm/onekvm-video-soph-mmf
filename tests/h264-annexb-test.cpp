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

	const uint8_t h265_idr[] = {0, 0, 0, 1, 0x26, 1};
	const uint8_t h265_idr_short[] = {0, 0, 1, 0x28, 1};
	const uint8_t h265_vps_idr[] = {0, 0, 0, 1, 0x40, 1, 0, 0, 0, 1, 0x26, 1};
	const uint8_t h265_p[] = {0, 0, 0, 1, 0x02, 1};
	if (annexb_has_idr(h265_idr, sizeof(h265_idr))) return 7;
	if (!annexb_has_h265_irap(h265_idr, sizeof(h265_idr))) return 8;
	if (!annexb_has_h265_irap(h265_idr_short, sizeof(h265_idr_short))) return 9;
	if (!annexb_has_h265_irap(h265_vps_idr, sizeof(h265_vps_idr))) return 10;
	if (annexb_has_h265_irap(h265_p, sizeof(h265_p))) return 11;
	if (annexb_has_h265_irap(empty, sizeof(empty))) return 12;
	if (annexb_has_h265_irap(nullptr, 8)) return 13;
	return 0;
}
