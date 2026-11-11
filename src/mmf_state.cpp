// Runtime state accessors for the upper layers (cvi_mpi runtime).
#include "mmf_internal.hpp"
#include "cvi_venc.h"

namespace onekvm::mmf {

bool runtime_initialized()
{
	return g_runtime.reference_count != 0;
}

bool capture_channel_running(int ch)
{
	return ch >= 0 && ch < MMF_VI_MAX_CHN && g_runtime.vi_chn_running[ch];
}

bool h26x_encoder_initialized(int ch)
{
	return ch >= 0 && ch < MMF_VENC_MAX_CHN && g_runtime.h26x_encoders[ch].initialized;
}

bool h26x_encoder_active()
{
	return g_runtime.h26x_encoder_active;
}

bool jpeg_encoder_initialized()
{
	return g_runtime.jpeg_initialized;
}

int jpeg_capture_channel()
{
	return g_runtime.jpeg_capture_channel;
}

int h26x_full_range(int ch)
{
	if (!h26x_encoder_initialized(ch))
		return -1;
	VENC_VUI_VIDEO_SIGNAL_S signal{};
	if (g_runtime.h26x_encoders[ch].cfg.codec == H26xCodec::H265) {
		VENC_H265_VUI_S vui{};
		if (CVI_VENC_GetH265Vui(ch, &vui) != CVI_SUCCESS)
			return -1;
		signal = vui.stVuiVideoSignal;
	} else {
		VENC_H264_VUI_S vui{};
		if (CVI_VENC_GetH264Vui(ch, &vui) != CVI_SUCCESS)
			return -1;
		signal = vui.stVuiVideoSignal;
	}
	// H.264/H.265 infer video_full_range_flag=0 when it is absent.
	return signal.video_signal_type_present_flag && signal.video_full_range_flag ? 1 : 0;
}

} // namespace onekvm::mmf
