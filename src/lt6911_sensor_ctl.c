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

#ifdef ARCH_CV182X
#include <linux/cvi_vip_snsr.h>
#include "cvi_comm_video.h"
#else
#include <linux/vi_snsr.h>
#include <linux/cvi_comm_video.h>
#endif
#include "cvi_sns_ctrl.h"
#include "lt6911_receiver.h"

void onekvm_lt6911_get_active_size(uint32_t *width, uint32_t *height);

#define LT6911_I2C_DEV 4
#define LT6911_I2C_BANK_ADDR 0xff
#define LT6911_CHIP_ID_ADDR_H 0xa000
#define LT6911_CHIP_ID_ADDR_L 0xa001
#define LT6911_CHIP_ID 0x1605

/* Linux i2c-dev takes a 7-bit address.  The vendor source used 0x56, which is
 * the wire-format address including the R/W bit. */
CVI_U8 lt6911_i2c_addr = 0x2b;
const CVI_U32 lt6911_addr_byte = 1;
const CVI_U32 lt6911_data_byte = 1;

static int g_fd[VI_MAX_PIPE_NUM] = {[0 ... (VI_MAX_PIPE_NUM - 1)] = -1};
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

static int valid_pipe(VI_PIPE pipe)
{
	return pipe >= 0 && pipe < VI_MAX_PIPE_NUM;
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
			CVI_TRACE_SNS(CVI_DBG_ERR, "LT6911 pinmux command failed: %s\n", commands[i]);
	}
}

int lt6911_i2c_init(VI_PIPE pipe)
{
	char path[16];
	int fd;

	if (!valid_pipe(pipe)) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "invalid VI pipe %d\n", pipe);
		return CVI_FAILURE;
	}
	if (g_fd[pipe] >= 0)
		return CVI_SUCCESS;

	snprintf(path, sizeof(path), "/dev/i2c-%u", LT6911_I2C_DEV);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "open %s failed: %s\n", path, strerror(errno));
		return CVI_FAILURE;
	}
	if (ioctl(fd, I2C_SLAVE_FORCE, lt6911_i2c_addr) < 0) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "select LT6911 at 0x%02x failed: %s\n",
			lt6911_i2c_addr, strerror(errno));
		close(fd);
		return CVI_FAILURE;
	}
	g_fd[pipe] = fd;
	return CVI_SUCCESS;
}

int lt6911_i2c_exit(VI_PIPE pipe)
{
	if (!valid_pipe(pipe))
		return CVI_FAILURE;
	if (g_fd[pipe] >= 0) {
		close(g_fd[pipe]);
		g_fd[pipe] = -1;
	}
	return CVI_SUCCESS;
}

int lt6911_read_register(VI_PIPE pipe, int addr)
{
	uint8_t reg = (uint8_t)addr;
	uint8_t data;
	ssize_t count;

	if (!valid_pipe(pipe) || g_fd[pipe] < 0)
		return CVI_FAILURE;
	count = write(g_fd[pipe], &reg, sizeof(reg));
	if (count != (ssize_t)sizeof(reg)) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "LT6911 register select 0x%02x failed: %s\n",
			reg, count < 0 ? strerror(errno) : "short write");
		return CVI_FAILURE;
	}
	count = read(g_fd[pipe], &data, sizeof(data));
	if (count != (ssize_t)sizeof(data)) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "LT6911 register read 0x%02x failed: %s\n",
			reg, count < 0 ? strerror(errno) : "short read");
		return CVI_FAILURE;
	}
	return data;
}

int lt6911_write_register(VI_PIPE pipe, int addr, int data)
{
	uint8_t transaction[2] = {(uint8_t)addr, (uint8_t)data};
	ssize_t count;

	if (!valid_pipe(pipe) || g_fd[pipe] < 0)
		return CVI_FAILURE;
	count = write(g_fd[pipe], transaction, sizeof(transaction));
	if (count != (ssize_t)sizeof(transaction)) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "LT6911 register write 0x%02x failed: %s\n",
			transaction[0], count < 0 ? strerror(errno) : "short write");
		return CVI_FAILURE;
	}
	return CVI_SUCCESS;
}

static int lt6911_i2c_read(VI_PIPE pipe, int reg_addr)
{
	if (lt6911_write_register(pipe, LT6911_I2C_BANK_ADDR,
		(uint8_t)(reg_addr >> 8)) != CVI_SUCCESS)
		return CVI_FAILURE;
	return lt6911_read_register(pipe, reg_addr & 0xff);
}

static int lt6911_i2c_write(VI_PIPE pipe, int reg_addr, int data)
{
	if (lt6911_write_register(pipe, LT6911_I2C_BANK_ADDR,
		(uint8_t)(reg_addr >> 8)) != CVI_SUCCESS)
		return CVI_FAILURE;
	return lt6911_write_register(pipe, reg_addr & 0xff, data);
}

static int lt6911_i2c_read_be16(VI_PIPE pipe, int reg_addr, uint32_t *value)
{
	int high = lt6911_i2c_read(pipe, reg_addr);
	int low = lt6911_i2c_read(pipe, reg_addr + 1);

	if (high < 0 || low < 0)
		return CVI_FAILURE;
	*value = ((uint32_t)high << 8) | (uint32_t)low;
	return CVI_SUCCESS;
}

int lt6911_read(VI_PIPE pipe, int addr)
{
	int data;
	int cleanup;

	pthread_mutex_lock(&g_i2c_lock);
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != CVI_SUCCESS)
		goto error;
	data = lt6911_i2c_read(pipe, addr);
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	if (data < 0 || cleanup != CVI_SUCCESS)
		goto error;
	pthread_mutex_unlock(&g_i2c_lock);
	return data;

error:
	pthread_mutex_unlock(&g_i2c_lock);
	return CVI_FAILURE;
}

int lt6911_write(VI_PIPE pipe, int addr, int data)
{
	int result;
	int cleanup;

	pthread_mutex_lock(&g_i2c_lock);
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != CVI_SUCCESS) {
		pthread_mutex_unlock(&g_i2c_lock);
		return CVI_FAILURE;
	}
	result = lt6911_i2c_write(pipe, addr, data);
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return result == CVI_SUCCESS && cleanup == CVI_SUCCESS
		? CVI_SUCCESS : CVI_FAILURE;
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
    VI_PIPE pipe = *(VI_PIPE *)context;
    for (size_t i = 0; i < size; ++i) {
        int value = lt6911_i2c_read(pipe, addr + i);
        if (value < 0) return -EIO;
        data[i] = (uint8_t)value;
    }
    return 0;
}

static int sensor_write(void *context, uint16_t addr, uint8_t value)
{
    return lt6911_i2c_write(*(VI_PIPE *)context, addr, value) == CVI_SUCCESS ? 0 : -EIO;
}

static int identify_chip(VI_PIPE pipe)
{
    struct onekvm_lt6911_io io = { &pipe, sensor_read, sensor_write };
    int chip = onekvm_lt6911_cached_chip();
    if (chip != ONEKVM_LT6911_UNKNOWN) return CVI_SUCCESS;
    if (onekvm_lt6911_identify(&io, &chip)) return CVI_FAILURE;
    onekvm_lt6911_cache_chip(chip);
    return CVI_SUCCESS;
}

/* Recheck both dimensions so a failed/torn sample never replaces geometry. */
static int read_active_pair(VI_PIPE pipe, int width_reg, int height_reg,
                           uint32_t *width, uint32_t *height)
{
    uint32_t w, h, verify_w, verify_h;
    if (lt6911_i2c_read_be16(pipe, height_reg, &h) ||
        lt6911_i2c_read_be16(pipe, width_reg, &w) ||
        lt6911_i2c_read_be16(pipe, height_reg, &verify_h) ||
        lt6911_i2c_read_be16(pipe, width_reg, &verify_w) ||
        w != verify_w || h != verify_h)
        return CVI_FAILURE;
    *width = w; *height = h;
    return CVI_SUCCESS;
}

static int read_family_active(VI_PIPE pipe, int chip,
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
        return CVI_FAILURE;
    }
    if (ret) return ret;
    /* A single nonzero dimension does not prove a live D/C/UXC input. */
    if (w && h) { timing->hdmi_width = w; timing->hdmi_height = h; }
    return CVI_SUCCESS;
}

int lt6911_get_input_timing_silent(VI_PIPE pipe,
                                   struct onekvm_lt6911_input_timing *timing)
{
    struct onekvm_lt6911_input_timing sample = {0};
    int ret;
    if (!timing || !valid_pipe(pipe)) return CVI_FAILURE;
    memset(timing, 0, sizeof(*timing));
    if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_UNKNOWN)
        return CVI_FAILURE; /* Discovery must finish before live polling. */
    if (lt6911_i2c_init(pipe) != CVI_SUCCESS) return CVI_FAILURE;
    pthread_mutex_lock(&g_i2c_lock);
    ret = read_family_active(pipe, onekvm_lt6911_cached_chip(), &sample);
    if (!ret) *timing = sample;
    pthread_mutex_unlock(&g_i2c_lock);
    return ret;
}

static int lt6911_get_input_timing_impl(VI_PIPE pipe,
        struct onekvm_lt6911_input_timing *timing, int pcie_rx)
{
    struct onekvm_lt6911_input_timing sample = {0};
    int chip, ret = CVI_FAILURE, cleanup = 0;
    uint32_t rx_half_width = 0;
    if (!timing || !valid_pipe(pipe)) return CVI_FAILURE;
    memset(timing, 0, sizeof(*timing));
    configure_pinmux_once();
    if (lt6911_i2c_init(pipe) != CVI_SUCCESS) return CVI_FAILURE;
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
            if (signal < 0) ret = CVI_FAILURE;
            else { sample.uxc_rx_width = rx_half_width * 2;
                   sample.uxc_rx_signal = (uint32_t)signal; }
        }
    }
close:
    if (chip != ONEKVM_LT6911_D)
        cleanup = lt6911_i2c_write(pipe, 0x80ee, 0);
    if (!ret && !cleanup) *timing = sample;
    else ret = CVI_FAILURE;
out:
    pthread_mutex_unlock(&g_i2c_lock);
    return ret;
}

int lt6911_get_input_timing(VI_PIPE pipe,
			    struct onekvm_lt6911_input_timing *timing)
{
	return lt6911_get_input_timing_impl(pipe, timing, 0);
}

int lt6911_get_input_timing_pcie(VI_PIPE pipe,
				 struct onekvm_lt6911_input_timing *timing)
{
	return lt6911_get_input_timing_impl(pipe, timing, 1);
}

int lt6911_get_input_size(VI_PIPE pipe, uint32_t *width, uint32_t *height)
{
	struct onekvm_lt6911_input_timing timing;
	int csi_ok;
	int hdmi_ok;
	uint64_t csi_px;
	uint64_t hdmi_px;

	if (width == NULL || height == NULL)
		return CVI_FAILURE;
	if (lt6911_get_input_timing(pipe, &timing) != CVI_SUCCESS)
		return CVI_FAILURE;

	csi_ok = supported_active_size(timing.csi_width, timing.csi_height);
	hdmi_ok = supported_active_size(timing.hdmi_width, timing.hdmi_height);
	csi_px = (uint64_t)timing.csi_width * timing.csi_height;
	hdmi_px = (uint64_t)timing.hdmi_width * timing.hdmi_height;
	if (csi_ok && hdmi_ok) {
		if (hdmi_px > csi_px) {
			*width = timing.hdmi_width;
			*height = timing.hdmi_height;
			return CVI_SUCCESS;
		}
		*width = timing.csi_width;
		*height = timing.csi_height;
		return CVI_SUCCESS;
	}
	if (hdmi_ok) {
		*width = timing.hdmi_width;
		*height = timing.hdmi_height;
		return CVI_SUCCESS;
	}
	if (csi_ok) {
		*width = timing.csi_width;
		*height = timing.csi_height;
		return CVI_SUCCESS;
	}
	*width = 0;
	*height = 0;
	return CVI_SUCCESS;
}

int lt6911_get_capture_size(uint32_t *width, uint32_t *height)
{
	return lt6911_get_input_size(0, width, height);
}

int lt6911_kick_hdmi(void)
{
	const VI_PIPE pipe = 0;
	int cleanup;

	configure_pinmux_once();
	if (lt6911_i2c_init(pipe) != CVI_SUCCESS)
		return CVI_FAILURE;
	/* D283=0x11 starts HDMI timing measurement.  Do this once when
	   opening VI, never from the live watcher. */
	pthread_mutex_lock(&g_i2c_lock);
	if (identify_chip(pipe)) {
		pthread_mutex_unlock(&g_i2c_lock);
		return CVI_FAILURE;
	}
	if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_D) {
		pthread_mutex_unlock(&g_i2c_lock);
		return CVI_SUCCESS;
	}
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != CVI_SUCCESS)
		goto error;
	if (lt6911_i2c_write(pipe, 0xd283, 0x11) != CVI_SUCCESS)
		goto error;
	usleep(50000);
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return cleanup == CVI_SUCCESS ? CVI_SUCCESS : CVI_FAILURE;
error:
	(void)lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return CVI_FAILURE;
}

static int lt6911_start_csi_impl(int remeasure)
{
	const VI_PIPE pipe = 0;
	int cleanup;

	configure_pinmux_once();
	if (lt6911_i2c_init(pipe) != CVI_SUCCESS)
		return CVI_FAILURE;
	/* Arm CSI before SAMPLE_PLAT_VI_INIT. A normal PCIe mode change has
	   already measured two matching receiver timings, so do not restart
	   HDMI measurement with D283. Cold start and recovery still measure.
	   Do not write 0x805a=0x88 after StartViChn: that zeros CSIBDG width. */
	pthread_mutex_lock(&g_i2c_lock);
	if (identify_chip(pipe)) {
		pthread_mutex_unlock(&g_i2c_lock);
		return CVI_FAILURE;
	}
	if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_D) {
		pthread_mutex_unlock(&g_i2c_lock);
		return CVI_SUCCESS;
	}
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != CVI_SUCCESS)
		goto error;
	if (lt6911_i2c_write(pipe, 0x805a, 0x80) != CVI_SUCCESS)
		goto error;
	if (lt6911_i2c_write(pipe, 0x8010, 0x00) != CVI_SUCCESS)
		goto error;
	/* The cold/recovery measurement keeps its vendor settling intervals.
	 * A measured mode can close the gate and start VI immediately. */
	if (remeasure) {
		usleep(100000);
		if (lt6911_i2c_write(pipe, 0xd283, 0x11) != CVI_SUCCESS)
			goto error;
		usleep(50000);
	}
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	if (cleanup != CVI_SUCCESS)
		return CVI_FAILURE;
	fprintf(stderr, "OneKVM: LT6911 CSI armed\n");
	return CVI_SUCCESS;
error:
	(void)lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return CVI_FAILURE;
}

int lt6911_start_csi(void)
{
	return lt6911_start_csi_impl(1);
}

int lt6911_start_csi_with_cached_timing(void)
{
	return lt6911_start_csi_impl(0);
}

int lt6911_probe(VI_PIPE pipe)
{
    int ret;
    configure_pinmux_once();
    if (lt6911_i2c_init(pipe) != CVI_SUCCESS) return CVI_FAILURE;
    pthread_mutex_lock(&g_i2c_lock);
    ret = identify_chip(pipe);
    pthread_mutex_unlock(&g_i2c_lock);
    return ret;
}

void lt6911_init(VI_PIPE pipe)
{
	(void)lt6911_i2c_init(pipe);
}

void lt6911_exit(VI_PIPE pipe)
{
	(void)lt6911_i2c_exit(pipe);
}
