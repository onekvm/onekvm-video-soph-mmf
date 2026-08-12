#include <onekvm/video_backend_v1.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char *kLibrary = "/usr/lib/onekvm/video-backends/nanokvm-mmf.so";

void fail(const char *operation, const char *detail) {
    std::fprintf(stderr, "%s: %s\n", operation, detail && detail[0] ? detail : "failed");
}

void print_h264_sps(const uint8_t *data, size_t size) {
    for (size_t offset = 0; offset + 8 < size; ++offset) {
        size_t nal = 0;
        if (data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
            nal = offset + 3;
        } else if (data[offset] == 0 && data[offset + 1] == 0 &&
                   data[offset + 2] == 0 && data[offset + 3] == 1) {
            nal = offset + 4;
        } else {
            continue;
        }
        if (nal + 3 < size && (data[nal] & 0x1f) == 7) {
            std::printf("h264_sps profile_idc=%u constraints=0x%02x level_idc=%u\n",
                        data[nal + 1], data[nal + 2], data[nal + 3]);
            return;
        }
    }
    std::printf("h264_sps unavailable\n");
}
}

int main() {
    void *library = dlopen(kLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fail("dlopen", dlerror());
        return 1;
    }
    auto query = reinterpret_cast<onekvm_video_backend_query_fn>(
        dlsym(library, ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL));
    if (!query) {
        fail("dlsym", dlerror());
        return 1;
    }
    const void *raw_api = nullptr;
    if (query(ONEKVM_VIDEO_BACKEND_ABI_V1, &raw_api) != 0 || !raw_api) {
        fail("query", "ABI v1 unavailable");
        return 1;
    }
    const auto *api = static_cast<const onekvm_video_backend_v1 *>(raw_api);
    std::printf("driver=%s abi=%u codecs=0x%llx\n", api->driver_id,
                api->abi_version, static_cast<unsigned long long>(api->codec_mask));

    char error[512] = {};
    onekvm_video_source_config_v1 source_config{};
    source_config.struct_size = sizeof(source_config);
    source_config.resolution = 1080;
    source_config.fps = 60;
    void *source = nullptr;
    if (api->source_create(&source_config, &source, error, sizeof(error)) != 0) {
        fail("source_create", error);
        return 1;
    }

    onekvm_video_encoder_config_v1 encoder_config{};
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H264;
    encoder_config.quality_factor = 0.7;
    encoder_config.fps = 60;
    void *encoder = nullptr;
    std::memset(error, 0, sizeof(error));
    if (api->encoder_create(&encoder_config, &encoder, error, sizeof(error)) != 0) {
        fail("encoder_create", error);
        api->source_destroy(source);
        return 1;
    }

    int exit_code = 1;
    for (int attempt = 0; attempt < 5; ++attempt) {
        onekvm_video_frame_v1 frame{};
        frame.struct_size = sizeof(frame);
        std::memset(error, 0, sizeof(error));
        if (api->source_read(source, &frame, error, sizeof(error)) != 0) {
            fail("source_read", error);
            continue;
        }
        std::printf("frame=%dx%d bytes=%llu token=%llu\n", frame.width, frame.height,
                    static_cast<unsigned long long>(frame.data_size),
                    static_cast<unsigned long long>(frame.token));
        onekvm_video_packet_v1 packet{};
        packet.struct_size = sizeof(packet);
        std::memset(error, 0, sizeof(error));
        int result = api->encoder_encode(encoder, &frame, &packet, error, sizeof(error));
        if (frame.token != 0) api->source_release(source, frame.token);
        if (result != 0) {
            fail("encoder_encode", error);
            continue;
        }
        if (packet.data_size == 0) {
            std::fprintf(stderr, "encoder_encode: no packet yet\n");
            continue;
        }
        std::printf("packet=%llu codec=%u\n",
                    static_cast<unsigned long long>(packet.data_size), packet.codec);
        print_h264_sps(packet.data, packet.data_size);
        exit_code = 0;
        break;
    }

    api->encoder_destroy(encoder);
    api->source_destroy(source);
    dlclose(library);
    return exit_code;
}
