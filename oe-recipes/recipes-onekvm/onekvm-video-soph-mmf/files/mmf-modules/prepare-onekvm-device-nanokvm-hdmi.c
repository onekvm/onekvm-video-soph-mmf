#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int write_register(int fd, uint8_t reg, uint8_t value)
{
	const uint8_t data[] = {reg, value};

	if (write(fd, data, sizeof(data)) != (ssize_t)sizeof(data)) {
		fprintf(stderr, "LT6911 write %02x=%02x failed: %s\n",
			reg, value, strerror(errno));
		return -1;
	}
	return 0;
}

int main(void)
{
	int fd = open("/dev/i2c-4", O_RDWR);

	if (fd < 0) {
		fprintf(stderr, "open /dev/i2c-4 failed: %s\n", strerror(errno));
		return 1;
	}
	if (ioctl(fd, I2C_SLAVE_FORCE, 0x2b) < 0) {
		fprintf(stderr, "select LT6911 failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	if (write_register(fd, 0xff, 0x80) ||
	    write_register(fd, 0xee, 0x01) ||
	    write_register(fd, 0x5a, 0x80) ||
	    write_register(fd, 0x10, 0x00)) {
		close(fd);
		return 1;
	}
	usleep(100000);
	if (write_register(fd, 0xff, 0xd2) ||
	    write_register(fd, 0x83, 0x11)) {
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
