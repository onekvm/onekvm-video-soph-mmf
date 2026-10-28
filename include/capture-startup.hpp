#pragma once

#include <cstdint>

namespace onekvm {

// Owned by Source under its mutex. Timing caches belong to the previous
// HDMI episode; only the new producer's deadline bounds its startup trial.
class CaptureStartup {
public:
    // CSI locking may outlast the 1.5 s grace used for an already live VENC.
    static constexpr uint64_t window_ns = 5000000000ull;

    void begin(uint64_t now) {
        started_ns_ = now;
        phase_ = Phase::Starting;
    }
    void confirm() { phase_ = Phase::Live; }
    void park() { phase_ = Phase::Placeholder; }
    void reset() { phase_ = Phase::Idle; }

    bool waiting(uint64_t now) const {
        return phase_ == Phase::Starting && now >= started_ns_ &&
            now - started_ns_ <= window_ns;
    }
    bool full_timing_probe_safe() const {
        return phase_ == Phase::Placeholder;
    }
    bool placeholder() const { return phase_ == Phase::Placeholder; }

private:
    enum class Phase { Idle, Starting, Live, Placeholder };
    Phase phase_ = Phase::Idle;
    uint64_t started_ns_ = 0;
};

} // namespace onekvm
