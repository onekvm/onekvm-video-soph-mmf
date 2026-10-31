#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" int render_no_signal_nv21(uint8_t *, int, int, int);
extern "C" int render_no_signal_nv21_for_output(uint8_t *, int, int, int, int, int);
extern "C" int no_signal_h264(int, int, const uint8_t **, size_t *);

namespace {

void write_nv21(const std::filesystem::path &path, int width, int height,
		uint8_t dark, uint8_t bright)
{
	const int luma = width * height;
	std::vector<uint8_t> frame(luma * 3 / 2, bright);
	std::fill(frame.begin(), frame.begin() + luma / 2, dark);
	std::fill(frame.begin() + luma, frame.end(), static_cast<uint8_t>(16));
	std::ofstream output(path, std::ios::binary);
	output.write(reinterpret_cast<const char *>(frame.data()),
		static_cast<std::streamsize>(frame.size()));
}

void seed_cache()
{
	const auto root = std::filesystem::temp_directory_path() /
		("onekvm-nosig-" + std::to_string(static_cast<long long>(::getpid())));
	std::filesystem::remove_all(root);
	const auto english = root / "en";
	std::filesystem::create_directories(english);
	write_nv21(english / "1920x1080.nv21", 1920, 1080, 220, 220);
	write_nv21(english / "1280x720.nv21", 1280, 720, 220, 220);
	write_nv21(english / "640x480.nv21", 640, 480, 16, 235);
	std::filesystem::create_symlink("en", root / "current");
	::setenv("ONEKVM_NO_SIGNAL_CACHE", root.c_str(), 1);
}

} // namespace

int main()
{
	seed_cache();
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

	{
		constexpr int width = 1024;
		constexpr int height = 768;
		constexpr int image_size = width * height * 3 / 2;
		std::vector<uint8_t> frame(image_size, 0xa5);
		if (render_no_signal_nv21(frame.data(), image_size, width, height) != image_size)
			return 13;
		if (frame[0] != 16)
			return 14;
		auto y_end = frame.begin() + width * height;
		if (std::all_of(frame.begin(), y_end,
			[](uint8_t value) { return value == 16; }))
			return 15;
		if (*std::max_element(frame.begin(), y_end) <= 200)
			return 16;
	}
	{
		constexpr int width = 800;
		constexpr int height = 600;
		constexpr int image_size = width * height * 3 / 2;
		std::vector<uint8_t> frame(image_size);
		if (render_no_signal_nv21(frame.data(), image_size, width, height) != image_size)
			return 17;
	}
	{
		/* A 640x480 receiver can probe while the encoded placeholder stays
		 * 1920x1080. Keep the 1080p artwork layout through VPSS scaling. */
		constexpr int output_w = 1920, output_h = 1080;
		constexpr int input_w = 640, input_h = 480;
		std::vector<uint8_t> output(output_w * output_h * 3 / 2);
		std::vector<uint8_t> input(input_w * input_h * 3 / 2);
		if (render_no_signal_nv21(output.data(), output.size(), output_w, output_h) < 0 ||
		    render_no_signal_nv21_for_output(input.data(), input.size(),
			input_w, input_h, output_w, output_h) < 0)
			return 18;
		unsigned long error = 0, samples = 0;
		for (int y = 0; y < input_h; y += 8) {
			for (int x = 0; x < input_w; x += 8) {
				const int expected = output[(y * output_h / input_h) * output_w +
				                            x * output_w / input_w];
				const int actual = input[y * input_w + x];
				error += static_cast<unsigned>(expected > actual
					? expected - actual : actual - expected);
				samples++;
			}
		}
		if (error > samples * 3)
			return 19;
	}
	return 0;
}
