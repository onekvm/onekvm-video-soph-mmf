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
static pthread_mutex_t g_i2c_lock = PTHREAD_MUTEX_INITIALIZER;

static int valid_pipe(VI_PIPE pipe)
{
	return pipe >= 0 && pipe < VI_MAX_PIPE_NUM;
}

static int supported_active_size(uint32_t width, uint32_t height)
{
	static const uint16_t sizes[][2] = {
		{1920, 1080}, {1600, 900}, {1440, 1080}, {1440, 900},
		{1280, 1024}, {1280, 960}, {1280, 800}, {1280, 720},
		{1152, 864}, {1024, 768}, {800, 600}, {640, 480},
	};
	size_t i;

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		if (width == sizes[i][0] && height == sizes[i][1])
			return 1;
	}
	return 0;
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

int lt6911_get_input_size(VI_PIPE pipe, uint32_t *width, uint32_t *height)
{
	int cleanup;
	uint32_t c_width = 0;
	uint32_t c_height = 0;
	uint32_t c_hdmi_width = 0;
	uint32_t c_hdmi_height = 0;
	uint32_t uxc_width = 0;
	uint32_t uxc_height = 0;
	uint32_t d_width = 0;
	uint32_t d_height = 0;

	if (width == NULL || height == NULL || !valid_pipe(pipe))
		return CVI_FAILURE;
	configure_pinmux_once();
	if (lt6911_i2c_init(pipe) != CVI_SUCCESS)
		return CVI_FAILURE;

	/* LT6911C CSI active size.  Keep the internal-register gate open for the
	 * complete snapshot so width and height cannot come from different HDMI
	 * modes while the source is switching. */
	pthread_mutex_lock(&g_i2c_lock);
	if (lt6911_i2c_write(pipe, 0x80ee, 0x01) != CVI_SUCCESS)
		goto error;
	if (lt6911_i2c_read_be16(pipe, 0xc206, &c_height) != CVI_SUCCESS ||
	    lt6911_i2c_read_be16(pipe, 0xc238, &c_width) != CVI_SUCCESS)
		goto error;

	/* During a mode transition the CSI active counters drop to zero until VI
	 * has been recreated with the new geometry.  Read the upstream HDMI active
	 * counters as well; otherwise that zero creates a circular dependency and
	 * the new mode can never be discovered. */
	/* D283=0x11 starts a fresh HDMI timing measurement on LT6911C.  Repeating
	 * that command from the live resolution watcher briefly disrupts CSI
	 * output on NanoKVM Cube and makes VI report alternating short width/height
	 * frames.  The active-size counters below are continuously maintained by
	 * the bridge, so observing them must remain read-only while streaming. */
	if (lt6911_i2c_read_be16(pipe, 0xd296, &c_hdmi_height) != CVI_SUCCESS ||
	    lt6911_i2c_read_be16(pipe, 0xd28b, &c_hdmi_width) != CVI_SUCCESS ||
	    lt6911_i2c_read_be16(pipe, 0x85f0, &uxc_height) != CVI_SUCCESS ||
	    lt6911_i2c_read_be16(pipe, 0x85ea, &uxc_width) != CVI_SUCCESS)
		goto error;
	c_hdmi_width *= 2;
	cleanup = lt6911_i2c_write(pipe, 0x80ee, 0x00);
	if (cleanup != CVI_SUCCESS)
		goto error;

	/* LT6911D exposes active size without the 0x80ee internal-register gate.
	 * Reading this family as a fallback also keeps the backend portable across
	 * NanoKVM board revisions. */
	if (lt6911_i2c_read_be16(pipe, 0xe08e, &d_height) != CVI_SUCCESS ||
	    lt6911_i2c_read_be16(pipe, 0xe08c, &d_width) != CVI_SUCCESS)
		goto error;
	d_width *= 2;

	if (supported_active_size(c_hdmi_width, c_hdmi_height)) {
		*width = c_hdmi_width;
		*height = c_hdmi_height;
	} else if (supported_active_size(c_width, c_height)) {
		*width = c_width;
		*height = c_height;
	} else if (supported_active_size(uxc_width, uxc_height)) {
		*width = uxc_width;
		*height = uxc_height;
	} else {
		*width = d_width;
		*height = d_height;
	}

	pthread_mutex_unlock(&g_i2c_lock);
	return CVI_SUCCESS;

error:
	/* Best effort: a failed register read must not leave normal LT6911
	 * register access enabled indefinitely. */
	(void)lt6911_i2c_write(pipe, 0x80ee, 0x00);
	pthread_mutex_unlock(&g_i2c_lock);
	return CVI_FAILURE;
}

int lt6911_get_capture_size(uint32_t *width, uint32_t *height)
{
	return lt6911_get_input_size(0, width, height);
}

int lt6911_probe(VI_PIPE pipe)
{
	int id_high;
	int id_low;

	configure_pinmux_once();
	usleep(50);
	if (lt6911_i2c_init(pipe) != CVI_SUCCESS)
		return CVI_FAILURE;

	id_high = lt6911_read(pipe, LT6911_CHIP_ID_ADDR_H);
	id_low = lt6911_read(pipe, LT6911_CHIP_ID_ADDR_L);
	if (id_high < 0 || id_low < 0)
		return CVI_FAILURE;

	/* Some LT6911 firmware revisions return 0x0000 for these registers while
	 * video is fully operational.  A successful I2C transaction proves that
	 * the bridge is present; the optional ID value must not reject it. */
	(void)LT6911_CHIP_ID;
	return CVI_SUCCESS;
}

void lt6911_init(VI_PIPE pipe)
{
	(void)lt6911_i2c_init(pipe);
}

void lt6911_exit(VI_PIPE pipe)
{
	(void)lt6911_i2c_exit(pipe);
}
