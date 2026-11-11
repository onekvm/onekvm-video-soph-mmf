#pragma once

// Shared state of the V4L2 runtime (soph-v4l2.so): soph-vi capture,
// soph-vpss scaling, soph-venc / soph-jpegenc encoding. It implements the
// onekvm::mmf interface in mmf.hpp without cvi_mpi.
//
// Threads:
// - control: the upper layers, serialized by MmfControlLock;
// - pipeline: one thread routes VI frames to the capture channels (directly,
//   or through a soph-vpss context) and from there to bound encoders and the
//   latest-frame slot;
// - reader: one per H.26x encoder, the sole bitstream dequeuer.
// `Runtime::mu` guards frame references and the routing state shared by them.
// Nothing blocking (thread joins, waits, LT6911 I2C) runs under it.

#include "mmf.hpp"
#include "v4l2_node.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace onekvm::mmf::rt {

constexpr int kEncoderChannels = 4; // 0: JPEG, 1..3: H.26x
constexpr int kFirstH26xChannel = 1;
constexpr int kSlots = 16;          // OUTPUT queue size of soph-venc / soph-vpss
constexpr int kNv21 = 19;           // PIXEL_FORMAT_NV21 as the upper layers pass it
constexpr int kEncoderInflight = 2; // frames queued to one encoder at once
constexpr uint32_t kNoCacheHints =
	V4L2_BUF_FLAG_NO_CACHE_INVALIDATE | V4L2_BUF_FLAG_NO_CACHE_CLEAN;

enum class Producer : uint8_t { Vi, Scaler, Staging };

struct Channel;

/* One physically contiguous NV21 picture buffer, exported as a DMA-BUF.
   VI and scaler buffers return to their queue on the last release; staging
   buffers (placeholder frames, CPU copies) stay with their owner. */
struct Frame {
	Producer producer = Producer::Staging;
	int index = 0;               // buffer index in its producer queue
	int slot = -1;               // OUTPUT slot in every consumer queue
	Channel *channel = nullptr;  // scaler buffers: owning channel
	v4l2::MappedBuffer mem;
	bool cached = false;
	uint32_t width = 0, height = 0, stride = 0;
	// Runtime::mu
	int refs = 0;
	bool orphan = false;         // producer closed: free on last release
	uint64_t timestamp_ns = 0;   // CLOCK_MONOTONIC VI frame end / submit time
	uint32_t sequence = 0;
	const uint8_t *source = nullptr; // staging: CPU picture last copied in
	uint64_t source_generation = 0;

	uint32_t bytes() const { return static_cast<uint32_t>(v4l2::nv_frame_bytes(stride, height)); }
};

/* Buffers allocated from an idle soph-vpss context, which is closed right
   after mmap/EXPBUF; the mappings and DMA-BUFs keep the memory. */
struct StagingSet {
	std::vector<Frame *> frames;
	uint32_t width = 0, height = 0;
};

struct Channel {
	// control thread; read by the pipeline under Runtime::mu
	bool open = false;
	bool running = false;
	bool passthrough = true;
	bool mirror = false, flip = false; // options applied at open
	int width = 0, height = 0;
	uint32_t stride = 0;
	int fps = 0, input_fps = 0;
	// scaler context (when not passthrough)
	v4l2::Node scaler;
	std::vector<Frame *> scaled;
	std::array<Frame *, kSlots> scaler_inputs{};
	int scaler_inflight = 0;
	// consumers, Runtime::mu
	Frame *latest = nullptr;
	Frame *borrowed = nullptr;
	const void *borrowed_data = nullptr;
	std::vector<uint8_t> tight;  // tightly packed copy for CPU consumers
	int ready_fd = -1;           // eventfd: a new latest frame
	uint64_t delivered = 0, dropped = 0;
};

struct Runtime {
	std::mutex mu;
	int reference_count = 0;
	bool teardown_failed = false;
	bool stale_checked = false;
	uint32_t vi_width = 0, vi_height = 0, vi_stride = 0;
	v4l2::Node vi;
	std::vector<Frame *> vi_frames;
	bool vi_streaming = false;
	bool user_frames = false;
	std::array<Channel, kMaxCaptureChannels> channels;
	uint32_t slot_mask = 0;
	// pipeline thread
	std::thread pipeline;
	std::atomic<bool> stop{false};
	int wake_fd = -1;
	// placeholder frames at the VI size
	StagingSet user;
	int user_next = 0;
	int user_free_fd = -1;
	uint64_t user_alloc_failed_ns = 0;
	const uint8_t *user_last_source = nullptr;
	uint64_t user_generation = 1;
	uint32_t user_sequence = 0;
	// measured VI rate
	uint64_t rate_window_ns = 0;
	uint32_t rate_frames = 0;
	std::atomic<int> vi_fps{0};
	std::atomic<uint64_t> last_vi_frame_ns{0};
	// JPEG input ownership (control thread)
	int jpeg_capture_channel = -1;
	bool jpeg_capture_owned = false;
};

extern Runtime g_rt;

uint64_t steady_ns();
void wake_pipeline();
void signal_fd(int fd);
void drain_fd(int fd);
/* Wait for `fd` readable up to `timeout_ms`; true if readable. */
bool wait_fd(int fd, int timeout_ms);

void release_frame_locked(Frame *frame);
int staging_alloc(StagingSet *set, uint32_t width, uint32_t height, int count);
void staging_free(StagingSet *set);
/* A staging frame nobody references, or nullptr. Runtime::mu. */
Frame *staging_take_locked(StagingSet *set);
void copy_tight_nv21(Frame *dst, const uint8_t *src);
/* Invalidate a cached frame before the CPU reads it. */
void sync_for_cpu(Frame *frame);
/* The borrowed frame whose CPU pointer is `data`, or nullptr. Runtime::mu. */
Frame *find_borrowed_frame_locked(const void *data, int *channel);

/* Implemented by the H.26x part: queue `frame` to every encoder bound to
   `capture` that takes input now. Runtime::mu. */
void feed_bound_encoders_locked(int capture, Frame *frame);
bool capture_has_bound_encoder_locked(int capture);
/* VI paused: idle encoders drop their imported input buffers (a stopped
   OUTPUT queue ends the encoder session; the next input starts a new one
   with an IDR), so freed capture buffers really return their memory. */
void release_idle_encoder_inputs();
void release_idle_jpeg_inputs();

} // namespace onekvm::mmf::rt
