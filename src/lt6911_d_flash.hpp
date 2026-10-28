#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>

namespace onekvm::video_backend {

// Sipeed D E0/E1 controller. Metadata lives in the 0x8000 sector;
// these helpers never address MCU program pages or use the UXC bank map.
struct DFlashIO {
    int (*write)(uint8_t, uint8_t, uint8_t);
    int (*read)(uint8_t, uint8_t, uint8_t *, size_t);
    int (*block)(uint8_t, uint8_t, const uint8_t *, size_t);
    void (*wait)(unsigned);
};
struct DFlashStep { uint8_t reg, value; unsigned delay_ms; };
template<size_t N>
inline int d_flash_steps(const DFlashIO &io, const DFlashStep (&steps)[N]) {
    for (const auto &s : steps) {
        int rc = io.write(0xe0, s.reg, s.value);
        if (rc) return rc;
        if (s.delay_ms) io.wait(s.delay_ms);
    }
    return 0;
}
inline int d_flash_end(const DFlashIO &io, int primary) {
    constexpr DFlashStep steps[]{{0x5a, 8, 1}, {0x5a, 0, 0}, {0x58, 0, 0}, {0xee, 0, 0}};
    for (const auto &s : steps) {
        int rc = io.write(0xe0, s.reg, s.value);
        if (!primary && rc) primary = rc;
        if (s.delay_ms) io.wait(s.delay_ms);
    }
    return primary;
}
inline int d_flash_begin(const DFlashIO &io) {
    constexpr DFlashStep steps[]{{0xee, 1, 0}, {0x5e, 0xdf, 0}, {0x58, 0, 0},
        {0x59, 0x50, 0}, {0x5a, 0x10, 1}, {0x5a, 0, 0}, {0x58, 0x21, 0}};
    return d_flash_steps(io, steps);
}
inline int d_flash_fifo(const DFlashIO &io) {
    int rc = io.write(0xe1, 3, 0xbf);
    if (rc) return rc;
    io.wait(1);
    if ((rc = io.write(0xe1, 3, 0xff))) return rc;
    constexpr DFlashStep steps[]{{0x5a, 4, 1}, {0x5a, 0, 0}};
    return d_flash_steps(io, steps);
}
inline int d_flash_address(const DFlashIO &io, unsigned address) {
    const DFlashStep steps[]{{0x5b, uint8_t(address >> 16), 0},
        {0x5c, uint8_t(address >> 8), 0}, {0x5d, uint8_t(address), 0}};
    return d_flash_steps(io, steps);
}
inline int d_flash_read(const DFlashIO &io, unsigned address, uint8_t *data, size_t size) {
    if (!data || !size || size % 32 || address % 32 ||
        address < 0x8000 || address >= 0x9000 || size > 0x9000 - address)
        return -EINVAL;
    int rc = d_flash_begin(io);
    constexpr DFlashStep pre[]{{0x5e, 0x5f, 0}, {0x5a, 0x20, 1}, {0x5a, 0, 0}};
    constexpr DFlashStep read[]{{0x5a, 0x10, 1}, {0x5a, 0, 0}};
    for (size_t offset = 0; !rc && offset < size; offset += 32) {
        if ((rc = d_flash_fifo(io)) || (rc = d_flash_steps(io, pre)) ||
            (rc = d_flash_address(io, address + offset)) ||
            (rc = d_flash_steps(io, read)) ||
            (rc = io.read(0xe0, 0x5f, data + offset, 32))) break;
        io.wait(10);
    }
    return d_flash_end(io, rc);
}

inline int d_flash_write_edid(const DFlashIO &io, const uint8_t *edid, size_t size) {
    if (!edid || size != 256) return -EINVAL;
    // Preserve all metadata in the addressed 4 KiB sector, including board text.
    std::array<uint8_t, 4096> desired{}, verify{};
    int rc = d_flash_read(io, 0x8000, desired.data(), desired.size());
    if (rc) return rc;
    rc = d_flash_read(io, 0x8000, verify.data(), verify.size());
    if (rc) return rc;
    if (desired != verify) return -EIO;
    if (!std::memcmp(desired.data(), edid, size)) return 0;
    std::memcpy(desired.data(), edid, size);
    constexpr DFlashStep reset[]{{0x5a, 4, 1}, {0x5a, 0, 0}};
    constexpr DFlashStep erase[]{{0x5a, 1, 1}, {0x5a, 0, 600}};
    constexpr DFlashStep pre[]{{0x5e, 0xdf, 0}, {0x5a, 0x20, 1},
        {0x5a, 0, 0}, {0x58, 0x21, 0}};
    constexpr DFlashStep program[]{{0x5a, 0x10, 5}, {0x5a, 0, 10}};
    rc = d_flash_begin(io);
    if (!rc) rc = d_flash_steps(io, reset);
    if (!rc) rc = d_flash_address(io, 0x8000);
    if (!rc) rc = d_flash_steps(io, erase);
    for (unsigned offset = 0; !rc && offset < desired.size(); offset += 32) {
        bool erased = true;
        for (unsigned i = 0; i < 32; ++i) erased &= desired[offset + i] == 0xff;
        if (erased) continue;
        if ((rc = d_flash_fifo(io)) || (rc = d_flash_steps(io, pre)) ||
            (rc = io.block(0xe0, 0x59, desired.data() + offset, 32)) ||
            (rc = d_flash_address(io, 0x8000 + offset)) ||
            (rc = d_flash_steps(io, program))) break;
    }
    rc = d_flash_end(io, rc);
    if (rc) return rc;
    rc = d_flash_read(io, 0x8000, verify.data(), verify.size());
    return rc ? rc : desired == verify ? 0 : -EIO;
}
} // namespace onekvm::video_backend
