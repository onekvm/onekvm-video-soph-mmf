#ifndef ONEKVM_INPUT_RESOLUTION_TRACKER_HPP
#define ONEKVM_INPUT_RESOLUTION_TRACKER_HPP

#include <array>
#include <chrono>
#include <cstdint>

namespace onekvm {

struct InputResolution {
    uint32_t width = 0;
    uint32_t height = 0;

    constexpr bool operator==(const InputResolution &other) const {
        return width == other.width && height == other.height;
    }
    constexpr bool operator!=(const InputResolution &other) const {
        return !(*this == other);
    }
};

/* Known/selectable output profiles. HDMI input acceptance is intentionally
   not a whitelist: PCs emit many valid timings such as 1366x768. */
constexpr std::array<InputResolution, 14> kKnownInputResolutions{{
    {2880, 1620}, {2560, 1440}, {1920, 1080}, {1600, 900}, {1440, 1080}, {1440, 900},
    {1280, 1024}, {1280, 960}, {1280, 800}, {1280, 720},
    {1152, 864}, {1024, 768}, {800, 600}, {640, 480},
}};

constexpr bool supported_input_resolution(InputResolution resolution) {
    if (resolution.width < 320 || resolution.height < 200)
        return false;
    if (resolution.width > 2880 || resolution.height > 1620)
        return false;
    return (resolution.width & 1u) == 0 && (resolution.height & 1u) == 0 &&
        static_cast<uint64_t>(resolution.width) * resolution.height <= 5000000ull;
}

/* LT6911 CSI active size (0xc238/0xc206). HDMI counters are only a fallback
   when CSI is 0 during a mode change or while SoC VI DMA is paused. */
constexpr bool csi_timing_present(InputResolution csi, InputResolution hdmi = {})
{
    return supported_input_resolution(csi) ||
        supported_input_resolution(hdmi);
}

constexpr InputResolution csi_reported_size(
    InputResolution csi, InputResolution hdmi)
{
    const bool csi_ok = supported_input_resolution(csi);
    const bool hdmi_ok = supported_input_resolution(hdmi);
    if (csi_ok && hdmi_ok) {
        const uint64_t hdmi_px =
            static_cast<uint64_t>(hdmi.width) * hdmi.height;
        const uint64_t csi_px =
            static_cast<uint64_t>(csi.width) * csi.height;
        if (hdmi_px > csi_px)
            return hdmi;
        return csi;
    }
    if (hdmi_ok)
        return hdmi;
    if (csi_ok)
        return csi;
    return {};
}

/* SG2002 input is capped at 5 MP and 2880x1620. Within that geometry limit,
   cap the frame rate to the 5 MP at 30 frames/s pixel-throughput budget. */
constexpr uint64_t kMaxVideoPixelsPerSecond = 5000000ull * 30ull;

constexpr bool supported_input_rate(InputResolution resolution, int fps)
{
    return fps > 0 && supported_input_resolution(resolution) &&
        static_cast<uint64_t>(resolution.width) * resolution.height *
            static_cast<uint64_t>(fps) <= kMaxVideoPixelsPerSecond;
}

/* sc_v1 has a native output width limit of 2880. 2880x1620 is the largest
   16:9 profile which also fits the 5 MP output budget. */
constexpr bool supported_output_resolution(InputResolution resolution)
{
    return supported_input_resolution(resolution) && resolution.width <= 2880 &&
        static_cast<uint64_t>(resolution.width) * resolution.height <= 5000000ull;
}

constexpr bool known_output_resolution(InputResolution resolution)
{
    for (const auto known : kKnownInputResolutions) {
        if (known == resolution)
            return true;
    }
    return false;
}

/* Config still stores an int: 0 = follow HDMI, 1080/720/480 are the
   historical height aliases, and width*10000+height packs the rest. */
constexpr int kPackedResolutionScale = 10000;

constexpr InputResolution decode_target_resolution(int code)
{
    if (code == 1080)
        return {1920, 1080};
    if (code == 720)
        return {1280, 720};
    if (code == 480)
        return {640, 480};
    if (code >= kPackedResolutionScale) {
        const InputResolution packed{
            static_cast<uint32_t>(code / kPackedResolutionScale),
            static_cast<uint32_t>(code % kPackedResolutionScale),
        };
        if (known_output_resolution(packed))
            return packed;
    }
    return {};
}

constexpr bool valid_target_resolution_code(int code)
{
    return code == 0 || decode_target_resolution(code).width != 0;
}

constexpr InputResolution target_output_resolution(
    int code, InputResolution input)
{
    if (code <= 0) {
        if (supported_output_resolution(input))
            return input;
        /* Unsupported inputs do not normally reach this path. Keep the
           historical safe fallback for callers with stale state. */
        return {2560, 1440};
    }
    const auto decoded = decode_target_resolution(code);
    if (decoded.width != 0)
        return decoded;
    return {1920, 1080};
}

enum class HdmiInputClass {
    None,
    Supported,
    OutOfRange,
};

constexpr bool plausible_hdmi_size(InputResolution resolution) {
    if (resolution.width < 320 || resolution.height < 200)
        return false;
    if (resolution.width > 4096 || resolution.height > 2160)
        return false;
    return (resolution.width & 1u) == 0 && (resolution.height & 1u) == 0;
}

constexpr HdmiInputClass classify_hdmi_input(
    InputResolution resolution, int fps = 30) {
    if (!plausible_hdmi_size(resolution))
        return HdmiInputClass::None;
    if (supported_input_rate(resolution, fps))
        return HdmiInputClass::Supported;
    return HdmiInputClass::OutOfRange;
}

enum class InputResolutionObservation {
    Invalid,
    Unchanged,
    Candidate,
    Changed,
};

constexpr unsigned kHDMIChangeFailureThreshold = 3;

constexpr uint64_t resolution_pixels(InputResolution resolution)
{
    return static_cast<uint64_t>(resolution.width) *
           static_cast<uint64_t>(resolution.height);
}

constexpr InputResolution kMaxViReceiver{1920, 1080};

/* Common VB is sized for the largest supported capture, the same way the
   old path reserved 1080p even on 800x600 HDMI. kMaxViReceiver is only
   the blanking-grow target; following current HDMI left the pool at
   1920x1080 while VI EnableChn asked for 2560x1440 NV21. */
constexpr InputResolution kMaxCaptureSize{2560, 1440};

constexpr InputResolution common_vb_pool_size(InputResolution active)
{
    InputResolution pool = active;
    if (pool.width < kMaxCaptureSize.width)
        pool.width = kMaxCaptureSize.width;
    if (pool.height < kMaxCaptureSize.height)
        pool.height = kMaxCaptureSize.height;
    return pool;
}

/* LT6911C HDMI counters often show half-width and a garbage height for one
   or two samples while the source is locking 1080p. Treat those as the
   matching supported mode so CSIBDG can grow before the first 1920 line. */
constexpr InputResolution infer_hdmi_mode(InputResolution hdmi)
{
    if (known_output_resolution(hdmi) || hdmi == InputResolution{3840, 2160})
        return hdmi;
    if (hdmi.width == 960 || hdmi.width == 1920)
        return {1920, 1080};
    if (hdmi.width == 1280 && hdmi.height == 1440)
        return {2560, 1440};
    if (hdmi.width == 1920 && hdmi.height == 2160)
        return {3840, 2160};
    if (hdmi.width == 640 || hdmi.width == 1280)
        return {1280, 720};
    if (hdmi.width == 400 || hdmi.width == 800)
        return {800, 600};
    return supported_input_resolution(hdmi) ? hdmi : InputResolution{};
}

/* CSIBDG must never be smaller than the MIPI frame about to arrive.
   Grow immediately when HDMI timing is already larger (800→1080).
   HDMI 0/blanking is a mode change, not a shrink: returning CSI 800
   while current is 1920 would program CSIBDG to 800 just as 1080 starts.
   If VI is already dead (no recent frames) and HDMI reports a different
   supported mode, follow HDMI: waiting for CSI after a false grow never
   unsticks a hung CSIBDG (1080 source, 1440 receiver). */
constexpr InputResolution choose_vi_receiver_size(
    InputResolution csi,
    InputResolution hdmi,
    InputResolution current,
    bool frames_live = true)
{
    const bool csi_ok = supported_input_resolution(csi);
    const bool hdmi_ok = supported_input_resolution(hdmi);
    if (csi_ok && hdmi_ok) {
        if (resolution_pixels(hdmi) > resolution_pixels(csi))
            return hdmi;
        return csi;
    }
    if (hdmi_ok && resolution_pixels(hdmi) > resolution_pixels(current))
        return hdmi;
    if (csi_ok) {
        if (hdmi.width == 0 &&
            resolution_pixels(csi) < resolution_pixels(current))
            return {};
        return csi;
    }
    if (!frames_live && hdmi_ok && hdmi != current)
        return hdmi;
    return {};
}

/* Leave a sub-1080 CSIBDG when HDMI is already a larger mode, or after
   consecutive blanking samples. A single HDMI 0/I2C miss at 800 must not
   rebuild to 1920 or the picture flaps. */
constexpr unsigned kHDMIBlankingGrowSamples = 3;

constexpr bool should_grow_to_max_vi_receiver(
    InputResolution current, InputResolution hdmi,
    unsigned blanking_samples = 0)
{
    if (current == kMaxViReceiver || current.width == 0)
        return false;
    if (hdmi.width == 0)
        return blanking_samples >= kHDMIBlankingGrowSamples;
    /* Do not cover choose_vi_receiver_size() for a larger supported mode.
       800→1440 would otherwise program CSIBDG as 1920 and GT-fault the
       2560 MIPI frame. 800→1080 still matches infer_hdmi_mode. */
    return infer_hdmi_mode(hdmi) == kMaxViReceiver && hdmi != current;
}

constexpr bool should_rebuild_vi_receiver(
    InputResolution current,
    InputResolution receiver,
    InputResolution csi,
    InputResolution hdmi = {},
    bool frames_live = true)
{
    if (receiver.width == 0 || receiver == current)
        return false;
    if (resolution_pixels(receiver) < resolution_pixels(current)) {
        if (csi == receiver && hdmi == receiver)
            return true;
        return !frames_live && hdmi == receiver;
    }
    return true;
}

/* Consecutive dead-VI probes with HDMI at a different supported size.
   Grow on I2C stays immediate; this only gates shrink/switch without CSI. */
constexpr unsigned kHDMIFollowDeadSamples = 3;

constexpr unsigned next_hdmi_follow_samples(
    bool frames_live,
    InputResolution current,
    InputResolution hdmi,
    unsigned follow_samples)
{
    if (frames_live)
        return 0;
    if (!supported_input_resolution(hdmi) || hdmi == current)
        return 0;
    if (follow_samples >= kHDMIFollowDeadSamples)
        return kHDMIFollowDeadSamples;
    return follow_samples + 1;
}

constexpr bool hdmi_stalled_follow_ready(
    InputResolution csi,
    InputResolution receiver,
    unsigned follow_samples)
{
    if (receiver.width == 0)
        return false;
    if (csi == receiver)
        return true;
    return follow_samples >= kHDMIFollowDeadSamples;
}

/* VI is running but CSI is 0x0, while HDMI I2C still reports the current
   supported mode. Waiting for CSI never recovers; reopen to re-arm
   LT6911. Idle with VI DMA paused also shows CSI 0x0; that is expected
   and must not re-arm. Placeholder unbinds VI→VPSS and looks the same
   while capture_live is true: callers must pass vi_dma_running=false
   then. Distinct from follow-samples, which reset when hdmi==current. */
constexpr unsigned kHDMICsiRearmSamples = 3;

constexpr unsigned next_hdmi_csi_rearm_samples(
    bool frames_live,
    InputResolution csi,
    InputResolution hdmi,
    InputResolution current,
    unsigned samples,
    bool vi_dma_running = true)
{
    if (!vi_dma_running || frames_live)
        return 0;
    if (csi.width != 0 || csi.height != 0)
        return 0;
    if (current.width == 0 || !supported_input_resolution(hdmi))
        return 0;
    if (hdmi != current)
        return 0;
    if (samples >= kHDMICsiRearmSamples)
        return kHDMICsiRearmSamples;
    return samples + 1;
}

constexpr bool hdmi_csi_rearm_ready(unsigned samples)
{
    return samples >= kHDMICsiRearmSamples;
}

/* Live AUs at ~half the configured pipeline fps. 1080p60 HDMI with LT6911
   CSI stuck at 30 still looks "live" (packet every 33 ms < 1.5 s stale
   window), so the watcher never probes. 720p120→60 is the same class.
   Do not fire for configured 30 (1440p / user fps), complete stalls
   (crypto 1 fps / no HDMI), or 50–59 jitter. */
constexpr int kCsiHalfRateMinTarget = 48;
constexpr unsigned kCsiHalfRateSamples = 4;
constexpr auto kCsiHalfRateSampleInterval = std::chrono::milliseconds(500);

constexpr bool csi_half_rate_locked(int target_fps, int enc_fps)
{
    if (target_fps < kCsiHalfRateMinTarget || enc_fps < 20)
        return false;
    if (enc_fps + 8 >= target_fps)
        return false;
    const int half = (target_fps + 1) / 2;
    return enc_fps >= half - 6 && enc_fps <= half + 6;
}

constexpr unsigned next_csi_half_rate_samples(
    int target_fps, int enc_fps, unsigned samples, bool already_rearmed)
{
    if (target_fps < kCsiHalfRateMinTarget)
        return 0;
    if (enc_fps + 8 >= target_fps)
        return 0;
    if (!csi_half_rate_locked(target_fps, enc_fps))
        return 0;
    if (already_rearmed)
        return samples >= kCsiHalfRateSamples ? kCsiHalfRateSamples : samples;
    if (samples >= kCsiHalfRateSamples)
        return kCsiHalfRateSamples;
    return samples + 1;
}

constexpr bool csi_half_rate_recovered(int target_fps, int enc_fps)
{
    return target_fps >= kCsiHalfRateMinTarget && enc_fps + 8 >= target_fps;
}

constexpr bool csi_half_rate_rearm_ready(unsigned samples, bool already_rearmed)
{
    return !already_rearmed && samples >= kCsiHalfRateSamples;
}

/* Interval is the only hard gate. 800→1080 keeps IntCnt ticking on CSI
   errors, so "recent frames" must not hide an upscale. */
constexpr bool hdmi_resolution_probe_due(
    unsigned failures,
    bool recent_frames,
    bool interval_elapsed)
{
    (void)failures;
    (void)recent_frames;
    return interval_elapsed;
}

/* cached_signal: 1 = live frames, 0 = no HDMI, -1 = unknown.
   capture_live is VI DMA + a bound consumer. Idle probes CSI I2C; a live
   1080p+ path must not. Sub-1080 still uses the 100 ms grow probe. */
constexpr auto hdmi_watch_interval(
    int cached_signal, InputResolution current = {},
    bool capture_live = true)
{
    using namespace std::chrono_literals;
    const bool grow = current.width != 0 &&
        resolution_pixels(current) < resolution_pixels(kMaxViReceiver);
    if (!capture_live)
        return grow ? 100ms : 1000ms;
    return cached_signal == 1 && grow ? 100ms : 1000ms;
}

constexpr bool hdmi_watch_probe_due(
    int cached_signal, InputResolution current,
    bool capture_live = true)
{
    if (!capture_live)
        return true;
    return cached_signal != 1 || current.width == 0 ||
        resolution_pixels(current) < resolution_pixels(kMaxViReceiver);
}

class InputResolutionTracker {
public:
    void set_current(InputResolution resolution) {
        current_ = resolution;
        candidate_ = {};
        candidate_samples_ = 0;
    }

    InputResolution current() const { return current_; }

    InputResolutionObservation observe(InputResolution resolution) {
        if (!supported_input_resolution(resolution)) {
            candidate_ = {};
            candidate_samples_ = 0;
            return InputResolutionObservation::Invalid;
        }
        if (resolution == current_) {
            candidate_ = {};
            candidate_samples_ = 0;
            return InputResolutionObservation::Unchanged;
        }
        if (resolution != candidate_) {
            candidate_ = resolution;
            candidate_samples_ = 1;
            return InputResolutionObservation::Candidate;
        }
        if (++candidate_samples_ < 2)
            return InputResolutionObservation::Candidate;

        current_ = candidate_;
        candidate_ = {};
        candidate_samples_ = 0;
        return InputResolutionObservation::Changed;
    }

private:
    InputResolution current_{};
    InputResolution candidate_{};
    unsigned candidate_samples_ = 0;
};

} // namespace onekvm

#endif
