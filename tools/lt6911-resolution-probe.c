#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*get_input_size_fn)(int, uint32_t *, uint32_t *);
typedef int (*register_read_fn)(int, int);
typedef int (*register_write_fn)(int, int, int);

static int load_function(void *handle, const char *name,
	void *destination, size_t destination_size)
{
	void *symbol;

	memset(destination, 0, destination_size);
	dlerror();
	symbol = dlsym(handle, name);
	if (symbol == NULL || destination_size != sizeof(symbol))
		return -1;
	/* POSIX specifies that dlsym results can represent function pointers. Use
	 * memcpy to avoid the object-to-function pointer cast forbidden by ISO C. */
	memcpy(destination, &symbol, sizeof(symbol));
	return 0;
}

int main(int argc, char **argv)
{
	const char *library = argc > 1 ? argv[1]
		: "/usr/lib/onekvm/video-backends/nanokvm-mmf.so";
	int samples = argc > 2 ? atoi(argv[2]) : 5;
	void *handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	get_input_size_fn get_input_size;
	register_read_fn register_read;
	register_write_fn register_write;
	register_write_fn raw_register_write;
	uint8_t *i2c_address;

	if (handle == NULL) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	if (load_function(handle, "lt6911_get_input_size",
		&get_input_size, sizeof(get_input_size)) != 0) {
		fprintf(stderr, "dlsym: %s\n", dlerror());
		dlclose(handle);
		return 1;
	}
	i2c_address = (uint8_t *)dlsym(handle, "lt6911_i2c_addr");
	if (i2c_address != NULL && argc > 3)
		*i2c_address = (uint8_t)strtoul(argv[3], NULL, 0);
	(void)load_function(handle, "lt6911_read",
		&register_read, sizeof(register_read));
	(void)load_function(handle, "lt6911_write",
		&register_write, sizeof(register_write));
	(void)load_function(handle, "lt6911_write_register",
		&raw_register_write, sizeof(raw_register_write));
	if (register_read != NULL && register_write != NULL) {
		if (argc > 4 && atoi(argv[4]) != 0) {
			printf("resetting LT6911 CSI output\n");
			if (raw_register_write != NULL) {
				(void)raw_register_write(0, 0xff, 0x80);
				(void)raw_register_write(0, 0x5a, 0x88);
				usleep(1000);
				(void)raw_register_write(0, 0xff, 0x80);
				(void)raw_register_write(0, 0x5a, 0x80);
			} else {
				(void)register_write(0, 0x805a, 0x88);
				usleep(1000);
				(void)register_write(0, 0x805a, 0x80);
			}
			usleep(50000);
		}
		printf("chip=%02x%02x\n", register_read(0, 0xa000),
		       register_read(0, 0xa001));
		(void)register_write(0, 0xd283, 0x11);
		usleep(5000);
		printf("csi=%02x%02x x %02x%02x hdmi=%02x%02x x %02x%02x\n",
		       register_read(0, 0xc238), register_read(0, 0xc239),
		       register_read(0, 0xc206), register_read(0, 0xc207),
		       register_read(0, 0xd28b), register_read(0, 0xd28c),
		       register_read(0, 0xd296), register_read(0, 0xd297));
	}
	while (samples-- > 0) {
		uint32_t width = 0;
		uint32_t height = 0;
		int result = get_input_size(0, &width, &height);
		printf("result=%d width=%u height=%u\n", result, width, height);
		if (register_read != NULL) {
			printf("raw csi=%02x%02x x %02x%02x hdmi=%02x%02x x %02x%02x\n",
			       register_read(0, 0xc238), register_read(0, 0xc239),
			       register_read(0, 0xc206), register_read(0, 0xc207),
			       register_read(0, 0xd28b), register_read(0, 0xd28c),
			       register_read(0, 0xd296), register_read(0, 0xd297));
		}
		usleep(500000);
	}
	dlclose(handle);
	return 0;
}
