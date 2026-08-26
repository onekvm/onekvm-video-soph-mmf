#include "../src/mmf.hpp"

int main()
{
	using onekvm::mmf::clamp_output_fps;
	using onekvm::mmf::kDefaultInputFps;
	using onekvm::mmf::kMaxOutputFps;
	using onekvm::mmf::venc_src_fps;

	if (clamp_output_fps(0) != 30) return 1;
	if (clamp_output_fps(60) != 60) return 2;
	if (clamp_output_fps(120) != 120) return 3;
	if (clamp_output_fps(200) != kMaxOutputFps) return 4;
	if (clamp_output_fps(120, 1280, 720) != 120) return 9;
	if (clamp_output_fps(120, 1920, 1080) != kDefaultInputFps) return 10;
	if (venc_src_fps(30) != kDefaultInputFps) return 5;
	if (venc_src_fps(60) != 60) return 6;
	if (venc_src_fps(120) != 120) return 7;
	if (venc_src_fps(200) != kMaxOutputFps) return 8;
	return 0;
}
