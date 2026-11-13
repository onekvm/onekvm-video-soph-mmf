#pragma once

// Thin helpers over the SG200x V4L2 nodes (soph-vi, soph-vpss, soph-venc,
// soph-jpegenc). Internal to the V4L2 runtime.

#include <linux/videodev2.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace onekvm::v4l2 {

#define ONEKVM_V4L2_INTERNAL __attribute__((visibility("hidden")))

/* Node names as /sys/class/video4linux/videoN/name reports them. */
inline constexpr const char *kViNode = "soph-vi";
inline constexpr const char *kScalerNode = "soph-vpss";
inline constexpr const char *kEncoderNode = "soph-venc";
inline constexpr const char *kJpegNode = "soph-jpegenc";

struct ONEKVM_V4L2_INTERNAL MappedBuffer {
	uint8_t *addr = nullptr;
	size_t length = 0;
	int dmabuf = -1;
};

ONEKVM_V4L2_INTERNAL uint64_t monotonic_ns();
ONEKVM_V4L2_INTERNAL uint64_t timeval_ns(const struct timeval &tv);
ONEKVM_V4L2_INTERNAL struct timeval ns_timeval(uint64_t ns);

/* One open video node. Not thread-safe; the owner serializes calls. */
class ONEKVM_V4L2_INTERNAL Node {
public:
	Node() = default;
	~Node();
	Node(const Node &) = delete;
	Node &operator=(const Node &) = delete;

	/* Open the node whose sysfs name is `name`, non-blocking. -errno. */
	int open(const char *name);
	void close();
	bool is_open() const { return fd_ >= 0; }
	int fd() const { return fd_; }
	bool mplane() const { return mplane_; }

	/* ioctl retrying EINTR; -errno on failure. */
	int ioctl(unsigned long request, void *arg) const;

	/* Single-plane-in-one-buffer formats only (num_planes == 1). */
	int set_format(uint32_t type, uint32_t fourcc, uint32_t width, uint32_t height,
		       uint32_t bytesperline, uint32_t sizeimage, struct v4l2_format *result);
	int set_selection(uint32_t type, uint32_t target, struct v4l2_rect *rect);
	int set_control(uint32_t id, int32_t value);
	int set_frame_rate(uint32_t type, uint32_t fps);

	/* REQBUFS; returns the count granted or -errno. count 0 frees. */
	int request_buffers(uint32_t type, uint32_t memory, unsigned int count,
			    bool cached = false);
	/* MMAP buffers: map every buffer and optionally export it. */
	int map_buffers(uint32_t type, unsigned int count, bool export_dmabuf,
			std::vector<MappedBuffer> *buffers);
	static void unmap_buffers(std::vector<MappedBuffer> *buffers);

	/* Queue an MMAP buffer, or a DMABUF (`dma` gives fd and length).
	   `flags` carries V4L2_BUF_FLAG_NO_CACHE_* hints for cached queues. */
	int queue(uint32_t type, uint32_t memory, unsigned int index, uint32_t bytesused,
		  uint64_t timestamp_ns, const MappedBuffer *dma = nullptr,
		  uint32_t flags = 0);
	/* 0 with a buffer, -EAGAIN when none is ready, other -errno. */
	int dequeue(uint32_t type, uint32_t memory, struct v4l2_buffer *buf,
		    struct v4l2_plane *plane);
	int stream(uint32_t type, bool on);

private:
	int fd_ = -1;
	bool mplane_ = false;
};

/* Planar 4:2:0 frame size: stride × height × 3 / 2. */
inline size_t nv_frame_bytes(uint32_t stride, uint32_t height)
{
	return static_cast<size_t>(stride) * height * 3u / 2u;
}

} // namespace onekvm::v4l2
