#include "snapshot_admission.hpp"

#include <cassert>

int main()
{
    using onekvm::SnapshotAdmission;
    assert(onekvm::snapshot_admission(1, 10, false) == SnapshotAdmission::allow);
    assert(onekvm::snapshot_admission(0, 10, false) == SnapshotAdmission::no_signal);
    assert(onekvm::snapshot_admission(-1, 0, false) == SnapshotAdmission::allow);
    assert(onekvm::snapshot_admission(-1, 10, false) ==
           SnapshotAdmission::signal_unavailable);
    assert(onekvm::snapshot_admission(1, 10, true) ==
           SnapshotAdmission::out_of_range);
    return 0;
}
