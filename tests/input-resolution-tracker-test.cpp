#include "input_resolution_tracker.hpp"

int main() {
    using namespace onekvm;

    static_assert(supported_input_resolution({1920, 1080}));
    static_assert(supported_input_resolution({640, 480}));
    static_assert(!supported_input_resolution({1366, 768}));
    static_assert(!supported_input_resolution({0, 0}));

    InputResolutionTracker tracker;
    tracker.set_current({1920, 1080});
    if (tracker.observe({640, 480}) != InputResolutionObservation::Candidate ||
        tracker.current() != InputResolution{1920, 1080}) return 1;
    if (tracker.observe({0, 0}) != InputResolutionObservation::Invalid) return 2;
    if (tracker.observe({640, 480}) != InputResolutionObservation::Candidate) return 3;
    if (tracker.observe({640, 480}) != InputResolutionObservation::Changed ||
        tracker.current() != InputResolution{640, 480}) return 4;
    if (tracker.observe({640, 480}) != InputResolutionObservation::Unchanged) return 5;

    if (hdmi_resolution_probe_due(0, false, true)) return 6;
    if (hdmi_resolution_probe_due(kHDMIChangeFailureThreshold, true, true)) return 7;
    if (hdmi_resolution_probe_due(kHDMIChangeFailureThreshold, false, false)) return 8;
    if (!hdmi_resolution_probe_due(kHDMIChangeFailureThreshold, false, true)) return 9;
    if (!hdmi_resolution_probe_due(kHDMIChangeFailureThreshold + 2, false, true)) return 10;

    if (classify_hdmi_input({0, 0}) != HdmiInputClass::None) return 11;
    if (classify_hdmi_input({1920, 1080}) != HdmiInputClass::Supported) return 12;
    if (classify_hdmi_input({1366, 768}) != HdmiInputClass::OutOfRange) return 13;
    if (classify_hdmi_input({2560, 1440}) != HdmiInputClass::OutOfRange) return 14;
    if (classify_hdmi_input({3840, 2160}) != HdmiInputClass::OutOfRange) return 15;
    if (classify_hdmi_input({1921, 1080}) != HdmiInputClass::None) return 16;

    if (decode_target_resolution(1080) != InputResolution{1920, 1080}) return 17;
    if (decode_target_resolution(720) != InputResolution{1280, 720}) return 18;
    if (decode_target_resolution(480) != InputResolution{640, 480}) return 19;
    if (decode_target_resolution(12801024) != InputResolution{1280, 1024}) return 20;
    if (decode_target_resolution(8540480).width != 0) return 21;
    if (!valid_target_resolution_code(0) || !valid_target_resolution_code(12800800))
        return 22;
    if (valid_target_resolution_code(854) || valid_target_resolution_code(13660768))
        return 23;
    if (target_output_resolution(0, {1280, 720}) != InputResolution{1280, 720})
        return 24;
    if (target_output_resolution(0, {1366, 768}) != InputResolution{1920, 1080})
        return 25;
    if (target_output_resolution(1080, {1280, 720}) != InputResolution{1920, 1080})
        return 26;

    return 0;
}
