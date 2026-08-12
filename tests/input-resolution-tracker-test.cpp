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

    return 0;
}
