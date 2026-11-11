// V4L2 runtime: H.264/H.265 encoding on soph-venc. A bound encoder takes
// capture-channel frames as DMA-BUFs from the pipeline thread; its reader
// thread is the only bitstream dequeuer and keeps the queue/IDR rules of the
// cvi_mpi runtime (see mmf_h26x.cpp). The manual path copies CPU frames into
// staging buffers.

#include "v4l2_runtime.hpp"
#include "h264_annexb.hpp"
#include "packet-buffer-pool.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

namespace onekvm::mmf {
namespace {

using rt::Frame;
using rt::g_rt;

constexpr uint32_t kOut = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr uint32_t kCap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
/* Absorbs short consumer stalls without back-pressuring the encoder; on
   overflow the chain is dropped and an IDR requested. */
constexpr std::size_t kReaderQueueMax = 16;
constexpr std::size_t kReaderPacketCapacity = 1024 * 1024;
constexpr int kBitstreamBuffers = 4;
constexpr int kDefaultMinQp = 12;
constexpr int kDefaultMaxQp = 48;
constexpr int kDefaultInitialQp = 22;
constexpr uint32_t kRcFpsGop = 1u << 0;
constexpr uint32_t kRcBitrate = 1u << 1;
constexpr uint32_t kRcQp = 1u << 2;

struct QueuedPacket {
	uint64_t encoded_ns = 0;
	std::vector<uint8_t> data;
	bool key_frame = false;
};

struct Reader {
	std::thread thread;
	std::atomic<bool> stop{false};
	std::atomic<bool> running{false};
	std::atomic<bool> want_idr{false};
	std::atomic<bool> force_idr{false};
	std::atomic<bool> idr_sent{false};
	std::atomic<bool> discard{false};
	std::atomic<int> pending_output_fps{0};
	std::atomic<int> pending_gop{0};
	std::atomic<int> pending_bitrate_kbps{0};
	std::atomic<int> pending_initial_qp{0};
	std::atomic<int> pending_min_qp{0};
	std::atomic<int> pending_max_qp{0};
	std::atomic<uint32_t> pending_rc_mask{0};
	std::atomic<uint64_t> last_packet_ns{0};
	std::atomic<uint64_t> last_drain_ns{0};
	std::atomic<int> read_error{0};
	std::atomic<int> ready_event_fd{-1};
	std::atomic<int> input_fps{0};
	int wake_fd = -1;
	std::mutex mu;
	std::mutex join_mu;
	std::deque<QueuedPacket> queue;
	std::shared_ptr<onekvm::EncodedPacketPool> packet_pool;
};

struct PendingFrame {
	uint64_t ts_us = 0;
	uint64_t queued_ns = 0;
	uint64_t capture_ns = 0;
};

struct Session {
	bool initialized = false;
	H26xEncoderConfig cfg{};
	RateControl rc{};
	v4l2::Node node;
	std::vector<v4l2::MappedBuffer> bitstream;
	uint32_t stride = 0;
	std::atomic<bool> suspended{false}; // OUTPUT stopped, no VENC channel
	// g_rt.mu
	std::array<Frame *, rt::kSlots> inflight{};
	int inflight_count = 0;
	bool bound = false;
	int capture = -1;
	std::array<PendingFrame, 8> pending{};
	unsigned pending_next = 0;
	uint64_t dropped = 0;
	uint64_t input_errors = 0;
	bool geometry_warned = false;
	// control thread
	bool bound_once = false;
	bool packet_pending = false;
	int held = -1;
	rt::StagingSet staging;
	uint64_t last_submit_ns = 0;
	// lock-free getters
	std::atomic<uint64_t> last_encode_ns{0};
	std::atomic<uint64_t> last_capture_ns{0};
	std::atomic<uint32_t> last_enc_fps{0};
};

Reader g_readers[rt::kEncoderChannels];
Session g_sessions[rt::kEncoderChannels];
bool g_h26x_active = false;

bool valid_channel(int ch)
{
	return ch >= 0 && ch < rt::kEncoderChannels;
}

uint32_t align64(uint32_t value)
{
	return (value + 63u) & ~63u;
}

void notify_ready(Reader &reader)
{
	rt::signal_fd(reader.ready_event_fd.load(std::memory_order_acquire));
}

void wake_reader(Reader &reader)
{
	rt::signal_fd(reader.wake_fd);
}

void notify(Reader &reader)
{
	notify_ready(reader);
	wake_reader(reader);
}

void ensure_events(Reader &reader)
{
	if (reader.wake_fd < 0)
		reader.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (reader.ready_event_fd.load(std::memory_order_acquire) >= 0)
		return;
	const int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0) {
		std::fprintf(stderr, "OneKVM: eventfd for VENC reader unavailable: %s\n",
			     std::strerror(errno));
		return;
	}
	reader.ready_event_fd.store(fd, std::memory_order_release);
}

void recycle_buffer(Reader &reader, std::vector<uint8_t> &&buffer)
{
	if (reader.packet_pool)
		reader.packet_pool->put(std::move(buffer));
}

std::vector<uint8_t> take_buffer(Reader &reader)
{
	return reader.packet_pool ? reader.packet_pool->take() : std::vector<uint8_t>{};
}

void clear_queue(Reader &reader)
{
	while (!reader.queue.empty()) {
		recycle_buffer(reader, std::move(reader.queue.front().data));
		reader.queue.pop_front();
	}
}

void reap_outputs_locked(Session &s)
{
	struct v4l2_buffer b;
	struct v4l2_plane p;

	while (s.node.dequeue(kOut, V4L2_MEMORY_DMABUF, &b, &p) == 0) {
		if (b.index < static_cast<uint32_t>(rt::kSlots)) {
			Frame *frame = s.inflight[b.index];
			s.inflight[b.index] = nullptr;
			if (frame) {
				--s.inflight_count;
				rt::release_frame_locked(frame);
			}
		}
		if ((b.flags & V4L2_BUF_FLAG_ERROR) && s.input_errors++ % 60 == 0)
			std::fprintf(stderr, "OneKVM: VENC dropped a failed frame (%llu so far)\n",
				     static_cast<unsigned long long>(s.input_errors));
	}
}

void record_pending_locked(Session &s, const Frame *frame, uint64_t queued_ns, int input_fps)
{
	PendingFrame &p = s.pending[s.pending_next++ % s.pending.size()];
	const uint64_t period = input_fps > 0 ? 1000000000ull / static_cast<unsigned>(input_fps) : 0;

	p.ts_us = frame->timestamp_ns / 1000;
	p.queued_ns = queued_ns;
	p.capture_ns = period + (queued_ns > frame->timestamp_ns ? queued_ns - frame->timestamp_ns : 0);
}

void record_done_locked(Session &s, uint64_t ts_us, uint64_t done_ns)
{
	for (PendingFrame &p : s.pending) {
		if (p.queued_ns == 0 || p.ts_us != ts_us)
			continue;
		s.last_encode_ns.store(done_ns > p.queued_ns ? done_ns - p.queued_ns : 0,
				       std::memory_order_relaxed);
		if (p.capture_ns)
			s.last_capture_ns.store(p.capture_ns, std::memory_order_relaxed);
		p.queued_ns = 0;
		return;
	}
}

int qp_control(const Session &s, int which)
{
	static const uint32_t ids[2][3] = {
		{V4L2_CID_MPEG_VIDEO_H264_MIN_QP, V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
		 V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP},
		{V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
		 V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP},
	};
	return static_cast<int>(ids[s.cfg.codec == H26xCodec::H265][which]);
}

int apply_qp(Session &s, const RateControl &rc)
{
	const int min_qp = rc.min_qp > 0 ? rc.min_qp : kDefaultMinQp;
	const int max_qp = rc.max_qp > 0 ? rc.max_qp : kDefaultMaxQp;
	const int initial_qp = rc.initial_qp > 0 ? rc.initial_qp : kDefaultInitialQp;

	if (min_qp > max_qp)
		return -EINVAL;
	int ret = s.node.set_control(qp_control(s, 0), min_qp);
	if (ret == 0)
		ret = s.node.set_control(qp_control(s, 1), max_qp);
	if (ret == 0)
		ret = s.node.set_control(qp_control(s, 2), initial_qp);
	return ret;
}

/* Reader thread, between dequeues. 1: frame timing changed. */
int apply_rate_control(int ch, uint32_t mask)
{
	Reader &r = g_readers[ch];
	Session &s = g_sessions[ch];
	H26xEncoderConfig &cfg = s.cfg;
	int fps = cfg.output_fps, gop = cfg.gop, kbps = cfg.bitrate_kbps;
	bool timing = false, changed = false;

	if (mask & kRcFpsGop) {
		fps = clamp_output_fps(r.pending_output_fps.load(std::memory_order_relaxed),
				       cfg.width, cfg.height);
		gop = r.pending_gop.load(std::memory_order_relaxed);
		if (fps <= 0)
			return -1;
		if (gop <= 0)
			gop = default_gop_frames(fps);
	}
	if (mask & kRcBitrate) {
		kbps = r.pending_bitrate_kbps.load(std::memory_order_relaxed);
		if (kbps <= 0)
			return -1;
	}
	if (fps != cfg.output_fps && s.node.set_frame_rate(kOut, fps) == 0) {
		cfg.output_fps = fps;
		timing = true;
	}
	if (gop != cfg.gop && s.node.set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop) == 0) {
		cfg.gop = gop;
		timing = true;
	}
	if (kbps != cfg.bitrate_kbps &&
	    s.node.set_control(V4L2_CID_MPEG_VIDEO_BITRATE, kbps * 1000) == 0) {
		cfg.bitrate_kbps = kbps;
		changed = true;
	}
	if (timing || changed)
		std::printf("OneKVM: VENC ch %d fps %d gop %d bitrate %d kbps\n", ch, cfg.output_fps,
			    cfg.gop, cfg.bitrate_kbps);
	if (mask & kRcQp) {
		RateControl rc{};
		rc.initial_qp = r.pending_initial_qp.load(std::memory_order_relaxed);
		rc.min_qp = r.pending_min_qp.load(std::memory_order_relaxed);
		rc.max_qp = r.pending_max_qp.load(std::memory_order_relaxed);
		if ((rc.initial_qp != s.rc.initial_qp || rc.min_qp != s.rc.min_qp ||
		     rc.max_qp != s.rc.max_qp) && apply_qp(s, rc) == 0)
			s.rc = rc;
	}
	return timing ? 1 : 0;
}

void issue_idr(int ch)
{
	if (g_sessions[ch].node.set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0) == 0)
		g_readers[ch].idr_sent.store(true, std::memory_order_release);
	else
		std::printf("OneKVM: reader IDR request failed on ch %d\n", ch);
}

void control_step(int ch)
{
	Reader &self = g_readers[ch];

	if (self.force_idr.exchange(false, std::memory_order_acq_rel)) {
		self.want_idr.store(true, std::memory_order_relaxed);
		self.idr_sent.store(false, std::memory_order_relaxed);
	}
	bool overrun = false;
	{
		std::lock_guard<std::mutex> lock(self.mu);
		if (self.queue.size() >= kReaderQueueMax) {
			clear_queue(self);
			self.want_idr.store(true, std::memory_order_relaxed);
			overrun = true;
		}
	}
	if (overrun)
		self.idr_sent.store(false, std::memory_order_relaxed);
	const uint32_t mask = self.pending_rc_mask.exchange(0, std::memory_order_acq_rel);
	if (mask != 0 && apply_rate_control(ch, mask) > 0)
		self.want_idr.store(true, std::memory_order_relaxed);
	if (!self.want_idr.load(std::memory_order_relaxed))
		self.idr_sent.store(false, std::memory_order_relaxed);
	else if (!self.idr_sent.load(std::memory_order_relaxed))
		issue_idr(ch);
}

void reader_failed(Reader &self, int error)
{
	{
		std::lock_guard<std::mutex> lock(self.mu);
		clear_queue(self);
	}
	self.read_error.store(error, std::memory_order_release);
	self.want_idr.store(true, std::memory_order_relaxed);
	self.idr_sent.store(false, std::memory_order_relaxed);
	notify_ready(self);
}

void reader_main(int ch, bool request_idr)
{
	Reader &self = g_readers[ch];
	Session &s = g_sessions[ch];
	const bool h265 = s.cfg.codec == H26xCodec::H265;
	uint64_t window_start = 0;
	uint32_t window_frames = 0;

	try {
		std::vector<uint8_t> scratch = take_buffer(self);
		AnnexBParameterSets parameter_sets;

		if (request_idr)
			issue_idr(ch);
		while (!self.stop.load(std::memory_order_acquire)) {
			try {
				const bool discard = self.discard.load(std::memory_order_acquire);
				if (!discard)
					control_step(ch);
				/* A suspended session has nothing to poll for. */
				const bool suspended = s.suspended.load(std::memory_order_acquire);
				struct pollfd fds[2] = {
					{suspended ? -1 : s.node.fd(), POLLIN | POLLOUT, 0},
					{self.wake_fd, POLLIN, 0},
				};
				int ret;
				do {
					ret = poll(fds, 2, discard ? 250 : 100);
				} while (ret < 0 && errno == EINTR);
				rt::drain_fd(self.wake_fd);
				{
					std::lock_guard<std::mutex> lock(g_rt.mu);
					reap_outputs_locked(s);
				}
				int error = 0;
				for (;;) {
					struct v4l2_buffer b;
					struct v4l2_plane p;
					const int got = s.node.dequeue(kCap, V4L2_MEMORY_MMAP, &b, &p);
					if (got == -EAGAIN)
						break;
					if (got < 0) {
						error = got;
						break;
					}
					const uint64_t encoded_ns = rt::steady_ns();
					const uint64_t done_ns = v4l2::monotonic_ns();
					const bool ok = !(b.flags & V4L2_BUF_FLAG_ERROR) &&
							b.bytesused > p.data_offset &&
							b.index < s.bitstream.size();
					std::size_t size = 0;
					if (ok) {
						size = b.bytesused - p.data_offset;
						if (scratch.capacity() == 0)
							scratch = take_buffer(self);
						const uint8_t *data = s.bitstream[b.index].addr + p.data_offset;
						scratch.assign(data, data + size);
					}
					(void)s.node.queue(kCap, V4L2_MEMORY_MMAP, b.index, 0, 0);
					if (!ok)
						continue;
					{
						std::lock_guard<std::mutex> lock(g_rt.mu);
						record_done_locked(s, v4l2::timeval_ns(b.timestamp) / 1000,
								   done_ns);
					}
					if (window_start == 0) {
						window_start = encoded_ns;
						window_frames = 0;
					} else if (++window_frames, encoded_ns - window_start >= 1000000000ull) {
						const uint64_t elapsed = encoded_ns - window_start;
						s.last_enc_fps.store(static_cast<uint32_t>(
							(window_frames * 1000000000ull + elapsed / 2) / elapsed),
							std::memory_order_relaxed);
						window_start = encoded_ns;
						window_frames = 0;
					}
					self.last_drain_ns.store(encoded_ns, std::memory_order_release);
					if (self.discard.load(std::memory_order_acquire))
						continue;
					const uint8_t *data = scratch.data();
					parameter_sets.update(data, size, h265);
					const bool key = (b.flags & V4L2_BUF_FLAG_KEYFRAME) ||
						(h265 ? annexb_has_h265_irap(data, size)
						      : annexb_has_idr(data, size));
					if (self.last_packet_ns.load(std::memory_order_relaxed) == 0)
						std::printf("OneKVM: VENC reader got first %zu-byte AU on ch %d\n",
							    size, ch);
					// Keep parameter sets above, but never publish a broken P chain.
					if (self.want_idr.load(std::memory_order_relaxed) && !key)
						continue;
					self.read_error.store(0, std::memory_order_release);
					QueuedPacket packet;
					packet.key_frame = key;
					packet.encoded_ns = encoded_ns;
					if (key && parameter_sets.needs_parameter_prefix(data, size, h265))
						packet.data = parameter_sets.augment_keyframe(data, size, h265);
					else
						packet.data = std::move(scratch);
					bool overflow = false;
					{
						std::lock_guard<std::mutex> lock(self.mu);
						if (self.queue.size() >= kReaderQueueMax) {
							overflow = true;
							recycle_buffer(self, std::move(packet.data));
						} else {
							self.queue.push_back(std::move(packet));
							if (key) {
								self.want_idr.store(false, std::memory_order_relaxed);
								self.idr_sent.store(false, std::memory_order_relaxed);
							}
							self.last_packet_ns.store(rt::steady_ns(),
										  std::memory_order_relaxed);
						}
					}
					if (overflow) {
						self.want_idr.store(true, std::memory_order_relaxed);
						self.idr_sent.store(false, std::memory_order_relaxed);
					} else {
						notify_ready(self);
					}
				}
				if (error != 0) {
					if (!self.discard.load(std::memory_order_acquire))
						reader_failed(self, error);
					usleep(5000);
				} else if (ret > 0 && (fds[0].revents & (POLLERR | POLLNVAL))) {
					usleep(5000);
				}
			} catch (const std::bad_alloc &) {
				reader_failed(self, -ENOMEM);
				usleep(5000);
			}
		}
	} catch (const std::bad_alloc &) {
		self.read_error.store(-ENOMEM, std::memory_order_release);
		notify_ready(self);
	}
	self.running.store(false, std::memory_order_release);
}

int start_session(Session &s, const H26xEncoderConfig &cfg, const RateControl &rc)
{
	const uint32_t w = static_cast<uint32_t>(cfg.width), h = static_cast<uint32_t>(cfg.height);
	struct v4l2_format fmt {};
	int ret = s.node.open(v4l2::kEncoderNode);

	if (ret == 0)
		ret = s.node.set_format(kCap, cfg.codec == H26xCodec::H265 ? V4L2_PIX_FMT_HEVC
									     : V4L2_PIX_FMT_H264,
					w, h, 0, 0, &fmt);
	if (ret == 0)
		ret = s.node.set_format(kOut, V4L2_PIX_FMT_NV21, w, h, align64(w), 0, &fmt);
	if (ret == 0 && (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV21 ||
			 fmt.fmt.pix_mp.width != w || fmt.fmt.pix_mp.height != h ||
			 fmt.fmt.pix_mp.plane_fmt[0].bytesperline != align64(w)))
		ret = -EINVAL;
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: VENC %ux%u format rejected: %s\n", w, h,
			     std::strerror(-ret));
		return ret;
	}
	s.stride = align64(w);
	s.cfg = cfg;
	(void)s.node.set_frame_rate(kOut, static_cast<uint32_t>(cfg.output_fps));
	ret = s.node.set_control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
				 V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	if (ret == 0)
		ret = s.node.set_control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1);
	if (ret == 0)
		ret = s.node.set_control(V4L2_CID_MPEG_VIDEO_BITRATE, cfg.bitrate_kbps * 1000);
	if (ret == 0)
		ret = s.node.set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, cfg.gop);
	if (ret == 0 && cfg.codec == H26xCodec::H264)
		ret = s.node.set_control(V4L2_CID_MPEG_VIDEO_H264_PROFILE,
					 V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
	if (ret == 0)
		ret = apply_qp(s, rc);
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: VENC controls rejected: %s\n", std::strerror(-ret));
		return ret;
	}
	const int count = s.node.request_buffers(kCap, V4L2_MEMORY_MMAP, kBitstreamBuffers);
	if (count < 2)
		return count < 0 ? count : -ENOMEM;
	ret = s.node.map_buffers(kCap, count, false, &s.bitstream);
	for (int i = 0; ret == 0 && i < count; ++i)
		ret = s.node.queue(kCap, V4L2_MEMORY_MMAP, i, 0, 0);
	if (ret == 0) {
		ret = s.node.request_buffers(kOut, V4L2_MEMORY_DMABUF, rt::kSlots);
		ret = ret < rt::kSlots ? (ret < 0 ? ret : -ENOMEM) : 0;
	}
	if (ret == 0)
		ret = s.node.stream(kOut, true);
	if (ret == 0)
		ret = s.node.stream(kCap, true);
	if (ret < 0)
		std::fprintf(stderr, "OneKVM: VENC session start failed: %s\n", std::strerror(-ret));
	return ret;
}

void close_session(Session &s)
{
	if (s.node.is_open()) {
		(void)s.node.stream(kOut, false);
		(void)s.node.stream(kCap, false);
	}
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (Frame *&frame : s.inflight) {
			if (frame)
				rt::release_frame_locked(frame);
			frame = nullptr;
		}
		s.inflight_count = 0;
		s.bound = false;
		s.capture = -1;
		for (PendingFrame &p : s.pending)
			p = PendingFrame{};
	}
	v4l2::Node::unmap_buffers(&s.bitstream);
	s.node.close();
	s.suspended.store(false, std::memory_order_release);
	rt::staging_free(&s.staging);
}

/* Stop OUTPUT and drop every imported buffer; CAPTURE keeps streaming. */
void suspend_session(int ch)
{
	Session &s = g_sessions[ch];

	s.suspended.store(true, std::memory_order_release);
	wake_reader(g_readers[ch]);
	(void)s.node.stream(kOut, false);
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (Frame *&frame : s.inflight) {
			if (frame)
				rt::release_frame_locked(frame);
			frame = nullptr;
		}
		s.inflight_count = 0;
		for (PendingFrame &p : s.pending)
			p = PendingFrame{};
	}
	(void)s.node.request_buffers(kOut, V4L2_MEMORY_DMABUF, 0);
	(void)s.node.request_buffers(kOut, V4L2_MEMORY_DMABUF, rt::kSlots);
}

int resume_session(int ch)
{
	Session &s = g_sessions[ch];

	if (!s.suspended.load(std::memory_order_acquire))
		return 0;
	const int ret = s.node.stream(kOut, true);
	if (ret < 0) {
		std::fprintf(stderr, "OneKVM: VENC ch %d restart failed: %s\n", ch,
			     std::strerror(-ret));
		return ret;
	}
	s.suspended.store(false, std::memory_order_release);
	wake_reader(g_readers[ch]);
	return 0;
}

/* Wait up to `timeout_ms` for one bitstream buffer (manual path). */
int dequeue_bitstream(Session &s, int timeout_ms, struct v4l2_buffer *b, struct v4l2_plane *p)
{
	const uint64_t deadline = rt::steady_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;

	for (;;) {
		const int ret = s.node.dequeue(kCap, V4L2_MEMORY_MMAP, b, p);
		if (ret != -EAGAIN)
			return ret;
		const uint64_t now = rt::steady_ns();
		if (now >= deadline)
			return -EAGAIN;
		struct pollfd pfd = {s.node.fd(), POLLIN, 0};
		int polled;
		do {
			polled = poll(&pfd, 1, static_cast<int>((deadline - now + 999999) / 1000000));
		} while (polled < 0 && errno == EINTR);
		if (polled > 0 && (pfd.revents & (POLLERR | POLLNVAL)))
			usleep(1000);
	}
}

void release_held(Session &s)
{
	if (s.held >= 0)
		(void)s.node.queue(kCap, V4L2_MEMORY_MMAP, s.held, 0, 0);
	s.held = -1;
}

} // namespace

namespace rt {

void feed_bound_encoders_locked(int capture, Frame *frame)
{
	for (int ch = 0; ch < kEncoderChannels; ++ch) {
		Session &s = g_sessions[ch];
		Reader &r = g_readers[ch];
		if (!s.initialized || !s.bound || s.capture != capture ||
		    s.suspended.load(std::memory_order_acquire) ||
		    !r.running.load(std::memory_order_acquire) ||
		    r.discard.load(std::memory_order_acquire))
			continue;
		if (frame->width != static_cast<uint32_t>(s.cfg.width) ||
		    frame->height != static_cast<uint32_t>(s.cfg.height) ||
		    frame->stride != s.stride || frame->slot < 0) {
			if (!s.geometry_warned)
				std::fprintf(stderr, "OneKVM: VENC ch %d %dx%d cannot take %ux%u stride %u\n",
					     ch, s.cfg.width, s.cfg.height, frame->width,
					     frame->height, frame->stride);
			s.geometry_warned = true;
			continue;
		}
		if (s.inflight_count >= kEncoderInflight || s.inflight[frame->slot]) {
			++s.dropped;
			continue;
		}
		const uint64_t now = v4l2::monotonic_ns();
		const int ret = s.node.queue(kOut, V4L2_MEMORY_DMABUF, frame->slot, frame->bytes(),
					     frame->timestamp_ns, &frame->mem);
		if (ret < 0) {
			if (s.dropped++ % 600 == 0)
				std::fprintf(stderr, "OneKVM: VENC ch %d input failed: %s\n", ch,
					     std::strerror(-ret));
			continue;
		}
		++frame->refs;
		s.inflight[frame->slot] = frame;
		++s.inflight_count;
		const int fps = r.input_fps.load(std::memory_order_relaxed);
		record_pending_locked(s, frame, now, fps > 0 ? fps : s.cfg.input_fps);
	}
}

bool capture_has_bound_encoder_locked(int capture)
{
	for (const Session &s : g_sessions)
		if (s.initialized && s.bound && s.capture == capture)
			return true;
	return false;
}

void release_idle_encoder_inputs()
{
	for (int ch = 0; ch < kEncoderChannels; ++ch) {
		Session &s = g_sessions[ch];
		if (!s.initialized || s.suspended.load(std::memory_order_acquire))
			continue;
		bool idle;
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			idle = s.bound ? g_readers[ch].discard.load(std::memory_order_acquire)
				       : !s.packet_pending;
		}
		if (idle)
			suspend_session(ch);
	}
}

} // namespace rt

int open_h26x_encoder(int ch, const H26xEncoderConfig &config, const RateControl &rate_control)
{
	if (!valid_channel(ch) || g_rt.reference_count == 0)
		return -1;
	Session &s = g_sessions[ch];
	if (s.initialized)
		return -EBUSY;
	if (g_h26x_active)
		return -ENOSPC;
	const H26xEncoderConfig &cfg = config;
	const RateControl &rc = rate_control;
	if ((cfg.codec != H26xCodec::H265 && cfg.codec != H26xCodec::H264) || cfg.width <= 0 ||
	    cfg.height <= 0 || (cfg.width & 1) || (cfg.height & 1) || cfg.gop <= 0 ||
	    cfg.input_fps <= 0 || cfg.output_fps <= 0 || cfg.output_fps > cfg.input_fps ||
	    cfg.bitrate_kbps <= 0 || cfg.pixel_format != rt::kNv21) {
		std::printf("Invalid venc config: %dx%d, gop:%d, fps:%d/%d, bitrate:%d\n", cfg.width,
			    cfg.height, cfg.gop, cfg.input_fps, cfg.output_fps, cfg.bitrate_kbps);
		return -1;
	}
	if (rc.initial_qp < 0 || rc.initial_qp > 51 || rc.min_qp < 0 || rc.min_qp > 51 ||
	    rc.max_qp < 0 || rc.max_qp > 51 ||
	    (rc.min_qp > 0 && rc.max_qp > 0 && rc.min_qp > rc.max_qp))
		return -1;
	stop_h26x_reader(ch);
	const int ret = start_session(s, cfg, rc);
	if (ret < 0) {
		close_session(s);
		/* Another process holds the only H.26x session. */
		return ret == -EBUSY ? -ENOSPC : -1;
	}
	s.rc = rc;
	s.initialized = true;
	s.bound_once = false;
	s.packet_pending = false;
	s.held = -1;
	s.geometry_warned = false;
	s.dropped = s.input_errors = 0;
	s.last_encode_ns.store(0, std::memory_order_relaxed);
	s.last_capture_ns.store(0, std::memory_order_relaxed);
	s.last_enc_fps.store(0, std::memory_order_relaxed);
	g_readers[ch].pending_rc_mask.store(0, std::memory_order_relaxed);
	g_readers[ch].input_fps.store(cfg.input_fps, std::memory_order_relaxed);
	g_h26x_active = true;
	std::printf("OneKVM: VENC ch %d %s %dx%d %d fps gop %d %d kbps\n", ch,
		    cfg.codec == H26xCodec::H265 ? "H.265" : "H.264", cfg.width, cfg.height,
		    cfg.output_fps, cfg.gop, cfg.bitrate_kbps);
	return 0;
}

int close_h26x_encoder(int ch)
{
	if (!valid_channel(ch))
		return -1;
	Session &s = g_sessions[ch];
	Reader &r = g_readers[ch];
	if (!s.initialized) {
		stop_h26x_reader(ch);
		return 0;
	}
	int capture = -1;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (s.bound)
			capture = s.capture;
		s.bound = false;
	}
	r.discard.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(r.mu);
		clear_queue(r);
	}
	stop_h26x_reader(ch);
	s.held = -1;
	close_session(s);
	s.initialized = false;
	s.packet_pending = false;
	g_h26x_active = false;
	for (const Session &other : g_sessions)
		g_h26x_active |= other.initialized;
	if (capture >= 0)
		(void)pause_vpss_channel(capture);
	notify_ready(r);
	return 0;
}

int close_all_h26x_encoders()
{
	for (int ch = 0; ch < rt::kEncoderChannels; ++ch)
		(void)close_h26x_encoder(ch);
	return 0;
}

int submit_h26x_frame(int ch, uint8_t *data, int w, int h, int format)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized || !data || format != rt::kNv21)
		return -1;
	Session &s = g_sessions[ch];
	if (w != s.cfg.width || h != s.cfg.height)
		return -1;
	if (s.packet_pending)
		return 0;
	if (resume_session(ch) != 0)
		return -1;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		reap_outputs_locked(s);
	}
	/* Without a reader nobody else applies rate-control updates. */
	if (!g_readers[ch].running.load(std::memory_order_acquire)) {
		const uint32_t mask = g_readers[ch].pending_rc_mask.exchange(0);
		if (mask)
			(void)apply_rate_control(ch, mask);
	}
	if (s.staging.frames.empty()) {
		const int ret = rt::staging_alloc(&s.staging, static_cast<uint32_t>(w),
						  static_cast<uint32_t>(h), 2);
		if (ret < 0) {
			rt::staging_free(&s.staging);
			return ret;
		}
	}
	Frame *frame = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		frame = rt::staging_take_locked(&s.staging);
		if (frame)
			++frame->refs;
	}
	if (!frame)
		return -EBUSY;
	rt::copy_tight_nv21(frame, data);
	const uint64_t now = v4l2::monotonic_ns();
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		frame->timestamp_ns = now;
		const int ret = frame->slot < 0 || s.inflight[frame->slot] ? -EBUSY :
			s.node.queue(kOut, V4L2_MEMORY_DMABUF, frame->slot, frame->bytes(), now,
				     &frame->mem);
		if (ret < 0) {
			rt::release_frame_locked(frame);
			return ret;
		}
		s.inflight[frame->slot] = frame;
		++s.inflight_count;
		record_pending_locked(s, frame, now, 0);
	}
	s.last_submit_ns = now;
	s.packet_pending = true;
	return 0;
}

int read_h26x_packet(int ch, uint8_t *dst, int capacity)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized)
		return -1;
	Session &s = g_sessions[ch];
	if (s.held >= 0)
		return -EBUSY;
	if (!s.packet_pending || !dst || capacity <= 0)
		return 0;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (s.bound)
			return 0;
	}
	struct v4l2_buffer b;
	struct v4l2_plane p;
	const int ret = dequeue_bitstream(s, 80, &b, &p);
	if (ret == -EAGAIN)
		return 0;
	if (ret < 0)
		return -1;
	s.packet_pending = false;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		reap_outputs_locked(s);
	}
	const bool ok = !(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused > p.data_offset &&
			b.index < s.bitstream.size();
	const uint32_t size = ok ? b.bytesused - p.data_offset : 0;
	if (!ok || size > static_cast<uint32_t>(capacity)) {
		(void)s.node.queue(kCap, V4L2_MEMORY_MMAP, b.index, 0, 0);
		return ok ? -2 : -EIO;
	}
	std::memcpy(dst, s.bitstream[b.index].addr + p.data_offset, size);
	s.held = static_cast<int>(b.index);
	s.last_encode_ns.store(v4l2::monotonic_ns() - s.last_submit_ns, std::memory_order_relaxed);
	return static_cast<int>(size);
}

int read_manual_h26x_frame(int ch, uint8_t *dst, int capacity, int timeout_ms)
{
	if (!valid_channel(ch) || timeout_ms < 1 || timeout_ms > 1000 || !dst || capacity <= 0)
		return -EINVAL;
	Session &s = g_sessions[ch];
	if (!s.initialized || !s.packet_pending)
		return -EINVAL;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (s.bound)
			return -EINVAL;
	}
	release_held(s);
	struct v4l2_buffer b;
	struct v4l2_plane p;
	const int ret = dequeue_bitstream(s, timeout_ms, &b, &p);
	if (ret == -EAGAIN)
		return -ETIMEDOUT;
	if (ret < 0)
		return ret;
	s.packet_pending = false;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		reap_outputs_locked(s);
	}
	const bool ok = !(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused > p.data_offset &&
			b.index < s.bitstream.size();
	const uint32_t size = ok ? b.bytesused - p.data_offset : 0;
	int result = ok ? static_cast<int>(size) : -EIO;
	if (ok && size > static_cast<uint32_t>(capacity))
		result = -2;
	else if (ok)
		std::memcpy(dst, s.bitstream[b.index].addr + p.data_offset, size);
	(void)s.node.queue(kCap, V4L2_MEMORY_MMAP, b.index, 0, 0);
	s.last_encode_ns.store(v4l2::monotonic_ns() - s.last_submit_ns, std::memory_order_relaxed);
	return result;
}

int release_h26x_packet(int ch)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized)
		return -1;
	release_held(g_sessions[ch]);
	return 0;
}

int request_h26x_idr(int ch)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized)
		return -1;
	Reader &r = g_readers[ch];
	r.want_idr.store(true, std::memory_order_relaxed);
	if (r.running.load(std::memory_order_acquire)) {
		wake_reader(r);
		return 0;
	}
	return g_sessions[ch].node.set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0) == 0 ? 0 : -1;
}

int set_h26x_rate_control(int ch, int output_fps, int gop, int bitrate_kbps, int initial_qp,
			  int min_qp, int max_qp)
{
	if (!valid_channel(ch) || output_fps <= 0)
		return -1;
	if (gop <= 0)
		gop = default_gop_frames(output_fps);
	Reader &r = g_readers[ch];
	r.pending_gop.store(gop, std::memory_order_relaxed);
	r.pending_output_fps.store(output_fps, std::memory_order_relaxed);
	if (bitrate_kbps > 0)
		r.pending_bitrate_kbps.store(bitrate_kbps, std::memory_order_relaxed);
	r.pending_initial_qp.store(initial_qp, std::memory_order_relaxed);
	r.pending_min_qp.store(min_qp, std::memory_order_relaxed);
	r.pending_max_qp.store(max_qp, std::memory_order_relaxed);
	uint32_t mask = kRcFpsGop | kRcQp;
	if (bitrate_kbps > 0)
		mask |= kRcBitrate;
	r.pending_rc_mask.fetch_or(mask, std::memory_order_release);
	wake_reader(r);
	return 0;
}

int start_h26x_reader(int ch, bool request_idr)
{
	if (!valid_channel(ch))
		return -EINVAL;
	Reader &r = g_readers[ch];
	Session &s = g_sessions[ch];
	std::lock_guard<std::mutex> join_lock(r.join_mu);
	try {
		if (!r.packet_pool)
			r.packet_pool = std::make_shared<onekvm::EncodedPacketPool>(kReaderPacketCapacity);
	} catch (const std::bad_alloc &) {
		r.read_error.store(-ENOMEM, std::memory_order_release);
		notify_ready(r);
		return -ENOMEM;
	}
	ensure_events(r);
	if (r.running.load(std::memory_order_acquire)) {
		{
			/* Reconnect: a fresh admission that starts at an IDR. Request it
			   before input resumes so the first new frame is the IDR. */
			std::lock_guard<std::mutex> lock(r.mu);
			r.last_packet_ns.store(0, std::memory_order_relaxed);
			r.read_error.store(0, std::memory_order_release);
			if (request_idr) {
				r.want_idr.store(true, std::memory_order_relaxed);
				if (s.node.set_control(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0) == 0)
					r.idr_sent.store(true, std::memory_order_relaxed);
				else
					r.force_idr.store(true, std::memory_order_release);
			}
			r.discard.store(false, std::memory_order_release);
		}
		notify(r);
		return r.read_error.load(std::memory_order_acquire);
	}
	if (r.thread.joinable())
		r.thread.join();
	if (!s.initialized)
		return -EINVAL;
	r.stop.store(false, std::memory_order_relaxed);
	r.last_packet_ns.store(0, std::memory_order_relaxed);
	r.last_drain_ns.store(0, std::memory_order_relaxed);
	r.read_error.store(0, std::memory_order_relaxed);
	/* A new session opens with an IDR; a reused one is asked for it. */
	r.want_idr.store(true, std::memory_order_relaxed);
	r.force_idr.store(false, std::memory_order_relaxed);
	r.idr_sent.store(!request_idr, std::memory_order_relaxed);
	r.discard.store(false, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(r.mu);
		clear_queue(r);
	}
	r.running.store(true, std::memory_order_release);
	try {
		r.thread = std::thread(reader_main, ch, request_idr);
	} catch (const std::system_error &) {
		r.running.store(false, std::memory_order_release);
		r.read_error.store(-EAGAIN, std::memory_order_release);
		notify_ready(r);
	}
	return r.read_error.load(std::memory_order_acquire);
}

void stop_h26x_reader(int ch)
{
	if (!valid_channel(ch))
		return;
	Reader &r = g_readers[ch];
	r.stop.store(true, std::memory_order_release);
	notify(r);
	std::lock_guard<std::mutex> join_lock(r.join_mu);
	if (!r.thread.joinable())
		return;
	if (r.thread.get_id() == std::this_thread::get_id()) {
		std::fprintf(stderr, "OneKVM: refusing to join VENC reader %d from itself\n", ch);
		return;
	}
	r.thread.join();
}

int bind_h26x_to_capture(int ch, int vpss_group, int capture)
{
	(void)vpss_group;
	if (!valid_channel(ch) || !g_sessions[ch].initialized || capture < 0 ||
	    capture >= kMaxCaptureChannels || !capture_channel_open(capture))
		return -1;
	Session &s = g_sessions[ch];
	bool same;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		same = s.bound && s.capture == capture;
		if (!same) {
			const rt::Channel &c = g_rt.channels[capture];
			if (c.width != s.cfg.width || c.height != s.cfg.height || c.stride != s.stride) {
				std::fprintf(stderr,
					     "OneKVM: VENC ch %d %dx%d does not match capture %d %dx%d\n",
					     ch, s.cfg.width, s.cfg.height, capture, c.width, c.height);
				return -EINVAL;
			}
			s.bound = true;
			s.capture = capture;
			s.geometry_warned = false;
		}
	}
	if (!same) {
		release_held(s);
		s.packet_pending = true;
		g_readers[ch].input_fps.store(g_rt.channels[capture].input_fps,
					      std::memory_order_relaxed);
	}
	const bool request_idr = same || s.bound_once;
	s.bound_once = true;
	int ret = resume_session(ch);
	if (ret == 0)
		ret = start_h26x_reader(ch, request_idr);
	if (ret == 0 && (resume_vi_dma() != 0 || resume_vpss_channel(capture) != 0))
		ret = -1;
	if (ret != 0 && !same) {
		{
			std::lock_guard<std::mutex> lock(g_rt.mu);
			s.bound = false;
			s.capture = -1;
		}
		s.packet_pending = false;
		(void)pause_vpss_channel(capture);
	}
	return ret;
}

int unbind_h26x_from_capture(int ch)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized)
		return -1;
	Session &s = g_sessions[ch];
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		s.bound = false;
		s.capture = -1;
	}
	release_held(s);
	s.packet_pending = false;
	park_unbound_vpss_channels();
	return 0;
}

int park_h26x_capture(int ch)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized)
		return -1;
	Session &s = g_sessions[ch];
	Reader &r = g_readers[ch];
	bool bound;
	int capture;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		bound = s.bound;
		capture = s.capture;
	}
	if (!bound) {
		park_unbound_vpss_channels();
		return pause_vi_dma();
	}
	/* Keep the session and reader: the next bind only reconnects. */
	r.discard.store(true, std::memory_order_release);
	const int vi = pause_vi_dma();
	const int channel = pause_vpss_channel(capture);
	{
		std::lock_guard<std::mutex> lock(r.mu);
		clear_queue(r);
	}
	notify(r);
	return vi != 0 ? vi : channel;
}

int begin_idle_h26x_drain(int capture, int *encoder_channel)
{
	if (!encoder_channel || capture < 0 || capture >= kMaxCaptureChannels)
		return -1;
	*encoder_channel = -1;
	int match = -1;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		for (int ch = 0; ch < rt::kEncoderChannels; ++ch) {
			const Session &s = g_sessions[ch];
			if (!s.initialized || !s.bound || s.capture != capture)
				continue;
			if (match >= 0)
				return -1;
			match = ch;
		}
	}
	if (match < 0)
		return 0;
	Reader &r = g_readers[match];
	/* A discarding encoder is not fed, so resuming the channel for a
	   snapshot leaves its reference chain untouched. */
	r.discard.store(true, std::memory_order_release);
	if (!r.running.load(std::memory_order_acquire)) {
		const int ret = start_h26x_reader(match, false);
		r.discard.store(true, std::memory_order_release);
		if (ret != 0)
			return ret;
	}
	r.read_error.store(0, std::memory_order_release);
	*encoder_channel = match;
	return 0;
}

int finish_idle_h26x_drain(int ch)
{
	if (!valid_channel(ch) || !g_sessions[ch].initialized)
		return -1;
	Session &s = g_sessions[ch];
	Reader &r = g_readers[ch];
	int capture;
	{
		std::lock_guard<std::mutex> lock(g_rt.mu);
		if (!s.bound)
			return -1;
		capture = s.capture;
	}
	if (pause_vpss_channel(capture) != 0 || pause_vi_dma() != 0) {
		r.want_idr.store(true, std::memory_order_relaxed);
		r.force_idr.store(true, std::memory_order_release);
		wake_reader(r);
		return -1;
	}
	{
		std::lock_guard<std::mutex> lock(r.mu);
		clear_queue(r);
	}
	if (r.read_error.load(std::memory_order_acquire) != 0) {
		r.want_idr.store(true, std::memory_order_relaxed);
		return -1;
	}
	notify(r);
	return 0;
}

void set_h26x_capture_input_fps(int capture, int input_fps)
{
	std::lock_guard<std::mutex> lock(g_rt.mu);
	for (int ch = 0; ch < rt::kEncoderChannels; ++ch)
		if (g_sessions[ch].initialized && g_sessions[ch].bound &&
		    g_sessions[ch].capture == capture)
			g_readers[ch].input_fps.store(input_fps, std::memory_order_relaxed);
}

int take_ready_h26x_into(int ch, std::vector<uint8_t> *dst, bool *key_frame, uint64_t *encoded_ns)
{
	if (!valid_channel(ch))
		return -1;
	Reader &r = g_readers[ch];
	std::lock_guard<std::mutex> lock(r.mu);
	if (r.queue.empty())
		return r.read_error.load(std::memory_order_acquire);
	QueuedPacket packet = std::move(r.queue.front());
	r.queue.pop_front();
	const int size = static_cast<int>(packet.data.size());
	if (encoded_ns)
		*encoded_ns = packet.encoded_ns;
	if (key_frame)
		*key_frame = packet.key_frame;
	if (dst) {
		recycle_buffer(r, std::move(*dst));
		*dst = std::move(packet.data);
	} else {
		recycle_buffer(r, std::move(packet.data));
	}
	return size;
}

int take_ready_h26x_packet(int ch, uint8_t *dst, int capacity, bool *key_frame)
{
	if (!valid_channel(ch) || !dst || capacity <= 0)
		return -1;
	Reader &r = g_readers[ch];
	std::lock_guard<std::mutex> lock(r.mu);
	if (r.queue.empty())
		return r.read_error.load(std::memory_order_acquire);
	QueuedPacket &front = r.queue.front();
	if (static_cast<int>(front.data.size()) > capacity)
		return -2;
	std::memcpy(dst, front.data.data(), front.data.size());
	const int size = static_cast<int>(front.data.size());
	if (key_frame)
		*key_frame = front.key_frame;
	recycle_buffer(r, std::move(front.data));
	r.queue.pop_front();
	return size;
}

std::shared_ptr<onekvm::EncodedPacketPool> h26x_packet_pool(int ch)
{
	return valid_channel(ch) ? g_readers[ch].packet_pool : nullptr;
}

bool wait_ready_h26x_packet(int ch, int timeout_ms)
{
	if (!valid_channel(ch) || timeout_ms < 0)
		return false;
	Reader &r = g_readers[ch];
	const uint64_t deadline = rt::steady_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ull;
	for (;;) {
		const int fd = r.ready_event_fd.load(std::memory_order_acquire);
		/* Drain notifications before checking; a producer racing the check
		   leaves the eventfd readable. */
		rt::drain_fd(fd);
		{
			std::lock_guard<std::mutex> lock(r.mu);
			if (!r.queue.empty())
				return true;
			if (r.stop.load(std::memory_order_relaxed) ||
			    r.read_error.load(std::memory_order_acquire) != 0)
				return false;
		}
		const uint64_t now = rt::steady_ns();
		if (now >= deadline)
			return false;
		const int remaining = static_cast<int>((deadline - now + 999999) / 1000000);
		if (fd < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(remaining < 1 ? remaining : 1));
			continue;
		}
		if (!rt::wait_fd(fd, remaining))
			return false;
	}
}

uint64_t h26x_reader_last_packet_ns(int ch)
{
	return valid_channel(ch) ? g_readers[ch].last_packet_ns.load(std::memory_order_relaxed) : 0;
}

uint64_t h26x_last_encode_ns(int ch)
{
	return valid_channel(ch) ? g_sessions[ch].last_encode_ns.load(std::memory_order_relaxed) : 0;
}

uint64_t h26x_last_capture_ns(int ch)
{
	return valid_channel(ch) ? g_sessions[ch].last_capture_ns.load(std::memory_order_relaxed) : 0;
}

uint32_t h26x_last_enc_fps(int ch)
{
	return valid_channel(ch) ? g_sessions[ch].last_enc_fps.load(std::memory_order_relaxed) : 0;
}

void refresh_h26x_hw_latency(int ch)
{
	(void)ch; // measured per access unit
}

void h26x_reader_want_idr(int ch)
{
	if (!valid_channel(ch))
		return;
	g_readers[ch].want_idr.store(true, std::memory_order_relaxed);
	wake_reader(g_readers[ch]);
}

void h26x_reader_force_idr(int ch)
{
	if (!valid_channel(ch))
		return;
	g_readers[ch].want_idr.store(true, std::memory_order_relaxed);
	g_readers[ch].force_idr.store(true, std::memory_order_release);
	wake_reader(g_readers[ch]);
}

/* soph-vi writes NV21 itself, so there is no separate VI-direct mode. */
int yield_h26x_vi_direct()
{
	return 0;
}

int rebind_h26x_to_vpss(int ch)
{
	return valid_channel(ch) ? 0 : -EINVAL;
}

bool h26x_vi_direct_failed(int ch)
{
	(void)ch;
	return false;
}

bool h26x_bound_to_vi(int ch)
{
	(void)ch;
	return false;
}

bool h26x_vi_direct_streaming(void)
{
	return false;
}

bool h26x_encoder_initialized(int ch)
{
	return valid_channel(ch) && g_sessions[ch].initialized;
}

bool h26x_encoder_active()
{
	return g_h26x_active;
}

int h26x_full_range(int ch)
{
	/* soph-venc writes no video_signal_type; decoders infer limited range. */
	return h26x_encoder_initialized(ch) ? 0 : -1;
}

} // namespace onekvm::mmf
