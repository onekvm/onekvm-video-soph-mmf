#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

#include "no_signal_frames.inc"
#include "no_signal_h264.inc"

namespace {

const no_signal_asset_t *find_asset(int width, int height)
{
	for (const no_signal_asset_t &asset : no_signal_assets) {
		if (asset.width == width && asset.height == height)
			return &asset;
	}
	return nullptr;
}

const no_signal_asset_t *nearest_asset(int width, int height)
{
	const no_signal_asset_t *best = nullptr;
	int64_t best_score = -1;
	for (const no_signal_asset_t &asset : no_signal_assets) {
		if (asset.width <= 0 || asset.height <= 0)
			continue;
		const int64_t dw = static_cast<int64_t>(asset.width) - width;
		const int64_t dh = static_cast<int64_t>(asset.height) - height;
		const int64_t score = dw * dw + dh * dh;
		if (best == nullptr || score < best_score) {
			best = &asset;
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
		    uint8_t *dst, int dst_w, int dst_h)
{
	fill_nv21_black(dst, dst_w, dst_h);
	if (src == nullptr || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		return;

	int fit_w;
	int fit_h;
	if (static_cast<int64_t>(src_w) * dst_h <= static_cast<int64_t>(src_h) * dst_w) {
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

bool unpack(const no_signal_asset_t &asset, uint8_t *output, std::size_t output_size)
{
	std::size_t input_offset = 0;
	std::size_t output_offset = 0;
	while (input_offset < asset.packed_size) {
		uint8_t control = asset.packed[input_offset++];
		if ((control & 0x80) != 0) {
			std::size_t length = (control & 0x7f) + 3;
			if (input_offset >= asset.packed_size || length > output_size - output_offset)
				return false;
			std::memset(output + output_offset, asset.packed[input_offset++], length);
			output_offset += length;
			continue;
		}

		std::size_t length = control + 1;
		if (length > asset.packed_size - input_offset || length > output_size - output_offset)
			return false;
		std::memcpy(output + output_offset, asset.packed + input_offset, length);
		input_offset += length;
		output_offset += length;
	}
	return output_offset == output_size;
}

} // namespace

extern "C" int render_no_signal_nv21(uint8_t *data, int capacity, int width, int height)
{
	/* NV21 stores one luma byte per pixel and one interleaved VU pair per 2x2
	 * block. Reject invalid geometry before converting it to size_t; otherwise
	 * a negative height becomes a very large unsigned allocation size. */
	if (data == nullptr || capacity <= 0 || width <= 0 || height <= 0 ||
		(width & 1) != 0 || (height & 1) != 0)
		return -1;
	const std::size_t pixels = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height);
	if (pixels > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 3 * 2)
		return -1;
	const std::size_t image_size = pixels + pixels / 2;
	if (image_size > static_cast<std::size_t>(capacity))
		return -1;

	const no_signal_asset_t *exact = find_asset(width, height);
	if (exact != nullptr) {
		if (!unpack(*exact, data, image_size))
			fill_nv21_black(data, width, height);
		return static_cast<int>(image_size);
	}

	/* Packed artwork is only 1080/720/480. Other VENC sizes (800x600,
	   1024x768, auto-follow HDMI) letterbox the nearest PNG. A still
	   black frame is better than failing encoder_read_packet. */
	const no_signal_asset_t *src = nearest_asset(width, height);
	if (src == nullptr) {
		fill_nv21_black(data, width, height);
		return static_cast<int>(image_size);
	}
	const std::size_t src_pixels = static_cast<std::size_t>(src->width) *
		static_cast<std::size_t>(src->height);
	const std::size_t src_size = src_pixels + src_pixels / 2;
	std::vector<uint8_t> unpacked;
	try {
		unpacked.resize(src_size);
	} catch (const std::bad_alloc &) {
		fill_nv21_black(data, width, height);
		return static_cast<int>(image_size);
	}
	if (!unpack(*src, unpacked.data(), src_size)) {
		fill_nv21_black(data, width, height);
		return static_cast<int>(image_size);
	}
	letterbox_nv21(unpacked.data(), src->width, src->height, data, width, height);
	return static_cast<int>(image_size);
}

extern "C" int no_signal_h264(int width, int height, const uint8_t **data, size_t *size)
{
	if (data == nullptr || size == nullptr)
		return -1;
	const no_signal_h264_asset_t *found = nullptr;
	for (const no_signal_h264_asset_t &asset : no_signal_h264_assets) {
		if (asset.width == width && asset.height == height) {
			found = &asset;
			break;
		}
	}
	if (found == nullptr)
		found = &no_signal_h264_assets[0];
	if (found->data == nullptr || found->size == 0)
		return -1;
	*data = found->data;
	*size = found->size;
	return 0;
}
