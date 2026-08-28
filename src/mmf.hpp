#pragma once

// Internal MMF implementation interface. This is not part of the OneKVM ABI.

#include <cstddef>
#include <cstdint>

extern "C" {
struct onekvm_lt6911_input_timing {
    uint32_t csi_width;
    uint32_t csi_height;
    uint32_t hdmi_width;
    uint32_t hdmi_height;
};

int render_no_signal_nv21(uint8_t *data, int capacity, int width, int height);
int no_signal_h264(int width, int height, const uint8_t **data, size_t *size);
int lt6911_get_input_timing(int pipe, struct onekvm_lt6911_input_timing *timing);
int lt6911_get_input_size(int pipe, uint32_t *width, uint32_t *height);
int lt6911_get_capture_size(uint32_t *width, uint32_t *height);
int lt6911_kick_hdmi(void);
void onekvm_lt6911_i2c_lock(void);
void onekvm_lt6911_i2c_unlock(void);
int onekvm_lt6911_set_active_size(uint32_t width, uint32_t height);
void onekvm_lt6911_get_active_size(uint32_t *width, uint32_t *height);
}

namespace onekvm::mmf {

enum class H26xCodec : uint8_t {
    H265 = 1,
    H264 = 2,
};

constexpr int kDefaultInputFps = 60;
constexpr int kMaxOutputFps = 120;

inline int clamp_output_fps(int fps, int width = 0, int height = 0)
{
	if (fps <= 0)
		return 30;
	int cap = kMaxOutputFps;
	if (width > 0 && height > 0) {
		const auto pixels = static_cast<int64_t>(width) * height;
		/* 1440p60 overruns WAVE4; 1080p120 overruns H.264 L4.2. */
		if (pixels > 1920 * 1080)
			cap = 30;
		else if (pixels > 1280 * 720)
			cap = kDefaultInputFps;
	}
	if (fps > cap)
		return cap;
	return fps;
}

/* VENC src must be >= dest. 1080p30 still uses src 60; 1440p30 uses src 30;
   720p120 uses src 120. */
inline int venc_src_fps(int output_fps, int width = 0, int height = 0)
{
	const int fps = clamp_output_fps(output_fps, width, height);
	int floor = kDefaultInputFps;
	if (width > 0 && height > 0 &&
	    static_cast<int64_t>(width) * height > 1920 * 1080)
		floor = 30;
	return fps < floor ? floor : fps;
}

struct H26xEncoderConfig {
    H26xCodec codec;
    int width;
    int height;
    int pixel_format;
    int gop;
    int input_fps;
    int output_fps;
    int bitrate_kbps;
};

struct RateControl {
    int initial_qp;
    int min_qp;
    int max_qp;
};

// init sys
int initialize(void);
int shutdown(void);

// manage vi channels(vi->vpssgroup->vpss->frame)
/* CV181x phy chn 0 is sc_d (max 1920). Always use chn 1 (sc_v1, max
   2880) so 1080p and 1440p share one scaler; HDMI grow/shrink does
   not switch channels. `width` is unused and kept for call sites. */
inline int vpss_phy_channel(int width = 0)
{
	(void)width;
	return 1;
}

int find_free_capture_channel(void);
int start_capture_pipeline(void);
int stop_capture_pipeline(void);
int open_capture_channel(int ch, int width, int height, int format, int fps);
int close_capture_channel(int ch);
int close_all_capture_channels(void);
int reset_capture_channel(int ch, int width, int height, int format, int fps);
bool capture_channel_open(int ch);
int pause_vpss_channel(int ch);
int resume_vpss_channel(int ch);
void park_unbound_vpss_channels();
void set_capture_mirror(int ch, bool en);
void set_capture_flip(int ch, bool en);

// get vi frame
int acquire_capture_frame(int ch, void **data, int *len, int *width, int *height, int *format);
void release_capture_frame(int ch);

// venc
int open_jpeg_encoder(int ch, int w, int h, int format, int quality);
int close_jpeg_encoder(int ch);
int submit_jpeg_frame(int ch, uint8_t *data, int w, int h, int format, int quality);
int read_jpeg_packet(int ch, uint8_t *dst, int capacity);
int release_jpeg_packet(int ch);
int open_h26x_encoder(int ch, const H26xEncoderConfig &config,
    const RateControl &rate_control);
int close_h26x_encoder(int ch);
int close_all_h26x_encoders();
int submit_h26x_frame(int ch, uint8_t *data, int w, int h, int format);
int read_h26x_packet(int ch, uint8_t *dst, int capacity);
// Copy and release exactly one access unit. H.26x reference frames must remain
// ordered; callers that drain stale units must request an IDR before resuming.
int read_latest_h26x_packet(int ch, uint8_t *dst, int capacity);
int read_latest_h26x_packet_nowait(int ch, uint8_t *dst, int capacity);
// Release every access unit already queued without waiting for a new one.
int drain_h26x_packets(int ch, uint8_t *scratch, int capacity);
void start_h26x_reader(int ch, bool request_idr = true);
void stop_h26x_reader(int ch);
int take_ready_h26x_packet(int ch, uint8_t *dst, int capacity,
	bool *key_frame = nullptr);
bool wait_ready_h26x_packet(int ch, int timeout_ms);
uint64_t h26x_reader_last_packet_ns(int ch);
uint64_t h26x_last_encode_ns(int ch);
uint64_t h26x_last_capture_ns(int ch);
void refresh_h26x_hw_latency(int ch);
void h26x_reader_want_idr(int ch);
int release_h26x_packet(int ch);
int request_h26x_idr(int ch);
int set_h26x_output_fps(int ch, int output_fps, int gop);
int set_h26x_rate_control(int ch, int output_fps, int gop, int bitrate_kbps,
	int initial_qp, int min_qp, int max_qp);
int bind_h26x_to_capture(int ch, int vpss_group, int vpss_channel);
int unbind_h26x_from_capture(int ch);

} // namespace onekvm::mmf
