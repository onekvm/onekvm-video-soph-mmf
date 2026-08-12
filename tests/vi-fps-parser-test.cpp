#include "../src/vi_fps_parser.hpp"

#include <cmath>

int main()
{
	double fps = -1;
	if (!parse_vi_fps_line("VIFPS\t\t\t:   0\n", &fps) || fps != 0) return 1;
	if (!parse_vi_fps_line("VIFPS 59.94\n", &fps) || std::fabs(fps - 59.94) >= 0.001) return 2;
	if (parse_vi_fps_line("VIDevFPS\t: 0\n", &fps)) return 3;
	if (parse_vi_fps_line("VIFPS\t: unavailable\n", &fps)) return 4;
	if (parse_vi_fps_line("VIFPS : nan\n", &fps)) return 5;
	if (parse_vi_fps_line("VIFPS : inf\n", &fps)) return 6;
	if (parse_vi_fps_line("VIFPS : -1\n", &fps)) return 7;
	return 0;
}
