#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

struct mmf_stream_t {
	uint8_t *data[8];
	int data_size[8];
	int count;
};

struct mmf_venc_cfg_t {
	uint8_t type;
	int w;
	int h;
	int fmt;
	uint8_t jpg_quality;
	int gop;
	int intput_fps;
	int output_fps;
	int bitrate;
};

extern int mmf_init();
extern int mmf_deinit();
extern int mmf_vi_init();
extern int mmf_vi_deinit();
extern int mmf_add_vi_channel(int, int, int, int);
extern int mmf_del_vi_channel(int);
extern int mmf_vi_frame_pop(int, void **, int *, int *, int *, int *);
extern void mmf_vi_frame_free(int);
extern int mmf_add_venc_channel(int, mmf_venc_cfg_t *);
extern int mmf_del_venc_channel(int);
extern int mmf_venc_push(int, uint8_t *, int, int, int);
extern int mmf_venc_pop(int, mmf_stream_t *);
extern int mmf_venc_free(int);

static const uint8_t *find_nal(const uint8_t *data, int size)
{
	for (int i = 0; i + 4 < size; ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			return data + i + 3;
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
			return data + i + 4;
	}
	return nullptr;
}

int main(int argc, char **argv)
{
	const bool h265 = argc > 1 && std::strcmp(argv[1], "h265") == 0;
	const char *codec = h265 ? "h265" : "h264";
	constexpr int channel = 0;
	constexpr int width = 1920;
	constexpr int height = 1080;
	constexpr int nv21 = 19;
	bool system_ready = false;
	bool vi_ready = false;
	bool vi_channel_ready = false;
	bool venc_ready = false;
	int result = 1;
	mmf_venc_cfg_t cfg{};

	if (mmf_init() != 0) {
		std::fprintf(stderr, "mmf_init failed\n");
		goto cleanup;
	}
	system_ready = true;
	if (mmf_vi_init() != 0) {
		std::fprintf(stderr, "mmf_vi_init failed\n");
		goto cleanup;
	}
	vi_ready = true;
	if (mmf_add_vi_channel(channel, width, height, nv21) != 0) {
		std::fprintf(stderr, "mmf_add_vi_channel failed\n");
		goto cleanup;
	}
	vi_channel_ready = true;

	cfg.type = h265 ? 1 : 2;
	cfg.w = width;
	cfg.h = height;
	cfg.fmt = nv21;
	cfg.gop = 30;
	cfg.intput_fps = 30;
	cfg.output_fps = 30;
	cfg.bitrate = 6000;
	if (mmf_add_venc_channel(channel, &cfg) != 0) {
		std::fprintf(stderr, "mmf_add_venc_channel(%s) failed\n", codec);
		goto cleanup;
	}
	venc_ready = true;

	for (int attempt = 0; attempt < 120; ++attempt) {
		void *frame = nullptr;
		int size = 0;
		int frame_width = 0;
		int frame_height = 0;
		int format = 0;
		if (mmf_vi_frame_pop(channel, &frame, &size, &frame_width, &frame_height, &format) != 0 ||
			frame == nullptr) {
			usleep(20000);
			continue;
		}

		int push_result = mmf_venc_push(channel, static_cast<uint8_t *>(frame),
			frame_width, frame_height, format);
		mmf_vi_frame_free(channel);
		if (push_result != 0) {
			std::fprintf(stderr, "mmf_venc_push(%s) failed: %d\n", codec, push_result);
			goto cleanup;
		}

		mmf_stream_t stream{};
		if (mmf_venc_pop(channel, &stream) != 0 || stream.count <= 0) {
			usleep(20000);
			continue;
		}

		int total = 0;
		const uint8_t *nal = nullptr;
		for (int i = 0; i < stream.count; ++i) {
			total += stream.data_size[i];
			if (nal == nullptr)
				nal = find_nal(stream.data[i], stream.data_size[i]);
		}
		int nal_type = -1;
		if (nal != nullptr)
			nal_type = h265 ? ((*nal >> 1) & 0x3f) : (*nal & 0x1f);
		mmf_venc_free(channel);
		std::printf("codec=%s packs=%d bytes=%d nal_type=%d\n",
			codec, stream.count, total, nal_type);
		result = total > 0 && nal_type >= 0 ? 0 : 1;
		break;
	}

cleanup:
	if (venc_ready)
		mmf_del_venc_channel(channel);
	if (vi_channel_ready)
		mmf_del_vi_channel(channel);
	if (vi_ready)
		mmf_vi_deinit();
	if (system_ready)
		mmf_deinit();
	return result;
}
