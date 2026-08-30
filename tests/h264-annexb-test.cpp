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

	const uint8_t h264_pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
	AnnexBParameterSets h264_parameters;
	h264_parameters.update(h264_pps, sizeof(h264_pps), false);
	h264_parameters.update(sps_idr, sizeof(sps_idr), false);
	if (!h264_parameters.complete(false)) return 14;
	const std::vector<uint8_t> h264_augmented =
		h264_parameters.augment_keyframe(sps_idr, sizeof(sps_idr), false);
	if (h264_augmented.size() <= sizeof(sps_idr)) return 15;
	if ((annexb_parameter_set_mask(h264_augmented.data(), h264_augmented.size(), false) &
	     (kAnnexBParamSPS | kAnnexBParamPPS)) !=
	    (kAnnexBParamSPS | kAnnexBParamPPS)) return 16;
	if (!annexb_has_idr(h264_augmented.data(), h264_augmented.size())) return 17;

	const uint8_t h264_complete[] = {
		0, 0, 0, 1, 0x67, 1,
		0, 0, 0, 1, 0x68, 2,
		0, 0, 0, 1, 0x65, 3,
	};
	const std::vector<uint8_t> unchanged = h264_parameters.augment_keyframe(
		h264_complete, sizeof(h264_complete), false);
	if (unchanged.size() != sizeof(h264_complete)) return 18;
	for (std::size_t index = 0; index < unchanged.size(); ++index)
		if (unchanged[index] != h264_complete[index]) return 19;

	const uint8_t h265_vps[] = {0, 0, 0, 1, 0x40, 1, 7};
	const uint8_t h265_sps[] = {0, 0, 1, 0x42, 1, 8};
	const uint8_t h265_pps[] = {0, 0, 0, 1, 0x44, 1, 9};
	AnnexBParameterSets h265_parameters;
	h265_parameters.update(h265_vps, sizeof(h265_vps), true);
	h265_parameters.update(h265_sps, sizeof(h265_sps), true);
	h265_parameters.update(h265_pps, sizeof(h265_pps), true);
	if (!h265_parameters.complete(true)) return 20;
	const std::vector<uint8_t> h265_augmented = h265_parameters.augment_keyframe(
		h265_idr, sizeof(h265_idr), true);
	if ((annexb_parameter_set_mask(h265_augmented.data(), h265_augmented.size(), true) &
	     (kAnnexBParamVPS | kAnnexBParamSPS | kAnnexBParamPPS)) !=
	    (kAnnexBParamVPS | kAnnexBParamSPS | kAnnexBParamPPS)) return 21;
	if (!annexb_has_h265_irap(h265_augmented.data(), h265_augmented.size())) return 22;

	const uint8_t sps_1080[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x28,
		0xdc, 0x07, 0x80, 0x22, 0x7e, 0x58, 0x40, 0x00,
		0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c, 0x83,
		0xc6, 0x0c, 0xe0,
	};
	int sps_w = 0;
	int sps_h = 0;
	if (!annexb_h264_sps_size(sps_1080, sizeof(sps_1080), &sps_w, &sps_h))
		return 23;
	if (sps_w != 1920 || sps_h != 1080) return 24;
	if (!annexb_h264_geometry_matches(sps_w, sps_h, 1920, 1080)) return 25;
	if (annexb_h264_geometry_matches(192, 64, 1920, 1080)) return 26;

	const uint8_t sps_192x64[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
		0xf4, 0x18, 0x4c,
	};
	if (!annexb_h264_sps_size(sps_192x64, sizeof(sps_192x64), &sps_w, &sps_h))
		return 27;
	if (sps_w != 192 || sps_h != 64) return 28;
	if (annexb_h264_sps_size(p_only, sizeof(p_only), &sps_w, &sps_h))
		return 29;
	return 0;
}
