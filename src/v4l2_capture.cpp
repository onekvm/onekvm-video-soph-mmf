// V4L2 runtime: lifecycle, soph-vi capture, capture channels (direct or
// through soph-vpss), the latest-frame slot, borrowed frames and placeholder
// frames. See v4l2_runtime.hpp for the threading model.

#include "v4l2_runtime.hpp"
#include "vi_direct_policy.hpp"

#include <linux/dma-buf.h>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <system_error>

namespace onekvm::mmf {

namespace rt {

Runtime g_rt;

namespace {

constexpr uint32_t kViType = V4L2_BUF_TYPE_VIDEO_CAPTURE;
constexpr uint32_t kOut = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr uint32_t kCap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
/* Above this frame size keep fewer buffers: the multimedia CMA is 64 MiB. */
constexpr size_t kLargeFrameBytes = 4u << 20;

uint32_t align64(uint32_t value)
{
	return (value + 63u) & ~63u;
}

int alloc_slot_locked()
{
	for (int slot = 0; slot < kSlots; ++slot) {
		if (!(g_rt.slot_mask & (1u << slot))) {
			g_rt.slot_mask |= 1u << slot;
			return slot;
		}
	}
	return -1;
}

void free_slot_locked(int slot)
{
	if (slot >= 0 && slot < kSlots)
		g_rt.slot_mask &= ~(1u << slot);
}

Frame *make_frame_locked(Producer producer, int index, const v4l2::MappedBuffer &mem,
			 uint32_t width, uint32_t height, uint32_t stride, bool cached)
{
	Frame *frame = new (std::nothrow) Frame;

	if (!frame)
		return nullptr;
	frame->producer = producer;
	frame->index = index;
	frame->mem = mem;
	frame->cached = cached;
	frame->width = width;
	frame->height = height;
	frame->stride = stride;
	frame->slot = alloc_slot_locked();
	if (frame->slot < 0)
		std::fprintf(stderr, "OneKVM: no V4L2 buffer slot left; frame %d is CPU-only\n",
			     index);
	return frame;
}

void destroy_frame_locked(Frame *frame)
{
	if (frame->mem.addr)
		munmap(frame->mem.addr, frame->mem.length);
	if (frame->mem.dmabuf >= 0)
		close(frame->mem.dmabuf);
	free_slot_locked(frame->slot);
	delete frame;
}

void unmap_unowned(std::vector<v4l2::MappedBuffer> *mems, size_t from)
{
	for (size_t i = from; i < mems->size(); ++i) {
		if ((*mems)[i].addr)
			munmap((*mems)[i].addr, (*mems)[i].length);
		if ((*mems)[i].dmabuf >= 0)
			close((*mems)[i].dmabuf);
	}
	mems->clear();
}

/* Producer is going away: free idle frames, orphan referenced ones. */
void retire_frames_locked(std::vector<Frame *> *frames)
{
	for (Frame *frame : *frames) {
		if (frame->refs > 0) {
			frame->orphan = true;
			frame->channel = nullptr;
		} else {
			destroy_frame_locked(frame);
		}
	}
	frames->clear();
}

void requeue_locked(Frame *frame)
{
	int ret = 0;

	if (frame->producer == Producer::Vi) {
		if (g_rt.vi.is_open())
			ret = g_rt.vi.queue(kViType, V4L2_MEMORY_MMAP, frame->index, 0, 0,
					    nullptr, kNoCacheHints);
	} else if (frame->producer == Producer::Scaler && frame->channel &&
		   frame->channel->scaler.is_open()) {
		ret = frame->channel->scaler.queue(kCap, V4L2_MEMORY_MMAP, frame->index, 0, 0,
						   nullptr, kNoCacheHints);
	}
	/* EINVAL: a racing STREAMOFF already handed it back and it was requeued. */
	if (ret < 0 && ret != -EINVAL)
		std::fprintf(stderr, "OneKVM: requeue of capture buffer %d failed: %s\n",
			     frame->index, std::strerror(-ret));
}

void deliver_locked(int ch, Frame *frame)
{
	Channel &c = g_rt.channels[ch];
	Frame *previous = c.latest;

	feed_bound_encoders_locked(ch, frame);
	++frame->refs;
	c.latest = frame;
	if (previous)
		release_frame_locked(previous);
	++c.delivered;
	signal_fd(c.ready_fd);
}

/* Send a VI-size picture into capture channel `ch`. */
void route_locked(int ch, Frame *source)
{
	Channel &c = g_rt.channels[ch];

	if (!c.open || !c.running)
		return;
	if (c.passthrough) {
		deliver_locked(ch, source);
		return;
	}
	/* One frame in the scaler at a time: a second would only queue. */
	if (!c.scaler.is_open() || source->slot < 0 || c.scaler_inflight > 0 ||
	    c.scaler_inputs[source->slot]) {
		++c.dropped;
		return;
	}
	const int ret = c.scaler.queue(kOut, V4L2_MEMORY_DMABUF, source->slot, source->bytes(),
				       source->timestamp_ns, &source->mem);
	if (ret < 0) {
		if (c.dropped++ % 600 == 0)
			std::fprintf(stderr, "OneKVM: scaler input on channel %d failed: %s\n", ch,
				     std::strerror(-ret));
		return;
	}
	++source->refs;
	c.scaler_inputs[source->slot] = source;
	++c.scaler_inflight;
}

void count_vi_frame_locked(uint64_t timestamp_ns)
{
	if (g_rt.rate_window_ns == 0 || timestamp_ns < g_rt.rate_window_ns) {
		g_rt.rate_window_ns = timestamp_ns;
		g_rt.rate_frames = 0;
		return;
	}
	++g_rt.rate_frames;
	const uint64_t elapsed = timestamp_ns - g_rt.rate_window_ns;
	if (elapsed >= 1000000000ull) {
		g_rt.vi_fps.store(static_cast<int>(
			(g_rt.rate_frames * 1000000000ull + elapsed / 2) / elapsed),
			std::memory_order_relaxed);
		g_rt.rate_window_ns = timestamp_ns;
		g_rt.rate_frames = 0;
	}
}

void handle_vi_locked()
{
	for (size_t n = 0; n < g_rt.vi_frames.size(); ++n) {
		struct v4l2_buffer b;
		struct v4l2_plane p;

		if (g_rt.vi.dequeue(kViType, V4L2_MEMORY_MMAP, &b, &p) != 0)
			return;
		if (b.index >= g_rt.vi_frames.size())
			continue;
		Frame *frame = g_rt.vi_frames[b.index];
		frame->timestamp_ns = v4l2::timeval_ns(b.timestamp);
		frame->sequence = b.sequence;
		frame->refs = 1;
		if (!(b.flags & V4L2_BUF_FLAG_ERROR)) {
			count_vi_frame_locked(frame->timestamp_ns);
			g_rt.last_vi_frame_ns.store(steady_ns(), std::memory_order_relaxed);
			for (int ch = 0; ch < kMaxCaptureChannels && !g_rt.user_frames; ++ch) {
				const Channel &c = g_rt.channels[ch];
				if (c.open && c.running &&
				    onekvm::keep_vi_frame(frame->sequence, c.input_fps, c.fps))
					route_locked(ch, frame);
			}
		}
		release_frame_locked(frame);
	}
}

void handle_scaler_locked(int ch)
{
	Channel &c = g_rt.channels[ch];
	struct v4l2_buffer b;
	struct v4l2_plane p;

	while (c.scaler.dequeue(kOut, V4L2_MEMORY_DMABUF, &b, &p) == 0) {
		if (b.index >= static_cast<uint32_t>(kSlots))
			continue;
		Frame *source = c.scaler_inputs[b.index];
		c.scaler_inputs[b.index] = nullptr;
		if (source) {
			--c.scaler_inflight;
			release_frame_locked(source);
		}
	}
	while (c.scaler.dequeue(kCap, V4L2_MEMORY_MMAP, &b, &p) == 0) {
		if (b.index >= c.scaled.size())
			continue;
		Frame *frame = c.scaled[b.index];
		frame->refs = 1;
		frame->timestamp_ns = v4l2::timeval_ns(b.timestamp);
		frame->sequence = b.sequence;
		if (!(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused && c.running)
			deliver_locked(ch, frame);
		release_frame_locked(frame);
	}
}

void pipeline_main()
{
	std::vector<struct pollfd> fds;

	set_thread_name("onekvm-v4l2");
	fds.reserve(2 + kMaxCaptureChannels);
	while (!g_rt.stop.load(std::memory_order_acquire)) {
		fds.clear();
		fds.push_back({g_rt.wake_fd, POLLIN, 0});
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			if (g_rt.vi_streaming)
				fds.push_back({g_rt.vi.fd(), POLLIN, 0});
			/* Poll a scaler only with an input in flight: an m2m context
			 * with nothing queued reports POLLERR. */
			for (const Channel &c : g_rt.channels)
				if (c.open && c.scaler.is_open() && c.scaler_inflight > 0)
					fds.push_back({c.scaler.fd(), POLLIN | POLLOUT, 0});
		}
		int ret;
		do {
			ret = poll(fds.data(), fds.size(), 250);
		} while (ret < 0 && errno == EINTR);
		drain_fd(g_rt.wake_fd);
		bool error = false;
		for (size_t i = 1; i < fds.size(); ++i)
			error |= (fds[i].revents & (POLLERR | POLLNVAL)) != 0;
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			if (g_rt.vi_streaming)
				handle_vi_locked();
			for (int ch = 0; ch < kMaxCaptureChannels; ++ch)
				if (g_rt.channels[ch].open && g_rt.channels[ch].scaler.is_open())
					handle_scaler_locked(ch);
		}
		if (error)
			usleep(2000);
	}
}

int make_eventfd(int *fd)
{
	if (*fd >= 0)
		return 0;
	*fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	return *fd < 0 ? -errno : 0;
}

int create_events()
{
	int ret = make_eventfd(&g_rt.wake_fd);

	if (ret == 0)
		ret = make_eventfd(&g_rt.user_free_fd);
	for (Channel &c : g_rt.channels)
		if (ret == 0)
			ret = make_eventfd(&c.ready_fd);
	return ret;
}

int start_pipeline()
{
	g_rt.stop.store(false, std::memory_order_release);
	try {
		g_rt.pipeline = std::thread(pipeline_main);
	} catch (const std::system_error &) {
		return -EAGAIN;
	}
	return 0;
}

void stop_pipeline()
{
	if (!g_rt.pipeline.joinable())
		return;
	g_rt.stop.store(true, std::memory_order_release);
	wake_pipeline();
	g_rt.pipeline.join();
}

int open_vi(uint32_t width, uint32_t height)
{
	struct v4l2_format fmt {};
	int ret = g_rt.vi.open(v4l2::kViNode);

	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: open %s failed: %s\n", v4l2::kViNode,
			     std::strerror(-ret));
		return ret;
	}
	ret = g_rt.vi.set_format(kViType, V4L2_PIX_FMT_NV21, width, height, 0, 0, &fmt);
	if (ret == 0 && (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV21 ||
			 fmt.fmt.pix.width != width || fmt.fmt.pix.height != height))
		ret = -EINVAL;
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: VI NV21 %ux%u rejected: %s\n", width, height,
			     std::strerror(-ret));
		return ret;
	}
	(void)g_rt.vi.set_frame_rate(kViType, kDefaultInputFps);
	g_rt.vi_width = width;
	g_rt.vi_height = height;
	g_rt.vi_stride = fmt.fmt.pix.bytesperline;
	return 0;
}

/* Frames orphaned by the last pause free their slots once encoders and
   scalers hand them back, within a frame time or two. */
void wait_for_slots(int needed)
{
	for (int i = 0; i < 50; ++i) {
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			if (kSlots - __builtin_popcount(g_rt.slot_mask) >= needed)
				return;
		}
		usleep(2000);
	}
}

/* VI buffers exist only while VI streams, so idle capture holds no CMA. */
int alloc_vi_buffers()
{
	const int want =
		v4l2::nv_frame_bytes(g_rt.vi_stride, g_rt.vi_height) <= kLargeFrameBytes ? 6 : 4;
	const int count = g_rt.vi.request_buffers(kViType, V4L2_MEMORY_MMAP, want, true);
	int ret = count < 4 ? (count < 0 ? count : -ENOMEM) : 0;
	std::vector<v4l2::MappedBuffer> mems;

	if (ret == 0) {
		wait_for_slots(count);
		ret = g_rt.vi.map_buffers(kViType, count, true, &mems);
		if (ret < 0)
			unmap_unowned(&mems, 0);
	}
	if (ret == 0) {
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (int i = 0; i < count; ++i) {
			Frame *frame = make_frame_locked(Producer::Vi, i, mems[i], g_rt.vi_width,
							 g_rt.vi_height, g_rt.vi_stride, true);
			if (!frame) {
				unmap_unowned(&mems, i);
				ret = -ENOMEM;
				break;
			}
			g_rt.vi_frames.push_back(frame);
			ret = g_rt.vi.queue(kViType, V4L2_MEMORY_MMAP, i, 0, 0, nullptr,
					    kNoCacheHints);
			if (ret < 0) {
				unmap_unowned(&mems, i + 1);
				break;
			}
		}
		if (ret < 0)
			retire_frames_locked(&g_rt.vi_frames);
	}
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: VI buffers unavailable: %s\n", std::strerror(-ret));
		(void)g_rt.vi.request_buffers(kViType, V4L2_MEMORY_MMAP, 0, true);
	}
	return ret;
}

/* After STREAMOFF. Frames an encoder or CPU consumer still holds become
   orphans and are freed on their last release. */
void free_vi_buffers()
{
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		retire_frames_locked(&g_rt.vi_frames);
	}
	(void)g_rt.vi.request_buffers(kViType, V4L2_MEMORY_MMAP, 0, true);
}

void close_vi()
{
	if (g_rt.vi.is_open()) {
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			g_rt.vi_streaming = false;
		}
		(void)g_rt.vi.stream(kViType, false);
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		retire_frames_locked(&g_rt.vi_frames);
	}
	g_rt.vi.close();
	g_rt.last_vi_frame_ns.store(0, std::memory_order_relaxed);
}

/* Negotiate formats; with `start`, also allocate output buffers and stream. */
int open_scaler(Channel &c, int width, int height, bool start)
{
	struct v4l2_format fmt {};
	int ret = c.scaler.open(v4l2::kScalerNode);

	if (ret == 0)
		ret = c.scaler.set_format(kOut, V4L2_PIX_FMT_NV21, g_rt.vi_width, g_rt.vi_height,
					  g_rt.vi_stride, 0, &fmt);
	if (ret == 0 && fmt.fmt.pix_mp.plane_fmt[0].bytesperline != g_rt.vi_stride)
		ret = -EINVAL;
	if (ret == 0)
		ret = c.scaler.set_format(kCap, V4L2_PIX_FMT_NV21, width, height, 0, 0, &fmt);
	if (ret == 0 && (fmt.fmt.pix_mp.width != static_cast<uint32_t>(width) ||
			 fmt.fmt.pix_mp.height != static_cast<uint32_t>(height)))
		ret = -EINVAL;
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: scaler %ux%u -> %dx%d rejected: %s\n",
			     g_rt.vi_width, g_rt.vi_height, width, height, std::strerror(-ret));
		return ret;
	}
	c.stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (c.mirror)
		(void)c.scaler.set_control(V4L2_CID_HFLIP, 1);
	if (c.flip)
		(void)c.scaler.set_control(V4L2_CID_VFLIP, 1);
	if (!start)
		return 0;
	ret = c.scaler.request_buffers(kOut, V4L2_MEMORY_DMABUF, kSlots);
	if (ret < kSlots)
		return ret < 0 ? ret : -ENOMEM;
	const int want = v4l2::nv_frame_bytes(c.stride, height) <= kLargeFrameBytes ? 4 : 3;
	const int count = c.scaler.request_buffers(kCap, V4L2_MEMORY_MMAP, want, true);
	if (count < 2)
		return count < 0 ? count : -ENOMEM;
	std::vector<v4l2::MappedBuffer> mems;
	ret = c.scaler.map_buffers(kCap, count, true, &mems);
	if (ret < 0)
		return ret;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (int i = 0; i < count; ++i) {
			Frame *frame = make_frame_locked(Producer::Scaler, i, mems[i], width, height,
							 c.stride, true);
			if (!frame) {
				unmap_unowned(&mems, i);
				return -ENOMEM;
			}
			frame->channel = &c;
			c.scaled.push_back(frame);
			ret = c.scaler.queue(kCap, V4L2_MEMORY_MMAP, i, 0, 0, nullptr, kNoCacheHints);
			if (ret < 0) {
				unmap_unowned(&mems, i + 1);
				return ret;
			}
		}
	}
	ret = c.scaler.stream(kOut, true);
	if (ret == 0)
		ret = c.scaler.stream(kCap, true);
	return ret;
}

void close_scaler(Channel &c)
{
	if (c.scaler.is_open()) {
		(void)c.scaler.stream(kOut, false);
		(void)c.scaler.stream(kCap, false);
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (Frame *&source : c.scaler_inputs) {
			if (source)
				release_frame_locked(source);
			source = nullptr;
		}
		c.scaler_inflight = 0;
		retire_frames_locked(&c.scaled);
		/* The pipeline thread reads the fd under the lock. */
		c.scaler.close();
	}
}

int destroy_runtime();

/* Wait for the latest frame of `ch` and lend it out. */
int borrow_latest(int ch, int timeout_ms, Frame **out)
{
	Channel &c = g_rt.channels[ch];
	const uint64_t deadline = steady_ns() +
		static_cast<uint64_t>(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;

	for (;;) {
		drain_fd(c.ready_fd);
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			if (!c.open)
				return -1;
			if (c.borrowed)
				return -EBUSY;
			if (c.latest) {
				c.borrowed = c.latest;
				c.latest = nullptr;
				c.borrowed_data = nullptr;
				*out = c.borrowed;
				return 0;
			}
		}
		const uint64_t now = steady_ns();
		if (now >= deadline)
			return -1;
		(void)wait_fd(c.ready_fd, static_cast<int>((deadline - now + 999999) / 1000000));
	}
}

bool valid_capture(int ch)
{
	return ch >= 0 && ch < kMaxCaptureChannels;
}

} // namespace

uint64_t steady_ns()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

void signal_fd(int fd)
{
	if (fd < 0)
		return;
	const uint64_t value = 1;
	while (write(fd, &value, sizeof(value)) < 0 && errno == EINTR) {
	}
}

void drain_fd(int fd)
{
	if (fd < 0)
		return;
	uint64_t value;
	ssize_t n;
	do {
		n = read(fd, &value, sizeof(value));
	} while (n < 0 && errno == EINTR);
}

bool wait_fd(int fd, int timeout_ms)
{
	struct pollfd p = {fd, POLLIN, 0};
	int ret;

	if (fd < 0)
		return false;
	do {
		ret = poll(&p, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	return ret > 0 && (p.revents & POLLIN);
}

void wake_pipeline()
{
	signal_fd(g_rt.wake_fd);
}

void set_thread_name(const char *name)
{
	(void)pthread_setname_np(pthread_self(), name);
}

void release_frame_locked(Frame *frame)
{
	if (!frame || --frame->refs > 0)
		return;
	frame->refs = 0;
	if (frame->orphan) {
		destroy_frame_locked(frame);
		return;
	}
	if (frame->producer == Producer::Staging) {
		signal_fd(g_rt.user_free_fd);
		return;
	}
	requeue_locked(frame);
}

int staging_alloc(StagingSet *set, uint32_t width, uint32_t height, int count)
{
	struct v4l2_format fmt {};
	v4l2::Node node;
	int ret = node.open(v4l2::kScalerNode);

	if (ret == 0)
		ret = node.set_format(kOut, V4L2_PIX_FMT_NV21, width, height, align64(width), 0,
				      &fmt);
	if (ret == 0 && (fmt.fmt.pix_mp.width != width || fmt.fmt.pix_mp.height != height ||
			 fmt.fmt.pix_mp.plane_fmt[0].bytesperline != align64(width)))
		ret = -EINVAL;
	if (ret < 0)
		return ret;
	const int granted = node.request_buffers(kOut, V4L2_MEMORY_MMAP, count);
	if (granted < count)
		return granted < 0 ? granted : -ENOMEM;
	std::vector<v4l2::MappedBuffer> mems;
	ret = node.map_buffers(kOut, granted, true, &mems);
	if (ret < 0)
		return ret;
	std::lock_guard<std::mutex> lock(g_rt.mu);
	for (int i = 0; i < granted; ++i) {
		Frame *frame = make_frame_locked(Producer::Staging, i, mems[i], width, height,
						 align64(width), false);
		if (!frame) {
			unmap_unowned(&mems, i);
			retire_frames_locked(&set->frames);
			return -ENOMEM;
		}
		set->frames.push_back(frame);
	}
	set->width = width;
	set->height = height;
	/* Closing the idle context leaves the mapped, exported buffers alive. */
	return 0;
}

void staging_free(StagingSet *set)
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	retire_frames_locked(&set->frames);
	set->width = set->height = 0;
}

Frame *staging_take_locked(StagingSet *set)
{
	for (Frame *frame : set->frames)
		if (frame->refs == 0 && !frame->orphan)
			return frame;
	return nullptr;
}

void copy_tight_nv21(Frame *dst, const uint8_t *src)
{
	const uint32_t w = dst->width, h = dst->height, stride = dst->stride;
	uint8_t *out = dst->mem.addr;

	if (stride == w) {
		std::memcpy(out, src, static_cast<size_t>(w) * h * 3 / 2);
		return;
	}
	for (uint32_t row = 0; row < h + h / 2; ++row)
		std::memcpy(out + static_cast<size_t>(row) * stride,
			    src + static_cast<size_t>(row) * w, w);
}

void sync_for_cpu(Frame *frame)
{
	if (!frame->cached || frame->mem.dmabuf < 0)
		return;
	struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	int ret;
	do {
		ret = ioctl(frame->mem.dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
	} while (ret < 0 && errno == EINTR);
}

Frame *find_borrowed_frame_locked(const void *data, int *channel)
{
	for (int ch = 0; ch < kMaxCaptureChannels; ++ch) {
		Channel &c = g_rt.channels[ch];
		if (c.borrowed && data && c.borrowed_data == data) {
			if (channel)
				*channel = ch;
			return c.borrowed;
		}
	}
	return nullptr;
}

namespace {

int destroy_runtime()
{
	(void)close_all_h26x_encoders();
	int failed = close_jpeg_encoder(0) != 0;
	failed |= close_all_capture_channels() != 0;
	if (failed) {
		g_rt.teardown_failed = true;
		g_rt.reference_count = 1;
		return -1;
	}
	(void)stop_capture_pipeline();
	stop_pipeline();
	close_vi();
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		g_rt.user_frames = false;
	}
	g_rt.jpeg_capture_channel = -1;
	g_rt.jpeg_capture_owned = false;
	g_rt.teardown_failed = false;
	g_rt.reference_count = 0;
	std::fprintf(stderr, "OneKVM: V4L2 runtime stopped\n");
	return 0;
}

} // namespace
} // namespace rt

using rt::Channel;
using rt::Frame;
using rt::g_rt;

int reclaim_stale_runtime(void)
{
	/* V4L2 sessions end with the process that owned them; there is no
	   multimedia state to reclaim. Only check that the capture node exists. */
	if (g_rt.reference_count != 0 || g_rt.stale_checked)
		return 0;
	v4l2::Node probe;
	const int ret = probe.open(v4l2::kViNode);
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: %s node unavailable: %s\n", v4l2::kViNode,
			     std::strerror(-ret));
		return -1;
	}
	g_rt.stale_checked = true;
	return 0;
}

int initialize(void)
{
	if (g_rt.teardown_failed && rt::destroy_runtime() != 0)
		return -EBUSY;
	if (g_rt.reference_count > 0) {
		++g_rt.reference_count;
		return 0;
	}
	if (reclaim_stale_runtime() != 0)
		return -1;
	uint32_t width = 0, height = 0;
	onekvm_lt6911_get_active_size(&width, &height);
	if (width == 0 || height == 0 || rt::create_events() != 0)
		return -1;
	int ret = rt::open_vi(width, height);
	if (ret == 0)
		ret = rt::start_pipeline();
	if (ret < 0) {
		rt::stop_pipeline();
		rt::close_vi();
		return -1;
	}
	g_rt.reference_count = 1;
	std::fprintf(stderr, "OneKVM: V4L2 runtime up, VI NV21 %ux%u stride %u\n",
		     g_rt.vi_width, g_rt.vi_height, g_rt.vi_stride);
	return 0;
}

int shutdown(void)
{
	if (g_rt.reference_count <= 0)
		return 0;
	if (--g_rt.reference_count > 0)
		return 0;
	return rt::destroy_runtime();
}

int find_free_capture_channel(void)
{
	for (int ch = 0; ch < kMaxCaptureChannels; ++ch)
		if (!g_rt.channels[ch].open)
			return ch;
	return -1;
}

int start_capture_pipeline(void)
{
	return g_rt.reference_count > 0 ? 0 : -1;
}

int stop_capture_pipeline(void)
{
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		g_rt.user_frames = false;
	}
	rt::staging_free(&g_rt.user);
	g_rt.user_last_source = nullptr;
	return 0;
}

int open_capture_channel(int ch, int width, int height, int format, int fps, int input_fps)
{
	if (g_rt.reference_count == 0 || !rt::valid_capture(ch) || width <= 0 || height <= 0 ||
	    (width & 1) || (height & 1) || fps < 1 || fps > kMaxOutputFps ||
	    format != rt::kNv21)
		return -1;
	Channel &c = g_rt.channels[ch];
	if (c.open)
		return -1;
	if (input_fps < 1)
		input_fps = kDefaultInputFps;
	const bool passthrough = static_cast<uint32_t>(width) == g_rt.vi_width &&
				 static_cast<uint32_t>(height) == g_rt.vi_height &&
				 !c.mirror && !c.flip;
	if (passthrough) {
		c.stride = g_rt.vi_stride;
	} else {
		/* Check the scaling now; the context itself starts with a consumer. */
		const int ret = rt::open_scaler(c, width, height, false);
		rt::close_scaler(c);
		if (ret != 0)
			return -1;
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		c.open = true;
		c.running = false;
		c.passthrough = passthrough;
		c.width = width;
		c.height = height;
		c.fps = fps;
		c.input_fps = input_fps;
		c.delivered = c.dropped = 0;
	}
	/* Nothing flows until a consumer resumes the channel. */
	(void)pause_vi_dma();
	std::fprintf(stderr, "OneKVM: capture channel %d %dx%d %s, %d of %d fps\n", ch, width,
		     height, passthrough ? "direct" : "scaled", fps, input_fps);
	return 0;
}

int close_capture_channel(int ch)
{
	if (!rt::valid_capture(ch))
		return -1;
	Channel &c = g_rt.channels[ch];
	if (!c.open)
		return 0;
	if (g_rt.jpeg_capture_channel == ch && close_jpeg_encoder(0) != 0)
		return -EBUSY;
	if (release_capture_frame(ch) != 0)
		return -EBUSY;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		c.open = false;
		c.running = false;
		if (c.latest)
			rt::release_frame_locked(c.latest);
		c.latest = nullptr;
	}
	rt::close_scaler(c);
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		c.passthrough = true;
		c.width = c.height = 0;
		c.stride = 0;
	}
	return 0;
}

int close_all_capture_channels(void)
{
	for (int ch = 0; ch < kMaxCaptureChannels; ++ch)
		if (close_capture_channel(ch) != 0)
			return -1;
	return 0;
}

int reset_capture_channel(int ch, int width, int height, int format, int fps, int input_fps)
{
	const int target = vpss_phy_channel(width);

	if (ch != target && close_capture_channel(ch) != 0)
		return -1;
	if (close_capture_channel(target) != 0)
		return -1;
	return open_capture_channel(target, width, height, format, fps, input_fps);
}

int capture_input_fps(void)
{
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (!g_rt.vi_streaming)
			return 0;
	}
	const uint64_t last = g_rt.last_vi_frame_ns.load(std::memory_order_relaxed);
	if (last == 0 || rt::steady_ns() - last > 1500000000ull)
		return 0;
	const int fps = g_rt.vi_fps.load(std::memory_order_relaxed);
	if (fps < 10 || fps > 240)
		return 0;
	return onekvm::normalize_capture_fps(fps);
}

int update_capture_frame_rate(int ch, int output_fps, int input_fps)
{
	if (!rt::valid_capture(ch) || output_fps < 1 || input_fps < 1)
		return -1;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		Channel &c = g_rt.channels[ch];
		if (!c.open)
			return -1;
		c.fps = output_fps;
		c.input_fps = input_fps;
	}
	set_h26x_capture_input_fps(ch, input_fps);
	return 0;
}

bool hevc_vi_direct_supported()
{
	return false;
}

bool capture_channel_open(int ch)
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	return rt::valid_capture(ch) && g_rt.channels[ch].open;
}

bool capture_channel_running(int ch)
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	return rt::valid_capture(ch) && g_rt.channels[ch].running;
}

namespace {

/* A paused scaled channel gives its soph-vpss context and buffers back. */
void idle_channel(Channel &c)
{
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		c.running = false;
		if (c.latest)
			rt::release_frame_locked(c.latest);
		c.latest = nullptr;
	}
	if (!c.passthrough && c.scaler.is_open())
		rt::close_scaler(c);
}

} // namespace

int pause_vpss_channel(int ch)
{
	if (!rt::valid_capture(ch))
		return -1;
	Channel &c = g_rt.channels[ch];
	if (!c.open)
		return 0;
	idle_channel(c);
	return 0;
}

int resume_vpss_channel(int ch)
{
	if (!rt::valid_capture(ch))
		return -1;
	Channel &c = g_rt.channels[ch];
	if (!c.open)
		return -1;
	if (!c.passthrough && !c.scaler.is_open() &&
	    rt::open_scaler(c, c.width, c.height, true) != 0) {
		rt::close_scaler(c);
		return -1;
	}
	std::lock_guard<std::mutex> lock(g_rt.mu);
	c.running = true;
	return 0;
}

namespace {

int start_vi(bool arm_csi)
{
	bool allocate;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (!g_rt.vi.is_open())
			return -1;
		if (g_rt.vi_streaming)
			return 0;
		allocate = g_rt.vi_frames.empty();
	}
	if (allocate) {
		const int ret = rt::alloc_vi_buffers();
		if (ret < 0)
			return ret;
	}
	/* VI stop drops the bridge's CSI transmitter; arm it before restarting. */
	if (arm_csi && lt6911_start_csi() != 0)
		std::fprintf(stderr, "OneKVM: LT6911 CSI arm failed before VI start\n");
	const int ret = g_rt.vi.stream(V4L2_BUF_TYPE_VIDEO_CAPTURE, true);
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: VI STREAMON failed: %s\n", std::strerror(-ret));
		return ret;
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		g_rt.vi_streaming = true;
		g_rt.rate_window_ns = 0;
		g_rt.vi_fps.store(0, std::memory_order_relaxed);
	}
	rt::wake_pipeline();
	return 0;
}

} // namespace

int enable_vi_dma(void)
{
	return start_vi(true);
}

int enable_vi_dma_for_teardown(void)
{
	return start_vi(false);
}

int pause_vi_dma(void)
{
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (!g_rt.vi_streaming)
			return 0;
		for (const Channel &c : g_rt.channels)
			if (c.borrowed)
				return 0;
		g_rt.vi_streaming = false;
		/* A frame from before the pause must not look current later. */
		for (Channel &c : g_rt.channels) {
			if (c.latest)
				rt::release_frame_locked(c.latest);
			c.latest = nullptr;
		}
	}
	const int ret = g_rt.vi.stream(V4L2_BUF_TYPE_VIDEO_CAPTURE, false);
	if (ret == 0) {
		rt::free_vi_buffers();
		rt::release_idle_encoder_inputs();
		rt::release_idle_jpeg_inputs();
	}
	g_rt.last_vi_frame_ns.store(0, std::memory_order_relaxed);
	return ret < 0 ? ret : 0;
}

int resume_vi_dma(void)
{
	if (yield_h26x_vi_direct() != 0)
		return -1;
	const int ret = enable_vi_dma();
	if (ret != 0)
		return ret;
	return capture_use_vi_frames();
}

bool vi_dma_running(void)
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	return g_rt.vi_streaming;
}

void park_unbound_vpss_channels()
{
	bool park[kMaxCaptureChannels] = {};
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (int ch = 0; ch < kMaxCaptureChannels; ++ch) {
			const Channel &c = g_rt.channels[ch];
			park[ch] = c.open && c.running && !c.borrowed &&
				   !rt::capture_has_bound_encoder_locked(ch);
		}
	}
	for (int ch = 0; ch < kMaxCaptureChannels; ++ch)
		if (park[ch])
			idle_channel(g_rt.channels[ch]);
}

void set_capture_mirror(int ch, bool en)
{
	if (rt::valid_capture(ch))
		g_rt.channels[ch].mirror = en;
}

void set_capture_flip(int ch, bool en)
{
	if (rt::valid_capture(ch))
		g_rt.channels[ch].flip = en;
}

int acquire_capture_frame_timeout(int ch, void **data, int *len, int *width, int *height,
				  int *format, int timeout_ms)
{
	if (!rt::valid_capture(ch) || !data || !len || !width || !height || !format ||
	    !capture_channel_open(ch))
		return -1;
	if (g_rt.jpeg_capture_channel >= 0 && close_jpeg_encoder(0) != 0)
		return -EBUSY;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (g_rt.channels[ch].borrowed)
			return -EBUSY;
	}
	if (resume_vi_dma() != 0 || resume_vpss_channel(ch) != 0)
		return -1;
	Frame *frame = nullptr;
	if (rt::borrow_latest(ch, timeout_ms, &frame) != 0)
		return -1;
	Channel &c = g_rt.channels[ch];
	rt::sync_for_cpu(frame);
	const size_t bytes = static_cast<size_t>(frame->width) * frame->height * 3 / 2;
	const uint8_t *pixels = frame->mem.addr;
	if (frame->stride != frame->width) {
		/* CPU consumers take tightly packed NV21. */
		try {
			c.tight.resize(bytes);
		} catch (const std::bad_alloc &) {
			(void)release_capture_frame(ch);
			return -1;
		}
		for (uint32_t row = 0; row < frame->height + frame->height / 2; ++row)
			std::memcpy(c.tight.data() + static_cast<size_t>(row) * frame->width,
				    frame->mem.addr + static_cast<size_t>(row) * frame->stride,
				    frame->width);
		pixels = c.tight.data();
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		c.borrowed_data = pixels;
	}
	*data = const_cast<uint8_t *>(pixels);
	*len = static_cast<int>(bytes);
	*width = static_cast<int>(frame->width);
	*height = static_cast<int>(frame->height);
	*format = rt::kNv21;
	return 0;
}

int acquire_capture_frame(int ch, void **data, int *len, int *width, int *height, int *format)
{
	return acquire_capture_frame_timeout(ch, data, len, width, height, format, 1000);
}

int acquire_capture_frame_for_encoder_timeout(int ch, int *width, int *height, int *format,
					      int timeout_ms)
{
	if (!rt::valid_capture(ch) || !width || !height || !format || timeout_ms < 0)
		return -EINVAL;
	if (resume_vi_dma() != 0 || resume_vpss_channel(ch) != 0)
		return -1;
	return read_capture_frame_for_encoder_timeout(ch, width, height, format, timeout_ms);
}

int read_capture_frame_for_encoder_timeout(int ch, int *width, int *height, int *format,
					   int timeout_ms)
{
	if (!rt::valid_capture(ch) || !width || !height || !format)
		return -EINVAL;
	if (g_rt.jpeg_capture_channel >= 0 && close_jpeg_encoder(0) != 0)
		return -EBUSY;
	Frame *frame = nullptr;
	const int ret = rt::borrow_latest(ch, timeout_ms, &frame);
	if (ret != 0)
		return ret == -EBUSY ? -EBUSY : -1;
	g_rt.jpeg_capture_channel = ch;
	g_rt.jpeg_capture_owned = true;
	*width = static_cast<int>(frame->width);
	*height = static_cast<int>(frame->height);
	*format = rt::kNv21;
	return 0;
}

int release_capture_frame(int ch)
{
	if (!rt::valid_capture(ch))
		return -EINVAL;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		Channel &c = g_rt.channels[ch];
		if (!c.open)
			return -EINVAL;
		if (!c.borrowed)
			return 0;
		/* An encoder still reading it holds its own reference. */
		rt::release_frame_locked(c.borrowed);
		c.borrowed = nullptr;
		c.borrowed_data = nullptr;
	}
	if (g_rt.jpeg_capture_channel == ch) {
		g_rt.jpeg_capture_channel = -1;
		g_rt.jpeg_capture_owned = false;
	}
	return 0;
}

int vpss_input_width()
{
	return static_cast<int>(g_rt.vi_width);
}

int vpss_input_height()
{
	return static_cast<int>(g_rt.vi_height);
}

int capture_use_user_frames()
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	g_rt.user_frames = true;
	return 0;
}

bool capture_uses_user_frames()
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	return g_rt.user_frames;
}

int capture_use_vi_frames()
{
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (!g_rt.user_frames)
			return 0;
		g_rt.user_frames = false;
	}
	rt::staging_free(&g_rt.user);
	g_rt.user_last_source = nullptr;
	return 0;
}

int submit_vpss_nv21(const uint8_t *nv21, int width, int height, bool content_changed)
{
	if (!nv21 || width <= 0 || height <= 0 || (width & 1) ||
	    static_cast<uint32_t>(width) != g_rt.vi_width ||
	    static_cast<uint32_t>(height) != g_rt.vi_height)
		return -1;
	if (g_rt.user.frames.empty()) {
		const uint64_t now = rt::steady_ns();
		if (g_rt.user_alloc_failed_ns != 0 &&
		    now - g_rt.user_alloc_failed_ns < 2000000000ull)
			return -1;
		const int ret = rt::staging_alloc(&g_rt.user, width, height, 2);
		if (ret != 0) {
			rt::staging_free(&g_rt.user);
			g_rt.user_alloc_failed_ns = now;
			std::fprintf(stderr, "OneKVM: placeholder frames unavailable: %s\n",
				     std::strerror(-ret));
			return -1;
		}
		g_rt.user_alloc_failed_ns = 0;
	}
	if (content_changed || nv21 != g_rt.user_last_source) {
		++g_rt.user_generation;
		g_rt.user_last_source = nv21;
	}
	const uint64_t deadline = rt::steady_ns() + 1000000000ull;
	Frame *frame = nullptr;
	for (;;) {
		rt::drain_fd(g_rt.user_free_fd);
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			frame = rt::staging_take_locked(&g_rt.user);
			if (frame)
				++frame->refs;
		}
		if (frame)
			break;
		const uint64_t now = rt::steady_ns();
		if (now >= deadline)
			return -ETIMEDOUT;
		(void)rt::wait_fd(g_rt.user_free_fd,
				  static_cast<int>((deadline - now + 999999) / 1000000));
	}
	if (frame->source != nv21 || frame->source_generation != g_rt.user_generation) {
		rt::copy_tight_nv21(frame, nv21);
		frame->source = nv21;
		frame->source_generation = g_rt.user_generation;
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		frame->timestamp_ns = v4l2::monotonic_ns();
		frame->sequence = g_rt.user_sequence++;
		for (int ch = 0; ch < kMaxCaptureChannels; ++ch)
			rt::route_locked(ch, frame);
		rt::release_frame_locked(frame);
	}
	/* A scaler input changes the pipeline's poll set. */
	rt::wake_pipeline();
	return 0;
}

bool runtime_initialized()
{
	return g_rt.reference_count != 0;
}

int jpeg_capture_channel()
{
	return g_rt.jpeg_capture_channel;
}

} // namespace onekvm::mmf

namespace {
std::atomic<uint32_t> g_active_width{1920};
std::atomic<uint32_t> g_active_height{1080};
} // namespace

extern "C" int onekvm_lt6911_set_active_size(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0 || width > 4096 || height > 2160 || (width & 1u) ||
	    (height & 1u))
		return -1;
	g_active_width.store(width, std::memory_order_relaxed);
	g_active_height.store(height, std::memory_order_relaxed);
	return 0;
}

extern "C" void onekvm_lt6911_get_active_size(uint32_t *width, uint32_t *height)
{
	if (width)
		*width = g_active_width.load(std::memory_order_relaxed);
	if (height)
		*height = g_active_height.load(std::memory_order_relaxed);
}
