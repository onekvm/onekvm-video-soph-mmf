#ifndef ONEKVM_INPUT_RESOLUTION_TRACKER_HPP
#define ONEKVM_INPUT_RESOLUTION_TRACKER_HPP

#include <array>
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

constexpr std::array<InputResolution, 13> kSupportedInputResolutions{{
    {2560, 1440}, {1920, 1080}, {1600, 900}, {1440, 1080}, {1440, 900},
    {1280, 1024}, {1280, 960}, {1280, 800}, {1280, 720},
    {1152, 864}, {1024, 768}, {800, 600}, {640, 480},
}};

constexpr bool supported_input_resolution(InputResolution resolution) {
    for (const auto supported : kSupportedInputResolutions) {
        if (resolution == supported) return true;
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
        if (supported_input_resolution(packed))
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
        if (supported_input_resolution(input))
            return input;
        return {1920, 1080};
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
    if ((resolution.width & 1u) != 0 || (resolution.height & 1u) != 0)
        return false;
    return true;
}

constexpr HdmiInputClass classify_hdmi_input(InputResolution resolution) {
    if (!plausible_hdmi_size(resolution))
        return HdmiInputClass::None;
    if (supported_input_resolution(resolution))
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
    if (supported_input_resolution(hdmi))
        return hdmi;
    if (hdmi.width == 960 || hdmi.width == 1920)
        return {1920, 1080};
    if (hdmi.width == 1280 && hdmi.height == 1440)
        return {2560, 1440};
    if (hdmi.width == 640 || hdmi.width == 1280)
        return {1280, 720};
    if (hdmi.width == 400 || hdmi.width == 800)
        return {800, 600};
    return hdmi;
}

/* CSIBDG must never be smaller than the MIPI frame about to arrive.
   Grow immediately when HDMI timing is already larger (800→1080).
   HDMI 0/blanking is a mode change, not a shrink: returning CSI 800
   while current is 1920 would program CSIBDG to 800 just as 1080 starts. */
constexpr InputResolution choose_vi_receiver_size(
    InputResolution csi,
    InputResolution hdmi,
    InputResolution current)
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
    InputResolution hdmi = {})
{
    if (receiver.width == 0 || receiver == current)
        return false;
    if (resolution_pixels(receiver) < resolution_pixels(current))
        return csi == receiver && hdmi == receiver;
    return true;
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
