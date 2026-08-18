#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>

inline bool parse_vi_fps_line(const char *line, double *fps)
{
	if (line == nullptr || fps == nullptr)
		return false;
	char name[32] = {};
	char separator = '\0';
	double value = 0;
	/* Current CVITEK kernels render `VIFPS : 0`; older builds omitted
	   the colon. Accept both without confusing VIDevFPS with VIFPS. */
	const bool labeled = std::sscanf(line, "%31s %c %lf", name, &separator, &value) == 3 &&
		separator == ':' && std::strcmp(name, "VIFPS") == 0;
	const bool plain = !labeled &&
		std::sscanf(line, "%31s %lf", name, &value) == 2 &&
		std::strcmp(name, "VIFPS") == 0;
	if ((!labeled && !plain) || !std::isfinite(value) || value < 0)
		return false;
	*fps = value;
	return true;
}

inline bool parse_vi_chn_status_header(const char *line)
{
	return line != nullptr && std::strstr(line, "VI CHN STATUS") != nullptr;
}

/* /proc/cvitek/vi CHN STATUS is safe to read. /proc/cvitek/vi_dbg blocks
   ~1s and can stall the CSI frontend. Sample:
     DevID ChnID Enable FrameRate IntCnt RecvPic LostFrame ...
       0     0     Y        60     7339    7339      0     */
inline bool parse_vi_chn_status_fps(const char *line, double *fps)
{
	if (line == nullptr || fps == nullptr)
		return false;
	int dev = -1;
	int chn = -1;
	int frame_rate = -1;
	int int_cnt = 0;
	int recv_pic = 0;
	int lost = 0;
	char enable[4] = {};
	if (std::sscanf(line, " %d %d %3s %d %d %d %d",
			&dev, &chn, enable, &frame_rate, &int_cnt, &recv_pic, &lost) != 7)
		return false;
	if (dev != 0 || chn != 0)
		return false;
	if ((enable[0] != 'Y' && enable[0] != 'N') || enable[1] != '\0')
		return false;
	if (frame_rate < 0)
		return false;
	*fps = enable[0] == 'Y' ? static_cast<double>(frame_rate) : 0;
	return true;
}
