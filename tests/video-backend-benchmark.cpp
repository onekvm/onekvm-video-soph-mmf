#include <onekvm/video_backend_v1.h>

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr const char *kLibrary = "/usr/lib/onekvm/video-backends/nanokvm-mmf.so";

struct Measurement {
    int captured = 0;
    int packets = 0;
    int empty_packets = 0;
    int errors = 0;
    uint64_t bytes = 0;
    uint64_t largest_packet = 0;
    double seconds = 0;
    double total_encode_us = 0;
    double maximum_encode_us = 0;
    double peak_kbps = 0;
};

const char *codec_name(uint32_t codec) {
    return codec == ONEKVM_VIDEO_CODEC_MJPEG ? "mjpeg" : "h264";
}

bool encode_one(const onekvm_video_backend_v1 *api, void *source, void *encoder,
                uint64_t *packet_bytes, double *encode_us, char *error,
                size_t error_capacity) {
    onekvm_video_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    std::memset(error, 0, error_capacity);
    if (api->source_read(source, &frame, error, error_capacity) != 0) {
        return false;
    }
    onekvm_video_packet_v1 packet{};
    packet.struct_size = sizeof(packet);
    const auto started = Clock::now();
    const int result = api->encoder_encode(encoder, &frame, &packet, error, error_capacity);
    const auto finished = Clock::now();
    if (frame.token != 0) api->source_release(source, frame.token);
    *encode_us = std::chrono::duration<double, std::micro>(finished - started).count();
    if (result != 0) return false;
    *packet_bytes = packet.data_size;
    return true;
}

Measurement measure(const onekvm_video_backend_v1 *api, void *source, void *encoder,
                    int duration_seconds) {
    char error[512] = {};
    for (int warmup = 0; warmup < 12; ++warmup) {
        uint64_t bytes = 0;
        double encode_us = 0;
        if (!encode_one(api, source, encoder, &bytes, &encode_us, error, sizeof(error)))
            std::fprintf(stderr, "warmup: %s\n", error[0] ? error : "failed");
    }

    Measurement value;
    const auto started = Clock::now();
    auto window_started = started;
    uint64_t window_bytes = 0;
    while (Clock::now() - started < std::chrono::seconds(duration_seconds)) {
        uint64_t packet_bytes = 0;
        double encode_us = 0;
        if (!encode_one(api, source, encoder, &packet_bytes, &encode_us, error, sizeof(error))) {
            ++value.errors;
            std::fprintf(stderr, "frame: %s\n", error[0] ? error : "failed");
            continue;
        }
        ++value.captured;
        value.total_encode_us += encode_us;
        value.maximum_encode_us = std::max(value.maximum_encode_us, encode_us);
        if (packet_bytes == 0) {
            ++value.empty_packets;
        } else {
            ++value.packets;
            value.bytes += packet_bytes;
            window_bytes += packet_bytes;
            value.largest_packet = std::max(value.largest_packet, packet_bytes);
        }
        const auto now = Clock::now();
        const double window_ms = std::chrono::duration<double, std::milli>(now - window_started).count();
        if (window_ms >= 1000) {
            value.peak_kbps = std::max(value.peak_kbps, window_bytes * 8.0 / window_ms);
            window_started = now;
            window_bytes = 0;
        }
    }
    const auto finished = Clock::now();
    value.seconds = std::chrono::duration<double>(finished - started).count();
    const double tail_ms = std::chrono::duration<double, std::milli>(finished - window_started).count();
    if (tail_ms > 100)
        value.peak_kbps = std::max(value.peak_kbps, window_bytes * 8.0 / tail_ms);
    return value;
}

Measurement measure_synthetic(const onekvm_video_backend_v1 *api, void *encoder,
                              int duration_seconds, bool correlated_motion) {
    constexpr int width = 1920;
    constexpr int height = 1080;
    std::vector<uint8_t> frames[2] = {
        std::vector<uint8_t>(width * height * 3 / 2),
        std::vector<uint8_t>(width * height * 3 / 2),
    };
    uint32_t random = 0x6f6e656b;
    if (correlated_motion) {
        /* A block-textured desktop-like picture is complex enough to exercise
         * rate control, while remaining motion-compensatable after scrolling.
         * Starting with pixel noise would make the first I-frame exceed the
         * frame-loss threshold and turn every following frame into PSKIP. */
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; column += 16) {
                uint32_t block = 0x6f6e656bU ^ static_cast<uint32_t>(row / 16) * 0x9e3779b9U
                                 ^ static_cast<uint32_t>(column / 16) * 0x85ebca6bU;
                block ^= block << 13;
                block ^= block >> 17;
                block ^= block << 5;
                const uint8_t luma = 32 + static_cast<uint8_t>(block % 192);
                std::memset(frames[0].data() + row * width + column, luma, 16);
            }
        }
        std::memset(frames[0].data() + width * height, 128, width * height / 2);
        constexpr int shift = 16;
        for (int row = 0; row < height; ++row) {
            const uint8_t *source = frames[0].data() + row * width;
            uint8_t *target = frames[1].data() + row * width;
            std::memcpy(target, source + shift, width - shift);
            std::memcpy(target + width - shift, source, shift);
        }
        const int chroma_offset = width * height;
        for (int row = 0; row < height / 2; ++row) {
            const uint8_t *source = frames[0].data() + chroma_offset + row * width;
            uint8_t *target = frames[1].data() + chroma_offset + row * width;
            std::memcpy(target, source + shift, width - shift);
            std::memcpy(target + width - shift, source, shift);
        }
    } else {
        for (auto &frame : frames) {
            for (uint8_t &value : frame) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                value = static_cast<uint8_t>(random);
            }
        }
    }

    Measurement value;
    char error[512] = {};
    const auto started = Clock::now();
    auto window_started = started;
    auto next_frame = started;
    uint64_t window_bytes = 0;
    int index = 0;
    while (Clock::now() - started < std::chrono::seconds(duration_seconds)) {
        onekvm_video_frame_v1 frame{};
        frame.struct_size = sizeof(frame);
        frame.data = frames[index++ & 1].data();
        frame.data_size = frames[0].size();
        frame.width = width;
        frame.height = height;
        frame.pixel_format = ONEKVM_VIDEO_PIXEL_NV21;
        frame.pts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
        onekvm_video_packet_v1 packet{};
        packet.struct_size = sizeof(packet);
        const auto encode_started = Clock::now();
        const int result = api->encoder_encode(encoder, &frame, &packet, error, sizeof(error));
        const auto now = Clock::now();
        const double encode_us = std::chrono::duration<double, std::micro>(
            now - encode_started).count();
        ++value.captured;
        value.total_encode_us += encode_us;
        value.maximum_encode_us = std::max(value.maximum_encode_us, encode_us);
        if (result != 0) {
            ++value.errors;
            std::fprintf(stderr, "synthetic frame: %s\n", error);
        } else if (packet.data_size == 0) {
            ++value.empty_packets;
        } else {
            ++value.packets;
            value.bytes += packet.data_size;
            window_bytes += packet.data_size;
            value.largest_packet = std::max(value.largest_packet, packet.data_size);
        }
        const double window_ms = std::chrono::duration<double, std::milli>(
            now - window_started).count();
        if (window_ms >= 1000) {
            value.peak_kbps = std::max(value.peak_kbps, window_bytes * 8.0 / window_ms);
            window_started = now;
            window_bytes = 0;
        }
        next_frame += std::chrono::nanoseconds(1000000000 / 60);
        std::this_thread::sleep_until(next_frame);
    }
    const auto finished = Clock::now();
    value.seconds = std::chrono::duration<double>(finished - started).count();
    const double tail_ms = std::chrono::duration<double, std::milli>(
        finished - window_started).count();
    if (tail_ms > 100)
        value.peak_kbps = std::max(value.peak_kbps, window_bytes * 8.0 / tail_ms);
    return value;
}

void print_measurement(const char *source, uint32_t codec, const Measurement &value) {
    const double fps = value.seconds > 0 ? value.packets / value.seconds : 0;
    const double capture_fps = value.seconds > 0 ? value.captured / value.seconds : 0;
    const double average_kbps = value.seconds > 0 ? value.bytes * 8.0 / value.seconds / 1000.0 : 0;
    const double average_encode_us = value.captured > 0 ? value.total_encode_us / value.captured : 0;
    std::printf(
        "{\"source\":\"%s\",\"codec\":\"%s\",\"seconds\":%.3f,\"capture_fps\":%.2f,"
        "\"encoded_fps\":%.2f,\"average_kbps\":%.1f,\"peak_kbps\":%.1f,"
        "\"packets\":%d,\"empty_packets\":%d,\"errors\":%d,"
        "\"largest_packet\":%llu,\"average_encode_us\":%.1f,"
        "\"maximum_encode_us\":%.1f}\n",
        source, codec_name(codec), value.seconds, capture_fps, fps, average_kbps, value.peak_kbps,
        value.packets, value.empty_packets, value.errors,
        static_cast<unsigned long long>(value.largest_packet), average_encode_us,
        value.maximum_encode_us);
    std::fflush(stdout);
}

bool maintains_realtime_cadence(const Measurement &value) {
    if (value.seconds <= 0) return false;
    return value.captured / value.seconds >= 55.0 &&
           value.packets / value.seconds >= 55.0;
}

double average_kbps(const Measurement &value) {
    return value.seconds > 0 ? value.bytes * 8.0 / value.seconds / 1000.0 : 0;
}

bool reset_codec(const onekvm_video_backend_v1 *api, void *source, void *encoder,
                 uint32_t codec, char *error, size_t error_capacity) {
    onekvm_video_source_config_v1 source_config{};
    source_config.struct_size = sizeof(source_config);
    source_config.resolution = 1080;
    source_config.fps = 60;
    if (api->source_reset(source, &source_config, error, error_capacity) != 0) return false;
    onekvm_video_encoder_config_v1 encoder_config{};
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = codec;
    encoder_config.quality_factor = 1.0;
    encoder_config.gop = 0; // exercise the production default GOP
    encoder_config.fps = 60;
    return api->encoder_reset(encoder, &encoder_config, error, error_capacity) == 0;
}

} // namespace

int main(int argc, char **argv) {
    const int duration_seconds = argc > 1 ? std::max(1, std::atoi(argv[1])) : 8;
    const int switch_cycles = argc > 2 ? std::max(0, std::atoi(argv[2])) : 8;
    void *library = dlopen(kLibrary, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto query = reinterpret_cast<onekvm_video_backend_query_fn>(
        dlsym(library, ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL));
    const void *raw_api = nullptr;
    if (!query || query(ONEKVM_VIDEO_BACKEND_ABI_V1, &raw_api) != 0 || !raw_api) {
        std::fprintf(stderr, "query ABI v1 failed\n");
        return 1;
    }
    const auto *api = static_cast<const onekvm_video_backend_v1 *>(raw_api);
    char error[512] = {};
    onekvm_video_source_config_v1 source_config{};
    source_config.struct_size = sizeof(source_config);
    source_config.resolution = 1080;
    source_config.fps = 60;
    void *source = nullptr;
    if (api->source_create(&source_config, &source, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "source_create: %s\n", error);
        return 1;
    }
    onekvm_video_encoder_config_v1 encoder_config{};
    encoder_config.struct_size = sizeof(encoder_config);
    encoder_config.codec = ONEKVM_VIDEO_CODEC_H264;
    encoder_config.quality_factor = 1.0;
    encoder_config.gop = 0; // exercise the production default GOP
    encoder_config.fps = 60;
    void *encoder = nullptr;
    if (api->encoder_create(&encoder_config, &encoder, error, sizeof(error)) != 0) {
        std::fprintf(stderr, "encoder_create: %s\n", error);
        api->source_destroy(source);
        return 1;
    }

    int failures = 0;
    const uint32_t codecs[] = {ONEKVM_VIDEO_CODEC_H264, ONEKVM_VIDEO_CODEC_MJPEG};
    for (uint32_t codec : codecs) {
        if (!reset_codec(api, source, encoder, codec, error, sizeof(error))) {
            std::fprintf(stderr, "reset %s: %s\n", codec_name(codec), error);
            ++failures;
            continue;
        }
        const Measurement value = measure(api, source, encoder, duration_seconds);
        print_measurement("hdmi", codec, value);
        if (value.errors != 0 || !maintains_realtime_cadence(value)) ++failures;
    }

    if (!reset_codec(api, source, encoder, ONEKVM_VIDEO_CODEC_H264, error, sizeof(error))) {
        std::fprintf(stderr, "reset synthetic h264: %s\n", error);
        ++failures;
    } else {
        const Measurement value = measure_synthetic(api, encoder, duration_seconds, true);
        print_measurement("synthetic-motion", ONEKVM_VIDEO_CODEC_H264, value);
        /* The scrolling block pattern is deterministic and sufficiently
         * complex to exercise VBR. Catch unit/mode regressions that silently
         * leave a nominal 10 Mbps quality setting near 1 Mbps. */
        if (value.errors != 0 || !maintains_realtime_cadence(value) ||
            average_kbps(value) < 5000.0) {
            std::fprintf(stderr, "synthetic-motion VBR response too low: %.1f kbps\n",
                         average_kbps(value));
            ++failures;
        }
    }

    if (!reset_codec(api, source, encoder, ONEKVM_VIDEO_CODEC_H264, error, sizeof(error))) {
        std::fprintf(stderr, "reset synthetic noise h264: %s\n", error);
        ++failures;
    } else {
        const Measurement value = measure_synthetic(api, encoder, duration_seconds, false);
        print_measurement("synthetic-noise", ONEKVM_VIDEO_CODEC_H264, value);
        if (value.errors != 0 || !maintains_realtime_cadence(value)) ++failures;
    }

    for (int cycle = 0; cycle < switch_cycles; ++cycle) {
        const uint32_t codec = cycle % 2 == 0 ? ONEKVM_VIDEO_CODEC_H264 : ONEKVM_VIDEO_CODEC_MJPEG;
        if (!reset_codec(api, source, encoder, codec, error, sizeof(error))) {
            std::fprintf(stderr, "switch cycle %d to %s: %s\n", cycle + 1,
                         codec_name(codec), error);
            ++failures;
            break;
        }
        for (int frame = 0; frame < 8; ++frame) {
            uint64_t bytes = 0;
            double encode_us = 0;
            if (!encode_one(api, source, encoder, &bytes, &encode_us, error, sizeof(error))) {
                std::fprintf(stderr, "switch cycle %d frame %d: %s\n", cycle + 1,
                             frame + 1, error);
                ++failures;
                break;
            }
        }
    }
    std::printf("{\"switch_cycles\":%d,\"failures\":%d}\n", switch_cycles, failures);
    api->encoder_destroy(encoder);
    api->source_destroy(source);
    dlclose(library);
    return failures == 0 ? 0 : 1;
}
