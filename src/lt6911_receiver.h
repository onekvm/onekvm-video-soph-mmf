#ifndef ONEKVM_LT6911_RECEIVER_H
#define ONEKVM_LT6911_RECEIVER_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Values shared with Lt6911Chip. Discovery runs with capture stopped. */
enum onekvm_lt6911_chip {
    ONEKVM_LT6911_UNKNOWN, ONEKVM_LT6911_UXC, ONEKVM_LT6911_C, ONEKVM_LT6911_D
};
struct onekvm_lt6911_io {
    void *context;
    int (*read)(void *, uint16_t, uint8_t *, size_t);
    int (*write)(void *, uint16_t, uint8_t);
};

static inline int onekvm_lt6911_identify(const struct onekvm_lt6911_io *io, int *chip)
{
    uint8_t id[2] = {0};
    int ret, cleanup;
    *chip = ONEKVM_LT6911_UNKNOWN;
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
    if (*chip != ONEKVM_LT6911_UNKNOWN) return 0;

    /* E080..83 are firmware bytes, not the D silicon ID. */
    ret = io->write(io->context, 0xe0ee, 1);
    if (!ret) ret = io->read(io->context, 0xe100, id, 2);
    cleanup = io->write(io->context, 0xe0ee, 0);
    if (ret || cleanup) return ret ? ret : cleanup;
    if (id[0] != 0x21 || id[1] != 0x02) return -ENODEV;
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
