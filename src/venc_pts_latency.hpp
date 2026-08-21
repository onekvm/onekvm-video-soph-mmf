#pragma once

#include <cstdint>

// pack_pts_us and now_us are CVI/VI CLOCK_MONOTONIC microseconds (ktime_get /
// CVI_SYS_GetCurPTS). encode_ns is subtracted so capture and encode do not
// overlap on the overlay. Zero means the sample is unusable.
inline uint64_t venc_capture_ns(uint64_t pack_pts_us, uint64_t now_us, uint64_t encode_ns)
{
	if (pack_pts_us == 0 || now_us < pack_pts_us)
		return 0;
	const uint64_t total_us = now_us - pack_pts_us;
	if (total_us >= 1000000ull)
		return 0;
	uint64_t capture_ns = total_us * 1000ull;
	if (encode_ns > 0 && capture_ns > encode_ns)
		capture_ns -= encode_ns;
	return capture_ns;
}
