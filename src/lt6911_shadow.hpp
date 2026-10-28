#pragma once
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace onekvm::video_backend {
// Caller has stopped VI and owns the LT6911 I2C bus.
struct ShadowIO {
    void *context;
    int (*read)(void *, uint8_t, uint8_t, uint8_t *);
    int (*write)(void *, uint8_t, uint8_t, uint8_t);
    void (*wait_us)(unsigned);
};
inline bool valid_shadow_edid(const uint8_t *edid) {
    static const uint8_t header[] = {0,255,255,255,255,255,255,0};
    if (!edid || std::memcmp(edid, header, 8) || edid[126] != 1) return false;
    for (unsigned block = 0; block < 2; ++block) {
        uint8_t sum = 0;
        for (unsigned i = 0; i < 128; ++i) sum += edid[block * 128 + i];
        if (sum) return false;
    }
    return true;
}
}
