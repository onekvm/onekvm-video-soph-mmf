#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

/* /proc/cvitek/vpss GRP WORK STATUS data line. Sample:
     # 0    155942  0  0  Y  6249  12484  6007  8176
   CostTime is the VPSS job (driver + HW) in microseconds. */
inline bool parse_vpss_grp_cost_us(const char *line, int grp, uint32_t *cost_us)
{
	if (line == nullptr || cost_us == nullptr || grp < 0)
		return false;
	int id = -1;
	unsigned recv = 0;
	unsigned lost = 0;
	unsigned fail = 0;
	char started[4] = {};
	unsigned cost = 0;
	unsigned max_cost = 0;
	unsigned hw = 0;
	unsigned max_hw = 0;
	if (std::sscanf(line, " # %d %u %u %u %3s %u %u %u %u",
			&id, &recv, &lost, &fail, started, &cost, &max_cost, &hw,
			&max_hw) != 9)
		return false;
	if (id != grp)
		return false;
	if ((started[0] != 'Y' && started[0] != 'N') || started[1] != '\0')
		return false;
	*cost_us = cost;
	return true;
}

/* /proc/cvitek/venc PERFORMANCE line. Sample:
     ID: 1 No.SendFramePerSec: 59 No.EncFramePerSec: 59 HwEncTime: 9188 us ... */
inline bool parse_venc_perf_line(const char *line, int channel,
	uint32_t *enc_fps, uint32_t *hwenc_us)
{
	if (line == nullptr || channel < 0)
		return false;
	int id = -1;
	unsigned send = 0;
	unsigned enc = 0;
	unsigned hwenc = 0;
	if (std::sscanf(line,
			"ID: %d No.SendFramePerSec: %u No.EncFramePerSec: %u HwEncTime: %u us",
			&id, &send, &enc, &hwenc) != 4)
		return false;
	if (id != channel)
		return false;
	if (enc_fps != nullptr)
		*enc_fps = enc;
	if (hwenc_us != nullptr)
		*hwenc_us = hwenc;
	return true;
}

inline bool parse_venc_hwenc_us(const char *line, int channel, uint32_t *hwenc_us)
{
	return parse_venc_perf_line(line, channel, nullptr, hwenc_us);
}

inline uint32_t bound_frame_period_us(int input_fps)
{
	if (input_fps <= 0)
		return 0;
	return 1000000u / static_cast<uint32_t>(input_fps);
}

/* HDMI IN → encoder input: one progressive frame fill plus the VPSS job.
   VENC receives each VPSS output about CostTime after VI frame done (VI
   stamps frame done; RcvFrmPts lag ≈ CostTime), so the third VI block in
   the common pool does not add a queued frame. Do not count VENC waitq,
   HwEncTime or snapshot u32Depth. Bound path has no source PTS. */
inline uint32_t bound_capture_us(uint32_t vpss_cost_us, int input_fps)
{
	return vpss_cost_us + bound_frame_period_us(input_fps);
}
