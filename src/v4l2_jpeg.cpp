// V4L2 runtime: JPEG on soph-jpegenc. One session serves snapshots, MJPEG and
// manual frames; a borrowed capture frame is encoded in place, other CPU
// pictures are copied into a staging buffer.

#include "v4l2_runtime.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace onekvm::mmf {
namespace {

using rt::Frame;
using rt::g_rt;

constexpr uint32_t kOut = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr uint32_t kCap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

struct Jpeg {
	bool initialized = false;
	bool cleanup_pending = false;
	int width = 0, height = 0, quality = 0;
	uint32_t stride = 0;
	v4l2::Node node;
	std::vector<v4l2::MappedBuffer> bitstream;
	std::array<Frame *, rt::kSlots> inflight{}; // g_rt.mu
	bool frame_pending = false;
	bool suspended = false; // OUTPUT stopped, no VENC channel
	int held = -1;
	rt::StagingSet staging;
};

Jpeg g_jpeg;

uint32_t align64(uint32_t value)
{
	return (value + 63u) & ~63u;
}

void reap_inputs_locked()
{
	struct v4l2_buffer b;
	struct v4l2_plane p;

	while (g_jpeg.node.dequeue(kOut, V4L2_MEMORY_DMABUF, &b, &p) == 0) {
		if (b.index >= static_cast<uint32_t>(rt::kSlots))
			continue;
		Frame *frame = g_jpeg.inflight[b.index];
		g_jpeg.inflight[b.index] = nullptr;
		if (frame)
			rt::release_frame_locked(frame);
	}
}

int set_quality(int quality)
{
	const int ret = g_jpeg.node.set_control(V4L2_CID_JPEG_COMPRESSION_QUALITY, quality);
	if (ret == 0)
		g_jpeg.quality = quality;
	return ret;
}

int start_session(int width, int height, int quality)
{
	const uint32_t w = static_cast<uint32_t>(width), h = static_cast<uint32_t>(height);
	struct v4l2_format fmt {};
	int ret = g_jpeg.node.open(v4l2::kJpegNode);

	if (ret == 0)
		ret = g_jpeg.node.set_format(kCap, V4L2_PIX_FMT_JPEG, w, h, 0,
					     jpeg_stream_buffer_bytes(width, height), &fmt);
	if (ret == 0)
		ret = g_jpeg.node.set_format(kOut, V4L2_PIX_FMT_NV21, w, h, align64(w), 0, &fmt);
	if (ret == 0 && (fmt.fmt.pix_mp.width != w || fmt.fmt.pix_mp.height != h ||
			 fmt.fmt.pix_mp.plane_fmt[0].bytesperline != align64(w)))
		ret = -EINVAL;
	if (ret == 0)
		ret = set_quality(quality);
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: JPEG %ux%u setup rejected: %s\n", w, h,
			     std::strerror(-ret));
		return ret;
	}
	g_jpeg.stride = align64(w);
	const int count = g_jpeg.node.request_buffers(kCap, V4L2_MEMORY_MMAP, 2);
	if (count < 1)
		return count < 0 ? count : -ENOMEM;
	ret = g_jpeg.node.map_buffers(kCap, count, false, &g_jpeg.bitstream);
	for (int i = 0; ret == 0 && i < count; ++i)
		ret = g_jpeg.node.queue(kCap, V4L2_MEMORY_MMAP, i, 0, 0);
	if (ret == 0) {
		ret = g_jpeg.node.request_buffers(kOut, V4L2_MEMORY_DMABUF, rt::kSlots);
		ret = ret < rt::kSlots ? (ret < 0 ? ret : -ENOMEM) : 0;
	}
	if (ret == 0)
		ret = g_jpeg.node.stream(kOut, true);
	if (ret == 0)
		ret = g_jpeg.node.stream(kCap, true);
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: JPEG session start failed: %s\n", std::strerror(-ret));
		return ret;
	}
	g_jpeg.width = width;
	g_jpeg.height = height;
	g_jpeg.initialized = true;
	return 0;
}

void stop_session()
{
	if (g_jpeg.node.is_open()) {
		(void)g_jpeg.node.stream(kOut, false);
		(void)g_jpeg.node.stream(kCap, false);
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (Frame *&frame : g_jpeg.inflight) {
			if (frame)
				rt::release_frame_locked(frame);
			frame = nullptr;
		}
	}
	v4l2::Node::unmap_buffers(&g_jpeg.bitstream);
	g_jpeg.node.close();
	rt::staging_free(&g_jpeg.staging);
	g_jpeg.initialized = false;
	g_jpeg.suspended = false;
	g_jpeg.frame_pending = false;
	g_jpeg.held = -1;
	g_jpeg.width = g_jpeg.height = 0;
}

int prepare(int ch, int width, int height, int format, int quality)
{
	if (!g_jpeg.initialized || width != g_jpeg.width || height != g_jpeg.height) {
		const int ret = open_jpeg_encoder(ch, width, height, format, quality);
		if (ret != 0)
			return ret;
	} else if (quality != g_jpeg.quality && set_quality(quality) != 0) {
		return -1;
	}
	if (g_jpeg.frame_pending || g_jpeg.held >= 0)
		return -EBUSY;
	if (g_jpeg.suspended) {
		const int ret = g_jpeg.node.stream(kOut, true);
		if (ret < 0) {
			std::fprintf(stderr, "OneKVM: JPEG restart failed: %s\n", std::strerror(-ret));
			return -1;
		}
		g_jpeg.suspended = false;
	}
	return 0;
}

/* Queue `frame` (one reference handed over) as the next JPEG input. */
int queue_input(Frame *frame)
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	reap_inputs_locked();
	int ret = -EBUSY;
	if (frame->slot >= 0 && !g_jpeg.inflight[frame->slot] && frame->stride == g_jpeg.stride)
		ret = g_jpeg.node.queue(kOut, V4L2_MEMORY_DMABUF, frame->slot, frame->bytes(),
					frame->timestamp_ns, &frame->mem);
	if (ret < 0) {
		rt::release_frame_locked(frame);
		return ret;
	}
	g_jpeg.inflight[frame->slot] = frame;
	return 0;
}

} // namespace

int open_jpeg_encoder(int ch, int w, int h, int format, int quality)
{
	if (ch < 0 || ch >= rt::kEncoderChannels || w <= 0 || h <= 0 || (w & 1) || (h & 1) ||
	    format != rt::kNv21 || quality < 51 || quality > 100 || g_rt.reference_count == 0)
		return -1;
	if (yield_h26x_vi_direct() != 0)
		return -EBUSY;
	if (g_jpeg.cleanup_pending && close_jpeg_encoder(ch) != 0)
		return -EBUSY;
	if (g_jpeg.initialized && w == g_jpeg.width && h == g_jpeg.height)
		return quality == g_jpeg.quality || set_quality(quality) == 0 ? 0 : -1;
	if (g_jpeg.initialized && close_jpeg_encoder(ch) != 0)
		return -1;
	const int ret = start_session(w, h, quality);
	if (ret < 0) {
		stop_session();
		return ret;
	}
	return 0;
}

int close_jpeg_encoder(int ch)
{
	(void)ch;
	g_jpeg.cleanup_pending = true;
	if (g_jpeg.initialized)
		stop_session();
	/* The session is gone, so the frame it read can go back. */
	if (g_rt.jpeg_capture_owned && g_rt.jpeg_capture_channel >= 0 &&
	    release_capture_frame(g_rt.jpeg_capture_channel) != 0)
		return -EIO;
	g_rt.jpeg_capture_channel = -1;
	g_rt.jpeg_capture_owned = false;
	g_jpeg.cleanup_pending = false;
	return 0;
}

int submit_jpeg_frame_timeout(int ch, uint8_t *data, int w, int h, int format, int quality,
			      int timeout_ms)
{
	(void)timeout_ms;
	if (!data)
		return -1;
	int ret = prepare(ch, w, h, format, quality);
	if (ret != 0)
		return ret;
	int capture = -1;
	Frame *frame = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		frame = rt::find_borrowed_frame_locked(data, &capture);
		if (frame && (frame->width != static_cast<uint32_t>(w) ||
			      frame->height != static_cast<uint32_t>(h)))
			frame = nullptr;
		if (frame)
			++frame->refs;
	}
	if (frame) {
		ret = queue_input(frame);
		if (ret == 0 && g_rt.jpeg_capture_channel < 0) {
			g_rt.jpeg_capture_channel = capture;
			g_rt.jpeg_capture_owned = false;
		}
	} else {
		if (g_jpeg.staging.frames.empty() &&
		    (ret = rt::staging_alloc(&g_jpeg.staging, static_cast<uint32_t>(w),
					     static_cast<uint32_t>(h), 1)) < 0) {
			rt::staging_free(&g_jpeg.staging);
			return ret;
		}
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			frame = rt::staging_take_locked(&g_jpeg.staging);
			if (frame)
				++frame->refs;
		}
		if (!frame)
			return -EBUSY;
		rt::copy_tight_nv21(frame, data);
		frame->timestamp_ns = v4l2::monotonic_ns();
		ret = queue_input(frame);
	}
	if (ret != 0)
		return ret;
	g_jpeg.frame_pending = true;
	return 0;
}

int submit_jpeg_frame(int ch, uint8_t *data, int w, int h, int format, int quality)
{
	return submit_jpeg_frame_timeout(ch, data, w, h, format, quality, 1000);
}

int submit_jpeg_capture_frame_timeout(int ch, int capture, int quality, int timeout_ms)
{
	(void)timeout_ms;
	if (capture < 0 || capture >= kMaxCaptureChannels)
		return -1;
	Frame *frame = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		frame = g_rt.channels[capture].borrowed;
		if (frame)
			++frame->refs;
	}
	if (!frame)
		return -1;
	int ret = prepare(ch, static_cast<int>(frame->width), static_cast<int>(frame->height),
			  rt::kNv21, quality);
	if (ret == 0) {
		ret = queue_input(frame);
	} else {
		std::lock_guard<std::mutex> lock(g_rt.mu);
		rt::release_frame_locked(frame);
	}
	if (ret != 0)
		return ret;
	g_rt.jpeg_capture_channel = capture;
	g_jpeg.frame_pending = true;
	return 0;
}

int read_jpeg_packet_timeout(int ch, uint8_t *dst, int capacity, int timeout_ms)
{
	(void)ch;
	if (!g_jpeg.initialized || !g_jpeg.frame_pending || g_jpeg.held >= 0 || !dst ||
	    capacity <= 0)
		return -1;
	const uint64_t deadline = rt::steady_ns() +
		static_cast<uint64_t>(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
	struct v4l2_buffer b;
	struct v4l2_plane p;
	for (;;) {
		const int ret = g_jpeg.node.dequeue(kCap, V4L2_MEMORY_MMAP, &b, &p);
		if (ret == 0)
			break;
		const uint64_t now = rt::steady_ns();
		if (ret != -EAGAIN || now >= deadline)
			return -1;
		struct pollfd pfd = {g_jpeg.node.fd(), POLLIN, 0};
		int polled;
		do {
			polled = poll(&pfd, 1, static_cast<int>((deadline - now + 999999) / 1000000));
		} while (polled < 0 && errno == EINTR);
		if (polled > 0 && (pfd.revents & (POLLERR | POLLNVAL)))
			usleep(1000);
	}
	g_jpeg.frame_pending = false;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		reap_inputs_locked();
	}
	const bool ok = !(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused > p.data_offset &&
			b.index < g_jpeg.bitstream.size();
	const uint32_t size = ok ? b.bytesused - p.data_offset : 0;
	if (!ok || size > static_cast<uint32_t>(capacity)) {
		(void)g_jpeg.node.queue(kCap, V4L2_MEMORY_MMAP, b.index, 0, 0);
		return ok ? -2 : -1;
	}
	std::memcpy(dst, g_jpeg.bitstream[b.index].addr + p.data_offset, size);
	g_jpeg.held = static_cast<int>(b.index);
	return static_cast<int>(size);
}

int read_jpeg_packet(int ch, uint8_t *dst, int capacity)
{
	return read_jpeg_packet_timeout(ch, dst, capacity, 1000);
}

int release_jpeg_packet(int ch)
{
	(void)ch;
	if (g_jpeg.held < 0)
		return 0;
	const int ret = g_jpeg.node.queue(kCap, V4L2_MEMORY_MMAP, g_jpeg.held, 0, 0);
	g_jpeg.held = -1;
	return ret;
}

bool jpeg_encoder_initialized()
{
	return g_jpeg.initialized;
}

namespace rt {

void release_idle_jpeg_inputs()
{
	if (!g_jpeg.initialized || g_jpeg.suspended || g_jpeg.frame_pending)
		return;
	(void)g_jpeg.node.stream(kOut, false);
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (Frame *&frame : g_jpeg.inflight) {
			if (frame)
				rt::release_frame_locked(frame);
			frame = nullptr;
		}
	}
	(void)g_jpeg.node.request_buffers(kOut, V4L2_MEMORY_DMABUF, 0);
	(void)g_jpeg.node.request_buffers(kOut, V4L2_MEMORY_DMABUF, rt::kSlots);
	g_jpeg.suspended = true;
}

} // namespace rt

} // namespace onekvm::mmf
