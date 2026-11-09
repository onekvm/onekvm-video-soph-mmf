/* Configure VI without starting a stream, then exercise failure cleanup.
 * Run only with Core and all other VI consumers stopped. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/vi_uapi.h>

static int sdk_control(int fd, unsigned int id, void *data)
{
	struct vi_ext_control control = {
		.id = VI_IOCTL_SDK_CTRL,
		.sdk_id = id,
		.sdk_cfg = { .dev = 0, .pipe = 0, .chn = 0, .ptr = data },
	};

	if (ioctl(fd, VI_IOC_S_CTRL, &control) < 0) {
		fprintf(stderr, "VI SDK control %u: %s\n", id, strerror(errno));
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	VI_DEV_ATTR_S device = { 0 };
	VI_CHN_ATTR_S channel = { 0 };
	int fd, failed = 0;

	if (argc != 2 || (strcmp(argv[1], "disable") && strcmp(argv[1], "close"))) {
		fprintf(stderr, "usage: %s disable|close\n", argv[0]);
		return 2;
	}

	fd = open("/dev/cvi-vi", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/cvi-vi");
		return 1;
	}
	device.enInputDataType = VI_DATA_TYPE_YUV;
	device.enWorkMode = VI_WORK_MODE_1Multiplex;
	device.stSize.u32Width = 640;
	device.stSize.u32Height = 480;
	channel.stSize = device.stSize;
	channel.enPixelFormat = PIXEL_FORMAT_NV21;
	channel.u32Depth = 1;

	/* SetChnAttr registers the channel before userspace allocates its VB
	 * pool. Stopping here models that allocation failing before EnableChn. */
	if (sdk_control(fd, VI_SDK_SET_DEV_ATTR, &device) ||
	    sdk_control(fd, VI_SDK_SET_CHN_ATTR, &channel))
		failed = 1;
	else if (!strcmp(argv[1], "disable") &&
		 sdk_control(fd, VI_SDK_DISABLE_CHN, NULL))
		failed = 1;
	if (close(fd)) {
		perror("close /dev/vi");
		failed = 1;
	}
	if (!failed)
		printf("VI partial-init cleanup (%s): passed\n", argv[1]);
	return failed;
}
