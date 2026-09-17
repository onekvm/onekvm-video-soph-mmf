#pragma once

#include <onekvm/video_backend_v1.h>

#include "input_resolution_tracker.hpp"
#include "mmf.hpp"

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
/* Dead VI after a false grow: probe HDMI more often so a later 1080 can
   rebuild CSIBDG without waiting on the live 2s interval. */
ONEKVM_VIDEO_INTERNAL inline constexpr auto kHDMIFollowProbeInterval =
    std::chrono::milliseconds(500);
/* HDMI 800→1080 changes CSI output before the video loop runs.  A dedicated
   watcher must see HDMI timing while the source is still 800, including when
   no WebRTC consumer is attached. */
ONEKVM_VIDEO_INTERNAL inline constexpr auto kHDMIChangeGrowProbeInterval =
    std::chrono::milliseconds(100);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kSignalProbeInterval = std::chrono::seconds(10);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kNoSignalProbeInterval = std::chrono::seconds(1);
static_assert(onekvm::hdmi_watch_interval(1, {800, 600}) ==
    kHDMIChangeGrowProbeInterval);
static_assert(onekvm::hdmi_watch_interval(1, {1920, 1080}) ==
    std::chrono::duration_cast<std::chrono::milliseconds>(kNoSignalProbeInterval));
static_assert(onekvm::hdmi_watch_interval(1, {1920, 1080}, false) ==
    std::chrono::duration_cast<std::chrono::milliseconds>(kNoSignalProbeInterval));
static_assert(onekvm::hdmi_watch_probe_due(1, {1920, 1080}, false));
static_assert(onekvm::csi_half_rate_locked(60, 30));
static_assert(!onekvm::csi_half_rate_locked(60, 59));
static_assert(!onekvm::csi_half_rate_locked(30, 30));
static_assert(onekvm::csi_half_rate_rearm_ready(4, false));
static_assert(!onekvm::csi_half_rate_rearm_ready(4, true));
static_assert(onekvm::hdmi_watch_interval(0, {}) ==
    std::chrono::duration_cast<std::chrono::milliseconds>(kNoSignalProbeInterval));
static_assert(onekvm::hdmi_watch_interval(-1, {}) ==
    std::chrono::duration_cast<std::chrono::milliseconds>(kNoSignalProbeInterval));
ONEKVM_VIDEO_INTERNAL inline constexpr auto kRecentFrameSignalWindow = std::chrono::milliseconds(500);
/* Inserting the canned no-signal IDR between live P-frames makes the
   picture jump.  Wait out a short GetStream stall first. */
ONEKVM_VIDEO_INTERNAL inline constexpr auto kVencLiveRecentWindow =
    std::chrono::milliseconds(1500);
/* Exclusive VI→VENC has no VPSS RecvCnt, and VI FrameRate stays 0 for about
   a second after bind. 1.5 s is too short: the packet path then treats
   "no AU yet" as no HDMI, probes LT6911 80ee, and stalls CSI. */
ONEKVM_VIDEO_INTERNAL inline constexpr auto kVencFirstAuWindow =
    std::chrono::milliseconds(5000);
ONEKVM_VIDEO_INTERNAL inline constexpr auto kInitialResolutionSampleDelay = std::chrono::milliseconds(20);

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
    std::atomic<uint64_t> last_capture_ns{0};
    std::atomic<uint64_t> last_signal_probe_ns{0};
    std::atomic<int> cached_signal{-1};
    std::atomic<int> last_vi_int_cnt{0};
    std::atomic<uint64_t> last_vi_int_change_ns{0};
    std::atomic_flag signal_probe_running = ATOMIC_FLAG_INIT;
    onekvm::InputResolutionTracker input_resolution;
    onekvm::InputResolution reported_input{};
    onekvm::InputResolution pending_receiver{};
    unsigned hdmi_blanking_samples = 0;
    unsigned hdmi_oor_samples = 0;
    unsigned hdmi_follow_samples = 0;
    unsigned hdmi_rearm_samples = 0;
    unsigned csi_half_rate_samples = 0;
    bool csi_half_rate_rearmed = false;
    int csi_half_rate_target = 0;
    uint64_t last_half_rate_check_ns = 0;
    int capture_width = 0;
    int capture_height = 0;
    std::atomic<uint64_t> cached_input_size{0};
    std::atomic<int32_t> cached_input_fps{0};
    std::atomic<bool> out_of_range{false};
    std::atomic<bool> capture_live{false};
    std::atomic<bool> hdmi_watch_stop{false};
    std::thread hdmi_watch;
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
    bool placeholder_need_key = false;
    unsigned placeholder_idr_tries = 0;
    bool placeholder_logged = false;
    bool request_keyframe = false;
    bool frame_pending = false;
    bool packet_borrowed = false;
    uint64_t pending_pts_ns = 0;
    size_t prepared_size = 0;
    uint64_t prepared_pts_ns = 0;
    uint64_t bound_since_ns = 0;
    std::vector<uint8_t> output;
    uint64_t mmf_generation = 0;
    Source *bound_source = nullptr;
    std::atomic<uint64_t> last_capture_ns{0};
    std::atomic<uint64_t> last_encode_ns{0};
    bool managed_allocation = false;
    uint64_t allocation_id = 0;
    uint32_t allocation_input_mode = 0;
    uint32_t allocation_purpose = 0;
    uint32_t allocation_pixel_format = ONEKVM_VIDEO_PIXEL_UNKNOWN;
    int allocation_width = 0;
    int allocation_height = 0;
};

extern ONEKVM_VIDEO_INTERNAL std::recursive_mutex g_mmf_mutex;
extern ONEKVM_VIDEO_INTERNAL std::atomic<uint64_t> g_mmf_generation;

ONEKVM_VIDEO_INTERNAL void set_error(
    char *error, uint32_t capacity, const char *format, ...);
ONEKVM_VIDEO_INTERNAL std::pair<int, int> resolution_size(int resolution);
ONEKVM_VIDEO_INTERNAL std::pair<int, int> source_output_size(const Source *source);
ONEKVM_VIDEO_INTERNAL uint64_t monotonic_ns();
ONEKVM_VIDEO_INTERNAL bool nv21_size(int width, int height, size_t *size);
/* Caller must hold source->mutex. 1 = rebuilt, 0 = no change, -1 = failed. */
ONEKVM_VIDEO_INTERNAL int maybe_rebuild_for_hdmi_change(
    Source *source, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int maybe_rebuild_for_hdmi_change_now(
    Source *source, char *error, uint32_t error_capacity);
/* Caller holds encoder and source mutexes. 1 = rebuilt, 0 = no change, -1 = failed. */
ONEKVM_VIDEO_INTERNAL int maybe_rearm_csi_half_rate(
    Encoder *encoder, Source *source, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int ensure_no_signal_nv21(
    Source *source, int width, int height,
    char *error, uint32_t error_capacity);
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
ONEKVM_VIDEO_INTERNAL int32_t source_latency(
    void *source, onekvm_video_latency_v1 *latency);
ONEKVM_VIDEO_INTERNAL int32_t source_snapshot(
    void *source, const onekvm_video_snapshot_request_v1 *request,
    uint8_t *data, uint64_t capacity,
    onekvm_video_snapshot_result_v1 *result,
    char *error, uint32_t error_capacity);
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
ONEKVM_VIDEO_INTERNAL int32_t encoder_latency(
    void *encoder, onekvm_video_latency_v1 *latency);
ONEKVM_VIDEO_INTERNAL int32_t encoder_resources(
    onekvm_video_encoder_resources_v1 *resources,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_allocate(
    const onekvm_video_encoder_allocation_request_v1 *request,
    void **encoder, onekvm_video_encoder_allocation_v1 *allocation,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t encoder_allocation(
    void *encoder, onekvm_video_encoder_allocation_v1 *allocation,
    char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t edid_capabilities(onekvm_video_edid_caps_v1 *caps);
ONEKVM_VIDEO_INTERNAL int32_t edid_get(
    onekvm_video_edid_blob_v1 *edid, char *error, uint32_t error_capacity);
ONEKVM_VIDEO_INTERNAL int32_t edid_set(
    const onekvm_video_edid_blob_v1 *edid,
    onekvm_video_edid_apply_result_v1 *apply,
    char *error, uint32_t error_capacity);

extern ONEKVM_VIDEO_INTERNAL const onekvm_video_backend_v1 kBackend;

} // namespace onekvm::video_backend
