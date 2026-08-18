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

/* Probe LT6911 only after capture/encode has already gone idle. A live
 * hitch of a few empty VENC reads must not touch internal registers. */
constexpr bool hdmi_resolution_probe_due(
    unsigned failures,
    bool recent_frames,
    bool interval_elapsed)
{
    if (recent_frames)
        return false;
    if (failures < kHDMIChangeFailureThreshold)
        return false;
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
