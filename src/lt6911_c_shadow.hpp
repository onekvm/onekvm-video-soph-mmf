#pragma once
#include "lt6911_shadow.hpp"

namespace onekvm::video_backend {
struct CShadowControl { uint8_t hpd, control; };

// C090 is a write-only streaming port in this verified C firmware. The caller
// supplies a known previous payload; neither the FIFO nor Flash is a RAM readback.
// prepare journals the previous payload/control in /run before changing hardware.
inline int c_shadow_apply(const ShadowIO &io, const uint8_t *before, const uint8_t *desired,
                          int (*prepare)(void *, const CShadowControl &) = nullptr,
                          const CShadowControl *recovery = nullptr) {
    if (!valid_shadow_edid(before) || !valid_shadow_edid(desired)) return -EINVAL;
    const auto rd = [&](uint8_t bank, uint8_t reg, uint8_t &v) { return io.read(io.context, bank, reg, &v); };
    const auto wr = [&](uint8_t bank, uint8_t reg, uint8_t v) { return io.write(io.context, bank, reg, v); };
    const auto payload = [&](const uint8_t *data) {
        if (wr(0xc0, 0x8e, 3)) return -EIO;
        for (unsigned i = 0; i < 256; ++i) if (wr(0xc0, 0x90, data[i])) return -EIO;
        return wr(0xc0, 0x8e, 2) ? -EIO : 0;
    };
    CShadowControl saved{};
    bool state_saved = false, dirty = false;
    int result = [&]() {
        // 80EE is not reliably readable on C; exit always uses canonical 00.
        if (wr(0x80, 0xee, 1)) return -EIO;
        uint8_t id0 = 0, id1 = 0;
        if (rd(0xa0, 0, id0) || rd(0xa0, 1, id1)) return -EIO;
        if (id0 != 0x16 || id1 != 5) return -ENODEV;
        if (recovery) saved = *recovery;
        else if (rd(0xc0, 6, saved.hpd) || rd(0xc0, 0x8e, saved.control)) return -EIO;
        if (saved.control != 2) return -EOPNOTSUPP;
        state_saved = true;
        if (prepare && prepare(io.context, saved)) return -EIO;
        if (wr(0xc0, 6, saved.hpd & ~8)) return -EIO;
        io.wait_us(200000);
        dirty = true; // Even a failed first transfer can have reached hardware.
        if (payload(desired)) return -EIO;
        if (wr(0xc0, 0x8e, saved.control) || wr(0xc0, 6, saved.hpd) || wr(0x80, 0xee, 0)) return -EIO;
        return 0;
    }();
    if (!result) return 0;
    bool restored = !dirty;
    if (dirty) {
        for (unsigned attempt = 0; attempt < 3 && !restored; ++attempt) {
            if (wr(0x80, 0xee, 1) || wr(0xc0, 6, saved.hpd & ~8)) continue;
            io.wait_us(200000);
            restored = payload(before) == 0;
        }
    }
    // Attempt every cleanup step, even when a preceding transfer failed.
    bool cleanup_failed = wr(0x80, 0xee, 1) != 0;
    if (state_saved) {
        if (wr(0xc0, 0x8e, saved.control)) cleanup_failed = true;
        if (wr(0xc0, 6, restored ? saved.hpd : saved.hpd & ~8)) cleanup_failed = true;
    }
    if (wr(0x80, 0xee, 0)) cleanup_failed = true;
    return !restored || cleanup_failed ? -EUCLEAN : result;
}
}
