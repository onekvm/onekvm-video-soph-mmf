#pragma once

#include <onekvm/video_backend_v1.h>

#include "input_resolution_tracker.hpp"
#include "mmf.hpp"
#include "vi_fps_parser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace onekvm::video_backend {

#define ONEKVM_VIDEO_INTERNAL __attribute__((visibility("hidden")))

ONEKVM_VIDEO_INTERNAL inline constexpr int kMMFNV21 = 19;
ONEKVM_VIDEO_INTERNAL inline constexpr int kJPEGChannel = 0;
ONEKVM_VIDEO_INTERNAL inline constexpr int kFirstVENCChannel = 1;
ONEKVM_VIDEO_INTERNAL inline constexpr int kLastVENCChannel = 3;
ONEKVM_VIDEO_INTERNAL inline constexpr size_t kVENCBufferSize = 1024 * 1024;
ONEKVM_VIDEO_INTERNAL inline constexpr size_t kJPEGBufferSize = 2 * 1024 * 1024;
ONEKVM_VIDEO_INTERNAL inline constexpr int kRecoveryFailureThreshold = 3;
ONEKVM_VIDEO_INTERNAL inline constexpr auto kRecoveryInterval = std::chrono::seconds(5);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kHDMIChangeIdleWindow = std::chrono::milliseconds(500);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kHDMIChangeProbeInterval = std::chrono::seconds(2);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kSignalProbeInterval = std::chrono::seconds(10);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kNoSignalProbeInterval = std::chrono::seconds(1);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kRecentFrameSignalWindow = std::chrono::milliseconds(500);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kInitialResolutionSampleDelay = std::chrono::milliseconds(20);
/* Prefer /proc/cvitek/vi. Reading vi_dbg holds the VI debug handler and
   can stall CSI (VIFPS drops to 0 while SOF/FE counters freeze). */
ONEKVM_VIDEO_INTERNAL inline constexpr const char *kVideoStatusPath = "/proc/cvitek/vi";

struct ONEKVM_VIDEO_INTERNAL Source {
    std::mutex mutex;
    int channel = -1;
    bool initialized = false;
    bool frame_pending = false;
    uint64_t frame_token = 0;
    onekvm_video_source_config_v1 config{};
    std::vector<uint8_t> no_signal_frame;
    int no_signal_width = 0;
    int no_signal_height = 0;
    int failures = 0;
    std::chrono::steady_clock::time_point last_recovery{};
    std::chrono::steady_clock::time_point last_hdmi_probe{};
    std::atomic<uint64_t> last_frame_ns{0};
    std::atomic<uint64_t> last_signal_probe_ns{0};
    std::atomic<int> cached_signal{-1};
    std::atomic<int> last_vi_int_cnt{0};
    std::atomic_flag signal_probe_running = ATOMIC_FLAG_INIT;
    onekvm::InputResolutionTracker input_resolution;
    onekvm::InputResolution reported_input{};
    std::atomic<uint64_t> cached_input_size{0};
    std::atomic<int32_t> cached_input_fps{0};
    std::atomic<bool> out_of_range{false};
};

struct ONEKVM_VIDEO_INTERNAL Encoder {
    std::mutex mutex;
    onekvm_video_encoder_config_v1 config{};
    int codec_type = 2;
    int channel = -1;
    int width = 0;
    int height = 0;
    bool initialized = false;
    bool source_bound = false;
    bool placeholder_frames = false;
    bool request_keyframe = false;
    bool frame_pending = false;
    bool packet_borrowed = false;
    uint64_t pending_pts_ns = 0;
    size_t prepared_size = 0;
    uint64_t prepared_pts_ns = 0;
    std::vector<uint8_t> output;
    uint64_t mmf_generation = 0;
    Source *bound_source = nullptr;
};

extern ONEKVM_VIDEO_INTERNAL std::recursive_mutex g_mmf_mutex;
extern ONEKVM_VIDEO_INTERNAL std::atomic<uint64_t> g_mmf_generation;

ONEKVM_VIDEO_INTERNAL void set_error(
    char *error, uint32_t capacity, const char *format, ...);
ONEKVM_VIDEO_INTERNAL std::pair<int, int> resolution_size(int resolution);
ONEKVM_VIDEO_INTERNAL uint64_t monotonic_ns();
ONEKVM_VIDEO_INTERNAL bool nv21_size(int width, int height, size_t *size);
/* Caller must hold source->mutex. 1 = rebuilt, 0 = no change, -1 = failed. */
ONEKVM_VIDEO_INTERNAL int maybe_rebuild_for_hdmi_change(
    Source *source, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int no_signal_frame(
    Source *source, onekvm_video_frame_v1 *frame,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int cached_signal_present(Source *source);

ONEKVM_VIDEO_INTERNAL int32_t source_create(
    const onekvm_video_source_config_v1 *config, void **result,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t source_reset(
    void *source, const onekvm_video_source_config_v1 *config,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t source_read(
    void *source, onekvm_video_frame_v1 *frame,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL void source_release(void *source, uint64_t token);
ONEKVM_VIDEO_INTERNAL int32_t source_signal_present(void *source);
ONEKVM_VIDEO_INTERNAL int32_t source_input_format(
    void *source, onekvm_video_format_v1 *format);
ONEKVM_VIDEO_INTERNAL void source_destroy(void *source);

ONEKVM_VIDEO_INTERNAL int32_t encoder_create(
    const onekvm_video_encoder_config_v1 *config, void **result,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_reset(
    void *encoder, const onekvm_video_encoder_config_v1 *config,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_encode(
    void *encoder, const onekvm_video_frame_v1 *frame,
    onekvm_video_packet_v1 *packet, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_set_quality(
    void *encoder, double quality, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_request_keyframe(
    void *encoder, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL uint32_t encoder_codec(void *encoder);
ONEKVM_VIDEO_INTERNAL void encoder_destroy(void *encoder);
ONEKVM_VIDEO_INTERNAL void encoder_release_packet(void *encoder);
ONEKVM_VIDEO_INTERNAL int32_t encoder_prepare(
    void *encoder, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_bind_source(
    void *encoder, void *source, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_read_packet(
    void *encoder, onekvm_video_packet_v1 *packet,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_unbind_source(
    void *encoder, char *error, uint32_t error_capacity);

extern ONEKVM_VIDEO_INTERNAL const onekvm_video_backend_v1 kBackend;

} // namespace onekvm::video_backend
