#!/usr/bin/env python3
"""Native fault tests extracting the production sensor control functions."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = Path(os.environ.get('ONEKVM_LT6911_SENSOR', root / 'src/lt6911_sensor_ctl.c')).read_text()
names = '''onekvm_lt6911_cached_chip onekvm_lt6911_cache_chip
supported_active_size normalize_half_or_full_width lt6911_i2c_read_be16
sensor_read sensor_write identify_chip read_active_pair read_family_active
lt6911_get_input_timing_silent lt6911_get_input_timing_impl
lt6911_kick_hdmi lt6911_start_csi_impl'''.split()
parts = []
for name in names:
    match = re.search(r'^(?:static )?(?:inline )?[^;{}\n]+\b' + name + r'\([^;]*?\n\{', source, re.M)
    assert match, name
    end = source.index('\n}\n', match.end()) + 3
    parts.append((match.start(), source[match.start():end]))
start = source.index('struct onekvm_lt6911_input_timing {')
end = source.index('\n};', start) + 3
parts.append((start, source[start:end]))
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lt6911_receiver.h"
typedef int VI_PIPE;
#define CVI_SUCCESS 0
#define CVI_FAILURE -1
static int g_chip, g_i2c_lock, calls, fail_at, writes, wrong_read, mixed;
static uint8_t regs[65536];
static int valid_pipe(int pipe) { return pipe == 0; }
static void configure_pinmux_once(void) {}
static int lt6911_i2c_init(int pipe) { return valid_pipe(pipe) ? 0 : -1; }
static void pthread_mutex_lock(int *p) { assert(!*p); *p = 1; }
static void pthread_mutex_unlock(int *p) { assert(*p); *p = 0; }
static int fault(void) { return ++calls == fail_at; }
static int lt6911_i2c_read(int pipe, int addr) {
    (void)pipe;
    assert(g_i2c_lock);
    if (fault()) return -EIO;
    if (mixed && addr == 0xe08d && calls > 4) return regs[addr] ^ 1;
    if (g_chip == ONEKVM_LT6911_D && (addr >> 8) != 0xe0) wrong_read++;
    if (g_chip == ONEKVM_LT6911_C && (addr >> 8) != 0xc2 && (addr >> 8) != 0xd2) wrong_read++;
    if (g_chip == ONEKVM_LT6911_UXC && (addr >> 8) != 0x85 && (addr >> 8) != 0x86) wrong_read++;
    return regs[addr];
}
static int lt6911_i2c_write(int pipe, int addr, int value) {
    (void)pipe;
    assert(g_i2c_lock);
    writes++;
    if (fault()) return -EIO;
    regs[addr] = value;
    return 0;
}
'''
tests = r'''
static void reset(int chip) {
    memset(regs, 0, sizeof(regs));
    g_chip = chip;
    calls = fail_at = writes = wrong_read = mixed = 0;
    assert(!g_i2c_lock);
}
static void be16(int addr, int value) { regs[addr] = value >> 8; regs[addr+1] = value; }
int main(void) {
    struct onekvm_lt6911_input_timing timing;
    /* Stale plausible dimensions in other maps must never win selection. */
    for (int chip = ONEKVM_LT6911_UXC; chip <= ONEKVM_LT6911_D; ++chip) {
        reset(chip);
        be16(0xd28b, 1280); be16(0xd296, 1440);
        be16(0x85ea, 2560); be16(0x85f0, 1440);
        be16(0xe08c, 1280); be16(0xe08e, 1440);
        if (chip == ONEKVM_LT6911_C) { be16(0xd28b,960); be16(0xd296,1080); be16(0xc238,1920); be16(0xc206,1080); }
        if (chip == ONEKVM_LT6911_UXC) { be16(0x85ea,1920); be16(0x85f0,1080); }
        if (chip == ONEKVM_LT6911_D) { be16(0xe08c,960); be16(0xe08e,1080); }
        assert(!lt6911_get_input_timing_silent(0,&timing));
        assert(timing.hdmi_width == 1920 && timing.hdmi_height == 1080 && !writes && !wrong_read);
        calls = 0;
        assert(!lt6911_get_input_timing_impl(0,&timing,1));
        assert(timing.hdmi_width == 1920 && timing.hdmi_height == 1080 && !wrong_read);
        assert(regs[0x80ee] == 0 && regs[0xe0ee] == 0);
        unsigned transfers = calls;
        for (unsigned n = 1; n <= transfers; ++n) {
            calls = 0; fail_at = n; memset(&timing,0x55,sizeof(timing));
            assert(lt6911_get_input_timing_impl(0,&timing,1) != 0);
            assert(!timing.hdmi_width && !timing.hdmi_height && !timing.csi_width);
            if (n < transfers) assert(!regs[0x80ee]);
        }
    }
    reset(ONEKVM_LT6911_D); be16(0xe08c,360); be16(0xe08e,400);
    assert(!lt6911_get_input_timing_impl(0,&timing,1));
    assert(timing.hdmi_width == 720 && timing.hdmi_height == 400 && !writes);
    assert(!lt6911_kick_hdmi() && !lt6911_start_csi_impl(1) && !writes);
    be16(0xe08e,0);
    assert(!lt6911_get_input_timing_silent(0,&timing) && !timing.hdmi_width && !timing.hdmi_height);
    be16(0xe08e,400); calls=0; mixed=1;
    assert(lt6911_get_input_timing_silent(0,&timing) != 0 && !timing.hdmi_width);
    reset(ONEKVM_LT6911_UNKNOWN);
    assert(lt6911_get_input_timing_silent(0,&timing) != 0 && !calls);
    /* Actual IDs, not D firmware/date bytes. Unknown and all failures close gates. */
    for (int chip = ONEKVM_LT6911_UXC; chip <= ONEKVM_LT6911_D; ++chip) {
        reset(0);
        if (chip == ONEKVM_LT6911_UXC) { regs[0x8100]=0x17; regs[0x8101]=4; }
        if (chip == ONEKVM_LT6911_C) { regs[0xa000]=0x16; regs[0xa001]=5; }
        if (chip == ONEKVM_LT6911_D) { regs[0xe100]=0x21; regs[0xe101]=2; }
        regs[0xe080]=0x99; /* firmware may vary */
        pthread_mutex_lock(&g_i2c_lock);
        assert(!identify_chip(0) && g_chip == chip);
        pthread_mutex_unlock(&g_i2c_lock);
        unsigned transfers=calls;
        for(unsigned n=1;n<=transfers;++n) {
            g_chip=0; calls=0; fail_at=n;
            pthread_mutex_lock(&g_i2c_lock);
            assert(identify_chip(0) != 0 && g_chip == 0);
            pthread_mutex_unlock(&g_i2c_lock);
        }
        fail_at=0; regs[0x80ee]=regs[0xe0ee]=0;
    }
    reset(0); regs[0xe080]=0x25; regs[0xe081]=0x10; regs[0xe082]=0x23; regs[0xe083]=1;
    pthread_mutex_lock(&g_i2c_lock);
    assert(identify_chip(0) != 0 && !g_chip);
    pthread_mutex_unlock(&g_i2c_lock);
    puts("MMF C/UXC/D: silicon IDs, isolated maps, coherent samples, no D control writes and fault cleanup passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mmf-lt6911-') as temp:
    work = Path(temp)
    (work / 'test.c').write_text(prefix + '\n'.join(text for _, text in sorted(parts)) + tests)
    subprocess.run(shlex.split(os.environ.get('CC','cc')) + ['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-sign-compare','-fsanitize=address,undefined','-g','-I'+str(root/'src'),str(work/'test.c'),'-o',str(work/'test')],check=True)
    subprocess.run([str(work/'test')],check=True)
