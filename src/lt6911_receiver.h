#ifndef ONEKVM_LT6911_RECEIVER_H
#define ONEKVM_LT6911_RECEIVER_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

/* Values shared with Lt6911Chip. Discovery runs with capture stopped. */
enum onekvm_lt6911_chip {
    ONEKVM_LT6911_UNKNOWN, ONEKVM_LT6911_UXC, ONEKVM_LT6911_C, ONEKVM_LT6911_D
};
struct onekvm_lt6911_io {
    void *context;
    int (*read)(void *, uint16_t, uint8_t *, size_t);
    int (*write)(void *, uint16_t, uint8_t);
    /* Optional bus address probe: 1 present, 0 absent, negative unknown. */
    int (*present)(void *, uint8_t);
};

/* One-byte read without changing the descriptor's selected address, as the
 * factory `i2cdetect -r` hardware detection does. */
static inline int onekvm_lt6911_bus_present(int fd, uint8_t address)
{
    uint8_t value;
    struct i2c_msg msg = { address, I2C_M_RD, 1, &value };
    struct i2c_rdwr_ioctl_data transfer = { &msg, 1 };
    if (fd < 0) return -EBADF;
    return ioctl(fd, I2C_RDWR, &transfer) == 1;
}

/* 0 found D, 1 another chip, negative I2C failure. The E0 window is closed. */
static inline int onekvm_lt6911_probe_d(const struct onekvm_lt6911_io *io)
{
    uint8_t id[2] = {0};
    int ret = io->write(io->context, 0xe0ee, 1), cleanup;
    if (!ret) ret = io->read(io->context, 0xe100, id, 2);
    cleanup = io->write(io->context, 0xe0ee, 0);
    if (ret || cleanup) return ret ? ret : cleanup;
    return id[0] == 0x21 && id[1] == 0x02 ? 0 : 1;
}

static inline int onekvm_lt6911_identify(const struct onekvm_lt6911_io *io, int *chip)
{
    uint8_t id[2] = {0};
    int ret, cleanup, d_first = 0;
    *chip = ONEKVM_LT6911_UNKNOWN;
    /* Factory detection maps 0x2b without 0x2c/0x44 to D and never opens the
     * C/UXC 80EE window there. Try D first on that map; the ID still decides. */
    if (io->present)
        d_first = io->present(io->context, 0x2c) == 0 &&
                  io->present(io->context, 0x44) == 0;
    if (d_first) {
        ret = onekvm_lt6911_probe_d(io);
        if (ret <= 0) {
            if (!ret) *chip = ONEKVM_LT6911_D;
            return ret;
        }
    }
    ret = io->write(io->context, 0x80ee, 1);
    if (!ret) ret = io->read(io->context, 0xa000, id, 2);
    if (!ret && id[0] == 0x16 && id[1] == 0x05) {
        *chip = ONEKVM_LT6911_C;
    } else if (!ret) {
        ret = io->read(io->context, 0x8100, id, 2);
        if (!ret && id[0] == 0x17 && id[1] == 0x04)
            *chip = ONEKVM_LT6911_UXC;
    }
    cleanup = io->write(io->context, 0x80ee, 0);
    if (ret || cleanup) {
        *chip = ONEKVM_LT6911_UNKNOWN;
        return ret ? ret : cleanup;
    }
    if (*chip != ONEKVM_LT6911_UNKNOWN || d_first) return *chip ? 0 : -ENODEV;

    /* E080..83 are firmware bytes, not the D silicon ID. */
    ret = onekvm_lt6911_probe_d(io);
    if (ret < 0) return ret;
    if (ret) return -ENODEV;
    *chip = ONEKVM_LT6911_D;
    return 0;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Cache-only access; callers serialize setters with the shared I2C lock. */
int onekvm_lt6911_cached_chip(void);
void onekvm_lt6911_cache_chip(int chip);
#ifdef __cplusplus
}
#endif
#endif
