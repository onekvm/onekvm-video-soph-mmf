#pragma once
#include "lt6911_shadow.hpp"

namespace onekvm::video_backend {
using UxcShadowIO = ShadowIO;
inline int uxc_shadow_apply(const UxcShadowIO &io, const uint8_t *desired, uint8_t *actual) {
    if (!actual || (desired && !valid_shadow_edid(desired))) return -EINVAL;
    const auto rd = [&](uint8_t bank, uint8_t reg, uint8_t &v) { return io.read(io.context, bank, reg, &v); };
    const auto wr = [&](uint8_t bank, uint8_t reg, uint8_t v) { return io.write(io.context, bank, reg, v); };
    const auto read_payload = [&](uint8_t *data) {
        for (unsigned i = 0; i < 256; ++i)
            if (wr(0xb0, 0x0a, i) || rd(0xb0, 0xb0, data[i])) return -EIO;
        return 0;
    };
    const auto write_payload = [&](const uint8_t *data) {
        for (unsigned i = 0; i < 256; ++i)
            if (wr(0xb0, 0x0a, i) || wr(0xb0, 0xb0, data[i])) return -EIO;
        return 0;
    };
    uint8_t gate = 0, watchdog = 0, index = 0, control = 0, hpd = 0;
    uint8_t before[256]{}, verify[256]{};
    bool gate_saved = false, watchdog_saved = false, state_saved = false;
    bool dirty = false, backed_up = false;
    int result = [&]() {
        if (rd(0x80, 0xee, gate)) return -EIO;
        if (gate != 0) return -EBUSY;
        gate_saved = true;
        if (wr(0x80, 0xee, 1)) return -EIO;
        if (rd(0x80, 0x10, watchdog)) return -EIO;
        watchdog_saved = true;
        if (wr(0x80, 0x10, 0)) return -EIO;
        uint8_t id[3]{};
        for (unsigned i = 0; i < 3; ++i) if (rd(0x81, i, id[i])) return -EIO;
        if (id[0] != 0x17 || id[1] != 4 || id[2] != 0x83) return -ENODEV;
        if (rd(0xb0, 0x0a, index) || rd(0xb0, 0x0b, control) || rd(0xb0, 0x00, hpd)) return -EIO;
        state_saved = true;
        if (!(control & 4)) return -EOPNOTSUPP;
        if (wr(0xb0, 0x00, hpd & ~1)) return -EIO;
        io.wait_us(200000);
        // Read and verify only with HPD low: source DDC traffic shares the window.
        if (read_payload(before) || read_payload(verify) || std::memcmp(before, verify, 256) ||
            !valid_shadow_edid(before)) return -EIO;
        backed_up = true;
        if (desired) {
            if (wr(0xb0, 0x0b, control & ~4)) return -EIO;
            dirty = true;
            if (write_payload(desired) || read_payload(verify) || std::memcmp(desired, verify, 256)) return -EIO;
            if (wr(0xb0, 0x0b, control) || wr(0x80, 0xee, 0)) return -EIO;
            io.wait_us(20000);
            if (wr(0x80, 0xee, 1) || read_payload(verify) || std::memcmp(desired, verify, 256)) return -EIO;
        }
        std::memcpy(actual, desired ? desired : before, 256);
        return 0;
    }();
    const auto restore = [&](bool keep_watchdog_disabled, bool reconnect) {
        int error = 0;
        // Retry entering access after a failed transfer, before restoring registers.
        if (gate_saved && wr(0x80, 0xee, 1)) error = -EIO;
        if (state_saved) {
            if (wr(0xb0, 0x0b, control)) error = -EIO;
            if (wr(0xb0, 0x0a, index)) error = -EIO;
            if (wr(0xb0, 0x00, reconnect ? hpd : hpd & ~1)) error = -EIO;
        }
        if (watchdog_saved && wr(0x80, 0x10, keep_watchdog_disabled ? 0 : watchdog)) error = -EIO;
        if (gate_saved && wr(0x80, 0xee, gate)) error = -EIO;
        return error;
    };
    if (!result) result = restore(false, true);
    if (result) {
        bool recovered = true;
        if (dirty && backed_up) {
            recovered = wr(0x80, 0xee, 1) == 0 && wr(0x80, 0x10, 0) == 0 && wr(0xb0, 0x00, hpd & ~1) == 0;
            if (recovered) {
                io.wait_us(200000);
                recovered = wr(0xb0, 0x0b, control & ~4) == 0 && write_payload(before) == 0 &&
                    read_payload(verify) == 0 && std::memcmp(before, verify, 256) == 0;
            }
        }
        const int cleanup = restore(!recovered, recovered);
        if (!recovered || cleanup) return -EUCLEAN;
    }
    return result;
}
}
