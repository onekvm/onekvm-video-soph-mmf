/* SPDX-License-Identifier: GPL-2.0-only */
/* Encode a synthetic moving pattern through the SG200x V4L2 encoders.
 * Writes the elementary stream (Annex B or concatenated JPEG) and reports
 * throughput and OUTPUT-queue-to-CAPTURE-dequeue latency.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
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

#define OUT_BUFS 3
#define CAP_BUFS 4

struct buf { void *addr; size_t len; };
static struct buf out_bufs[OUT_BUFS], cap_bufs[CAP_BUFS];

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;

	do { ret = ioctl(fd, req, arg); } while (ret < 0 && errno == EINTR);
	return ret;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int find_node(const char *name, char *path, size_t size)
{
	char sys[64], buf[64];
	int i;

	for (i = 0; i < 64; i++) {
		FILE *f;

		snprintf(sys, sizeof(sys), "/sys/class/video4linux/video%d/name", i);
		f = fopen(sys, "r");
		if (!f)
			continue;
		if (fgets(buf, sizeof(buf), f)) {
			buf[strcspn(buf, "\n")] = 0;
			if (!strcmp(buf, name)) {
				fclose(f);
				snprintf(path, size, "/dev/video%d", i);
				return 0;
			}
		}
		fclose(f);
	}
	return -1;
}

static uint32_t fourcc_of(const char *s)
{
	if (!strcmp(s, "nv12")) return V4L2_PIX_FMT_NV12;
	if (!strcmp(s, "nv21")) return V4L2_PIX_FMT_NV21;
	if (!strcmp(s, "uyvy")) return V4L2_PIX_FMT_UYVY;
	if (!strcmp(s, "yuyv")) return V4L2_PIX_FMT_YUYV;
	fprintf(stderr, "unknown raw format %s\n", s);
	exit(2);
}

/* Moving diagonal bars over a horizontal luma ramp. */
static void fill(const struct v4l2_pix_format_mplane *f, uint8_t *p, unsigned int t)
{
	uint32_t w = f->width, h = f->height, bpl = f->plane_fmt[0].bytesperline;
	int packed = f->pixelformat == V4L2_PIX_FMT_UYVY || f->pixelformat == V4L2_PIX_FMT_YUYV;
	int uyvy = f->pixelformat == V4L2_PIX_FMT_UYVY;
	uint32_t x, y;

	for (y = 0; y < h; y++) {
		uint8_t *row = p + (size_t)y * bpl;

		for (x = 0; x < w; x++) {
			uint8_t luma = (uint8_t)(x * 220 / w + 16);

			if (((x + y + t * 8) / 64) & 1)
				luma = (uint8_t)(235 - (luma - 16));
			if (!packed) {
				row[x] = luma;
			} else {
				uint8_t chroma = (x & 1) ? (uint8_t)(128 + (y * 64 / h)) :
						 (uint8_t)(128 + (x * 64 / w) - 32);

				row[2 * x + (uyvy ? 1 : 0)] = luma;
				row[2 * x + (uyvy ? 0 : 1)] = chroma;
			}
		}
	}
	if (!packed) {
		uint8_t *c = p + (size_t)bpl * h;

		for (y = 0; y < h / 2; y++)
			for (x = 0; x < w; x += 2) {
				c[(size_t)y * bpl + x] = (uint8_t)(128 + (int)((x + t * 4) % 128) - 64);
				c[(size_t)y * bpl + x + 1] = (uint8_t)(128 + (int)(y % 96) - 48);
			}
	}
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: v4l2-encode-smoke [-c h264|hevc|jpeg] [-s WxH] [-p nv12|nv21|uyvy|yuyv]\n"
		"         [-n frames] [-r fps] [-b bitrate] [-g gop] [-q quality] [-R] [-F]\n"
		"         [-d device] [-o out]\n"
		"  -R  queue frames at the frame rate instead of as fast as possible\n"
		"  -F  redraw the pattern before every frame (CPU write each frame)\n"
		"  -D  finish with V4L2_ENC_CMD_STOP and wait for LAST and EOS\n");
	exit(2);
}

static void set_ctrl(int fd, uint32_t id, int32_t value, const char *name)
{
	struct v4l2_control c = { .id = id, .value = value };

	if (xioctl(fd, VIDIOC_S_CTRL, &c))
		fprintf(stderr, "warning: %s=%d: %s\n", name, value, strerror(errno));
}

int main(int argc, char **argv)
{
	const char *codec = "h264", *raw = "nv12", *out_path = NULL, *dev = NULL;
	unsigned int w = 1920, h = 1080, frames = 300, fps = 30, gop = 0, quality = 0;
	unsigned int bitrate = 0, queued = 0, done = 0, keys = 0, errors = 0, i;
	unsigned int free_out[OUT_BUFS], nfree = OUT_BUFS;
	int realtime = 0, redraw = 0, drain = 0, stop_sent = 0, got_last = 0, opt, fd, jpeg;
	uint64_t bytes = 0, t_start, *lat;
	char path[64];
	FILE *out = NULL;
	struct v4l2_format of = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct v4l2_format cf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	struct v4l2_requestbuffers rb;
	enum v4l2_buf_type t;

	while ((opt = getopt(argc, argv, "c:s:p:n:r:b:g:q:RFDd:o:h")) != -1) {
		switch (opt) {
		case 'c': codec = optarg; break;
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) usage(); break;
		case 'p': raw = optarg; break;
		case 'n': frames = strtoul(optarg, NULL, 0); break;
		case 'r': fps = strtoul(optarg, NULL, 0); break;
		case 'b': bitrate = strtoul(optarg, NULL, 0); break;
		case 'g': gop = strtoul(optarg, NULL, 0); break;
		case 'q': quality = strtoul(optarg, NULL, 0); break;
		case 'R': realtime = 1; break;
		case 'F': redraw = 1; break;
		case 'D': drain = 1; break;
		case 'd': dev = optarg; break;
		case 'o': out_path = optarg; break;
		default: usage();
		}
	}
	if (!frames || !fps)
		usage();
	jpeg = !strcmp(codec, "jpeg");
	if (!dev) {
		if (find_node(jpeg ? "soph-jpegenc" : "soph-venc", path, sizeof(path)))
			die("encoder node");
		dev = path;
	}
	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		die(dev);
	if (out_path && !(out = fopen(out_path, "wb")))
		die(out_path);

	/* CAPTURE first: the raw formats on offer follow the codec. */
	cf.fmt.pix_mp.pixelformat = jpeg ? V4L2_PIX_FMT_JPEG :
		!strcmp(codec, "hevc") ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
	if (xioctl(fd, VIDIOC_S_FMT, &cf))
		die("S_FMT capture");
	of.fmt.pix_mp.width = w;
	of.fmt.pix_mp.height = h;
	of.fmt.pix_mp.pixelformat = fourcc_of(raw);
	if (xioctl(fd, VIDIOC_S_FMT, &of))
		die("S_FMT output");
	if (of.fmt.pix_mp.pixelformat != fourcc_of(raw)) {
		fprintf(stderr, "%s does not take %s\n", codec, raw);
		return 1;
	}
	if (xioctl(fd, VIDIOC_G_FMT, &cf))
		die("G_FMT capture");
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = fps;
	if (xioctl(fd, VIDIOC_S_PARM, &parm))
		die("S_PARM");
	if (bitrate)
		set_ctrl(fd, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate");
	if (gop)
		set_ctrl(fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop");
	if (quality)
		set_ctrl(fd, V4L2_CID_JPEG_COMPRESSION_QUALITY, quality, "quality");
	printf("%s: %s %ux%u bpl %u size %u -> %s, capture %u bytes\n", dev, raw,
	       of.fmt.pix_mp.width, of.fmt.pix_mp.height,
	       of.fmt.pix_mp.plane_fmt[0].bytesperline, of.fmt.pix_mp.plane_fmt[0].sizeimage,
	       codec, cf.fmt.pix_mp.plane_fmt[0].sizeimage);

	for (t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;; t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		int output = t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		unsigned int count = output ? OUT_BUFS : CAP_BUFS;
		struct buf *bufs = output ? out_bufs : cap_bufs;

		memset(&rb, 0, sizeof(rb));
		rb.count = count;
		rb.type = t;
		rb.memory = V4L2_MEMORY_MMAP;
		if (xioctl(fd, VIDIOC_REQBUFS, &rb) || rb.count < count)
			die("REQBUFS");
		for (i = 0; i < count; i++) {
			struct v4l2_plane pl = { 0 };
			struct v4l2_buffer b = { .index = i, .type = t, .memory = V4L2_MEMORY_MMAP,
						 .length = 1, .m.planes = &pl };

			if (xioctl(fd, VIDIOC_QUERYBUF, &b))
				die("QUERYBUF");
			bufs[i].len = pl.length;
			bufs[i].addr = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED,
					    fd, pl.m.mem_offset);
			if (bufs[i].addr == MAP_FAILED)
				die("mmap");
			if (output) {
				fill(&of.fmt.pix_mp, bufs[i].addr, i);
			} else if (xioctl(fd, VIDIOC_QBUF, &b)) {
				die("QBUF capture");
			}
		}
		if (!output)
			break;
	}
	for (i = 0; i < OUT_BUFS; i++)
		free_out[i] = OUT_BUFS - 1 - i;
	lat = calloc(frames, sizeof(*lat));
	if (!lat)
		die("calloc");
	if (drain) {
		struct v4l2_event_subscription sub = { .type = V4L2_EVENT_EOS };

		if (xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub))
			die("SUBSCRIBE_EVENT");
	}
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &t))
		die("STREAMON capture");
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &t))
		die("STREAMON output");

	t_start = now_ns();
	while (drain ? !got_last : done < frames) {
		struct pollfd p = { .fd = fd, .events = POLLIN | POLLOUT };
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = { .length = 1, .m.planes = &pl, .memory = V4L2_MEMORY_MMAP };
		uint64_t due = t_start + (uint64_t)queued * 1000000000ull / fps;

		/* Queue every OUTPUT buffer the driver has handed back. */
		while (queued < frames && nfree && (!realtime || now_ns() >= due)) {
			unsigned int idx = free_out[--nfree];
			uint64_t ts;

			if (redraw)
				fill(&of.fmt.pix_mp, out_bufs[idx].addr, queued);
			/* Latency starts when the frame is ready, as a capture would hand it over. */
			ts = now_ns();
			memset(&b, 0, sizeof(b));
			memset(&pl, 0, sizeof(pl));
			b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.index = idx;
			b.length = 1;
			b.m.planes = &pl;
			pl.bytesused = of.fmt.pix_mp.plane_fmt[0].sizeimage;
			b.timestamp.tv_sec = ts / 1000000000ull;
			b.timestamp.tv_usec = ts % 1000000000ull / 1000;
			if (xioctl(fd, VIDIOC_QBUF, &b))
				die("QBUF output");
			queued++;
			due = t_start + (uint64_t)queued * 1000000000ull / fps;
		}
		if (drain && queued == frames && !stop_sent) {
			struct v4l2_encoder_cmd ec = { .cmd = V4L2_ENC_CMD_STOP };

			if (xioctl(fd, VIDIOC_ENCODER_CMD, &ec))
				die("ENCODER_CMD STOP");
			stop_sent = 1;
		}
		if (poll(&p, 1, realtime ? 5 : 2000) < 0)
			die("poll");
		/* OUTPUT buffers come back in completion order. */
		for (;;) {
			memset(&b, 0, sizeof(b));
			memset(&pl, 0, sizeof(pl));
			b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.length = 1;
			b.m.planes = &pl;
			if (xioctl(fd, VIDIOC_DQBUF, &b))
				break;
			free_out[nfree++] = b.index;
			if (b.flags & V4L2_BUF_FLAG_ERROR)
				errors++;
		}
		for (;;) {
			uint64_t ts;

			memset(&b, 0, sizeof(b));
			memset(&pl, 0, sizeof(pl));
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			b.memory = V4L2_MEMORY_MMAP;
			b.length = 1;
			b.m.planes = &pl;
			if (xioctl(fd, VIDIOC_DQBUF, &b)) {
				if (errno != EAGAIN)
					die("DQBUF capture");
				break;
			}
			got_last |= !!(b.flags & V4L2_BUF_FLAG_LAST);
			ts = (uint64_t)b.timestamp.tv_sec * 1000000000ull +
			     (uint64_t)b.timestamp.tv_usec * 1000;
			if (pl.bytesused && done < frames) {
				lat[done] = now_ns() - ts;
				bytes += pl.bytesused;
				keys += !!(b.flags & V4L2_BUF_FLAG_KEYFRAME);
				if (out && fwrite(cap_bufs[b.index].addr, 1, pl.bytesused, out) != pl.bytesused)
					die("write");
				done++;
			} else if (!pl.bytesused && !(b.flags & V4L2_BUF_FLAG_LAST)) {
				errors++;
			}
			/* After LAST the queue stays empty until streaming restarts. */
			if (got_last)
				break;
			if (xioctl(fd, VIDIOC_QBUF, &b))
				die("QBUF capture");
		}
		if (errors > frames / 2 + 4) {
			fprintf(stderr, "too many failed frames\n");
			break;
		}
		if (drain ? got_last : queued == frames && done + errors >= frames)
			break;
	}
	{
		double secs = (now_ns() - t_start) / 1e9;
		unsigned int n = done;

		qsort(lat, n, sizeof(*lat), cmp_u64);
		printf("frames %u keyframes %u errors %u bytes %llu in %.2f s: %.1f fps %.0f kbit/s\n",
		       done, keys, errors, (unsigned long long)bytes, secs, done / secs,
		       secs > 0 ? bytes * 8 / 1000.0 / secs : 0);
		if (n) {
			uint64_t sum = 0;

			for (i = 0; i < n; i++)
				sum += lat[i];
			printf("latency ms: avg %.2f p50 %.2f p95 %.2f max %.2f\n",
			       sum / 1e6 / n, lat[n / 2] / 1e6, lat[n * 95 / 100] / 1e6,
			       lat[n - 1] / 1e6);
		}
	}
	if (drain) {
		struct v4l2_event ev = { 0 };
		int eos = !xioctl(fd, VIDIOC_DQEVENT, &ev) && ev.type == V4L2_EVENT_EOS;

		printf("drain: LAST %s, EOS %s\n", got_last ? "yes" : "no", eos ? "yes" : "no");
		if (!got_last || !eos)
			errors++;
	}
	t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	xioctl(fd, VIDIOC_STREAMOFF, &t);
	t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(fd, VIDIOC_STREAMOFF, &t);
	if (out && fclose(out))
		die("write");
	close(fd);
	return done == frames && !errors ? 0 : 1;
}
