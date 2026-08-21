#include "../src/venc_pts_latency.hpp"

int main()
{
	if (venc_capture_ns(0, 1000, 0) != 0)
		return 1;
	if (venc_capture_ns(2000, 1000, 0) != 0)
		return 2;
	if (venc_capture_ns(1000, 1000, 0) != 0)
		return 3;
	if (venc_capture_ns(1000, 1000 + 1000000ull, 0) != 0)
		return 4;
	if (venc_capture_ns(1000, 8000, 0) != 7000ull * 1000ull)
		return 5;
	if (venc_capture_ns(1000, 8000, 2000ull * 1000ull) != 5000ull * 1000ull)
		return 6;
	if (venc_capture_ns(1000, 8000, 9000ull * 1000ull) != 7000ull * 1000ull)
		return 7;
	return 0;
}
