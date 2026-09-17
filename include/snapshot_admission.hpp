#pragma once

#include <cstdint>

namespace onekvm {

enum class SnapshotAdmission : uint8_t {
    allow,
    no_signal,
    signal_unavailable,
    out_of_range,
};

constexpr SnapshotAdmission snapshot_admission(
    int cached_signal, uint64_t last_signal_probe_ns, bool out_of_range)
{
    if (out_of_range)
        return SnapshotAdmission::out_of_range;
    if (cached_signal > 0)
        return SnapshotAdmission::allow;
    if (cached_signal == 0)
        return SnapshotAdmission::no_signal;
    /* A new source starts unknown before its first signal sample. Preserve
       that grace so snapshot may wait for the first live VPSS completion.
       Once a probe has failed, waiting cannot manufacture a producer. */
    return last_signal_probe_ns == 0
        ? SnapshotAdmission::allow
        : SnapshotAdmission::signal_unavailable;
}

} // namespace onekvm
