/* SPDX-License-Identifier: GPL-2.0-only */
/* Decode real length-prefixed access units through the public V4L2 API.
 * Input: repeated little-endian uint32_t length followed by that packet.
 * Output: visible planar raw frames, without coded-size padding.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define BUFFERS 4
#define MAX_VIDEO_NODES 256
struct plane { void *address; size_t length; };
struct slot { struct plane planes[VIDEO_MAX_PLANES]; unsigned int count; };
struct packet { unsigned char *data; size_t size; };
static struct slot input[BUFFERS], output[BUFFERS];
static unsigned int capture_count, capture_planes;
static struct v4l2_format capture;
static unsigned int visible_width, visible_height;

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static int xioctl(int fd, unsigned long request, void *value)
{
	int ret;
	do { ret = ioctl(fd, request, value); } while (ret < 0 && errno == EINTR);
	return ret;
}

static void stream(int fd, enum v4l2_buf_type type, int start)
{
	if (xioctl(fd, start ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type))
		die(start ? "STREAMON" : "STREAMOFF");
}

static unsigned int device_capabilities(const struct v4l2_capability *caps)
{
	if (caps->capabilities & V4L2_CAP_DEVICE_CAPS)
		return caps->device_caps;
	return caps->capabilities;
}

static int is_vdec_driver(const struct v4l2_capability *caps)
{
	const size_t suffix_length = sizeof("-vdec") - 1;
	size_t length = strnlen((const char *)caps->driver, sizeof(caps->driver));

	return length > suffix_length &&
		!memcmp(caps->driver + length - suffix_length, "-vdec", suffix_length);
}

static int query_decoder(int fd)
{
	struct v4l2_capability caps = { 0 };
	unsigned int capabilities;

	if (xioctl(fd, VIDIOC_QUERYCAP, &caps))
		return 0;
	capabilities = device_capabilities(&caps);
	return is_vdec_driver(&caps) &&
		(capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) &&
		(capabilities & V4L2_CAP_STREAMING);
}

/* Video minor numbers are allocated dynamically. Select the decoder by its
 * stable alias where available, then by advertised driver identity and
 * capabilities instead of assuming video0. */
static int find_decoder(char *path, size_t path_size)
{
	static const char alias[] = "/dev/onekvm-vdec";
	unsigned int i;
	int fd;

	fd = open(alias, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		if (query_decoder(fd)) {
			if (sizeof(alias) > path_size) {
				close(fd);
				errno = ENAMETOOLONG;
				return -1;
			}
			memcpy(path, alias, sizeof(alias));
			return fd;
		}
		close(fd);
	}

	for (i = 0; i < MAX_VIDEO_NODES; i++) {
		int length;

		length = snprintf(path, path_size, "/dev/video%u", i);
		if (length < 0 || (size_t)length >= path_size) {
			errno = ENAMETOOLONG;
			return -1;
		}
		fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!query_decoder(fd)) {
			close(fd);
			continue;
		}
		return fd;
	}

	errno = ENODEV;
	return -1;
}

static unsigned int allocate(int fd, enum v4l2_buf_type type,
			     struct slot *slots, unsigned int planes)
{
	struct v4l2_requestbuffers request = {
		.type = type, .memory = V4L2_MEMORY_MMAP, .count = BUFFERS,
	};
	unsigned int i, p;
	if (xioctl(fd, VIDIOC_REQBUFS, &request)) die("REQBUFS");
	if (!request.count || request.count > BUFFERS) {
		errno = EOVERFLOW; die("buffer count");
	}
	for (i = 0; i < request.count; i++) {
		struct v4l2_plane entries[VIDEO_MAX_PLANES] = { 0 };
		struct v4l2_buffer buffer = {
			.type = type, .memory = V4L2_MEMORY_MMAP,
			.index = i, .length = planes, .m.planes = entries,
		};
		if (xioctl(fd, VIDIOC_QUERYBUF, &buffer)) die("QUERYBUF");
		slots[i].count = planes;
		for (p = 0; p < planes; p++) {
			slots[i].planes[p].length = entries[p].length;
			slots[i].planes[p].address = mmap(NULL, entries[p].length,
				PROT_READ | PROT_WRITE, MAP_SHARED, fd, entries[p].m.mem_offset);
			if (slots[i].planes[p].address == MAP_FAILED) die("mmap");
		}
	}
	return request.count;
}

static void queue(int fd, enum v4l2_buf_type type, struct slot *slots,
		  unsigned int index, const struct packet *packet)
{
	struct v4l2_plane entries[VIDEO_MAX_PLANES] = { 0 };
	struct v4l2_buffer buffer = {
		.type = type, .memory = V4L2_MEMORY_MMAP, .index = index,
		.length = slots[index].count, .m.planes = entries,
	};
	unsigned int p;
	for (p = 0; p < buffer.length; p++)
		entries[p].length = slots[index].planes[p].length;
	if (packet) {
		if (packet->size > entries[0].length) {
			errno = EOVERFLOW; die("input packet");
		}
		memcpy(slots[index].planes[0].address, packet->data, packet->size);
		entries[0].bytesused = packet->size;
	}
	if (xioctl(fd, VIDIOC_QBUF, &buffer)) {
		fprintf(stderr, "QBUF %s index=%u planes=%u errno=%d\n",
			V4L2_TYPE_IS_OUTPUT(type) ? "output" : "capture",
			index, buffer.length, errno);
		die(V4L2_TYPE_IS_OUTPUT(type) ? "QBUF output" : "QBUF capture");
	}
}

static void setup_capture(int fd)
{
	struct v4l2_selection selection = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .target = V4L2_SEL_TGT_COMPOSE,
	};
	unsigned int i, p;
	if (capture_count) {
		struct v4l2_requestbuffers request = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
		};
		stream(fd, request.type, 0);
		for (i = 0; i < capture_count; i++)
			for (p = 0; p < capture_planes; p++)
				munmap(output[i].planes[p].address, output[i].planes[p].length);
		if (xioctl(fd, VIDIOC_REQBUFS, &request)) die("release capture");
	}
	memset(&capture, 0, sizeof(capture));
	capture.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_G_FMT, &capture)) die("G_FMT");
	if (xioctl(fd, VIDIOC_G_SELECTION, &selection)) die("G_SELECTION");
	visible_width = selection.r.width;
	visible_height = selection.r.height;
	capture_planes = capture.fmt.pix_mp.num_planes;
	if (!visible_width || !visible_height || !capture_planes || capture_planes > 3) {
		errno = EINVAL; die("capture geometry");
	}
	fprintf(stderr, "capture %ux%u coded %ux%u fourcc %.4s planes %u\n",
		visible_width, visible_height, capture.fmt.pix_mp.width,
		capture.fmt.pix_mp.height, (char *)&capture.fmt.pix_mp.pixelformat,
		capture_planes);
	capture_count = allocate(fd, capture.type, output, capture_planes);
	for (i = 0; i < capture_count; i++) queue(fd, capture.type, output, i, NULL);
	stream(fd, capture.type, 1);
}

static void write_frame(FILE *file, const struct v4l2_buffer *buffer)
{
	unsigned int p;
	for (p = 0; p < capture_planes; p++) {
		unsigned int row, columns = visible_width, rows = visible_height;
		unsigned int stride = capture.fmt.pix_mp.plane_fmt[p].bytesperline;
		unsigned char *data = output[buffer->index].planes[p].address;
		uint32_t fourcc = capture.fmt.pix_mp.pixelformat;
		if (p && fourcc != V4L2_PIX_FMT_YUV444M) columns = (columns + 1) / 2;
		if (p && fourcc == V4L2_PIX_FMT_YUV420M) rows = (rows + 1) / 2;
		for (row = 0; row < rows; row++)
			if (fwrite(data + row * stride, 1, columns, file) != columns)
				die("write raw frame");
	}
}

int main(int argc, char **argv)
{
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
	struct v4l2_decoder_cmd stop = { .cmd = V4L2_DEC_CMD_STOP };
	struct packet *packets = NULL;
	size_t count = 0, next = 0, completed = 0, frames = 0;
	unsigned int buffers, i, restarts = 0, restarted = 0;
	char decoder_path[64];
	int fd, draining = 0, finished = 0, changing = 0;
	time_t last_progress;
	FILE *source, *destination;
	if (argc < 4 || argc > 6) {
		fprintf(stderr, "usage: %s h264|mjpeg|jpeg packets.bin raw.yuv [hold-seconds [restarts]]\n", argv[0]);
		return 2;
	}
	if (argc == 6) {
		restarts = strtoul(argv[5], NULL, 10);
		if (restarts > 10) { errno = EINVAL; die("restart count"); }
	}
	source = fopen(argv[2], "rb");
	if (!source) die("packet file");
	for (;;) {
		unsigned char length[4];
		uint32_t size;
		struct packet *expanded;
		if (!fread(length, 1, 4, source)) break;
		if (feof(source) || ferror(source)) die("packet header");
		size = (uint32_t)length[0] | (uint32_t)length[1] << 8 |
			(uint32_t)length[2] << 16 | (uint32_t)length[3] << 24;
		if (!size || size > 2 * 1024 * 1024 || count >= 1024) {
			errno = EINVAL; die("packet size/count");
		}
		expanded = realloc(packets, (count + 1) * sizeof(*packets));
		if (!expanded) die("packets");
		packets = expanded;
		packets[count].data = malloc(size);
		packets[count].size = size;
		if (!packets[count].data) die("packet");
		if (fread(packets[count].data, 1, size, source) != size) die("packet data");
		count++;
	}
	fclose(source);
	if (!count) { errno = EINVAL; die("empty packet file"); }
	destination = fopen(argv[3], "wb");
	if (!destination) die("raw file");
	fd = find_decoder(decoder_path, sizeof(decoder_path));
	if (fd < 0) die("find /dev/onekvm-vdec or *-vdec decoder");
	fprintf(stderr, "using %s\n", decoder_path);
	if (!strcmp(argv[1], "h264")) fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	else if (!strcmp(argv[1], "mjpeg")) fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
	else if (!strcmp(argv[1], "jpeg")) fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
	else { errno = EINVAL; die("codec"); }
	fmt.fmt.pix_mp.width = 640;
	fmt.fmt.pix_mp.height = 480;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt)) die("S_FMT");
	if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) die("SUBSCRIBE_EVENT");
	buffers = allocate(fd, fmt.type, input, 1);
	for (i = 0; i < buffers && next < count; i++)
		queue(fd, fmt.type, input, i, &packets[next++]);
	stream(fd, fmt.type, 1);
	if (argc >= 5) {
		fprintf(stderr, "holding decoder session\n");
		sleep(strtoul(argv[4], NULL, 10));
	}
	last_progress = time(NULL);
	while (!finished) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT | POLLPRI };
		struct v4l2_event event;
		int progress = 0;
		if (poll(&pfd, 1, 10000) <= 0) { errno = ETIMEDOUT; die("decode poll"); }
		while (!xioctl(fd, VIDIOC_DQEVENT, &event)) {
			progress = 1;
			if (event.type == V4L2_EVENT_SOURCE_CHANGE) {
				if (capture_count) changing = 1;
				else setup_capture(fd);
			}
		}
		for (;;) {
			struct v4l2_plane entries[VIDEO_MAX_PLANES] = { 0 };
			struct v4l2_buffer buffer = {
				.type = fmt.type, .memory = V4L2_MEMORY_MMAP,
				.length = 1, .m.planes = entries,
			};
			if (xioctl(fd, VIDIOC_DQBUF, &buffer)) {
				if (errno != EAGAIN) die("DQBUF input");
				break;
			}
			if (buffer.flags & V4L2_BUF_FLAG_ERROR) { errno = EIO; die("input decode"); }
			completed++; progress = 1;
			if (next < count) queue(fd, fmt.type, input, buffer.index, &packets[next++]);
		}
		if (next == count && !draining) {
			if (xioctl(fd, VIDIOC_DECODER_CMD, &stop)) die("DECODER_CMD STOP");
			if (xioctl(fd, VIDIOC_DECODER_CMD, &stop)) die("repeated STOP");
			draining = 1;
		}
		while (capture_count) {
			struct v4l2_plane entries[VIDEO_MAX_PLANES] = { 0 };
			struct v4l2_buffer buffer = {
				.type = capture.type, .memory = V4L2_MEMORY_MMAP,
				.length = capture_planes, .m.planes = entries,
			};
			if (xioctl(fd, VIDIOC_DQBUF, &buffer)) {
				if (errno != EAGAIN) die("DQBUF capture");
				break;
			}
			progress = 1;
			if (buffer.flags & V4L2_BUF_FLAG_ERROR) { errno = EIO; die("capture decode"); }
			if (buffer.flags & V4L2_BUF_FLAG_LAST) {
				/* A change may have arrived after the event loop above.
				 * The driver queues its event before completing LAST. */
				while (!xioctl(fd, VIDIOC_DQEVENT, &event))
					if (event.type == V4L2_EVENT_SOURCE_CHANGE) changing = 1;
				if (changing) { setup_capture(fd); changing = 0; }
				else if (restarted < restarts) {
					struct v4l2_decoder_cmd start = { .cmd = V4L2_DEC_CMD_START };
					if (xioctl(fd, VIDIOC_DECODER_CMD, &stop)) die("stopped STOP");
					if (xioctl(fd, VIDIOC_DECODER_CMD, &start)) die("DECODER_CMD START");
					queue(fd, capture.type, output, buffer.index, NULL);
					next = 0;
					for (i = 0; i < buffers && next < count; i++)
						queue(fd, fmt.type, input, i, &packets[next++]);
					draining = 0;
					restarted++;
					fprintf(stderr, "restarted decoder %u/%u\n", restarted, restarts);
				} else finished = 1;
				break;
			}
			/* SOURCE_CHANGE may be queued after the poll/DQEVENT pass but
			 * before this capture buffer is dequeued. Do not requeue an old
			 * capture buffer after the event has made the queue stale. */
			while (!xioctl(fd, VIDIOC_DQEVENT, &event))
				if (event.type == V4L2_EVENT_SOURCE_CHANGE)
					changing = 1;
			if (changing) {
				setup_capture(fd);
				changing = 0;
				break;
			}
			write_frame(destination, &buffer);
			frames++;
			queue(fd, capture.type, output, buffer.index, NULL);
		}
		if (progress) last_progress = time(NULL);
		else if (time(NULL) - last_progress > 10) { errno = ETIMEDOUT; die("no decoder progress"); }
		else usleep(1000);
	}
	stream(fd, fmt.type, 0);
	if (capture_count) stream(fd, capture.type, 0);
	close(fd);
	if (fclose(destination)) die("close raw file");
	fprintf(stderr, "decoded %zu frames, consumed %zu/%zu packets\n",
		frames, completed, count * (restarts + 1));
	/* A decoder may consume the final queued input while draining STOP. */
	return frames == count * (restarts + 1) ? 0 : 1;
}
