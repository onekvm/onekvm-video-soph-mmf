#include <onekvm/video_backend_v1.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
constexpr const char *kLibrary =
    "/usr/lib/onekvm/video-backends/nanokvm-mmf.so";

bool read_until_packet(const onekvm_video_backend_v1 *api, void *encoder,
                       std::chrono::seconds timeout, bool *key_frame) {
    char error[512] = {};
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        onekvm_video_packet_v1 packet{};
        packet.struct_size = sizeof(packet);
        std::memset(error, 0, sizeof(error));
        if (api->encoder_read_packet(
                encoder, &packet, error, sizeof(error)) != 0) {
            std::fprintf(stderr, "read packet: %s\n",
                         error[0] ? error : "failed");
            return false;
        }
        if (packet.data_size != 0) {
            *key_frame = packet.key_frame != 0;
            api->encoder_release_packet(encoder);
            return true;
        }
    }
    std::fprintf(stderr, "timed out waiting for an encoded packet\n");
    return false;
}

} // namespace

int main(int argc, char **argv) {
    const int cycles = argc > 1 ? std::max(1, std::atoi(argv[1])) : 10;
    const int stall_seconds = argc > 2 ? std::max(1, std::atoi(argv[2])) : 2;
    const int deadline_seconds =
        argc > 3 ? std::max(0, std::atoi(argv[3])) : 0;
    if (deadline_seconds != 0) {
        std::thread([deadline_seconds]() {
            std::this_thread::sleep_for(
                std::chrono::seconds(deadline_seconds));
            std::fprintf(stderr, "stress deadline expired\n");
            std::_Exit(124);
        }).detach();
    }
    void *library = dlopen(kLibrary, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto query = reinterpret_cast<onekvm_video_backend_query_fn>(
        dlsym(library, ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL));
    const void *raw_api = nullptr;
    if (query == nullptr ||
        query(ONEKVM_VIDEO_BACKEND_ABI_V1, &raw_api) != 0 ||
        raw_api == nullptr) {
        std::fprintf(stderr, "query ABI v1 failed\n");
        dlclose(library);
        return 1;
    }
    const auto *api = static_cast<const onekvm_video_backend_v1 *>(raw_api);
    if ((api->features & ONEKVM_VIDEO_FEATURE_BOUND_ENCODER) == 0 ||
        api->encoder_bind_source == nullptr ||
        api->encoder_read_packet == nullptr ||
        api->encoder_unbind_source == nullptr) {
        std::fprintf(stderr, "bound encoder ABI is unavailable\n");
        dlclose(library);
        return 1;
    }

    char error[512] = {};
    onekvm_video_source_config_v1 source_config{};
    source_config.struct_size = sizeof(source_config);
    source_config.resolution = 1080;
    source_config.fps = 60;
    void *source = nullptr;
    if (api->source_create(
            &source_config, &source, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "source_create: %s\n", error);
        dlclose(library);
        return 1;
    }

    onekvm_video_encoder_config_v1 encoder_config{};
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H264;
    encoder_config.quality_factor = 1.0;
    encoder_config.fps = 60;
    void *encoder = nullptr;
    if (api->encoder_create(
            &encoder_config, &encoder, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "encoder_create: %s\n", error);
        api->source_destroy(source);
        dlclose(library);
        return 1;
    }
    if (api->encoder_bind_source(
            encoder, source, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "encoder_bind_source: %s\n", error);
        api->encoder_destroy(encoder);
        api->source_destroy(source);
        dlclose(library);
        return 1;
    }

    int failures = 0;
    bool key_frame = false;
    if (!read_until_packet(api, encoder, std::chrono::seconds(3), &key_frame))
        ++failures;
    for (int cycle = 0; cycle < cycles && failures == 0; ++cycle) {
        std::this_thread::sleep_for(std::chrono::seconds(stall_seconds));
        key_frame = false;
        if (!read_until_packet(
                api, encoder, std::chrono::seconds(3), &key_frame)) {
            ++failures;
            break;
        }
        if (!key_frame) {
            std::fprintf(stderr,
                         "cycle %d resumed with a non-IDR packet\n", cycle + 1);
            ++failures;
            break;
        }
        std::printf("cycle=%d key_frame=1\n", cycle + 1);
        std::fflush(stdout);
    }

    if (api->encoder_unbind_source(
            encoder, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "encoder_unbind_source: %s\n", error);
        ++failures;
    }
    api->encoder_destroy(encoder);
    api->source_destroy(source);
    dlclose(library);
    return failures == 0 ? 0 : 1;
}
