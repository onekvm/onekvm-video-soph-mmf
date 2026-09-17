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
	if (!parse_vi_chn_status_header(
		    "-------------------------------VI CHN STATUS------------------------------------\n"))
		return 8;
	if (parse_vi_chn_status_header("VI DEV ATTR1\n")) return 9;
	if (!parse_vi_chn_status_fps("  0  0  Y   60 7339 7339    0    019201080\n", &fps) ||
	    fps != 60)
		return 10;
	if (!parse_vi_chn_status_fps("  0  0  N   0 0 0    0    019201080\n", &fps) ||
	    fps != 0)
		return 11;
	if (!parse_vi_chn_status_fps("  0  0  Y    0 1946 1946    0    019201080\n",
					&fps) ||
	    fps != 60)
		return 14;
	if (parse_vi_chn_status_fps(
		    "  0  0 019201080  N N  -1  -1NV21  SDR8  -1\n", &fps))
		return 12;
	if (parse_vi_chn_status_fps("  1  0  Y   60 1 1    0    019201080\n", &fps))
		return 13;
	if (!parse_vi_chn_status_fps("  0  0  Y  120 1403 1403    0    0 1280  720\n",
					&fps) ||
	    fps != 120)
		return 15;
	/* 6.18 seq_printf uses tabs and %5d; FrameRate 0 must not glue to IntCnt. */
	if (!parse_vi_chn_status_fps(
		    "\t  0\t  0\t  Y\t    0\t\t 3100\t 3100\t    1\t\t    0\t1920\t1080\n",
		    &fps) ||
	    fps != 60)
		return 16;
	return 0;
}
