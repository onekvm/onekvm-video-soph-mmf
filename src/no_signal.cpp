#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

struct FrameSpec {
	const char *name;
	int width;
	int height;
};

constexpr FrameSpec kFrames[] = {
	{"1920x1080", 1920, 1080},
	{"1280x720", 1280, 720},
	{"640x480", 640, 480},
};

std::string cache_root()
{
	const char *env = std::getenv("ONEKVM_NO_SIGNAL_CACHE");
	if (env != nullptr && env[0] != '\0')
		return env;
	return "/data/onekvm/cache/no-signal";
}

const FrameSpec *find_frame(int width, int height)
{
	for (const FrameSpec &frame : kFrames) {
		if (frame.width == width && frame.height == height)
			return &frame;
	}
	return nullptr;
}

const FrameSpec *nearest_frame(int width, int height)
{
	const FrameSpec *best = nullptr;
	int64_t best_score = -1;
	for (const FrameSpec &frame : kFrames) {
		const int64_t dw = static_cast<int64_t>(frame.width) - width;
		const int64_t dh = static_cast<int64_t>(frame.height) - height;
		const int64_t score = dw * dw + dh * dh;
		if (best == nullptr || score < best_score) {
			best = &frame;
			best_score = score;
		}
	}
	return best;
}

void fill_nv21_black(uint8_t *data, int width, int height)
{
	const std::size_t y = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height);
	std::memset(data, 16, y);
	std::memset(data + y, 128, y / 2);
}

void letterbox_nv21(const uint8_t *src, int src_w, int src_h,
		    uint8_t *dst, int dst_w, int dst_h, bool stretch = false)
{
	fill_nv21_black(dst, dst_w, dst_h);
	if (src == nullptr || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		return;

	int fit_w;
	int fit_h;
	if (stretch) {
		fit_w = dst_w;
		fit_h = dst_h;
	} else if (static_cast<int64_t>(src_w) * dst_h <= static_cast<int64_t>(src_h) * dst_w) {
		fit_h = dst_h;
		fit_w = static_cast<int>(static_cast<int64_t>(src_w) * dst_h / src_h);
	} else {
		fit_w = dst_w;
		fit_h = static_cast<int>(static_cast<int64_t>(src_h) * dst_w / src_w);
	}
	fit_w &= ~1;
	fit_h &= ~1;
	if (fit_w < 2 || fit_h < 2)
		return;

	const int x0 = ((dst_w - fit_w) / 2) & ~1;
	const int y0 = ((dst_h - fit_h) / 2) & ~1;
	for (int y = 0; y < fit_h; ++y) {
		const int sy = y * src_h / fit_h;
		uint8_t *drow = dst + static_cast<std::size_t>(y0 + y) * dst_w + x0;
		const uint8_t *srow = src + static_cast<std::size_t>(sy) * src_w;
		for (int x = 0; x < fit_w; ++x)
			drow[x] = srow[x * src_w / fit_w];
	}

	const uint8_t *src_c = src + static_cast<std::size_t>(src_w) * src_h;
	uint8_t *dst_c = dst + static_cast<std::size_t>(dst_w) * dst_h;
	const int fit_cw = fit_w / 2;
	const int fit_ch = fit_h / 2;
	const int cx0 = x0 / 2;
	const int cy0 = y0 / 2;
	for (int y = 0; y < fit_ch; ++y) {
		const int sy = y * (src_h / 2) / fit_ch;
		uint8_t *drow = dst_c + static_cast<std::size_t>(cy0 + y) * dst_w +
			cx0 * 2;
		const uint8_t *srow = src_c + static_cast<std::size_t>(sy) * src_w;
		for (int x = 0; x < fit_cw; ++x) {
			const int sx = x * (src_w / 2) / fit_cw;
			drow[x * 2] = srow[sx * 2];
			drow[x * 2 + 1] = srow[sx * 2 + 1];
		}
	}
}

std::string frame_file(const FrameSpec &frame)
{
	return cache_root() + "/current/" + frame.name + ".nv21";
}

const FrameSpec *selected_frame(int width, int height, int output_width, int output_height)
{
	const bool scaled = width != output_width || height != output_height;
	if (!scaled) {
		if (const FrameSpec *exact = find_frame(width, height))
			return exact;
	}
	return nearest_frame(output_width, output_height);
}

bool load_nv21(const std::string &path, int width, int height, std::vector<uint8_t> &out)
{
	const std::size_t expect = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height) * 3 / 2;
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return false;
	out.resize(expect);
	input.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(expect));
	if (input.gcount() != static_cast<std::streamsize>(expect))
		return false;
	char extra = 0;
	input.read(&extra, 1);
	return !input.gcount();
}

} // namespace

extern "C" int no_signal_cache_identity(int output_width, int output_height,
		uint64_t *device, uint64_t *inode, int64_t *mtime_sec, int64_t *mtime_nsec)
{
	if (device != nullptr)
		*device = 0;
	if (inode != nullptr)
		*inode = 0;
	if (mtime_sec != nullptr)
		*mtime_sec = 0;
	if (mtime_nsec != nullptr)
		*mtime_nsec = 0;
	const FrameSpec *frame = selected_frame(output_width, output_height,
		output_width, output_height);
	if (frame == nullptr)
		return -1;
	struct stat info {};
	if (stat(frame_file(*frame).c_str(), &info) != 0)
		return -1;
	if (device != nullptr)
		*device = static_cast<uint64_t>(info.st_dev);
	if (inode != nullptr)
		*inode = static_cast<uint64_t>(info.st_ino);
	if (mtime_sec != nullptr)
		*mtime_sec = static_cast<int64_t>(info.st_mtim.tv_sec);
	if (mtime_nsec != nullptr)
		*mtime_nsec = static_cast<int64_t>(info.st_mtim.tv_nsec);
	return 0;
}

extern "C" int render_no_signal_nv21_for_output(uint8_t *data, int capacity,
		int width, int height, int output_width, int output_height)
{
	/* NV21 stores one luma byte per pixel and one interleaved VU pair per 2x2
	 * block. Reject invalid geometry before converting it to size_t; otherwise
	 * a negative height becomes a very large unsigned allocation size. */
	if (data == nullptr || capacity <= 0 || width <= 0 || height <= 0 ||
		output_width <= 0 || output_height <= 0 ||
		(width & 1) != 0 || (height & 1) != 0)
		return -1;
	const std::size_t pixels = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height);
	if (pixels > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 3 * 2)
		return -1;
	const std::size_t image_size = pixels + pixels / 2;
	if (image_size > static_cast<std::size_t>(capacity))
		return -1;

	const bool scaled_for_output = width != output_width || height != output_height;
	const FrameSpec *src = selected_frame(width, height, output_width, output_height);
	std::vector<uint8_t> unpacked;
	if (src == nullptr || !load_nv21(frame_file(*src), src->width, src->height, unpacked)) {
		fill_nv21_black(data, width, height);
		return static_cast<int>(image_size);
	}
	if (!scaled_for_output && src->width == width && src->height == height) {
		std::memcpy(data, unpacked.data(), image_size);
		return static_cast<int>(image_size);
	}
	letterbox_nv21(unpacked.data(), src->width, src->height, data, width, height,
			scaled_for_output);
	return static_cast<int>(image_size);
}

extern "C" int render_no_signal_nv21(uint8_t *data, int capacity,
		int width, int height)
{
	return render_no_signal_nv21_for_output(data, capacity, width, height,
			width, height);
}
