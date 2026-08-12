#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "no_signal_frames.inc"

namespace {

const no_signal_asset_t *find_asset(int width, int height)
{
	for (const no_signal_asset_t &asset : no_signal_assets) {
		if (asset.width == width && asset.height == height)
			return &asset;
	}
	return nullptr;
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
	const no_signal_asset_t *asset = find_asset(width, height);
	if (asset == nullptr)
		return -1;
	const std::size_t pixels = static_cast<std::size_t>(width) *
		static_cast<std::size_t>(height);
	if (pixels > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 3 * 2)
		return -1;
	const std::size_t image_size = pixels + pixels / 2;
	if (image_size > static_cast<std::size_t>(capacity) || !unpack(*asset, data, image_size))
		return -1;
	return static_cast<int>(image_size);
}
