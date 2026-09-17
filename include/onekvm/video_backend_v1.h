#ifndef ONEKVM_VIDEO_BACKEND_V1_H
#define ONEKVM_VIDEO_BACKEND_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OneKVM in-process video backend ABI v1.
 *
 * This is a C ABI on purpose: backend packages may be built with a different
 * compiler or language runtime than onekvm-server. Structures are append-only;
 * callers and backends must use struct_size before reading future fields.
 */
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
    ONEKVM_VIDEO_PIXEL_RGB565 = 5,
};

#define ONEKVM_VIDEO_FEATURE_SIGNAL_PRESENT (1ull << 0)
#define ONEKVM_VIDEO_FEATURE_INTERRUPT_READ  (1ull << 1)
#define ONEKVM_VIDEO_FEATURE_KEYFRAME        (1ull << 2)
#define ONEKVM_VIDEO_FEATURE_BORROWED_PACKET (1ull << 3)
#define ONEKVM_VIDEO_FEATURE_PREPARE_ENCODE  (1ull << 4)
#define ONEKVM_VIDEO_FEATURE_BOUND_ENCODER   (1ull << 5)
#define ONEKVM_VIDEO_FEATURE_LATENCY         (1ull << 6)
#define ONEKVM_VIDEO_FEATURE_EDID            (1ull << 7)
#define ONEKVM_VIDEO_FEATURE_ENCODER_ALLOCATION (1ull << 8)
#define ONEKVM_VIDEO_FEATURE_SOURCE_SNAPSHOT    (1ull << 9)
#define ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY     (1ull << 10)
#define ONEKVM_VIDEO_FEATURE_SOURCE_DISPLAY_RGA (1ull << 11)
#define ONEKVM_VIDEO_FEATURE_SOURCE_OVERLAY     (1ull << 12)

#define ONEKVM_VIDEO_DISPLAY_FLAG_RGA (1u << 0)
#define ONEKVM_VIDEO_OVERLAY_FLAG_RGA (1u << 0)

enum onekvm_video_resource_result_v1 {
    ONEKVM_VIDEO_RESOURCE_OK = 0,
    ONEKVM_VIDEO_RESOURCE_INVALID = -1,
    ONEKVM_VIDEO_RESOURCE_UNSUPPORTED = -2,
    ONEKVM_VIDEO_RESOURCE_BUSY = -3,
    ONEKVM_VIDEO_RESOURCE_NO_MEMORY = -4,
    ONEKVM_VIDEO_RESOURCE_UNINITIALIZED = -5,
    ONEKVM_VIDEO_RESOURCE_INTERNAL = -6,
    ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL = -7,
    ONEKVM_VIDEO_RESOURCE_TIMEOUT = -8,
};

enum onekvm_video_encoder_input_mode_v1 {
    ONEKVM_VIDEO_ENCODER_INPUT_MANUAL = 1,
    ONEKVM_VIDEO_ENCODER_INPUT_BOUND = 2,
};

#define ONEKVM_VIDEO_ENCODER_INPUT_MASK_MANUAL \
    (1u << ONEKVM_VIDEO_ENCODER_INPUT_MANUAL)
#define ONEKVM_VIDEO_ENCODER_INPUT_MASK_BOUND \
    (1u << ONEKVM_VIDEO_ENCODER_INPUT_BOUND)

enum onekvm_video_encoder_purpose_v1 {
    ONEKVM_VIDEO_ENCODER_PURPOSE_REALTIME = 1,
    ONEKVM_VIDEO_ENCODER_PURPOSE_BACKGROUND = 2,
};

#define ONEKVM_VIDEO_ENCODER_PURPOSE_MASK_REALTIME \
    (1u << ONEKVM_VIDEO_ENCODER_PURPOSE_REALTIME)
#define ONEKVM_VIDEO_ENCODER_PURPOSE_MASK_BACKGROUND \
    (1u << ONEKVM_VIDEO_ENCODER_PURPOSE_BACKGROUND)

#define ONEKVM_VIDEO_EDID_MAX_BYTES 512

enum onekvm_video_edid_apply_v1 {
    ONEKVM_VIDEO_EDID_APPLY_NONE = 0,
    ONEKVM_VIDEO_EDID_APPLY_HOTPLUG = 1,
    ONEKVM_VIDEO_EDID_APPLY_REBOOT = 2,
    ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE = 3,
};

#define ONEKVM_VIDEO_EDID_CAP_WRITABLE   (1u << 0)
#define ONEKVM_VIDEO_EDID_CAP_PERSISTENT (1u << 1)
#define ONEKVM_VIDEO_EDID_CAP_HOTPLUG    (1u << 2)

struct onekvm_video_source_config_v1 {
    uint32_t struct_size;
    const char *device; /* borrowed for the duration of the call */
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
    /* Non-zero tokens must be released exactly once through source_release. */
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

/* Last observed capture/encode durations. Zero means the backend has not
 * sampled that stage yet. Status/SSE must not probe hardware here. */
struct onekvm_video_latency_v1 {
    uint32_t struct_size;
    uint64_t capture_ns;
    uint64_t encode_ns;
};

struct onekvm_video_edid_caps_v1 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t min_bytes;
    uint32_t max_bytes;
    /* Expected activation after a successful edid_set on this board. */
    uint32_t apply_policy;
    const char *chip_id;
    const char *board_id;
};

struct onekvm_video_edid_blob_v1 {
    uint32_t struct_size;
    uint32_t size;
    uint8_t data[ONEKVM_VIDEO_EDID_MAX_BYTES];
};

struct onekvm_video_edid_apply_result_v1 {
    uint32_t struct_size;
    /* What the host/UI must do after this write. Must not reboot here. */
    uint32_t apply_required;
};

struct onekvm_video_encoder_resources_v1 {
    uint32_t struct_size;
    uint32_t policy_h26x_capacity;
    uint32_t policy_jpeg_capacity;
    uint32_t active_h26x;
    uint32_t active_jpeg;
    uint32_t hardware_channel_capacity;
    uint32_t input_mode_mask;
    uint32_t purpose_mask;
    uint64_t flags; /* Must be zero in ABI v1. */
};

struct onekvm_video_encoder_allocation_request_v1 {
    uint32_t struct_size;
    struct onekvm_video_encoder_config_v1 config;
    int32_t width;
    int32_t height;
    uint32_t pixel_format;
    uint32_t input_mode;
    uint32_t purpose;
    uint64_t flags; /* Must be zero in ABI v1. */
};

struct onekvm_video_encoder_allocation_v1 {
    uint32_t struct_size;
    uint64_t allocation_id;
    uint32_t codec;
    uint32_t input_mode;
    uint32_t purpose;
    int32_t width;
    int32_t height;
    uint32_t pixel_format;
    uint32_t flags; /* Zero until allocation capabilities are defined. */
};

struct onekvm_video_snapshot_request_v1 {
    uint32_t struct_size;
    int32_t width;
    int32_t height;
    double quality_factor;
    uint32_t timeout_ms;
    uint32_t flags; /* Must be zero in ABI v1. */
};

struct onekvm_video_snapshot_result_v1 {
    uint32_t struct_size;
    uint64_t data_size;
    int32_t width;
    int32_t height;
    uint64_t pts_ns;
};

/* Optional display: packed RGB565 (little-endian), no row padding.
 * width/height 0 follows the backend default. Explicit sizes must both
 * be even and positive. timeout_ms 0 uses 250; cap is 1000. flags on
 * the request must be zero. Result flags may set DISPLAY_FLAG_RGA when
 * the scale used 2D hardware. */
struct onekvm_video_display_request_v1 {
    uint32_t struct_size;
    int32_t width;
    int32_t height;
    uint32_t pixel_format; /* 0 or ONEKVM_VIDEO_PIXEL_RGB565 */
    uint32_t timeout_ms;
    uint32_t flags; /* Must be zero in ABI v1. */
};

struct onekvm_video_display_result_v1 {
    uint32_t struct_size;
    uint64_t data_size;
    int32_t width;
    int32_t height;
    uint32_t pixel_format;
    uint64_t pts_ns;
    uint32_t flags;
};

struct onekvm_video_overlay_request_v1 {
    uint32_t struct_size;
    int32_t dest_width;
    int32_t dest_height;
    int32_t dest_stride; /* pixels; 0 = dest_width */
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t zoom; /* 1, 2, or 4; 0 = 1 */
    int32_t pan_x; /* 0–1000 */
    int32_t pan_y;
    uint32_t pixel_format; /* 0 or ONEKVM_VIDEO_PIXEL_RGB565 */
    uint32_t timeout_ms;
    uint32_t flags; /* Must be zero in ABI v1. */
};

struct onekvm_video_overlay_result_v1 {
    uint32_t struct_size;
    int32_t width;
    int32_t height;
    uint32_t pixel_format;
    uint64_t pts_ns;
    uint32_t flags; /* OVERLAY_FLAG_RGA when 2D hardware composed */
};

struct onekvm_video_backend_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *driver_id;
    uint64_t codec_mask;
    uint64_t features;
    const struct onekvm_video_format_v1 *formats;
    uint32_t format_count;

    int32_t (*source_create)(const struct onekvm_video_source_config_v1 *config,
                             void **source, char *error, uint32_t error_capacity);
    int32_t (*source_reset)(void *source,
                            const struct onekvm_video_source_config_v1 *config,
                            char *error, uint32_t error_capacity);
    int32_t (*source_read)(void *source, struct onekvm_video_frame_v1 *frame,
                           char *error, uint32_t error_capacity);
    void (*source_release)(void *source, uint64_t token);
    int32_t (*source_signal_present)(void *source);
    void (*source_interrupt)(void *source);
    void (*source_destroy)(void *source);

    int32_t (*encoder_create)(const struct onekvm_video_encoder_config_v1 *config,
                              void **encoder, char *error, uint32_t error_capacity);
    int32_t (*encoder_reset)(void *encoder,
                             const struct onekvm_video_encoder_config_v1 *config,
                             char *error, uint32_t error_capacity);
    int32_t (*encoder_encode)(void *encoder,
                              const struct onekvm_video_frame_v1 *frame,
                              struct onekvm_video_packet_v1 *packet,
                              char *error, uint32_t error_capacity);
    int32_t (*encoder_set_quality)(void *encoder, double quality_factor,
                                   char *error, uint32_t error_capacity);
    int32_t (*encoder_request_keyframe)(void *encoder,
                                        char *error, uint32_t error_capacity);
    uint32_t (*encoder_codec)(void *encoder);
    void (*encoder_destroy)(void *encoder);
    /* Optional tail: release storage returned by encoder_encode. */
    void (*encoder_release_packet)(void *encoder);
    /* Optional tail: finish the previous asynchronous encode before Core
     * borrows the next source frame. encoder_encode then submits the current
     * frame and returns the packet prepared by this call. */
    int32_t (*encoder_prepare)(void *encoder,
                               char *error, uint32_t error_capacity);
    /* Optional tail: connect a source's hardware output directly to VENC.
     * While bound, callers read encoded access units without source_read or
     * encoder_encode. Unbind must stop production and discard queued output. */
    int32_t (*encoder_bind_source)(void *encoder, void *source,
                                   char *error, uint32_t error_capacity);
    int32_t (*encoder_read_packet)(void *encoder,
                                   struct onekvm_video_packet_v1 *packet,
                                   char *error, uint32_t error_capacity);
    int32_t (*encoder_unbind_source)(void *encoder,
                                     char *error, uint32_t error_capacity);
    /* Optional tail: report the physical input format currently observed by
     * the source.  This is distinct from the configured/scaled output format
     * and must not probe hardware when queried from a status path. */
    int32_t (*source_input_format)(void *source,
                                   struct onekvm_video_format_v1 *format);
    /* Optional tail: last capture/encode durations. Must load cached samples
     * only; do not GetFrame, GetStream, or dump procfs from a status path. */
    int32_t (*source_latency)(void *source,
                              struct onekvm_video_latency_v1 *latency);
    int32_t (*encoder_latency)(void *encoder,
                               struct onekvm_video_latency_v1 *latency);
    /* Optional tail: HDMI sink EDID advertised to the capture host.
     * Library-level; does not require a source handle. */
    int32_t (*edid_capabilities)(struct onekvm_video_edid_caps_v1 *caps);
    int32_t (*edid_get)(struct onekvm_video_edid_blob_v1 *edid,
                        char *error, uint32_t error_capacity);
    int32_t (*edid_set)(const struct onekvm_video_edid_blob_v1 *edid,
                        struct onekvm_video_edid_apply_result_v1 *apply,
                        char *error, uint32_t error_capacity);
    /* Optional tail: explicit, backend-owned encoder resource allocation.
     * Callers select policy and input shape, never a hardware channel. */
    int32_t (*encoder_resources)(
        struct onekvm_video_encoder_resources_v1 *resources,
        char *error, uint32_t error_capacity);
    int32_t (*encoder_allocate)(
        const struct onekvm_video_encoder_allocation_request_v1 *request,
        void **encoder,
        struct onekvm_video_encoder_allocation_v1 *allocation,
        char *error, uint32_t error_capacity);
    int32_t (*encoder_allocation)(
        void *encoder,
        struct onekvm_video_encoder_allocation_v1 *allocation,
        char *error, uint32_t error_capacity);
    int32_t (*source_snapshot)(
        void *source,
        const struct onekvm_video_snapshot_request_v1 *request,
        uint8_t *data, uint64_t capacity,
        struct onekvm_video_snapshot_result_v1 *result,
        char *error, uint32_t error_capacity);
    /* Optional tail: scaled RGB565 display. Does not use a JPEG encoder.
     * NULL / missing FEATURE_SOURCE_DISPLAY means the machine has no
     * display ABI; callers fall back to encoded or snapshot. */
    int32_t (*source_display)(
        void *source,
        const struct onekvm_video_display_request_v1 *request,
        uint8_t *data, uint64_t capacity,
        struct onekvm_video_display_result_v1 *result,
        char *error, uint32_t error_capacity);
    /* Optional tail: compose a cover-scaled RGB565 view into dest.
     * dest is packed LE RGB565, dest_stride pixels (0 = dest_width).
     * zoom is 1/2/4; pan_x/pan_y are 0–1000. Does not clear dest outside
     * the rect. SPI/fbdev panels have no VOP plane; this is the overlay. */
    int32_t (*source_overlay)(
        void *source,
        const struct onekvm_video_overlay_request_v1 *request,
        uint8_t *dest, uint64_t dest_bytes,
        struct onekvm_video_overlay_result_v1 *result,
        char *error, uint32_t error_capacity);
};

/*
 * The returned table and all strings/format arrays it references remain valid
 * until the shared object is unloaded. Encoded packet storage remains valid
 * until the next encoder call; frame storage remains valid until release.
 */
typedef int32_t (*onekvm_video_backend_query_fn)(
    uint32_t requested_abi, const void **backend_api);

ONEKVM_VIDEO_EXPORT int32_t onekvm_video_backend_query(
    uint32_t requested_abi, const void **backend_api);

#ifdef __cplusplus
}
#endif

#endif
