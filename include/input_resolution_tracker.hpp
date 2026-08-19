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

constexpr std::array<InputResolution, 12> kSupportedInputResolutions{{
    {1920, 1080}, {1600, 900}, {1440, 1080}, {1440, 900},
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

/* CSIBDG must never be smaller than the MIPI frame about to arrive.
   Grow immediately when HDMI timing is already larger (800→1080).
   Shrink only when CSI active size has actually followed (1080→800). */
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
    if (csi_ok)
        return csi;
    return {};
}

constexpr bool should_rebuild_vi_receiver(
    InputResolution current,
    InputResolution receiver,
    InputResolution csi)
{
    if (receiver.width == 0 || receiver == current)
        return false;
    if (resolution_pixels(receiver) < resolution_pixels(current))
        return csi == receiver;
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
