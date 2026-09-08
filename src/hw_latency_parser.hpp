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
inline bool parse_venc_hwenc_us(const char *line, int channel, uint32_t *hwenc_us)
{
	if (line == nullptr || hwenc_us == nullptr || channel < 0)
		return false;
	int id = -1;
	unsigned hwenc = 0;
	if (std::sscanf(line,
			"ID: %d No.SendFramePerSec: %*u No.EncFramePerSec: %*u HwEncTime: %u us",
			&id, &hwenc) != 2)
		return false;
	if (id != channel)
		return false;
	*hwenc_us = hwenc;
	return true;
}

inline uint32_t bound_frame_period_us(int input_fps)
{
	if (input_fps <= 0)
		return 0;
	return 1000000u / static_cast<uint32_t>(input_fps);
}

/* Bound HDMI/VI/VPSS/VENC jobs, not snapshot u32Depth (that is GetChnFrame
   doneq and runs beside bind). Progressive fill is one frame. VPSS workq is
   CostTime, not another full period. A third VI VB can occupy VPSS waitq=1.
   VENC CHN_TYPE_IN waitq=1 sits after VPSS before HwEncTime. */
inline uint32_t bound_capture_queue_frames(int vi_blocks, int vpss_waitq,
					  int venc_waitq)
{
	uint32_t frames = 1;
	if (vi_blocks > 2 && vpss_waitq > 0)
		frames += 1;
	if (venc_waitq > 0)
		frames += static_cast<uint32_t>(venc_waitq);
	return frames;
}

/* Bound path has no source PTS (pack u64PTS is encode-complete). Do not read
   /proc/cvitek/vi. vi_blocks is the VI common pool. */
inline uint32_t bound_capture_us(uint32_t vpss_cost_us, int input_fps,
				 int vi_blocks, int vpss_waitq, int venc_waitq)
{
	uint32_t capture_us = vpss_cost_us;
	const uint32_t frame_us = bound_frame_period_us(input_fps);
	if (frame_us == 0)
		return capture_us;
	capture_us += bound_capture_queue_frames(vi_blocks, vpss_waitq, venc_waitq) *
		      frame_us;
	return capture_us;
}
