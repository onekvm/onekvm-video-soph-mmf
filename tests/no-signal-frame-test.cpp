#include <algorithm>
#include <cstdint>
#include <vector>

extern "C" int render_no_signal_nv21(uint8_t *, int, int, int);
extern "C" int no_signal_h264(int, int, const uint8_t **, size_t *);

int main()
{
	constexpr int width = 640;
	constexpr int height = 480;
	constexpr int image_size = width * height * 3 / 2;
	std::vector<uint8_t> frame(image_size + 16, 0xa5);
	if (render_no_signal_nv21(frame.data(), image_size, width, height) != image_size) return 1;
	if (!std::all_of(frame.begin() + image_size, frame.end(),
		[](uint8_t value) { return value == 0xa5; })) return 2;
	auto y_minmax = std::minmax_element(frame.begin(), frame.begin() + width * height);
	if (*y_minmax.first >= 32 || *y_minmax.second <= 200) return 3;
	if (!std::any_of(frame.begin() + width * height, frame.begin() + image_size,
		[](uint8_t value) { return value < 110 || value > 145; })) return 4;
	if (render_no_signal_nv21(frame.data(), image_size - 1, width, height) >= 0) return 5;
	if (render_no_signal_nv21(frame.data(), image_size, width - 1, height) >= 0) return 6;
	if (render_no_signal_nv21(frame.data(), image_size, width, -height) >= 0) return 7;
	if (render_no_signal_nv21(frame.data(), image_size, -width, height) >= 0) return 8;
	if (render_no_signal_nv21(nullptr, image_size, width, height) >= 0) return 9;
	const uint8_t *h264 = nullptr;
	size_t h264_size = 0;
	if (no_signal_h264(1920, 1080, &h264, &h264_size) != 0 ||
		h264 == nullptr || h264_size < 8) return 10;
	if (h264[0] != 0 || h264[1] != 0 || h264[2] != 0 || h264[3] != 1)
		return 11;
	if (no_signal_h264(0, 0, &h264, &h264_size) != 0 || h264_size < 8)
		return 12;
	return 0;
}
