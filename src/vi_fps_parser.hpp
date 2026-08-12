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
