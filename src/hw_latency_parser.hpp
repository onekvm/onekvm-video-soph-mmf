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

/* Online VI delivers a progressive frame over 1/input_fps. VPSS CostTime is
   the scale job after that frame is in DRAM. Pack u64PTS is encode-complete
   and cannot be used as a capture start. */
inline uint32_t bound_capture_us(uint32_t vpss_cost_us, int input_fps)
{
	uint32_t capture_us = vpss_cost_us;
	if (input_fps > 0)
		capture_us += 1000000u / static_cast<uint32_t>(input_fps);
	return capture_us;
}
