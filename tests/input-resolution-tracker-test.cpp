#include "input_resolution_tracker.hpp"

int main() {
    using namespace onekvm;

    static_assert(supported_input_resolution({1920, 1080}));
    static_assert(supported_input_resolution({2560, 1440}));
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

    using namespace std::chrono_literals;
    if (hdmi_watch_interval(1) != 100ms) return 46;
    if (hdmi_watch_interval(0) != 1s) return 47;
    if (hdmi_watch_interval(-1) != 1s) return 48;

    if (hdmi_resolution_probe_due(0, false, false)) return 6;
    if (hdmi_resolution_probe_due(0, true, false)) return 7;
    if (!hdmi_resolution_probe_due(0, false, true)) return 8;
    if (!hdmi_resolution_probe_due(0, true, true)) return 9;
    if (!hdmi_resolution_probe_due(kHDMIChangeFailureThreshold, true, true)) return 10;

    if (classify_hdmi_input({0, 0}) != HdmiInputClass::None) return 11;
    if (classify_hdmi_input({1920, 1080}) != HdmiInputClass::Supported) return 12;
    if (classify_hdmi_input({1366, 768}) != HdmiInputClass::OutOfRange) return 13;
    if (classify_hdmi_input({2560, 1440}) != HdmiInputClass::Supported) return 14;
    if (classify_hdmi_input({3840, 2160}) != HdmiInputClass::OutOfRange) return 15;
    if (classify_hdmi_input({1921, 1080}) != HdmiInputClass::None) return 16;

    if (decode_target_resolution(1080) != InputResolution{1920, 1080}) return 17;
    if (decode_target_resolution(720) != InputResolution{1280, 720}) return 18;
    if (decode_target_resolution(480) != InputResolution{640, 480}) return 19;
    if (decode_target_resolution(12801024) != InputResolution{1280, 1024}) return 20;
    if (decode_target_resolution(25601440) != InputResolution{2560, 1440}) return 42;
    if (target_output_resolution(0, {2560, 1440}) != InputResolution{2560, 1440})
        return 43;
    if (infer_hdmi_mode({1280, 1440}) != InputResolution{2560, 1440}) return 44;
    if (infer_hdmi_mode({1280, 720}) != InputResolution{1280, 720}) return 45;
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
    if (choose_vi_receiver_size({800, 600}, {1920, 1080}, {800, 600}) !=
        InputResolution{1920, 1080})
        return 27;
    if (choose_vi_receiver_size({1920, 1080}, {800, 600}, {1920, 1080}) !=
        InputResolution{1920, 1080})
        return 28;
    if (choose_vi_receiver_size({0, 0}, {1920, 1080}, {800, 600}) !=
        InputResolution{1920, 1080})
        return 29;
    if (choose_vi_receiver_size({0, 0}, {800, 600}, {1920, 1080}).width != 0)
        return 30;
    if (choose_vi_receiver_size({800, 600}, {0, 0}, {1920, 1080}).width != 0)
        return 40;

    if (!should_rebuild_vi_receiver(
            {800, 600}, {1920, 1080}, {800, 600}))
        return 31;
    if (should_rebuild_vi_receiver(
            {1920, 1080}, {800, 600}, {1920, 1080}))
        return 32;
    if (should_rebuild_vi_receiver(
            {1920, 1080}, {800, 600}, {800, 600}, {0, 0}))
        return 41;
    if (!should_rebuild_vi_receiver(
            {1920, 1080}, {800, 600}, {800, 600}, {800, 600}))
        return 33;
    if (should_rebuild_vi_receiver(
            {800, 600}, {800, 600}, {800, 600}))
        return 34;
    if (should_rebuild_vi_receiver(
            {800, 600}, {}, {800, 600}))
        return 35;
    if (infer_hdmi_mode({960, 366}) != InputResolution{1920, 1080})
        return 36;
    if (infer_hdmi_mode({1920, 888}) != InputResolution{1920, 1080})
        return 37;
    if (infer_hdmi_mode({800, 600}) != InputResolution{800, 600})
        return 38;
    if (infer_hdmi_mode({1366, 768}) != InputResolution{1366, 768})
        return 39;
    if (should_grow_to_max_vi_receiver({800, 600}, {0, 0}, 0))
        return 42;
    if (should_grow_to_max_vi_receiver({800, 600}, {0, 0}, 2))
        return 47;
    if (!should_grow_to_max_vi_receiver({800, 600}, {0, 0}, 3))
        return 48;
    if (!should_grow_to_max_vi_receiver({800, 600}, {1920, 1080}))
        return 43;
    if (should_grow_to_max_vi_receiver({800, 600}, {800, 600}))
        return 44;
    if (should_grow_to_max_vi_receiver({1920, 1080}, {0, 0}, 3))
        return 45;
    if (should_grow_to_max_vi_receiver({800, 600}, {3568, 256}))
        return 46;
    if (common_vb_pool_size({800, 600}) != kMaxCaptureSize)
        return 50;
    if (common_vb_pool_size({1920, 1080}) != kMaxCaptureSize)
        return 51;
    if (common_vb_pool_size({2560, 1440}) != kMaxCaptureSize)
        return 52;
    if (choose_vi_receiver_size({800, 600}, {2560, 1440}, {800, 600}) !=
        InputResolution{2560, 1440})
        return 53;
    if (should_grow_to_max_vi_receiver({800, 600}, {2560, 1440}))
        return 54;
    if (should_grow_to_max_vi_receiver({1920, 1080}, {2560, 1440}))
        return 55;
    if (!should_rebuild_vi_receiver(
            {1920, 1080}, {2560, 1440}, {2560, 1440}, {2560, 1440}))
        return 56;

    /* Live 1440 must not shrink on HDMI-only 1080 (MIPI may still be 2560). */
    if (choose_vi_receiver_size(
            {0, 0}, {1920, 1080}, {2560, 1440}, true).width != 0)
        return 57;
    if (should_rebuild_vi_receiver(
            {2560, 1440}, {1920, 1080}, {0, 0}, {1920, 1080}, true))
        return 58;

    /* Dead VI after a false 1440 grow: follow the later 1080 HDMI reading. */
    if (choose_vi_receiver_size(
            {0, 0}, {1920, 1080}, {2560, 1440}, false) !=
        InputResolution{1920, 1080})
        return 59;
    if (!should_rebuild_vi_receiver(
            {2560, 1440}, {1920, 1080}, {0, 0}, {1920, 1080}, false))
        return 60;
    if (next_hdmi_follow_samples(true, {2560, 1440}, {1920, 1080}, 2) != 0)
        return 61;
    if (next_hdmi_follow_samples(false, {2560, 1440}, {2560, 1440}, 2) != 0)
        return 62;
    if (next_hdmi_follow_samples(false, {2560, 1440}, {1920, 1080}, 0) != 1)
        return 63;
    if (next_hdmi_follow_samples(false, {2560, 1440}, {1920, 1080}, 2) != 3)
        return 64;
    if (hdmi_stalled_follow_ready({0, 0}, {1920, 1080}, 2))
        return 65;
    if (!hdmi_stalled_follow_ready({0, 0}, {1920, 1080}, 3))
        return 66;
    if (!hdmi_stalled_follow_ready({1920, 1080}, {1920, 1080}, 0))
        return 67;

    return 0;
}
