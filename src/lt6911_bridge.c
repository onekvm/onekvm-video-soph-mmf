#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "lt6911_receiver.h"

/* LT6911 bridge control over i2c-dev. Plain C: both the cvi_mpi sensor glue
 * and the V4L2 backend use it. Functions return 0 or -1. */
#define LT6911_MAX_PIPE 1
#define LT6911_ERR(...) fprintf(stderr, "lt6911: " __VA_ARGS__)

void onekvm_lt6911_get_active_size(uint32_t *width, uint32_t *height);

#define LT6911_I2C_DEV 4
#define LT6911_I2C_BANK_ADDR 0xff
#define LT6911_CHIP_ID_ADDR_H 0xa000
#define LT6911_CHIP_ID_ADDR_L 0xa001
#define LT6911_CHIP_ID 0x1605

/* Linux i2c-dev takes a 7-bit address.  The vendor source used 0x56, which is
 * the wire-format address including the R/W bit. */
uint8_t lt6911_i2c_addr = 0x2b;
const uint32_t lt6911_addr_byte = 1;
const uint32_t lt6911_data_byte = 1;

static int g_fd[LT6911_MAX_PIPE] = {-1};
static int g_pinmux_configured;
static int g_chip;

int onekvm_lt6911_cached_chip(void)
{
    return __atomic_load_n(&g_chip, __ATOMIC_ACQUIRE);
}

void onekvm_lt6911_cache_chip(int chip)
{
    __atomic_store_n(&g_chip, chip, __ATOMIC_RELEASE);
}
static pthread_mutex_t g_i2c_lock = PTHREAD_MUTEX_INITIALIZER;

void onekvm_lt6911_i2c_lock(void)
{
	pthread_mutex_lock(&g_i2c_lock);
}

void onekvm_lt6911_i2c_unlock(void)
{
	pthread_mutex_unlock(&g_i2c_lock);
}

static int valid_pipe(int pipe)
{
	return pipe >= 0 && pipe < LT6911_MAX_PIPE;
}

static int supported_active_size(uint32_t width, uint32_t height)
{
	return width >= 320 && height >= 200 && width <= 4096 && height <= 2160 &&
		(width & 1u) == 0 && (height & 1u) == 0;
}

/* LT6911C HDMI/D counters are half-width on some firmware and full-width on
   others. Prefer the doubled value when it is a supported mode so 960→1920
   still works, but do not invent 3840 when the register already holds 1920. */
static void normalize_half_or_full_width(uint32_t *width, uint32_t height)
{
	if (width == NULL)
		return;
	if ((*width == 960 && height == 1080) ||
	    (*width == 1280 && height == 1440) ||
	    (*width == 1920 && height == 2160) ||
	    (*width == 640 && height == 720) ||
	    (*width == 400 && height == 600))
		*width *= 2;
}


static void configure_pinmux_once(void)
{
	static const char *const commands[] = {
		"devmem 0x0300116C 32 0x3", "devmem 0x03001170 32 0x3",
		"devmem 0x03001174 32 0x3", "devmem 0x03001178 32 0x3",
		"devmem 0x0300117C 32 0x3", "devmem 0x03001180 32 0x3",
		"devmem 0x03001184 32 0x3", "devmem 0x03001188 32 0x3",
		"devmem 0x0300118C 32 0x3", "devmem 0x03001190 32 0x3",
	};
	size_t i;

	if (__atomic_exchange_n(&g_pinmux_configured, 1, __ATOMIC_ACQ_REL))
		return;
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
		if (system(commands[i]) != 0)
			LT6911_ERR("LT6911 pinmux command failed: %s\n", commands[i]);
	}
}

int lt6911_i2c_init(int pipe)
{
	char path[16];
	int fd;

	if (!valid_pipe(pipe)) {
		LT6911_ERR("invalid VI pipe %d\n", pipe);
		return -1;
	}
	if (g_fd[pipe] >= 0)
		return 0;

	snprintf(path, sizeof(path), "/dev/i2c-%u", LT6911_I2C_DEV);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		LT6911_ERR("open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	if (ioctl(fd, I2C_SLAVE_FORCE, lt6911_i2c_addr) < 0) {
		LT6911_ERR("select LT6911 at 0x%02x failed: %s\n",
			lt6911_i2c_addr, strerror(errno));
		close(fd);
		return -1;
	}
	g_fd[pipe] = fd;
	return 0;
}

static int lt6911_i2c_present(int pipe, uint8_t address)
{
	if (!valid_pipe(pipe)) return -EINVAL;
	return onekvm_lt6911_bus_present(g_fd[pipe], address);
}

int lt6911_i2c_exit(int pipe)
{
	if (!valid_pipe(pipe))
		return -1;
	if (g_fd[pipe] >= 0) {
		close(g_fd[pipe]);
		g_fd[pipe] = -1;
	}
	return 0;
}

int lt6911_read_register(int pipe, int addr)
{
	uint8_t reg = (uint8_t)addr;
	uint8_t data;
	ssize_t count;

	if (!valid_pipe(pipe) || g_fd[pipe] < 0)
		return -1;
	count = write(g_fd[pipe], &reg, sizeof(reg));
	if (count != (ssize_t)sizeof(reg)) {
		LT6911_ERR("LT6911 register select 0x%02x failed: %s\n",
			reg, count < 0 ? strerror(errno) : "short write");
		return -1;
	}
	count = read(g_fd[pipe], &data, sizeof(data));
	if (count != (ssize_t)sizeof(data)) {
		LT6911_ERR("LT6911 register read 0x%02x failed: %s\n",
			reg, count < 0 ? strerror(errno) : "short read");
		return -1;
	}
	return data;
}

int lt6911_write_register(int pipe, int addr, int data)
{
	uint8_t transaction[2] = {(uint8_t)addr, (uint8_t)data};
	ssize_t count;

	if (!valid_pipe(pipe) || g_fd[pipe] < 0)
		return -1;
	count = write(g_fd[pipe], transaction, sizeof(transaction));
	if (count != (ssize_t)sizeof(transaction)) {
		LT6911_ERR("LT6911 register write 0x%02x failed: %s\n",
			transaction[0], count < 0 ? strerror(errno) : "short write");
		return -1;
	}
	return 0;
}

static int lt6911_i2c_read(int pipe, int reg_addr)
{
	if (lt6911_write_register(pipe, LT6911_I2C_BANK_ADDR,
		(uint8_t)(reg_addr >> 8)) != 0)
		return -1;
	return lt6911_read_register(pipe, reg_addr & 0xff);
}

static int lt6911_i2c_write(int pipe, int reg_addr, int data)
{
	if (lt6911_write_register(pipe, LT6911_I2C_BANK_ADDR,
		(uint8_t)(reg_addr >> 8)) != 0)
		return -1;
	return lt6911_write_register(pipe, reg_addr & 0xff, data);
}

static int lt6911_i2c_read_be16(int pipe, int reg_addr, uint32_t *value)
{
	int high = lt6911_i2c_read(pipe, reg_addr);
	int low = lt6911_i2c_read(pipe, reg_addr + 1);

	if (high < 0 || low < 0)
		return -1;
	*value = ((uint32_t)high << 8) | (uint32_t)low;
	return 0;
}

int lt6911_read(int pipe, int addr)
{
	int data;
	int cleanup;

	pthread_mutex_lock(&g_i2c_lock);
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != 0)
		goto error;
	data = lt6911_i2c_read(pipe, addr);
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	if (data < 0 || cleanup != 0)
		goto error;
	pthread_mutex_unlock(&g_i2c_lock);
	return data;

error:
	pthread_mutex_unlock(&g_i2c_lock);
	return -1;
}

int lt6911_write(int pipe, int addr, int data)
{
	int result;
	int cleanup;

	pthread_mutex_lock(&g_i2c_lock);
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != 0) {
		pthread_mutex_unlock(&g_i2c_lock);
		return -1;
	}
	result = lt6911_i2c_write(pipe, addr, data);
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return result == 0 && cleanup == 0
		? 0 : -1;
}

struct onekvm_lt6911_input_timing {
	uint32_t csi_width;
	uint32_t csi_height;
	uint32_t hdmi_width;
	uint32_t hdmi_height;
	uint32_t uxc_rx_width;
	uint32_t uxc_rx_height;
	uint32_t uxc_rx_signal;
};

/* Caller owns g_i2c_lock. Never select a family from stale geometry. */
static int sensor_read(void *context, uint16_t addr, uint8_t *data, size_t size)
{
    int pipe = *(int *)context;
    for (size_t i = 0; i < size; ++i) {
        int value = lt6911_i2c_read(pipe, addr + i);
        if (value < 0) return -EIO;
        data[i] = (uint8_t)value;
    }
    return 0;
}

static int sensor_write(void *context, uint16_t addr, uint8_t value)
{
    return lt6911_i2c_write(*(int *)context, addr, value) == 0 ? 0 : -EIO;
}

static int sensor_present(void *context, uint8_t address)
{
    return lt6911_i2c_present(*(int *)context, address);
}

static int identify_chip(int pipe)
{
    struct onekvm_lt6911_io io = { &pipe, sensor_read, sensor_write, sensor_present };
    int chip = onekvm_lt6911_cached_chip();
    if (chip != ONEKVM_LT6911_UNKNOWN) return 0;
    if (onekvm_lt6911_identify(&io, &chip)) return -1;
    onekvm_lt6911_cache_chip(chip);
    return 0;
}

/* Recheck both dimensions so a failed/torn sample never replaces geometry. */
static int read_active_pair(int pipe, int width_reg, int height_reg,
                           uint32_t *width, uint32_t *height)
{
    uint32_t w, h, verify_w, verify_h;
    if (lt6911_i2c_read_be16(pipe, height_reg, &h) ||
        lt6911_i2c_read_be16(pipe, width_reg, &w) ||
        lt6911_i2c_read_be16(pipe, height_reg, &verify_h) ||
        lt6911_i2c_read_be16(pipe, width_reg, &verify_w) ||
        w != verify_w || h != verify_h)
        return -1;
    *width = w; *height = h;
    return 0;
}

static int read_family_active(int pipe, int chip,
                             struct onekvm_lt6911_input_timing *timing)
{
    uint32_t w = 0, h = 0;
    int ret;
    switch (chip) {
    case ONEKVM_LT6911_C:
        ret = read_active_pair(pipe, 0xd28b, 0xd296, &w, &h);
        normalize_half_or_full_width(&w, h);
        break;
    case ONEKVM_LT6911_UXC:
        ret = read_active_pair(pipe, 0x85ea, 0x85f0, &w, &h);
        break;
    case ONEKVM_LT6911_D:
        ret = read_active_pair(pipe, 0xe08c, 0xe08e, &w, &h);
        w *= 2; /* D's capture geometry uses half-pixel horizontal units. */
        break;
    default:
        return -1;
    }
    if (ret) return ret;
    /* A single nonzero dimension does not prove a live D/C/UXC input. */
    if (w && h) { timing->hdmi_width = w; timing->hdmi_height = h; }
    return 0;
}

int lt6911_get_input_timing_silent(int pipe,
                                   struct onekvm_lt6911_input_timing *timing)
{
    struct onekvm_lt6911_input_timing sample = {0};
    int ret;
    if (!timing || !valid_pipe(pipe)) return -1;
    memset(timing, 0, sizeof(*timing));
    if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_UNKNOWN)
        return -1; /* Discovery must finish before live polling. */
    if (lt6911_i2c_init(pipe) != 0) return -1;
    pthread_mutex_lock(&g_i2c_lock);
    ret = read_family_active(pipe, onekvm_lt6911_cached_chip(), &sample);
    if (!ret) *timing = sample;
    pthread_mutex_unlock(&g_i2c_lock);
    return ret;
}

static int lt6911_get_input_timing_impl(int pipe,
        struct onekvm_lt6911_input_timing *timing, int pcie_rx)
{
    struct onekvm_lt6911_input_timing sample = {0};
    int chip, ret = -1, cleanup = 0;
    uint32_t rx_half_width = 0;
    if (!timing || !valid_pipe(pipe)) return -1;
    memset(timing, 0, sizeof(*timing));
    configure_pinmux_once();
    if (lt6911_i2c_init(pipe) != 0) return -1;
    pthread_mutex_lock(&g_i2c_lock);
    if (identify_chip(pipe)) goto out;
    chip = onekvm_lt6911_cached_chip();
    /* D's E0 telemetry is external: never open either internal window here. */
    if (chip != ONEKVM_LT6911_D) {
        ret = lt6911_i2c_write(pipe, 0x80ee, 1);
        if (ret) goto close;
    }
    ret = read_family_active(pipe, chip, &sample);
    if (ret) goto close;
    if (chip == ONEKVM_LT6911_C) {
        ret = read_active_pair(pipe, 0xc238, 0xc206,
                              &sample.csi_width, &sample.csi_height);
    } else if (chip == ONEKVM_LT6911_UXC && pcie_rx) {
        ret = read_active_pair(pipe, 0x8680, 0x867e,
                              &rx_half_width, &sample.uxc_rx_height);
        if (!ret) {
            int signal = lt6911_i2c_read(pipe, 0x86a3);
            if (signal < 0) ret = -1;
            else { sample.uxc_rx_width = rx_half_width * 2;
                   sample.uxc_rx_signal = (uint32_t)signal; }
        }
    }
close:
    if (chip != ONEKVM_LT6911_D)
        cleanup = lt6911_i2c_write(pipe, 0x80ee, 0);
    if (!ret && !cleanup) *timing = sample;
    else ret = -1;
out:
    pthread_mutex_unlock(&g_i2c_lock);
    return ret;
}

int lt6911_get_input_timing(int pipe,
			    struct onekvm_lt6911_input_timing *timing)
{
	return lt6911_get_input_timing_impl(pipe, timing, 0);
}

int lt6911_get_input_timing_pcie(int pipe,
				 struct onekvm_lt6911_input_timing *timing)
{
	return lt6911_get_input_timing_impl(pipe, timing, 1);
}

int lt6911_get_input_size(int pipe, uint32_t *width, uint32_t *height)
{
	struct onekvm_lt6911_input_timing timing;
	int csi_ok;
	int hdmi_ok;
	uint64_t csi_px;
	uint64_t hdmi_px;

	if (width == NULL || height == NULL)
		return -1;
	if (lt6911_get_input_timing(pipe, &timing) != 0)
		return -1;

	csi_ok = supported_active_size(timing.csi_width, timing.csi_height);
	hdmi_ok = supported_active_size(timing.hdmi_width, timing.hdmi_height);
	csi_px = (uint64_t)timing.csi_width * timing.csi_height;
	hdmi_px = (uint64_t)timing.hdmi_width * timing.hdmi_height;
	if (csi_ok && hdmi_ok) {
		if (hdmi_px > csi_px) {
			*width = timing.hdmi_width;
			*height = timing.hdmi_height;
			return 0;
		}
		*width = timing.csi_width;
		*height = timing.csi_height;
		return 0;
	}
	if (hdmi_ok) {
		*width = timing.hdmi_width;
		*height = timing.hdmi_height;
		return 0;
	}
	if (csi_ok) {
		*width = timing.csi_width;
		*height = timing.csi_height;
		return 0;
	}
	*width = 0;
	*height = 0;
	return 0;
}

int lt6911_get_capture_size(uint32_t *width, uint32_t *height)
{
	return lt6911_get_input_size(0, width, height);
}

int lt6911_kick_hdmi(void)
{
	const int pipe = 0;
	int cleanup;

	configure_pinmux_once();
	if (lt6911_i2c_init(pipe) != 0)
		return -1;
	/* D283=0x11 starts HDMI timing measurement.  Do this once when
	   opening VI, never from the live watcher. */
	pthread_mutex_lock(&g_i2c_lock);
	if (identify_chip(pipe)) {
		pthread_mutex_unlock(&g_i2c_lock);
		return -1;
	}
	if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_D) {
		pthread_mutex_unlock(&g_i2c_lock);
		return 0;
	}
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != 0)
		goto error;
	if (lt6911_i2c_write(pipe, 0xd283, 0x11) != 0)
		goto error;
	usleep(50000);
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return cleanup == 0 ? 0 : -1;
error:
	(void)lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return -1;
}

static int lt6911_start_csi_impl(int remeasure)
{
	const int pipe = 0;
	int cleanup;

	configure_pinmux_once();
	if (lt6911_i2c_init(pipe) != 0)
		return -1;
	/* Arm CSI before VI starts. A normal PCIe mode change has
	   already measured two matching receiver timings, so do not restart
	   HDMI measurement with D283. Cold start and recovery still measure.
	   Do not write 0x805a=0x88 after StartViChn: that zeros CSIBDG width. */
	pthread_mutex_lock(&g_i2c_lock);
	if (identify_chip(pipe)) {
		pthread_mutex_unlock(&g_i2c_lock);
		return -1;
	}
	if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_D) {
		pthread_mutex_unlock(&g_i2c_lock);
		return 0;
	}
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != 0)
		goto error;
	if (lt6911_i2c_write(pipe, 0x805a, 0x80) != 0)
		goto error;
	if (lt6911_i2c_write(pipe, 0x8010, 0x00) != 0)
		goto error;
	/* The cold/recovery measurement keeps its vendor settling intervals.
	 * A measured mode can close the gate and start VI immediately. */
	if (remeasure) {
		usleep(100000);
		if (lt6911_i2c_write(pipe, 0xd283, 0x11) != 0)
			goto error;
		usleep(50000);
	}
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	if (cleanup != 0)
		return -1;
	fprintf(stderr, "OneKVM: LT6911 CSI armed\n");
	return 0;
error:
	(void)lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return -1;
}

int lt6911_start_csi(void)
{
	return lt6911_start_csi_impl(1);
}

int lt6911_start_csi_with_cached_timing(void)
{
	return lt6911_start_csi_impl(0);
}

int lt6911_probe(int pipe)
{
    int ret;
    configure_pinmux_once();
    if (lt6911_i2c_init(pipe) != 0) return -1;
    pthread_mutex_lock(&g_i2c_lock);
    ret = identify_chip(pipe);
    pthread_mutex_unlock(&g_i2c_lock);
    return ret;
}

void lt6911_init(int pipe)
{
	(void)lt6911_i2c_init(pipe);
}

void lt6911_exit(int pipe)
{
	(void)lt6911_i2c_exit(pipe);
}
