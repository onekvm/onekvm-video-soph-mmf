/* SPDX-License-Identifier: GPL-2.0-only */
/* Capture UYVY frames from the SG200x V4L2 VI node ("soph-vi") and optionally
 * encode them through soph-venc / soph-jpegenc by DMA-BUF, without copies.
 * Reports frame rate, sequence gaps and latency from the VI end-of-frame
 * timestamp to the capture dequeue, or to the encoded access unit.
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

#define MAX_BUFS 32
#define ENC_BUFS 4

struct buf { void *addr; size_t len; int dmafd; };
static struct buf cap_bufs[MAX_BUFS], enc_bufs[ENC_BUFS];

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

static uint64_t tv_ns(const struct timeval *tv)
{
	return (uint64_t)tv->tv_sec * 1000000000ull + (uint64_t)tv->tv_usec * 1000ull;
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

static int open_node(const char *dev, const char *name)
{
	char path[32];
	int fd;

	if (!dev) {
		if (find_node(name, path, sizeof(path))) {
			fprintf(stderr, "no %s node\n", name);
			exit(1);
		}
		dev = path;
	}
	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		die(dev);
	return fd;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: v4l2-capture-smoke [-d dev] [-s WxH] [-r fps] [-n frames] [-b buffers]\n"
		"         [-C] [-H hold_ms] [-o frame.uyvy [-k index]] [-e hevc|jpeg -O out]\n"
		"  -s  source size (S_FMT), default 1920x1080\n"
		"  -C  cached buffers (V4L2_MEMORY_FLAG_NON_COHERENT)\n"
		"  -H  hold each frame this long before requeueing it\n"
		"  -o  write frame -k (default 30) as raw UYVY\n"
		"  -e  encode through soph-venc (hevc) or soph-jpegenc (jpeg) by DMA-BUF\n");
	exit(2);
}

/* Encoder session fed with the capture buffers' DMA-BUFs. */
static int enc_open(const char *codec, const struct v4l2_pix_format *pix, unsigned int fps,
		    unsigned int nbufs)
{
	int jpeg = !strcmp(codec, "jpeg");
	struct v4l2_requestbuffers rb;
	struct v4l2_streamparm parm;
	struct v4l2_format f;
	unsigned int i;
	int fd, type;

	if (!jpeg && strcmp(codec, "hevc"))
		usage();
	fd = open_node(NULL, jpeg ? "soph-jpegenc" : "soph-venc");

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f.fmt.pix_mp.pixelformat = jpeg ? V4L2_PIX_FMT_JPEG : V4L2_PIX_FMT_HEVC;
	f.fmt.pix_mp.width = pix->width;
	f.fmt.pix_mp.height = pix->height;
	f.fmt.pix_mp.num_planes = 1;
	if (xioctl(fd, VIDIOC_S_FMT, &f))
		die("encoder S_FMT capture");

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_UYVY;
	f.fmt.pix_mp.width = pix->width;
	f.fmt.pix_mp.height = pix->height;
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.plane_fmt[0].bytesperline = pix->bytesperline;
	if (xioctl(fd, VIDIOC_S_FMT, &f))
		die("encoder S_FMT output");
	if (f.fmt.pix_mp.plane_fmt[0].bytesperline != pix->bytesperline) {
		fprintf(stderr, "encoder stride %u, capture %u\n",
			f.fmt.pix_mp.plane_fmt[0].bytesperline, pix->bytesperline);
		exit(1);
	}

	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = fps;
	xioctl(fd, VIDIOC_S_PARM, &parm);

	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	rb.memory = V4L2_MEMORY_DMABUF;
	rb.count = nbufs;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb) || rb.count < nbufs)
		die("encoder REQBUFS output");

	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rb.memory = V4L2_MEMORY_MMAP;
	rb.count = ENC_BUFS;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb) || rb.count < ENC_BUFS)
		die("encoder REQBUFS capture");
	for (i = 0; i < ENC_BUFS; i++) {
		struct v4l2_plane pl;
		struct v4l2_buffer b;

		memset(&b, 0, sizeof(b));
		memset(&pl, 0, sizeof(pl));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		b.m.planes = &pl;
		b.length = 1;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b))
			die("encoder QUERYBUF");
		enc_bufs[i].len = pl.length;
		enc_bufs[i].addr = mmap(NULL, pl.length, PROT_READ, MAP_SHARED, fd, pl.m.mem_offset);
		if (enc_bufs[i].addr == MAP_FAILED)
			die("encoder mmap");
		if (xioctl(fd, VIDIOC_QBUF, &b))
			die("encoder QBUF capture");
	}
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type))
		die("encoder STREAMON output");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &type))
		die("encoder STREAMON capture");
	return fd;
}

static void cap_qbuf(int fd, unsigned int index)
{
	struct v4l2_buffer b;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	if (xioctl(fd, VIDIOC_QBUF, &b))
		die("QBUF capture");
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *out = NULL, *codec = NULL, *enc_out = NULL;
	unsigned int w = 1920, h = 1080, fps = 60, frames = 300, nbufs = 4, dump_idx = 30;
	unsigned int hold_ms = 0, cached = 0, i, done = 0, gaps = 0, enc_errors = 0;
	uint64_t *lat, first_ts = 0, last_ts = 0, prev_ts = 0, ivl_min = ~0ull, ivl_max = 0;
	uint64_t read_ns = 0, bytes = 0, t_start;
	uint32_t prev_seq = 0;
	struct v4l2_requestbuffers rb;
	struct v4l2_streamparm parm;
	struct v4l2_format f;
	FILE *enc_file = NULL;
	int fd, enc = -1, type, opt;

	while ((opt = getopt(argc, argv, "d:s:r:n:b:CH:o:k:e:O:h")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) usage(); break;
		case 'r': fps = strtoul(optarg, NULL, 0); break;
		case 'n': frames = strtoul(optarg, NULL, 0); break;
		case 'b': nbufs = strtoul(optarg, NULL, 0); break;
		case 'C': cached = 1; break;
		case 'H': hold_ms = strtoul(optarg, NULL, 0); break;
		case 'o': out = optarg; break;
		case 'k': dump_idx = strtoul(optarg, NULL, 0); break;
		case 'e': codec = optarg; break;
		case 'O': enc_out = optarg; break;
		default: usage();
		}
	}
	if (!frames || !fps || nbufs < 3 || nbufs > MAX_BUFS || (codec && !enc_out))
		usage();
	lat = calloc(frames, sizeof(*lat));
	if (!lat)
		die("calloc");

	fd = open_node(dev, "soph-vi");
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = w;
	f.fmt.pix.height = h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY;
	if (xioctl(fd, VIDIOC_S_FMT, &f))
		die("S_FMT");
	memset(&parm, 0, sizeof(parm));
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	if (xioctl(fd, VIDIOC_S_PARM, &parm))
		die("S_PARM");

	memset(&rb, 0, sizeof(rb));
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	rb.count = nbufs;
	if (cached)
		rb.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb))
		die("REQBUFS");
	if (cached && !(rb.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS))
		fprintf(stderr, "warning: no cache hints, buffers stay coherent\n");
	nbufs = rb.count;
	for (i = 0; i < nbufs; i++) {
		struct v4l2_exportbuffer eb;
		struct v4l2_buffer b;

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b))
			die("QUERYBUF");
		cap_bufs[i].len = b.length;
		cap_bufs[i].addr = mmap(NULL, b.length, PROT_READ, MAP_SHARED, fd, b.m.offset);
		if (cap_bufs[i].addr == MAP_FAILED)
			die("mmap");
		cap_bufs[i].dmafd = -1;
		if (codec) {
			memset(&eb, 0, sizeof(eb));
			eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			eb.index = i;
			eb.flags = O_RDONLY | O_CLOEXEC;
			if (xioctl(fd, VIDIOC_EXPBUF, &eb))
				die("EXPBUF");
			cap_bufs[i].dmafd = eb.fd;
		}
		cap_qbuf(fd, i);
	}
	printf("capture %ux%u stride %u size %u, %u buffers%s\n", f.fmt.pix.width,
	       f.fmt.pix.height, f.fmt.pix.bytesperline, f.fmt.pix.sizeimage, nbufs,
	       cached ? " (cached)" : "");

	if (codec) {
		enc = enc_open(codec, &f.fmt.pix, fps, nbufs);
		enc_file = fopen(enc_out, "wb");
		if (!enc_file)
			die(enc_out);
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	t_start = now_ns();
	if (xioctl(fd, VIDIOC_STREAMON, &type))
		die("STREAMON");

	while (done < frames) {
		struct pollfd pfd[2] = {
			{ .fd = fd, .events = POLLIN },
			{ .fd = enc, .events = POLLIN | POLLOUT },
		};
		struct v4l2_buffer b;
		int n = poll(pfd, enc >= 0 ? 2 : 1, 2000);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			die("poll");
		if (!n) {
			fprintf(stderr, "no frame within 2 s after %u frames\n", done);
			exit(1);
		}

		if (pfd[0].revents & POLLIN) {
			uint64_t ts, t;

			memset(&b, 0, sizeof(b));
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			b.memory = V4L2_MEMORY_MMAP;
			if (xioctl(fd, VIDIOC_DQBUF, &b) == 0) {
				t = now_ns();
				ts = tv_ns(&b.timestamp);
				if (!first_ts) {
					first_ts = ts;
					printf("first frame %.1f ms after STREAMON, sequence %u\n",
					       (t - t_start) / 1e6, b.sequence);
				} else {
					uint64_t ivl = ts - prev_ts;

					if (ivl < ivl_min) ivl_min = ivl;
					if (ivl > ivl_max) ivl_max = ivl;
					gaps += b.sequence - prev_seq - 1;
				}
				prev_ts = last_ts = ts;
				prev_seq = b.sequence;
				if (b.flags & V4L2_BUF_FLAG_ERROR)
					fprintf(stderr, "buffer %u flagged ERROR\n", b.index);

				if (enc < 0) {
					lat[done] = t - ts;
					if (out && done == dump_idx) {
						uint64_t r0 = now_ns();
						FILE *o = fopen(out, "wb");

						if (!o || fwrite(cap_bufs[b.index].addr, 1, f.fmt.pix.sizeimage, o)
						    != f.fmt.pix.sizeimage || fclose(o))
							die(out);
						read_ns = now_ns() - r0;
					}
					if (hold_ms)
						usleep(hold_ms * 1000);
					cap_qbuf(fd, b.index);
					done++;
				} else {
					struct v4l2_plane pl;
					struct v4l2_buffer ob;

					memset(&ob, 0, sizeof(ob));
					memset(&pl, 0, sizeof(pl));
					ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
					ob.memory = V4L2_MEMORY_DMABUF;
					ob.index = b.index;
					ob.m.planes = &pl;
					ob.length = 1;
					ob.timestamp = b.timestamp;
					pl.m.fd = cap_bufs[b.index].dmafd;
					pl.length = cap_bufs[b.index].len;
					pl.bytesused = f.fmt.pix.sizeimage;
					if (xioctl(enc, VIDIOC_QBUF, &ob))
						die("encoder QBUF output");
				}
			} else if (errno != EAGAIN) {
				die("DQBUF capture");
			}
		}

		if (enc >= 0 && (pfd[1].revents & (POLLIN | POLLOUT))) {
			struct v4l2_plane pl;

			for (;;) {	/* bitstream */
				memset(&b, 0, sizeof(b));
				memset(&pl, 0, sizeof(pl));
				b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
				b.memory = V4L2_MEMORY_MMAP;
				b.m.planes = &pl;
				b.length = 1;
				if (xioctl(enc, VIDIOC_DQBUF, &b)) {
					if (errno != EAGAIN)
						die("encoder DQBUF capture");
					break;
				}
				if (done < frames)
					lat[done++] = now_ns() - tv_ns(&b.timestamp);
				if (fwrite(enc_bufs[b.index].addr, 1, pl.bytesused, enc_file)
				    != pl.bytesused)
					die(enc_out);
				bytes += pl.bytesused;
				if (xioctl(enc, VIDIOC_QBUF, &b))
					die("encoder QBUF capture");
			}
			for (;;) {	/* frames the encoder has read */
				memset(&b, 0, sizeof(b));
				memset(&pl, 0, sizeof(pl));
				b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
				b.memory = V4L2_MEMORY_DMABUF;
				b.m.planes = &pl;
				b.length = 1;
				if (xioctl(enc, VIDIOC_DQBUF, &b)) {
					if (errno != EAGAIN)
						die("encoder DQBUF output");
					break;
				}
				if (b.flags & V4L2_BUF_FLAG_ERROR)
					enc_errors++;
				cap_qbuf(fd, b.index);
			}
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	if (enc >= 0) {
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		xioctl(enc, VIDIOC_STREAMOFF, &type);
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(enc, VIDIOC_STREAMOFF, &type);
		if (fclose(enc_file))
			die(enc_out);
	}

	{
		uint64_t sum = 0;
		double span = (last_ts - first_ts) / 1e9;

		for (i = 0; i < frames; i++)
			sum += lat[i];
		qsort(lat, frames, sizeof(*lat), cmp_u64);
		printf("frames %u in %.3f s: %.2f fps, interval min %.2f max %.2f ms, sequence gaps %u\n",
		       frames, span, span > 0 ? (frames - 1) / span : 0.0,
		       ivl_min / 1e6, ivl_max / 1e6, gaps);
		printf("latency (%s) avg %.2f P50 %.2f P95 %.2f max %.2f ms\n",
		       enc >= 0 ? "VI frame end -> bitstream dequeue" : "VI frame end -> DQBUF",
		       sum / 1e6 / frames, lat[frames / 2] / 1e6, lat[frames * 95 / 100] / 1e6,
		       lat[frames - 1] / 1e6);
		if (out)
			printf("frame %u written to %s, CPU read %.2f ms\n", dump_idx, out, read_ns / 1e6);
		if (enc >= 0)
			printf("encoded %llu bytes to %s, %u encoder errors\n",
			       (unsigned long long)bytes, enc_out, enc_errors);
	}
	return 0;
}
