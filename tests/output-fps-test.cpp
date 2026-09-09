#include "../src/mmf.hpp"

int main()
{
	using onekvm::mmf::clamp_output_fps;
	using onekvm::mmf::clamp_pipeline_fps;
	using onekvm::mmf::capture_pool_blocks;
	using onekvm::mmf::kDefaultInputFps;
	using onekvm::mmf::kMaxOutputFps;
	using onekvm::mmf::vi_common_pool_blocks;
	using onekvm::mmf::venc_src_fps;

	if (clamp_output_fps(0) != 30) return 1;
	if (clamp_output_fps(60) != 60) return 2;
	if (clamp_output_fps(120) != 120) return 3;
	if (clamp_output_fps(200) != kMaxOutputFps) return 4;
	if (clamp_output_fps(120, 1280, 720) != 120) return 9;
	if (clamp_output_fps(120, 1920, 1080) != 72) return 10;
	if (clamp_output_fps(60, 2560, 1440) != 40) return 11;
	if (clamp_output_fps(30, 2880, 1620) != 30) return 22;
	if (clamp_output_fps(60, 2880, 1620) != 32) return 23;
	if (venc_src_fps(30) != kDefaultInputFps) return 5;
	if (venc_src_fps(60) != 60) return 6;
	if (venc_src_fps(120) != 120) return 7;
	if (venc_src_fps(200) != kMaxOutputFps) return 8;
	if (venc_src_fps(10, 2560, 1440) != 10) return 12;
	if (venc_src_fps(60, 2560, 1440) != 40) return 13;
	if (venc_src_fps(60, 2880, 1620) != 32) return 24;
	if (capture_pool_blocks(1920, 1080) != 3) return 19;
	if (capture_pool_blocks(2560, 1440) != 3) return 20;
	if (vi_common_pool_blocks(1920, 1080) != 2) return 17;
	if (vi_common_pool_blocks(2560, 1440) != 2) return 18;
	if (vi_common_pool_blocks(2880, 1620) != 2) return 21;
	if (onekvm::mmf::vpss_phy_channel(1280) != 1) return 14;
	if (onekvm::mmf::vpss_phy_channel(1920) != 1) return 15;
	if (onekvm::mmf::vpss_phy_channel(2560) != 1) return 16;
	if (onekvm::mmf::vpss_phy_channel(2880) != 1) return 25;
	if (clamp_pipeline_fps(60, 1920, 1080, 2880, 1620) != 32) return 26;
	if (clamp_pipeline_fps(30, 2880, 1620, 1920, 1080) != 30) return 28;
	return 0;
}
