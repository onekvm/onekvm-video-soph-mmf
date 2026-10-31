#include <cstddef>
#include <cstdint>

#include "no_signal_h264.inc"

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
