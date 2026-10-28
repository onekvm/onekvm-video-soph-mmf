#include "capture-startup.hpp"

#include <cassert>
#include <cstdio>

using onekvm::CaptureStartup;
constexpr uint64_t ms = 1000000;

int main() {
    CaptureStartup capture;
    assert(!capture.waiting(0));
    assert(!capture.full_timing_probe_safe());

    // Loss parks VI. The C and UXC recovery probes may now open 80ee.
    capture.park();
    assert(capture.placeholder());
    assert(capture.full_timing_probe_safe());

    // Reopening after EDID/HPD must leave the old placeholder episode.
    // The field timing cache may still say "no signal" throughout startup.
    const auto reopened = 10000 * ms;
    capture.begin(reopened);
    assert(!capture.placeholder());
    assert(!capture.full_timing_probe_safe());
    assert(capture.waiting(reopened + 28 * ms));
    assert(capture.waiting(reopened + 1501 * ms));
    assert(capture.waiting(reopened + CaptureStartup::window_ns));
    assert(!capture.waiting(reopened + CaptureStartup::window_ns + 1));
    // A deadline is not permission for a full snapshot of attached VI.
    assert(!capture.full_timing_probe_safe());
    capture.park();
    assert(capture.full_timing_probe_safe());

    // Rebinding the same VENC after user frames starts a new trial too.
    const auto rebound = reopened + 7000 * ms;
    capture.begin(rebound);
    assert(!capture.waiting(rebound - 1));
    assert(capture.waiting(rebound + 4999 * ms));
    assert(!capture.full_timing_probe_safe());
    capture.confirm();
    assert(!capture.waiting(rebound + 100 * ms));
    assert(!capture.full_timing_probe_safe());

    // Closing or idling the producer must not preserve its old deadline.
    capture.begin(rebound + 1000 * ms);
    capture.reset();
    assert(!capture.waiting(rebound + 1001 * ms));
    assert(!capture.full_timing_probe_safe());
    capture.begin(rebound + 2000 * ms);
    assert(capture.waiting(rebound + 2001 * ms));

    std::puts("capture startup transitions passed");
}
