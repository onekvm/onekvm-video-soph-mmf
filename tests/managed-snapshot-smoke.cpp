#include <onekvm/video_backend_v1.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <thread>

namespace {
constexpr const char *kLibrary = "/usr/lib/onekvm/video-backends/soph-mmf.so";

bool complete_jpeg(const uint8_t *data, uint64_t size)
{
    return data != nullptr && size >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
        data[size - 2] == 0xff && data[size - 1] == 0xd9;
}
}

int main()
{
    void *library = dlopen(kLibrary, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto query = reinterpret_cast<onekvm_video_backend_query_fn>(
        dlsym(library, ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL));
    const void *raw_api = nullptr;
    if (query == nullptr || query(ONEKVM_VIDEO_BACKEND_ABI_V1, &raw_api) != 0 ||
        raw_api == nullptr) {
        std::fprintf(stderr, "video backend ABI unavailable\n");
        return 1;
    }
    const auto *api = static_cast<const onekvm_video_backend_v1 *>(raw_api);
    if (api->struct_size < offsetof(onekvm_video_backend_v1, source_snapshot) +
            sizeof(api->source_snapshot) ||
        api->encoder_allocate == nullptr || api->source_snapshot == nullptr) {
        std::fprintf(stderr, "managed snapshot unavailable\n");
        return 1;
    }

    char error[512]{};
    onekvm_video_source_config_v1 source_config{};
    source_config.struct_size = sizeof(source_config);
    source_config.resolution = 1080;
    source_config.fps = 60;
    void *source = nullptr;
    if (api->source_create(&source_config, &source, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "source_create: %s\n", error);
        return 1;
    }

    onekvm_video_encoder_allocation_request_v1 stream_request{};
    stream_request.struct_size = sizeof(stream_request);
    stream_request.config.struct_size = sizeof(stream_request.config);
    stream_request.config.codec = ONEKVM_VIDEO_CODEC_H264;
    stream_request.config.quality_factor = 0.7;
    stream_request.config.fps = 60;
    stream_request.width = 1920;
    stream_request.height = 1080;
    stream_request.pixel_format = ONEKVM_VIDEO_PIXEL_NV21;
    stream_request.input_mode = ONEKVM_VIDEO_ENCODER_INPUT_BOUND;
    stream_request.purpose = ONEKVM_VIDEO_ENCODER_PURPOSE_REALTIME;
    onekvm_video_encoder_allocation_v1 stream_allocation{};
    stream_allocation.struct_size = sizeof(stream_allocation);
    void *stream = nullptr;
    if (api->encoder_allocate(&stream_request, &stream, &stream_allocation,
                              error, sizeof(error)) != ONEKVM_VIDEO_RESOURCE_OK) {
        std::fprintf(stderr, "stream allocation: %s\n", error);
        api->source_destroy(source);
        return 1;
    }
    if (api->encoder_bind_source(stream, source, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "stream bind: %s\n", error);
        api->encoder_destroy(stream);
        api->source_destroy(source);
        return 1;
    }

    std::atomic<bool> stop{false};
    std::atomic<unsigned> stream_packets{0};
    std::thread reader([&] {
        char reader_error[512]{};
        while (!stop.load(std::memory_order_relaxed)) {
            onekvm_video_packet_v1 packet{};
            packet.struct_size = sizeof(packet);
            if (api->encoder_read_packet(
                    stream, &packet, reader_error, sizeof(reader_error)) == 0 &&
                packet.data_size != 0) {
                stream_packets.fetch_add(1, std::memory_order_relaxed);
                api->encoder_release_packet(stream);
            }
        }
    });

    /* Let the HDMI watcher and bound reader reach steady state before taking
       the one-shot JPEG. A standalone process starts with no cached signal
       observation, and the receiver can take several seconds to settle after
       the previous MMF owner shuts down. */
    for (int attempt = 0; attempt < 100 &&
         stream_packets.load(std::memory_order_relaxed) < 5; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const unsigned packets_before =
        stream_packets.load(std::memory_order_relaxed);
    if (packets_before < 5)
        std::fprintf(stderr, "bound stream did not become ready\n");

    uint8_t jpeg[2 * 1024 * 1024]{};
    onekvm_video_snapshot_request_v1 snapshot_request{};
    snapshot_request.struct_size = sizeof(snapshot_request);
    snapshot_request.width = 1920;
    snapshot_request.height = 1080;
    snapshot_request.quality_factor = 0.8;
    snapshot_request.timeout_ms = 1000;
    onekvm_video_snapshot_result_v1 snapshot_result{};
    snapshot_result.struct_size = sizeof(snapshot_result);
    int exit_code = 1;
    const int snapshot = packets_before < 5 ? ONEKVM_VIDEO_RESOURCE_TIMEOUT :
        api->source_snapshot(source, &snapshot_request, jpeg, sizeof(jpeg),
                             &snapshot_result, error, sizeof(error));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const unsigned packets_after =
        stream_packets.load(std::memory_order_relaxed);
    if (snapshot == ONEKVM_VIDEO_RESOURCE_OK &&
        snapshot_result.width == 1920 && snapshot_result.height == 1080 &&
        complete_jpeg(jpeg, snapshot_result.data_size) &&
        packets_after > packets_before) {
        FILE *jpeg_file = std::fopen("/tmp/managed-snapshot.jpg", "wb");
        bool saved = false;
        if (jpeg_file != nullptr) {
            const size_t written = std::fwrite(
                jpeg, 1, static_cast<size_t>(snapshot_result.data_size), jpeg_file);
            const int close_result = std::fclose(jpeg_file);
            saved = written == snapshot_result.data_size && close_result == 0;
        }
        if (!saved) {
            std::fprintf(stderr, "save /tmp/managed-snapshot.jpg failed\n");
        } else {
            std::printf("jpeg=%llu stream_packets_before=%u stream_packets_after=%u\n",
            static_cast<unsigned long long>(snapshot_result.data_size),
            packets_before, packets_after);
            exit_code = 0;
        }
    } else {
        std::fprintf(stderr, "source_snapshot: %s\n", error);
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();
    api->encoder_unbind_source(stream, error, sizeof(error));
    api->encoder_destroy(stream);
    api->source_destroy(source);
    dlclose(library);
    return exit_code;
}
