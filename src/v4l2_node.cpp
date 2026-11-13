#include "v4l2_node.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace onekvm::v4l2 {

uint64_t monotonic_ns()
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
	       static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t timeval_ns(const struct timeval &tv)
{
	return static_cast<uint64_t>(tv.tv_sec) * 1000000000ull +
	       static_cast<uint64_t>(tv.tv_usec) * 1000ull;
}

struct timeval ns_timeval(uint64_t ns)
{
	struct timeval tv;

	tv.tv_sec = static_cast<time_t>(ns / 1000000000ull);
	tv.tv_usec = static_cast<suseconds_t>(ns % 1000000000ull / 1000ull);
	return tv;
}

Node::~Node()
{
	close();
}

int Node::open(const char *name)
{
	char sys[64], label[64], path[32];

	close();
	for (int i = 0; i < 64; ++i) {
		std::snprintf(sys, sizeof(sys), "/sys/class/video4linux/video%d/name", i);
		FILE *f = std::fopen(sys, "re");

		if (!f)
			continue;
		const bool match = std::fgets(label, sizeof(label), f) &&
				   (label[std::strcspn(label, "\n")] = 0, !std::strcmp(label, name));
		std::fclose(f);
		if (!match)
			continue;
		std::snprintf(path, sizeof(path), "/dev/video%d", i);
		fd_ = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd_ < 0)
			return -errno;
		struct v4l2_capability cap {};
		if (ioctl(VIDIOC_QUERYCAP, &cap) < 0) {
			const int err = -errno;
			close();
			return err;
		}
		const uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ?
				      cap.device_caps : cap.capabilities;
		mplane_ = caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_CAPTURE_MPLANE |
				  V4L2_CAP_VIDEO_OUTPUT_MPLANE);
		return 0;
	}
	return -ENODEV;
}

void Node::close()
{
	if (fd_ >= 0)
		::close(fd_);
	fd_ = -1;
	mplane_ = false;
}

int Node::ioctl(unsigned long request, void *arg) const
{
	int ret;

	do {
		ret = ::ioctl(fd_, request, arg);
	} while (ret < 0 && errno == EINTR);
	return ret < 0 ? -errno : ret;
}

int Node::set_format(uint32_t type, uint32_t fourcc, uint32_t width, uint32_t height,
		     uint32_t bytesperline, uint32_t sizeimage, struct v4l2_format *result)
{
	struct v4l2_format f {};

	f.type = type;
	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		f.fmt.pix_mp.pixelformat = fourcc;
		f.fmt.pix_mp.width = width;
		f.fmt.pix_mp.height = height;
		f.fmt.pix_mp.field = V4L2_FIELD_NONE;
		f.fmt.pix_mp.num_planes = 1;
		f.fmt.pix_mp.plane_fmt[0].bytesperline = bytesperline;
		f.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;
	} else {
		f.fmt.pix.pixelformat = fourcc;
		f.fmt.pix.width = width;
		f.fmt.pix.height = height;
		f.fmt.pix.field = V4L2_FIELD_NONE;
		f.fmt.pix.bytesperline = bytesperline;
		f.fmt.pix.sizeimage = sizeimage;
	}
	const int ret = ioctl(VIDIOC_S_FMT, &f);

	if (ret == 0 && result)
		*result = f;
	return ret;
}

int Node::set_selection(uint32_t type, uint32_t target, struct v4l2_rect *rect)
{
	struct v4l2_selection s {};

	/* The selection API takes the single-planar buffer types. */
	s.type = type;
	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		s.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		s.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	s.target = target;
	s.r = *rect;
	const int ret = ioctl(VIDIOC_S_SELECTION, &s);

	if (ret == 0)
		*rect = s.r;
	return ret;
}

int Node::set_control(uint32_t id, int32_t value)
{
	struct v4l2_control c {};

	c.id = id;
	c.value = value;
	return ioctl(VIDIOC_S_CTRL, &c);
}

int Node::set_frame_rate(uint32_t type, uint32_t fps)
{
	struct v4l2_streamparm parm {};

	parm.type = type;
	if (V4L2_TYPE_IS_OUTPUT(type)) {
		parm.parm.output.timeperframe.numerator = 1;
		parm.parm.output.timeperframe.denominator = fps;
	} else {
		parm.parm.capture.timeperframe.numerator = 1;
		parm.parm.capture.timeperframe.denominator = fps;
	}
	return ioctl(VIDIOC_S_PARM, &parm);
}

int Node::request_buffers(uint32_t type, uint32_t memory, unsigned int count, bool cached)
{
	struct v4l2_requestbuffers rb {};

	rb.count = count;
	rb.type = type;
	rb.memory = memory;
	if (cached)
		rb.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
	const int ret = ioctl(VIDIOC_REQBUFS, &rb);

	return ret < 0 ? ret : static_cast<int>(rb.count);
}

int Node::map_buffers(uint32_t type, unsigned int count, bool export_dmabuf,
		      std::vector<MappedBuffer> *buffers)
{
	buffers->assign(count, MappedBuffer{});
	for (unsigned int i = 0; i < count; ++i) {
		struct v4l2_plane plane {};
		struct v4l2_buffer b {};
		uint32_t offset;

		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
			b.m.planes = &plane;
			b.length = 1;
		}
		int ret = ioctl(VIDIOC_QUERYBUF, &b);
		if (ret < 0)
			goto fail;
		if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
			(*buffers)[i].length = plane.length;
			offset = plane.m.mem_offset;
		} else {
			(*buffers)[i].length = b.length;
			offset = b.m.offset;
		}
		{
			void *addr = mmap(nullptr, (*buffers)[i].length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, fd_, offset);
			if (addr == MAP_FAILED) {
				ret = -errno;
				goto fail;
			}
			(*buffers)[i].addr = static_cast<uint8_t *>(addr);
		}
		if (export_dmabuf) {
			struct v4l2_exportbuffer eb {};

			eb.type = type;
			eb.index = i;
			eb.flags = O_RDWR | O_CLOEXEC;
			ret = ioctl(VIDIOC_EXPBUF, &eb);
			if (ret < 0)
				goto fail;
			(*buffers)[i].dmabuf = eb.fd;
		}
		continue;
fail:
		unmap_buffers(buffers);
		return ret;
	}
	return 0;
}

void Node::unmap_buffers(std::vector<MappedBuffer> *buffers)
{
	for (MappedBuffer &b : *buffers) {
		if (b.addr)
			munmap(b.addr, b.length);
		if (b.dmabuf >= 0)
			::close(b.dmabuf);
	}
	buffers->clear();
}

int Node::queue(uint32_t type, uint32_t memory, unsigned int index, uint32_t bytesused,
		uint64_t timestamp_ns, const MappedBuffer *dma, uint32_t flags)
{
	struct v4l2_plane plane {};
	struct v4l2_buffer b {};

	b.type = type;
	b.memory = memory;
	b.index = index;
	b.flags = flags;
	b.field = V4L2_FIELD_NONE;
	b.timestamp = ns_timeval(timestamp_ns);
	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		plane.bytesused = bytesused;
		if (memory == V4L2_MEMORY_DMABUF) {
			plane.m.fd = dma->dmabuf;
			plane.length = static_cast<uint32_t>(dma->length);
		}
		b.m.planes = &plane;
		b.length = 1;
	} else {
		b.bytesused = bytesused;
		if (memory == V4L2_MEMORY_DMABUF) {
			b.m.fd = dma->dmabuf;
			b.length = static_cast<uint32_t>(dma->length);
		}
	}
	return ioctl(VIDIOC_QBUF, &b);
}

int Node::dequeue(uint32_t type, uint32_t memory, struct v4l2_buffer *buf,
		  struct v4l2_plane *plane)
{
	std::memset(buf, 0, sizeof(*buf));
	std::memset(plane, 0, sizeof(*plane));
	buf->type = type;
	buf->memory = memory;
	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		buf->m.planes = plane;
		buf->length = 1;
	}
	const int ret = ioctl(VIDIOC_DQBUF, buf);

	if (ret == 0 && V4L2_TYPE_IS_MULTIPLANAR(type))
		buf->bytesused = plane->bytesused;
	return ret;
}

int Node::stream(uint32_t type, bool on)
{
	return ioctl(on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
}

} // namespace onekvm::v4l2
