/* SPDX-License-Identifier: GPL-2.0-only */
/* Scale and convert through the SG200x V4L2 VPSS node ("soph-vpss").
 *
 * -t converts colour bars and checks the bar centres of the result. Without
 * it, soph-vi frames go to soph-vpss by DMA-BUF and, with -e, the scaled
 * frames on to soph-venc or soph-jpegenc; the tool reports frame rate and
 * latency from the VI frame end (timestamps are copied along the chain).
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

#define MAX_BUFS 16
#define OUT_MP V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP_MP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE

struct buf { uint8_t *addr; size_t len; int dmafd; };

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

static struct timeval ns_tv(uint64_t ns)
{
	return (struct timeval){ .tv_sec = ns / 1000000000ull, .tv_usec = ns % 1000000000ull / 1000 };
}

static int open_node(const char *name)
{
	char sys[64], buf[64], path[32];
	int i, fd;

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
				snprintf(path, sizeof(path), "/dev/video%d", i);
				fd = open(path, O_RDWR | O_NONBLOCK);
				if (fd < 0)
					die(path);
				return fd;
			}
		}
		fclose(f);
	}
	fprintf(stderr, "no %s node\n", name);
	exit(1);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static uint32_t parse_fourcc(const char *s)
{
	if (strlen(s) != 4)
		return 0;
	return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}

static const char *fourcc_str(uint32_t f)
{
	static char s[4][5];
	static unsigned int n;
	char *r = s[n++ % 4];

	memcpy(r, &f, 4);
	r[4] = 0;
	return r;
}

static void set_fmt(int fd, int type, uint32_t fourcc, unsigned int w, unsigned int h,
		    unsigned int bpl, struct v4l2_pix_format_mplane *res)
{
	struct v4l2_format f;

	memset(&f, 0, sizeof(f));
	f.type = type;
	f.fmt.pix_mp.pixelformat = fourcc;
	f.fmt.pix_mp.width = w;
	f.fmt.pix_mp.height = h;
	f.fmt.pix_mp.num_planes = 1;
	f.fmt.pix_mp.plane_fmt[0].bytesperline = bpl;
	if (xioctl(fd, VIDIOC_S_FMT, &f))
		die("S_FMT");
	*res = f.fmt.pix_mp;
}

static void set_sel(int fd, int type, int target, const struct v4l2_rect *r)
{
	struct v4l2_selection s = { .type = type, .target = target, .r = *r };

	if (xioctl(fd, VIDIOC_S_SELECTION, &s))
		die("S_SELECTION");
	if (memcmp(&s.r, r, sizeof(*r)))
		printf("rectangle adjusted to %d,%d %ux%u\n", s.r.left, s.r.top, s.r.width,
		       s.r.height);
}

static unsigned int reqbufs(int fd, int type, int memory, unsigned int count, int cached)
{
	struct v4l2_requestbuffers rb = { .count = count, .type = type, .memory = memory };

	if (cached)
		rb.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb) || rb.count < count)
		die("REQBUFS");
	return rb.count;
}

/* MMAP buffers of a multi-planar queue, mapped and optionally exported. */
static void map_bufs(int fd, int type, unsigned int n, struct buf *bufs, int export)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		struct v4l2_plane pl;
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i,
					 .m.planes = &pl, .length = 1 };

		memset(&pl, 0, sizeof(pl));
		if (xioctl(fd, VIDIOC_QUERYBUF, &b))
			die("QUERYBUF");
		bufs[i].len = pl.length;
		bufs[i].addr = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
				    pl.m.mem_offset);
		if (bufs[i].addr == MAP_FAILED)
			die("mmap");
		bufs[i].dmafd = -1;
		if (export) {
			struct v4l2_exportbuffer eb = { .type = type, .index = i,
							.flags = O_RDWR | O_CLOEXEC };

			if (xioctl(fd, VIDIOC_EXPBUF, &eb))
				die("EXPBUF");
			bufs[i].dmafd = eb.fd;
		}
	}
}

static void qbuf(int fd, int type, int memory, unsigned int index, const struct buf *dma,
		 size_t used, struct timeval ts)
{
	struct v4l2_plane pl = { .bytesused = used };
	struct v4l2_buffer b = { .type = type, .memory = memory, .index = index,
				 .m.planes = &pl, .length = 1, .timestamp = ts };

	if (memory == V4L2_MEMORY_DMABUF) {
		pl.m.fd = dma->dmafd;
		pl.length = dma->len;
	}
	if (xioctl(fd, VIDIOC_QBUF, &b))
		die("QBUF");
}

/* 0 with a buffer, -1 when none is ready. */
static int dqbuf(int fd, int type, int memory, struct v4l2_buffer *b, struct v4l2_plane *pl)
{
	memset(b, 0, sizeof(*b));
	memset(pl, 0, sizeof(*pl));
	b->type = type;
	b->memory = memory;
	b->m.planes = pl;
	b->length = 1;
	if (!xioctl(fd, VIDIOC_DQBUF, b))
		return 0;
	if (errno != EAGAIN)
		die("DQBUF");
	return -1;
}

static void stream(int fd, int type, int on)
{
	if (xioctl(fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type))
		die(on ? "STREAMON" : "STREAMOFF");
}

/* ---- colour bars -------------------------------------------------------- */

static const uint8_t bars_rgb[8][3] = {
	{ 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
	{ 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 },
};
/* BT.601 limited range */
static const uint8_t bars_yuv[8][3] = {
	{ 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
	{ 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 },
};

static int is_rgb(uint32_t f)
{
	return f == V4L2_PIX_FMT_RGB24 || f == V4L2_PIX_FMT_BGR24;
}

/* Byte offsets of Y, U, V (or R, G, B) of pixel x in a packed line. */
static int packed_offsets(uint32_t f, unsigned int x, unsigned int o[3])
{
	unsigned int p = (x & ~1u) * 2;

	switch (f) {
	case V4L2_PIX_FMT_UYVY: o[0] = x * 2 + 1; o[1] = p; o[2] = p + 2; return 1;
	case V4L2_PIX_FMT_VYUY: o[0] = x * 2 + 1; o[1] = p + 2; o[2] = p; return 1;
	case V4L2_PIX_FMT_YUYV: o[0] = x * 2; o[1] = p + 1; o[2] = p + 3; return 1;
	case V4L2_PIX_FMT_YVYU: o[0] = x * 2; o[1] = p + 3; o[2] = p + 1; return 1;
	case V4L2_PIX_FMT_RGB24: o[0] = x * 3; o[1] = o[0] + 1; o[2] = o[0] + 2; return 1;
	case V4L2_PIX_FMT_BGR24: o[0] = x * 3 + 2; o[1] = x * 3 + 1; o[2] = x * 3; return 1;
	}
	return 0;
}

/* Pointers to the three components of pixel (x, y). */
static int pixel(const struct v4l2_pix_format_mplane *p, uint8_t *base, unsigned int x,
		 unsigned int y, uint8_t *c[3])
{
	unsigned int bpl = p->plane_fmt[0].bytesperline, h = p->height, o[3], i;
	uint8_t *line = base + (size_t)y * bpl, *chroma = base + (size_t)bpl * h;

	if (packed_offsets(p->pixelformat, x, o)) {
		for (i = 0; i < 3; i++)
			c[i] = line + o[i];
		return 0;
	}
	c[0] = line + x;
	switch (p->pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		c[1] = chroma + (size_t)(y / 2) * bpl + (x & ~1u);
		break;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		c[1] = chroma + (size_t)y * bpl + (x & ~1u);
		break;
	case V4L2_PIX_FMT_YUV420:
		c[1] = chroma + (size_t)(y / 2) * (bpl / 2) + x / 2;
		c[2] = c[1] + (size_t)(bpl / 2) * (h / 2);
		return 0;
	case V4L2_PIX_FMT_GREY:
		c[1] = c[2] = NULL;
		return 0;
	default:
		return -1;
	}
	c[2] = c[1] + 1;
	if (p->pixelformat == V4L2_PIX_FMT_NV21 || p->pixelformat == V4L2_PIX_FMT_NV61) {
		uint8_t *t = c[1];

		c[1] = c[2];
		c[2] = t;
	}
	return 0;
}

static void fill_bars(const struct v4l2_pix_format_mplane *p, uint8_t *base)
{
	const uint8_t (*val)[3] = is_rgb(p->pixelformat) ? bars_rgb : bars_yuv;
	unsigned int x, y, i;

	for (y = 0; y < p->height; y++)
		for (x = 0; x < p->width; x++) {
			uint8_t *c[3];

			if (pixel(p, base, x, y, c)) {
				fprintf(stderr, "no pattern for %s\n", fourcc_str(p->pixelformat));
				exit(1);
			}
			for (i = 0; i < 3; i++)
				if (c[i])
					*c[i] = val[x * 8 / p->width][i];
		}
}

/* Bar centres in the middle line; returns the largest deviation. */
static int clamp8(double v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5);
}

/*
 * Expected bar values. VPSS converts between YUV and RGB with the BT.601
 * coefficients and keeps the value range, so limited-range YUV bars give
 * limited-range RGB and full-range RGB bars give full-range YUV.
 */
static void expected_bar(int bar, uint32_t src_fmt, uint32_t dst_fmt, int e[3])
{
	int i;

	if (is_rgb(src_fmt) == is_rgb(dst_fmt)) {
		for (i = 0; i < 3; i++)
			e[i] = is_rgb(src_fmt) ? bars_rgb[bar][i] : bars_yuv[bar][i];
	} else if (is_rgb(dst_fmt)) {
		double y = bars_yuv[bar][0], u = bars_yuv[bar][1] - 128.0,
		       v = bars_yuv[bar][2] - 128.0;

		e[0] = clamp8(y + 1.402 * v);
		e[1] = clamp8(y - 0.344136 * u - 0.714136 * v);
		e[2] = clamp8(y + 1.772 * u);
	} else {
		double r = bars_rgb[bar][0], g = bars_rgb[bar][1], b = bars_rgb[bar][2];

		e[0] = clamp8(0.299 * r + 0.587 * g + 0.114 * b);
		e[1] = clamp8(128 - 0.168736 * r - 0.331264 * g + 0.5 * b);
		e[2] = clamp8(128 + 0.5 * r - 0.418688 * g - 0.081312 * b);
	}
}

static int check_bars(const struct v4l2_pix_format_mplane *p, uint32_t src_fmt, uint8_t *base,
		      int hflip, int verbose)
{
	int worst = 0, k, i;

	for (k = 0; k < 8; k++) {
		unsigned int x = (2 * k + 1) * p->width / 16;
		int bar = hflip ? 7 - k : k;
		uint8_t *c[3];
		int e[3];

		pixel(p, base, x, p->height / 2, c);
		expected_bar(bar, src_fmt, p->pixelformat, e);
		if (verbose)
			printf("  bar %d:", k);
		for (i = 0; i < 3; i++) {
			int d;

			if (!c[i])
				continue;
			d = abs(*c[i] - e[i]);
			if (d > worst)
				worst = d;
			if (verbose)
				printf(" %3u(%3d)", *c[i], e[i]);
		}
		if (verbose)
			printf("\n");
	}
	return worst;
}

/* ---- main --------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr,
		"usage: v4l2-scale-smoke [-t] [-s WxH] [-i FOURCC] [-S WxH] [-f FOURCC]\n"
		"         [-c x,y,w,h] [-p x,y,w,h] [-F h|v|hv] [-n frames] [-r fps] [-b buffers]\n"
		"         [-C] [-o out.raw [-k index]] [-e h264|hevc|jpeg -O out]\n"
		"  -t  colour bars instead of soph-vi; -i input format, default UYVY\n"
		"      (soph-vi: UYVY, NV21 or NV12)\n"
		"  -s  source size, default 1920x1080; -S scaled size, default the source size\n"
		"  -f  scaled format, default NV12\n"
		"  -c  crop on the source; -p compose rectangle in the scaled frame\n"
		"  -C  cached scaled buffers (V4L2_MEMORY_FLAG_NON_COHERENT)\n"
		"  -o  write scaled frame -k (default 30) raw\n"
		"  -e  encode the scaled frames by DMA-BUF\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *out = NULL, *codec = NULL, *enc_out = NULL;
	unsigned int w = 1920, h = 1080, sw = 0, sh = 0, frames = 300, fps = 60, nbufs = 4;
	unsigned int dump_idx = 30, i, done = 0, sent = 0, errors = 0, gaps = 0, hflip = 0, vflip = 0;
	unsigned int nvi = 0, nsc, nenc = 4;
	uint32_t in_fmt = V4L2_PIX_FMT_UYVY, sc_fmt = V4L2_PIX_FMT_NV12, prev_seq = 0;
	int synthetic = 0, cached = 0, crop = 0, compose = 0, worst = -1, opt;
	struct v4l2_rect crop_r = { 0 }, comp_r = { 0 };
	struct v4l2_pix_format_mplane src, dst, enc_src;
	struct buf vi_bufs[MAX_BUFS], src_bufs[MAX_BUFS], sc_bufs[MAX_BUFS], enc_bufs[MAX_BUFS];
	uint64_t *lat, first = 0, last = 0, t_start, bytes = 0;
	FILE *enc_file = NULL;
	int vi = -1, sc, enc = -1;
	char vflag[3] = "";

	while ((opt = getopt(argc, argv, "ts:i:S:f:c:p:F:n:r:b:Co:k:e:O:h")) != -1) {
		switch (opt) {
		case 't': synthetic = 1; break;
		case 's': if (sscanf(optarg, "%ux%u", &w, &h) != 2) usage(); break;
		case 'i': in_fmt = parse_fourcc(optarg); break;
		case 'S': if (sscanf(optarg, "%ux%u", &sw, &sh) != 2) usage(); break;
		case 'f': sc_fmt = parse_fourcc(optarg); break;
		case 'c':
			if (sscanf(optarg, "%d,%d,%u,%u", &crop_r.left, &crop_r.top, &crop_r.width,
				   &crop_r.height) != 4)
				usage();
			crop = 1;
			break;
		case 'p':
			if (sscanf(optarg, "%d,%d,%u,%u", &comp_r.left, &comp_r.top, &comp_r.width,
				   &comp_r.height) != 4)
				usage();
			compose = 1;
			break;
		case 'F': hflip = !!strchr(optarg, 'h'); vflip = !!strchr(optarg, 'v'); break;
		case 'n': frames = strtoul(optarg, NULL, 0); break;
		case 'r': fps = strtoul(optarg, NULL, 0); break;
		case 'b': nbufs = strtoul(optarg, NULL, 0); break;
		case 'C': cached = 1; break;
		case 'o': out = optarg; break;
		case 'k': dump_idx = strtoul(optarg, NULL, 0); break;
		case 'e': codec = optarg; break;
		case 'O': enc_out = optarg; break;
		default: usage();
		}
	}
	if (!frames || !fps || nbufs < 3 || nbufs > MAX_BUFS || !in_fmt || !sc_fmt ||
	    (codec && !enc_out))
		usage();
	if (!sw) {
		sw = w;
		sh = h;
	}
	lat = calloc(frames, sizeof(*lat));
	if (!lat)
		die("calloc");

	/* soph-vpss: OUTPUT takes the source, CAPTURE the scaled frame. */
	sc = open_node("soph-vpss");
	if (!synthetic) {
		struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
		struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

		vi = open_node("soph-vi");
		f.fmt.pix = (struct v4l2_pix_format){ .width = w, .height = h,
						      .pixelformat = in_fmt };
		if (xioctl(vi, VIDIOC_S_FMT, &f))
			die("VI S_FMT");
		if (f.fmt.pix.pixelformat != in_fmt) {
			fprintf(stderr, "soph-vi gives %s\n", fourcc_str(f.fmt.pix.pixelformat));
			exit(1);
		}
		parm.parm.capture.timeperframe = (struct v4l2_fract){ 1, fps };
		if (xioctl(vi, VIDIOC_S_PARM, &parm))
			die("VI S_PARM");
		set_fmt(sc, OUT_MP, in_fmt, w, h, f.fmt.pix.bytesperline, &src);
		if (src.plane_fmt[0].bytesperline != f.fmt.pix.bytesperline) {
			fprintf(stderr, "VPSS stride %u, VI %u\n", src.plane_fmt[0].bytesperline,
				f.fmt.pix.bytesperline);
			exit(1);
		}
		nvi = reqbufs(vi, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, nbufs, 0);
		for (i = 0; i < nvi; i++) {
			struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
						 .memory = V4L2_MEMORY_MMAP, .index = i };
			struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .index = i,
							.flags = O_RDONLY | O_CLOEXEC };

			if (xioctl(vi, VIDIOC_QUERYBUF, &b) || xioctl(vi, VIDIOC_EXPBUF, &eb))
				die("VI QUERYBUF/EXPBUF");
			vi_bufs[i] = (struct buf){ .len = b.length, .dmafd = eb.fd };
			if (xioctl(vi, VIDIOC_QBUF, &b))
				die("VI QBUF");
		}
		reqbufs(sc, OUT_MP, V4L2_MEMORY_DMABUF, nvi, 0);
	} else {
		set_fmt(sc, OUT_MP, in_fmt, w, h, 0, &src);
		reqbufs(sc, OUT_MP, V4L2_MEMORY_MMAP, nbufs, 0);
		map_bufs(sc, OUT_MP, nbufs, src_bufs, 0);
		for (i = 0; i < nbufs; i++)
			fill_bars(&src, src_bufs[i].addr);
	}
	set_fmt(sc, CAP_MP, sc_fmt, sw, sh, 0, &dst);
	if (crop)
		set_sel(sc, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_SEL_TGT_CROP, &crop_r);
	if (compose)
		set_sel(sc, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_SEL_TGT_COMPOSE, &comp_r);
	if (hflip || vflip) {
		struct v4l2_control c = { .id = V4L2_CID_HFLIP, .value = hflip };

		if (xioctl(sc, VIDIOC_S_CTRL, &c))
			die("HFLIP");
		c = (struct v4l2_control){ .id = V4L2_CID_VFLIP, .value = vflip };
		if (xioctl(sc, VIDIOC_S_CTRL, &c))
			die("VFLIP");
		snprintf(vflag, sizeof(vflag), "%s%s", hflip ? "h" : "", vflip ? "v" : "");
	}
	nsc = reqbufs(sc, CAP_MP, V4L2_MEMORY_MMAP, nbufs, cached);
	map_bufs(sc, CAP_MP, nsc, sc_bufs, codec != NULL);
	printf("%s %ux%u stride %u -> %s %ux%u stride %u size %u%s%s%s\n",
	       fourcc_str(src.pixelformat), src.width, src.height, src.plane_fmt[0].bytesperline,
	       fourcc_str(dst.pixelformat), dst.width, dst.height, dst.plane_fmt[0].bytesperline,
	       dst.plane_fmt[0].sizeimage, cached ? ", cached" : "", *vflag ? ", flip " : "", vflag);
	printf("colorimetry: OUTPUT %u/%u/%u, CAPTURE %u/%u/%u (colorspace/ycbcr_enc/quantization)\n",
	       src.colorspace, src.ycbcr_enc, src.quantization, dst.colorspace, dst.ycbcr_enc,
	       dst.quantization);

	if (codec) {
		int jpeg = !strcmp(codec, "jpeg");
		struct v4l2_pix_format_mplane cf;
		struct v4l2_streamparm parm = { .type = OUT_MP };

		if (!jpeg && strcmp(codec, "h264") && strcmp(codec, "hevc"))
			usage();
		enc = open_node(jpeg ? "soph-jpegenc" : "soph-venc");
		set_fmt(enc, CAP_MP, jpeg ? V4L2_PIX_FMT_JPEG : !strcmp(codec, "hevc") ?
			V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264, dst.width, dst.height, 0, &cf);
		set_fmt(enc, OUT_MP, dst.pixelformat, dst.width, dst.height,
			dst.plane_fmt[0].bytesperline, &enc_src);
		if (enc_src.plane_fmt[0].bytesperline != dst.plane_fmt[0].bytesperline ||
		    enc_src.pixelformat != dst.pixelformat) {
			fprintf(stderr, "encoder takes %s stride %u\n", fourcc_str(enc_src.pixelformat),
				enc_src.plane_fmt[0].bytesperline);
			exit(1);
		}
		parm.parm.output.timeperframe = (struct v4l2_fract){ 1, fps };
		xioctl(enc, VIDIOC_S_PARM, &parm);
		reqbufs(enc, OUT_MP, V4L2_MEMORY_DMABUF, nsc, 0);
		nenc = reqbufs(enc, CAP_MP, V4L2_MEMORY_MMAP, nenc, 0);
		map_bufs(enc, CAP_MP, nenc, enc_bufs, 0);
		for (i = 0; i < nenc; i++)
			qbuf(enc, CAP_MP, V4L2_MEMORY_MMAP, i, NULL, 0, (struct timeval){ 0 });
		stream(enc, OUT_MP, 1);
		stream(enc, CAP_MP, 1);
		enc_file = fopen(enc_out, "wb");
		if (!enc_file)
			die(enc_out);
	}

	for (i = 0; i < nsc; i++)
		qbuf(sc, CAP_MP, V4L2_MEMORY_MMAP, i, NULL, 0, (struct timeval){ 0 });
	t_start = now_ns();
	stream(sc, OUT_MP, 1);
	stream(sc, CAP_MP, 1);
	if (synthetic) {
		for (i = 0; i < nbufs && sent < frames; i++, sent++)
			qbuf(sc, OUT_MP, V4L2_MEMORY_MMAP, i, NULL, src.plane_fmt[0].sizeimage,
			     ns_tv(now_ns()));
	} else {
		stream(vi, V4L2_BUF_TYPE_VIDEO_CAPTURE, 1);
	}

	while (done < frames) {
		struct pollfd pfd[3] = {
			{ .fd = sc, .events = POLLIN | POLLOUT },
			{ .fd = vi, .events = POLLIN },
			{ .fd = enc, .events = POLLIN | POLLOUT },
		};
		struct v4l2_buffer b;
		struct v4l2_plane pl;
		int n = poll(pfd, 3, 2000);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			die("poll");
		if (!n) {
			fprintf(stderr, "stalled after %u frames\n", done);
			exit(1);
		}
		/* VI frame -> VPSS OUTPUT, same index */
		if (vi >= 0 && (pfd[1].revents & POLLIN)) {
			struct v4l2_buffer vb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
						  .memory = V4L2_MEMORY_MMAP };

			if (!xioctl(vi, VIDIOC_DQBUF, &vb)) {
				if (sent++)
					gaps += vb.sequence - prev_seq - 1;
				prev_seq = vb.sequence;
				qbuf(sc, OUT_MP, V4L2_MEMORY_DMABUF, vb.index, &vi_bufs[vb.index],
				     src.plane_fmt[0].sizeimage, vb.timestamp);
			} else if (errno != EAGAIN) {
				die("VI DQBUF");
			}
		}
		/* VPSS has read a source frame */
		while (!dqbuf(sc, OUT_MP, vi >= 0 ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP, &b, &pl)) {
			if (vi >= 0) {
				struct v4l2_buffer vb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
							  .memory = V4L2_MEMORY_MMAP, .index = b.index };

				if (xioctl(vi, VIDIOC_QBUF, &vb))
					die("VI QBUF");
			} else if (sent < frames) {
				qbuf(sc, OUT_MP, V4L2_MEMORY_MMAP, b.index, NULL,
				     src.plane_fmt[0].sizeimage, ns_tv(now_ns()));
				sent++;
			}
		}
		/* scaled frame */
		while (!dqbuf(sc, CAP_MP, V4L2_MEMORY_MMAP, &b, &pl)) {
			uint64_t ts = tv_ns(&b.timestamp);

			if (b.flags & V4L2_BUF_FLAG_ERROR)
				errors++;
			if (!first)
				first = ts;
			last = ts;
			if (enc >= 0) {
				qbuf(enc, OUT_MP, V4L2_MEMORY_DMABUF, b.index, &sc_bufs[b.index],
				     dst.plane_fmt[0].sizeimage, b.timestamp);
				continue;
			}
			if (done < frames) {
				lat[done] = now_ns() - ts;
				if (done == 0)
					printf("first frame %.1f ms after STREAMON\n",
					       (now_ns() - t_start) / 1e6);
				if (synthetic && !crop && !compose && !vflip && worst < 0)
					worst = check_bars(&dst, src.pixelformat,
							   sc_bufs[b.index].addr, hflip, 1);
				if (out && done == dump_idx) {
					FILE *o = fopen(out, "wb");

					if (!o || fwrite(sc_bufs[b.index].addr, 1, pl.bytesused, o) !=
					    pl.bytesused || fclose(o))
						die(out);
				}
				done++;
			}
			qbuf(sc, CAP_MP, V4L2_MEMORY_MMAP, b.index, NULL, 0, (struct timeval){ 0 });
		}
		if (enc < 0)
			continue;
		/* bitstream */
		while (!dqbuf(enc, CAP_MP, V4L2_MEMORY_MMAP, &b, &pl)) {
			if (done < frames)
				lat[done++] = now_ns() - tv_ns(&b.timestamp);
			if (fwrite(enc_bufs[b.index].addr, 1, pl.bytesused, enc_file) != pl.bytesused)
				die(enc_out);
			bytes += pl.bytesused;
			qbuf(enc, CAP_MP, V4L2_MEMORY_MMAP, b.index, NULL, 0, (struct timeval){ 0 });
		}
		/* the encoder has read a scaled frame */
		while (!dqbuf(enc, OUT_MP, V4L2_MEMORY_DMABUF, &b, &pl)) {
			if (b.flags & V4L2_BUF_FLAG_ERROR)
				errors++;
			qbuf(sc, CAP_MP, V4L2_MEMORY_MMAP, b.index, NULL, 0, (struct timeval){ 0 });
		}
	}

	if (vi >= 0)
		stream(vi, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
	stream(sc, OUT_MP, 0);
	stream(sc, CAP_MP, 0);
	if (enc >= 0) {
		stream(enc, OUT_MP, 0);
		stream(enc, CAP_MP, 0);
		if (fclose(enc_file))
			die(enc_out);
	}

	{
		uint64_t sum = 0;
		double span = synthetic ? (now_ns() - t_start) / 1e9 : (last - first) / 1e9;

		for (i = 0; i < frames; i++)
			sum += lat[i];
		qsort(lat, frames, sizeof(*lat), cmp_u64);
		printf("frames %u in %.3f s: %.2f fps, errors %u", frames, span,
		       span > 0 ? (synthetic ? frames : frames - 1) / span : 0.0, errors);
		if (!synthetic)
			printf(", VI sequence gaps %u", gaps);
		printf("\nlatency (%s) avg %.2f P50 %.2f P95 %.2f max %.2f ms\n",
		       synthetic ? "OUTPUT QBUF -> CAPTURE DQBUF" :
		       enc >= 0 ? "VI frame end -> bitstream DQBUF" : "VI frame end -> scaled DQBUF",
		       sum / 1e6 / frames, lat[frames / 2] / 1e6, lat[frames * 95 / 100] / 1e6,
		       lat[frames - 1] / 1e6);
		if (worst >= 0)
			printf("colour bars: largest deviation %d\n", worst);
		if (out)
			printf("frame %u written to %s\n", dump_idx, out);
		if (enc >= 0)
			printf("encoded %llu bytes to %s\n", (unsigned long long)bytes, enc_out);
	}
	return 0;
}
