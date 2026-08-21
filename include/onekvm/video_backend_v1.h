#ifndef ONEKVM_VIDEO_BACKEND_V1_H
#define ONEKVM_VIDEO_BACKEND_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ONEKVM_VIDEO_BACKEND_ABI_V1 1u
#define ONEKVM_VIDEO_BACKEND_QUERY_SYMBOL "onekvm_video_backend_query"

#if defined(__GNUC__)
#define ONEKVM_VIDEO_EXPORT __attribute__((visibility("default")))
#else
#define ONEKVM_VIDEO_EXPORT
#endif

enum onekvm_video_codec_v1 {
    ONEKVM_VIDEO_CODEC_AUTO = 0,
    ONEKVM_VIDEO_CODEC_H264 = 1,
    ONEKVM_VIDEO_CODEC_H265 = 2,
    ONEKVM_VIDEO_CODEC_MJPEG = 3,
};

#define ONEKVM_VIDEO_CODEC_MASK_AUTO   (1ull << ONEKVM_VIDEO_CODEC_AUTO)
#define ONEKVM_VIDEO_CODEC_MASK_H264   (1ull << ONEKVM_VIDEO_CODEC_H264)
#define ONEKVM_VIDEO_CODEC_MASK_H265   (1ull << ONEKVM_VIDEO_CODEC_H265)
#define ONEKVM_VIDEO_CODEC_MASK_MJPEG  (1ull << ONEKVM_VIDEO_CODEC_MJPEG)

enum onekvm_video_pixel_format_v1 {
    ONEKVM_VIDEO_PIXEL_UNKNOWN = 0,
    ONEKVM_VIDEO_PIXEL_YUV420P = 1,
    ONEKVM_VIDEO_PIXEL_NV12 = 2,
    ONEKVM_VIDEO_PIXEL_NV21 = 3,
    ONEKVM_VIDEO_PIXEL_MJPEG = 4,
};

#define ONEKVM_VIDEO_FEATURE_SIGNAL_PRESENT (1ull << 0)
#define ONEKVM_VIDEO_FEATURE_INTERRUPT_READ  (1ull << 1)
#define ONEKVM_VIDEO_FEATURE_KEYFRAME        (1ull << 2)
#define ONEKVM_VIDEO_FEATURE_BORROWED_PACKET (1ull << 3)
#define ONEKVM_VIDEO_FEATURE_PREPARE_ENCODE  (1ull << 4)
#define ONEKVM_VIDEO_FEATURE_BOUND_ENCODER   (1ull << 5)
#define ONEKVM_VIDEO_FEATURE_LATENCY         (1ull << 6)

struct onekvm_video_source_config_v1 {
    uint32_t struct_size;
    const char *device;
    int32_t resolution;
    int32_t fps;
};

struct onekvm_video_encoder_config_v1 {
    uint32_t struct_size;
    uint32_t codec;
    double quality_factor;
    int32_t gop;
    int32_t fps;
    /* Optional advanced VBR overrides. Zero keeps backend automatic values. */
    int32_t bitrate_kbps;
    int32_t initial_qp;
    int32_t min_qp;
    int32_t max_qp;
};

#define ONEKVM_VIDEO_FORMAT_OUT_OF_RANGE (1u << 0)

struct onekvm_video_format_v1 {
    uint32_t struct_size;
    int32_t width;
    int32_t height;
    int32_t fps;
    uint32_t pixel_format;
    uint32_t flags;
};

#define ONEKVM_VIDEO_FORMAT_V1_BASE_SIZE \
    (offsetof(struct onekvm_video_format_v1, pixel_format) + sizeof(uint32_t))

struct onekvm_video_frame_v1 {
    uint32_t struct_size;
    const uint8_t *data;
    uint64_t data_size;
    int32_t width;
    int32_t height;
    uint32_t pixel_format;
    uint64_t pts_ns;
    uint64_t token;
};

struct onekvm_video_packet_v1 {
    uint32_t struct_size;
    const uint8_t *data;
    uint64_t data_size;
    uint32_t codec;
    uint32_t key_frame;
    uint64_t pts_ns;
};

struct onekvm_video_latency_v1 {
    uint32_t struct_size;
    uint64_t capture_ns;
    uint64_t encode_ns;
};

struct onekvm_video_backend_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *driver_id;
    uint64_t codec_mask;
    uint64_t features;
    const struct onekvm_video_format_v1 *formats;
    uint32_t format_count;
    int32_t (*source_create)(const struct onekvm_video_source_config_v1 *, void **, char *, uint32_t);
    int32_t (*source_reset)(void *, const struct onekvm_video_source_config_v1 *, char *, uint32_t);
    int32_t (*source_read)(void *, struct onekvm_video_frame_v1 *, char *, uint32_t);
    void (*source_release)(void *, uint64_t);
    int32_t (*source_signal_present)(void *);
    void (*source_interrupt)(void *);
    void (*source_destroy)(void *);
    int32_t (*encoder_create)(const struct onekvm_video_encoder_config_v1 *, void **, char *, uint32_t);
    int32_t (*encoder_reset)(void *, const struct onekvm_video_encoder_config_v1 *, char *, uint32_t);
    int32_t (*encoder_encode)(void *, const struct onekvm_video_frame_v1 *, struct onekvm_video_packet_v1 *, char *, uint32_t);
    int32_t (*encoder_set_quality)(void *, double, char *, uint32_t);
    int32_t (*encoder_request_keyframe)(void *, char *, uint32_t);
    uint32_t (*encoder_codec)(void *);
    void (*encoder_destroy)(void *);
    void (*encoder_release_packet)(void *);
    int32_t (*encoder_prepare)(void *, char *, uint32_t);
    int32_t (*encoder_bind_source)(void *, void *, char *, uint32_t);
    int32_t (*encoder_read_packet)(void *, struct onekvm_video_packet_v1 *, char *, uint32_t);
    int32_t (*encoder_unbind_source)(void *, char *, uint32_t);
    int32_t (*source_input_format)(void *, struct onekvm_video_format_v1 *);
    int32_t (*source_latency)(void *, struct onekvm_video_latency_v1 *);
    int32_t (*encoder_latency)(void *, struct onekvm_video_latency_v1 *);
};

typedef int32_t (*onekvm_video_backend_query_fn)(uint32_t, const void **);

ONEKVM_VIDEO_EXPORT int32_t onekvm_video_backend_query(
    uint32_t requested_abi, const void **backend_api);

#ifdef __cplusplus
}
#endif

#endif
