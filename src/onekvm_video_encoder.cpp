#include "onekvm_video_backend_internal.hpp"
#include "h264_annexb.hpp"
#include "mmf_internal.hpp"

#pragma GCC visibility push(hidden)
namespace onekvm::video_backend {

int codec_type(uint32_t codec) {
    switch (codec) {
    case ONEKVM_VIDEO_CODEC_AUTO:
    case ONEKVM_VIDEO_CODEC_H264:
        return 2;
    case ONEKVM_VIDEO_CODEC_H265:
        return 1;
    case ONEKVM_VIDEO_CODEC_MJPEG:
        return 0;
    default:
        return -1;
    }
}

uint32_t public_codec(int type) {
    if (type == 0) return ONEKVM_VIDEO_CODEC_MJPEG;
    if (type == 1) return ONEKVM_VIDEO_CODEC_H265;
    return ONEKVM_VIDEO_CODEC_H264;
}

namespace {
std::atomic<uint64_t> g_next_allocation_id{1};
Encoder *g_encoder_owners[kLastVENCChannel + 1]{};
uint64_t g_encoder_owner_generations[kLastVENCChannel + 1]{};

uint64_t next_allocation_id() {
    uint64_t id = g_next_allocation_id.load(std::memory_order_relaxed);
    while (id != UINT64_MAX) {
        if (g_next_allocation_id.compare_exchange_weak(
                id, id + 1, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return id;
    }
    return 0;
}

void fill_allocation(const Encoder *encoder,
                     onekvm_video_encoder_allocation_v1 *allocation) {
    allocation->allocation_id = encoder->allocation_id;
    allocation->codec = public_codec(encoder->codec_type);
    allocation->input_mode = encoder->allocation_input_mode;
    allocation->purpose = encoder->allocation_purpose;
    allocation->width = encoder->allocation_width;
    allocation->height = encoder->allocation_height;
    allocation->pixel_format = encoder->allocation_pixel_format;
    allocation->flags = 0;
}
} // namespace

int jpeg_quality(double quality) {
    if (quality <= 0) return 80;
    quality = std::min(quality, 1.0);
    return 55 + static_cast<int>(quality * 35);
}

int bitrate(int width, int height, double quality, int fps = 0) {
    if (quality <= 0) quality = 1.0;
    quality = std::min(quality, 1.0);
    /* NanoKVM's highest H.264 quality preset is 5 Mbps.  OneKVM's maximum
       quality doubles that ceiling for sharper text and motion at 1080p60. */
    /* Cast before multiplying: 1920 * 1080 * 10000 exceeds signed int32 and
       previously wrapped before division, silently falling through to the
       100 kbps safety floor. */
    const int64_t base = static_cast<int64_t>(width) * height * 10000 /
                         (static_cast<int64_t>(1920) * 1080);
    const int output_fps = mmf::clamp_output_fps(fps, width, height);
    const double fps_scale = output_fps > mmf::kDefaultInputFps
        ? static_cast<double>(output_fps) / mmf::kDefaultInputFps
        : 1.0;
    return std::max(100, static_cast<int>(static_cast<double>(base) * quality * fps_scale));
}

static int normalized_output_fps(int fps, int width = 0, int height = 0)
{
    return mmf::clamp_output_fps(fps, width, height);
}

static int normalized_gop(int gop, int fps)
{
    if (gop > 0)
        return gop;
    return std::max(1, fps);
}

void close_encoder(Encoder *encoder) {
    if (!encoder->initialized) {
        return;
    }
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    const bool live = encoder->mmf_generation ==
        g_mmf_generation.load(std::memory_order_acquire);
    if (live && encoder->packet_borrowed && encoder->codec_type != 0) {
        mmf::release_h26x_packet(encoder->channel);
        encoder->packet_borrowed = false;
    }
    /* Do not detach a live VPSS producer here. close_h26x_encoder() owns the
       complete teardown: it joins the reader, wakes the parked scaler, then
       performs the two StopRecvFrame calls around UnBind. Splitting that
       sequence across this wrapper can strand the vendor bind worker. */
    if (live) {
        if (encoder->codec_type == 0) {
            mmf::close_jpeg_encoder(encoder->channel);
        } else {
            mmf::close_h26x_encoder(encoder->channel);
        }
    }
    if (encoder->channel >= 0 && encoder->channel <= kLastVENCChannel &&
        g_encoder_owners[encoder->channel] == encoder) {
        g_encoder_owners[encoder->channel] = nullptr;
        g_encoder_owner_generations[encoder->channel] = 0;
    }
    encoder->initialized = false;
    encoder->channel = -1;
    encoder->width = 0;
    encoder->height = 0;
    encoder->frame_pending = false;
    encoder->source_bound = false;
    encoder->placeholder_frames = false;
    encoder->placeholder_need_key = false;
    encoder->placeholder_idr_tries = 0;
    encoder->placeholder_logged = false;
    encoder->packet_borrowed = false;
    encoder->pending_pts_ns = 0;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
    encoder->bound_since_ns = 0;
    encoder->mmf_generation = 0;
    encoder->bound_source = nullptr;
}

void invalidate_stale_encoder(Encoder *encoder) {
    if (!encoder->initialized || encoder->mmf_generation ==
        g_mmf_generation.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->channel >= 0 && encoder->channel <= kLastVENCChannel &&
            g_encoder_owners[encoder->channel] == encoder) {
            g_encoder_owners[encoder->channel] = nullptr;
            g_encoder_owner_generations[encoder->channel] = 0;
        }
    }
    encoder->initialized = false;
    encoder->channel = -1;
    encoder->width = 0;
    encoder->height = 0;
    encoder->source_bound = false;
    encoder->placeholder_frames = false;
    encoder->placeholder_need_key = false;
    encoder->placeholder_idr_tries = 0;
    encoder->placeholder_logged = false;
    encoder->request_keyframe = true;
    encoder->frame_pending = false;
    encoder->packet_borrowed = false;
    encoder->pending_pts_ns = 0;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
    encoder->bound_since_ns = 0;
    encoder->mmf_generation = 0;
}

void recover_encoder_after_stream_error(Encoder *encoder) {
    /* GetStream can consume and release a malformed/oversized access unit
       before reporting an error. Reusing the old frame_pending flag then
       deadlocks the software state: no old unit exists, but no new frame is
       submitted either. Recreate the channel on the next call and force a
       clean decoder reference chain. Preserve the desired source binding so
       the bound path can reconnect itself. */
    Source *bound_source = encoder->bound_source;
    close_encoder(encoder);
    encoder->bound_source = bound_source;
    encoder->request_keyframe = true;
}

int configure_encoder(Encoder *encoder, int width, int height,
                      char *error, uint32_t error_capacity) {
    invalidate_stale_encoder(encoder);
    if (encoder->initialized && encoder->width == width && encoder->height == height) {
        return 0;
    }
    close_encoder(encoder);
    size_t frame_size = 0;
    if (!nv21_size(width, height, &frame_size)) {
        set_error(error, error_capacity, "invalid encoder frame size %dx%d",
                  width, height);
        return -1;
    }
    (void)frame_size;
    const size_t output_size = encoder->codec_type == 0
        ? kJPEGBufferSize : kVENCBufferSize;
    try {
        /* Allocate before opening a hardware channel so bad_alloc cannot leak
           a channel that has already started its receive worker. */
        encoder->output.resize(output_size);
    } catch (const std::bad_alloc &) {
        set_error(error, error_capacity, "allocate encoder output: out of memory");
        return ONEKVM_VIDEO_RESOURCE_NO_MEMORY;
    }
    int fps = normalized_output_fps(encoder->config.fps, width, height);
    // The capture/VPSS path can continue delivering frames at the HDMI input
    // cadence even when the requested output is lower.  Tell VENC the
    // expected input cadence separately from the requested destination rate;
    // using the target for both fields makes the driver assume every submitted
    // frame is already at the target rate and it will not perform rate
    // conversion (the stream then remains close to the source FPS).
    const int venc_input_fps = mmf::venc_src_fps(fps, width, height);
    /* Keep the default GOP at one second. This avoids spending excessive VBR
       budget on IDR frames while still bounding decoder recovery latency. */
    int gop = encoder->config.gop > 0 ? encoder->config.gop : std::max(1, fps);
    int result = 0;
    int channel = kJPEGChannel;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->codec_type == 0) {
            channel = kJPEGChannel;
            const uint64_t generation =
                g_mmf_generation.load(std::memory_order_acquire);
            if (g_encoder_owner_generations[channel] != generation) {
                g_encoder_owners[channel] = nullptr;
                g_encoder_owner_generations[channel] = 0;
            }
            if (g_encoder_owners[channel] != nullptr &&
                g_encoder_owners[channel] != encoder) {
                set_error(error, error_capacity,
                          "MMF JPEG encoder is already allocated");
                return ONEKVM_VIDEO_RESOURCE_BUSY;
            }
            result = mmf::open_jpeg_encoder(channel, width, height, kMMFNV21,
                                      jpeg_quality(encoder->config.quality_factor));
        } else {
            mmf::H26xEncoderConfig config{};
            config.codec = encoder->codec_type == 1
                ? mmf::H26xCodec::H265 : mmf::H26xCodec::H264;
            config.width = width;
            config.height = height;
            config.pixel_format = kMMFNV21;
            config.gop = gop;
            config.input_fps = venc_input_fps;
            config.output_fps = fps;
            config.bitrate_kbps = encoder->config.bitrate_kbps > 0
                ? encoder->config.bitrate_kbps
                : bitrate(width, height, encoder->config.quality_factor, fps);
            mmf::RateControl rc{};
            rc.initial_qp = encoder->config.initial_qp;
            rc.min_qp = encoder->config.min_qp;
            rc.max_qp = encoder->config.max_qp;
            /* MMF's channel table is authoritative. Try each channel and let
               MMF report EBUSY atomically instead of mirroring occupancy in a
               second bitmap that can become stale while protocol sessions
               overlap during connect/disconnect. */
            result = -ENOSPC;
            for (int candidate = kFirstVENCChannel;
                 candidate <= kLastVENCChannel; ++candidate) {
                result = mmf::open_h26x_encoder(candidate, config, rc);
                if (result == 0) {
                    channel = candidate;
                    break;
                }
                if (result != -EBUSY) break;
            }
            if (result == -EBUSY || result == -ENOSPC) {
                set_error(error, error_capacity,
                          "no free MMF H.26x encoder channel");
                return ONEKVM_VIDEO_RESOURCE_BUSY;
            }
        }
        if (result == 0) {
            g_encoder_owners[channel] = encoder;
            g_encoder_owner_generations[channel] =
                g_mmf_generation.load(std::memory_order_acquire);
        }
    }
    if (result != 0) {
        set_error(error, error_capacity, "initialize MMF encoder failed: %d", result);
        return ONEKVM_VIDEO_RESOURCE_INTERNAL;
    }
    encoder->channel = channel;
    encoder->width = width;
    encoder->height = height;
    encoder->initialized = true;
    encoder->mmf_generation = g_mmf_generation.load(std::memory_order_acquire);
    return 0;
}

bool valid_encoder_config(const onekvm_video_encoder_config_v1 *config) {
    constexpr size_t base_size = offsetof(onekvm_video_encoder_config_v1, bitrate_kbps);
    if (config == nullptr || config->struct_size < base_size ||
        codec_type(config->codec) < 0 || !std::isfinite(config->quality_factor) ||
        config->quality_factor < 0 || config->quality_factor > 1 ||
        config->fps < 0 || config->gop < 0)
        return false;
    if (config->struct_size < sizeof(*config)) return true;
    if (config->bitrate_kbps != 0 &&
        (config->bitrate_kbps < 100 || config->bitrate_kbps > 50000)) return false;
    if (config->initial_qp < 0 || config->initial_qp > 51 ||
        config->min_qp < 0 || config->min_qp > 51 ||
        config->max_qp < 0 || config->max_qp > 51) return false;
    if (config->bitrate_kbps != 0 &&
        (config->initial_qp != 0 || config->min_qp != 0 || config->max_qp != 0)) return false;
    return config->min_qp == 0 || config->max_qp == 0 || config->min_qp <= config->max_qp;
}

onekvm_video_encoder_config_v1 normalized_encoder_config(
        const onekvm_video_encoder_config_v1 *config) {
    onekvm_video_encoder_config_v1 normalized{};
    const size_t copy_size = std::min<size_t>(config->struct_size, sizeof(normalized));
    std::memcpy(&normalized, config, copy_size);
    normalized.struct_size = sizeof(normalized);
    return normalized;
}

int32_t encoder_create(const onekvm_video_encoder_config_v1 *config, void **result,
                       char *error, uint32_t error_capacity) {
    if (result == nullptr || !valid_encoder_config(config)) {
        set_error(error, error_capacity, "invalid encoder configuration");
        return -1;
    }
    *result = nullptr;
    Encoder *encoder = new (std::nothrow) Encoder();
    if (encoder == nullptr) {
        set_error(error, error_capacity, "allocate encoder: out of memory");
        return -1;
    }
    encoder->config = normalized_encoder_config(config);
    encoder->codec_type = codec_type(config->codec);
    *result = encoder;
    return 0;
}

int32_t encoder_reset(void *opaque, const onekvm_video_encoder_config_v1 *config,
                      char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr || !valid_encoder_config(config)) {
        set_error(error, error_capacity, "invalid encoder reset");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    const auto next = normalized_encoder_config(config);
    const int next_codec = codec_type(config->codec);
    if (encoder->managed_allocation && next_codec != encoder->codec_type) {
        set_error(error, error_capacity,
                  "managed encoder codec cannot change after allocation");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    /* Cube cannot close/recreate a live VENC channel from this thread:
       GetStream holds EnterVcodecLock on the reader. FPS/GOP/bitrate/QP
       are posted for that thread. Bound codec changes still fail. */
    if (encoder->initialized && encoder->source_bound &&
        encoder->codec_type != 0 && next_codec != encoder->codec_type) {
        set_error(error, error_capacity,
                  "cannot change bound VENC codec; VPSS would fill waitq");
        return -1;
    }
    if (encoder->initialized && encoder->codec_type == 0 && next_codec == 0) {
        encoder->config = next;
        encoder->codec_type = next_codec;
        return 0;
    }
    if (encoder->initialized && encoder->codec_type != 0 &&
        encoder->codec_type == next_codec && encoder->channel >= 0 &&
        encoder->width > 0 && encoder->height > 0) {
        const int fps = normalized_output_fps(
            next.fps, encoder->width, encoder->height);
        const int gop = normalized_gop(next.gop, fps);
        const int bitrate_kbps = next.bitrate_kbps > 0
            ? next.bitrate_kbps
            : bitrate(encoder->width, encoder->height, next.quality_factor, fps);
        if (mmf::set_h26x_rate_control(
                encoder->channel, fps, gop, bitrate_kbps,
                next.initial_qp, next.min_qp, next.max_qp) != 0) {
            set_error(error, error_capacity,
                      "queue VENC rate control failed");
            return -1;
        }
        encoder->config = next;
        encoder->codec_type = next_codec;
        return 0;
    }
    close_encoder(encoder);
    encoder->config = next;
    encoder->codec_type = next_codec;
    encoder->request_keyframe = false;
    encoder->output.clear();
    return 0;
}

size_t complete_jpeg_size(const uint8_t *data, size_t size) {
    if (size < 4 || data[0] != 0xff || data[1] != 0xd8) {
        return 0;
    }
    for (size_t index = size - 1; index > 2; --index) {
        if (data[index - 1] == 0xff && data[index] == 0xd9) {
            return index + 1;
        }
    }
    return 0;
}

int drain_venc_packet(Encoder *encoder, const uint8_t **output_data,
                      uint64_t *output_pts_ns, bool *key_frame) {
    /* Do not expose VENC-owned pack addresses to Go/Pion.  RTP packetization
       and batched socket writes can outlive the driver's safe borrow window;
       keeping the stream held during that work also lets the producer fill
       its 12-pack queue, at which point cached SPS/PPS headers are dropped.

       Copy the encoded access unit into the encoder's reusable staging buffer
       and release the driver stream before crossing the ABI.  This is only a
       copy of the compressed bitstream (not the multi-megabyte raw frame), and
       performs no allocation in the steady state. */
    if (encoder->output.size() < kVENCBufferSize)
        encoder->output.resize(kVENCBufferSize);
    const int result = mmf::take_ready_h26x_packet(
        encoder->channel, encoder->output.data(),
        static_cast<int>(encoder->output.size()), key_frame);
    if (result < 0) {
        recover_encoder_after_stream_error(encoder);
        return result;
    }
    if (result == 0) {
        return result;
    }
    encoder->packet_borrowed = false;
    *output_data = encoder->output.data();
    *output_pts_ns = encoder->pending_pts_ns;
    encoder->frame_pending = false;
    encoder->pending_pts_ns = 0;
    return result;
}

int prepare_venc_packet(Encoder *encoder, char *error, uint32_t error_capacity) {
    if (encoder->codec_type == 0 || encoder->prepared_size != 0 ||
        !encoder->frame_pending) {
        return 0;
    }
    const int result = mmf::read_h26x_packet(
        encoder->channel, encoder->output.data(),
        static_cast<int>(encoder->output.size()));
    if (result == 0) {
        set_error(error, error_capacity, "MMF encoded frame was not ready");
        return -1;
    }
    if (result == -2) {
        set_error(error, error_capacity, "encoded frame exceeds %zu bytes",
                  encoder->output.size());
        recover_encoder_after_stream_error(encoder);
        return -1;
    }
    if (result < 0) {
        set_error(error, error_capacity, "prepare MMF encoded frame failed: %d",
                  result);
        recover_encoder_after_stream_error(encoder);
        return -1;
    }
    if (mmf::release_h26x_packet(encoder->channel) != 0) {
        set_error(error, error_capacity, "release prepared MMF frame failed");
        recover_encoder_after_stream_error(encoder);
        return -1;
    }
    encoder->prepared_size = static_cast<size_t>(result);
    encoder->prepared_pts_ns = encoder->pending_pts_ns;
    encoder->frame_pending = false;
    encoder->pending_pts_ns = 0;
    return 0;
}

int32_t encoder_prepare(void *opaque, char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) {
        set_error(error, error_capacity, "encoder is null");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    invalidate_stale_encoder(encoder);
    if (!encoder->initialized || encoder->codec_type == 0) {
        return 0;
    }
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    return prepare_venc_packet(encoder, error, error_capacity);
}

int bind_encoder_to_source_locked(Encoder *encoder, Source *source,
                                  char *error, uint32_t error_capacity) {
    if (!source->initialized || source->frame_pending || encoder->codec_type == 0) {
        set_error(error, error_capacity,
                  "bound encoding requires an idle source and H.264/H.265 encoder");
        return -1;
    }
    const auto [width, height] = source_output_size(source);
    if (encoder->managed_allocation &&
        (encoder->allocation_input_mode != ONEKVM_VIDEO_ENCODER_INPUT_BOUND ||
         width != encoder->allocation_width ||
         height != encoder->allocation_height)) {
        set_error(error, error_capacity,
                  "source does not match managed bound encoder allocation");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    const int configure_result = configure_encoder(
        encoder, width, height, error, error_capacity);
    if (configure_result != 0)
        return configure_result;
    /* Core binds every iteration. Placeholder keeps VPSS→VENC bound and
       only switches the VPSS input to user frames. */
    if (encoder->placeholder_frames && encoder->bound_source == source)
        return 0;
    if (encoder->source_bound && encoder->bound_source == source)
        return 0;

    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    const int result = mmf::bind_h26x_to_capture(encoder->channel, 0, source->channel);
    if (result != 0) {
        set_error(error, error_capacity, "bind VPSS directly to VENC failed: %d", result);
        return -1;
    }
    encoder->source_bound = true;
    encoder->bound_source = source;
    encoder->frame_pending = false;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
    encoder->bound_since_ns = monotonic_ns();
    return 0;
}

int32_t encoder_bind_source(void *encoder_opaque, void *source_opaque,
                            char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(encoder_opaque);
    auto *source = static_cast<Source *>(source_opaque);
    if (encoder == nullptr || source == nullptr) {
        set_error(error, error_capacity, "encoder or source is null");
        return -1;
    }
    std::lock_guard<std::mutex> encoder_lock(encoder->mutex);
    std::lock_guard<std::mutex> source_lock(source->mutex);
    return bind_encoder_to_source_locked(encoder, source, error, error_capacity);
}

int32_t fill_bound_empty_packet(Encoder *encoder, onekvm_video_packet_v1 *packet);
int32_t fill_bound_live_packet(Encoder *encoder, Source *source,
                               onekvm_video_packet_v1 *packet,
                               const uint8_t *output_data, int result,
                               bool key_frame, bool mark_hdmi);

bool placeholder_au_usable(const Encoder *encoder, const uint8_t *data,
                           int size, bool key_frame, int *sps_width,
                           int *sps_height) {
    if (sps_width != nullptr)
        *sps_width = 0;
    if (sps_height != nullptr)
        *sps_height = 0;
    if (encoder == nullptr || data == nullptr || size <= 0)
        return false;
    if (encoder->placeholder_need_key && !key_frame)
        return false;
    if (encoder->codec_type != 2)
        return true;
    int parsed_w = 0;
    int parsed_h = 0;
    if (!annexb_h264_sps_size(data, static_cast<std::size_t>(size),
                              &parsed_w, &parsed_h))
        return !key_frame && !encoder->placeholder_need_key;
    if (sps_width != nullptr)
        *sps_width = parsed_w;
    if (sps_height != nullptr)
        *sps_height = parsed_h;
    const int want_w = encoder->width > 0
        ? encoder->width : mmf::vpss_input_width();
    const int want_h = encoder->height > 0
        ? encoder->height : mmf::vpss_input_height();
    return annexb_h264_geometry_matches(parsed_w, parsed_h, want_w, want_h);
}

int encode_bound_placeholder(Encoder *encoder, Source *source,
                             onekvm_video_packet_v1 *packet,
                             char *error, uint32_t error_capacity) {
    const bool entering = !encoder->placeholder_frames;
    encoder->placeholder_frames = true;
    if (entering) {
        encoder->placeholder_need_key = true;
        encoder->placeholder_idr_tries = 0;
        encoder->placeholder_logged = false;
    }
    if (encoder->channel < 0 || !encoder->initialized || !encoder->source_bound) {
        set_error(error, error_capacity, "placeholder requires a bound VENC channel");
        return -1;
    }
    int width = mmf::vpss_input_width();
    int height = mmf::vpss_input_height();
    if (width <= 0 || height <= 0) {
        width = encoder->width;
        height = encoder->height;
    }
    if (ensure_no_signal_nv21(source, width, height, error, error_capacity) != 0)
        return -1;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (mmf::capture_use_user_frames() != 0) {
            set_error(error, error_capacity,
                      "unbind VI from VPSS for placeholder failed");
            encoder->placeholder_frames = false;
            return -1;
        }
        if (source->channel >= 0)
            (void)mmf::resume_vpss_channel(source->channel);
        const int push = mmf::submit_vpss_nv21(
            source->no_signal_frame.data(), width, height);
        if (push != 0) {
            set_error(error, error_capacity,
                      "submit no-signal frame to VPSS failed: %d", push);
            return -1;
        }
    }
    /* StartRecvFrame already ran during the live-grace window. WAVE4 then
       encodes the still as P/skip, and the reader coalesces RequestIDR while
       last_packet_ns is still 0. Force the ioctl from the reader thread. */
    if (encoder->placeholder_need_key &&
        (entering || encoder->placeholder_idr_tries % 15u == 0)) {
        const int ch = encoder->channel;
        if (entering) {
            (void)mmf::wait_ready_h26x_packet(ch, 80);
            if (encoder->output.size() < kVENCBufferSize)
                encoder->output.resize(kVENCBufferSize);
            bool unused_key = false;
            while (mmf::take_ready_h26x_into(ch, nullptr, &unused_key) > 0) {
            }
        }
        mmf::h26x_reader_force_idr(ch);
        std::lock_guard<std::recursive_mutex> submit_lock(g_mmf_mutex);
        (void)mmf::submit_vpss_nv21(
            source->no_signal_frame.data(), width, height);
    }
    if (encoder->placeholder_need_key)
        encoder->placeholder_idr_tries++;
    const uint8_t *output_data = nullptr;
    uint64_t output_pts_ns = 0;
    bool key_frame = false;
    int result = drain_venc_packet(
        encoder, &output_data, &output_pts_ns, &key_frame);
    if (result == 0) {
        (void)mmf::wait_ready_h26x_packet(encoder->channel, 80);
        result = drain_venc_packet(
            encoder, &output_data, &output_pts_ns, &key_frame);
    }
    if (result < 0) {
        set_error(error, error_capacity,
                  "read placeholder VENC packet failed: %d", result);
        return -1;
    }
    if (result == 0)
        return fill_bound_empty_packet(encoder, packet);
    int sps_w = 0;
    int sps_h = 0;
    if (!placeholder_au_usable(
            encoder, output_data, result, key_frame, &sps_w, &sps_h)) {
        if (encoder->placeholder_idr_tries <= 2) {
            std::fprintf(stderr,
                         "OneKVM: drop placeholder AU %d bytes key=%d sps=%dx%d want=%dx%d\n",
                         result, key_frame ? 1 : 0, sps_w, sps_h,
                         encoder->width, encoder->height);
        }
        return fill_bound_empty_packet(encoder, packet);
    }
    if (key_frame) {
        encoder->placeholder_need_key = false;
        encoder->placeholder_idr_tries = 0;
    }
    if (!encoder->placeholder_logged) {
        std::fprintf(stderr,
                     "OneKVM: placeholder AU %d bytes key=%d sps=%dx%d\n",
                     result, key_frame ? 1 : 0, sps_w, sps_h);
        encoder->placeholder_logged = true;
    }
    return fill_bound_live_packet(
        encoder, source, packet, output_data, result, key_frame, false);
}

int32_t fill_bound_live_packet(Encoder *encoder, Source *source,
                               onekvm_video_packet_v1 *packet,
                               const uint8_t *output_data, int result,
                               bool key_frame, bool mark_hdmi) {
    if (mark_hdmi) {
        source->last_frame_ns.store(monotonic_ns(), std::memory_order_relaxed);
        /* A live AU is enough to keep the decoder fed. Do not read
           /proc/cvitek/vi here: that dump stalls the 60 fps path. HDMI
           loss is detected when VENC goes stale, not per packet. */
        source->cached_signal.store(1, std::memory_order_relaxed);
        source->failures = 0;
    }
    packet->data = output_data;
    packet->data_size = static_cast<uint64_t>(result);
    packet->codec = public_codec(encoder->codec_type);
    packet->key_frame = key_frame ? 1 : 0;
    packet->pts_ns = monotonic_ns();
    encoder->last_encode_ns.store(mmf::h26x_last_encode_ns(encoder->channel),
                                 std::memory_order_relaxed);
    uint64_t capture_ns = mmf::h26x_last_capture_ns(encoder->channel);
    if (capture_ns == 0 && source != nullptr)
        capture_ns = source->last_capture_ns.load(std::memory_order_relaxed);
    if (capture_ns > 0) {
        encoder->last_capture_ns.store(capture_ns, std::memory_order_relaxed);
        if (source != nullptr)
            source->last_capture_ns.store(capture_ns, std::memory_order_relaxed);
    }
    return 0;
}

int32_t fill_bound_empty_packet(Encoder *encoder, onekvm_video_packet_v1 *packet) {
    packet->data = nullptr;
    packet->data_size = 0;
    packet->codec = public_codec(encoder->codec_type);
    packet->key_frame = 0;
    packet->pts_ns = monotonic_ns();
    return 0;
}

int32_t encoder_read_packet(void *opaque, onekvm_video_packet_v1 *packet,
                            char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr || packet == nullptr ||
        packet->struct_size < sizeof(*packet)) {
        set_error(error, error_capacity, "invalid bound encoder read");
        return -1;
    }
    std::unique_lock<std::mutex> lock(encoder->mutex);
    Source *source = encoder->bound_source;
    if (source == nullptr || encoder->codec_type == 0) {
        set_error(error, error_capacity, "encoder is not bound to a source");
        return -1;
    }
    std::unique_lock<std::mutex> source_lock(source->mutex);
    invalidate_stale_encoder(encoder);
    if (!encoder->initialized ||
        (!encoder->source_bound && !encoder->placeholder_frames)) {
        if (bind_encoder_to_source_locked(encoder, source, error, error_capacity) != 0)
            return -1;
        encoder->request_keyframe = true;
    }
    const uint8_t *output_data = nullptr;
    uint64_t output_pts_ns = monotonic_ns();
    int result = 0;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->packet_borrowed) {
            if (mmf::release_h26x_packet(encoder->channel) != 0) {
                set_error(error, error_capacity, "release previous bound packet failed");
                return -1;
            }
            encoder->packet_borrowed = false;
        }
        if (encoder->request_keyframe) {
            /* Only mark the reader. CVI_VENC_RequestIDR shares the channel
               ioctl lock with GetStream and must stay off the video loop. */
            encoder->prepared_size = 0;
            encoder->prepared_pts_ns = 0;
            encoder->request_keyframe = false;
            if (encoder->placeholder_frames) {
                encoder->placeholder_need_key = true;
                encoder->placeholder_idr_tries = 0;
                encoder->placeholder_logged = false;
                mmf::h26x_reader_force_idr(encoder->channel);
            } else {
                mmf::h26x_reader_want_idr(encoder->channel);
            }
        }
        if (encoder->prepared_size != 0) {
            result = static_cast<int>(encoder->prepared_size);
            output_data = encoder->output.data();
            output_pts_ns = encoder->prepared_pts_ns;
            encoder->prepared_size = 0;
            encoder->prepared_pts_ns = 0;
        }
    }
    bool key_frame = false;
    /* Never fopen /proc/cvitek/vi on the bound packet path. That dump stalls
       the video thread (~1 s) and collapses Core to 1 FPS while VENC stays 60. */
    const bool missing_signal =
        source->out_of_range.load(std::memory_order_relaxed) ||
        source->cached_signal.load(std::memory_order_relaxed) == 0;
    /* Do not take_ready before the placeholder path. The reader queue holds
       one AU: popping it here then draining again in encode_bound_placeholder
       drops the still. Live HDMI still drains first so we can return it. */
    if (!encoder->placeholder_frames) {
        if (result == 0 && output_data == nullptr)
            result = drain_venc_packet(
                encoder, &output_data, &output_pts_ns, &key_frame);
        if (result < 0) {
            set_error(error, error_capacity, "read bound MMF frame failed: %d", result);
            return -1;
        }
        /* A drained AU is live video. Do not throw it away because the HDMI
           watcher has not yet published cached_signal, and do not read
           /proc/cvitek/vi on this path. */
        if (result > 0)
            return fill_bound_live_packet(
                encoder, source, packet, output_data, result, key_frame, true);

        const uint64_t last_live = mmf::h26x_reader_last_packet_ns(encoder->channel);
        const uint64_t now = monotonic_ns();
        /* last_live==0 means the reader has not delivered a unit yet, not that
           HDMI is gone. A recent live AU also suppresses the artwork: the VI
           FrameRate field stays 0 for the first second, and inserting a canned
           IDR between live P frames breaks the decoder. */
        const uint64_t live_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kVencLiveRecentWindow).count());
        /* cached_signal starts at unknown/no-signal and the HDMI watcher may
           not publish its first sample before Core opens WebRTC.  Do not tear
           down a newly bound producer merely because the VENC worker has not
           emitted its first AU yet.  Unbinding with its first VPSS frames
           still queued and immediately calling SendFrame creates a vendor
           lock inversion (venc-handler waits for EnterVcodecLock while the
           caller waits for the VPU timelock), permanently pinning the VPSS
           VB pool. */
        const bool awaiting_first_au = last_live == 0 &&
            encoder->bound_since_ns != 0 && now >= encoder->bound_since_ns &&
            now - encoder->bound_since_ns <= live_ns;
        const bool live_recent = last_live != 0 &&
            now >= last_live && now - last_live <= live_ns;
        const bool venc_stale = last_live != 0 &&
            now > last_live && now - last_live > live_ns;

        /* Between live access units the queue is often empty. The old path
           read /proc/cvitek/vi and slept 10 ms while holding both mutexes,
           which dropped Enc/actual_fps into the 30-50 range. Wait on the
           reader and leave HDMI rebuilds to the watcher. */
        if (awaiting_first_au || live_recent) {
            const int ch = encoder->channel;
            source_lock.unlock();
            lock.unlock();
            (void)mmf::wait_ready_h26x_packet(ch, 40);
            lock.lock();
            source = encoder->bound_source;
            if (source == nullptr || encoder->codec_type == 0) {
                set_error(error, error_capacity, "encoder is not bound to a source");
                return -1;
            }
            source_lock = std::unique_lock<std::mutex>(source->mutex);
            invalidate_stale_encoder(encoder);
            if (!encoder->initialized)
                return fill_bound_empty_packet(encoder, packet);
            result = drain_venc_packet(
                encoder, &output_data, &output_pts_ns, &key_frame);
            if (result < 0) {
                set_error(error, error_capacity, "read bound MMF frame failed: %d", result);
                return -1;
            }
            if (result > 0)
                return fill_bound_live_packet(
                    encoder, source, packet, output_data, result, key_frame,
                    true);
            return fill_bound_empty_packet(encoder, packet);
        }

        if (!missing_signal && !venc_stale) {
            source->failures++;
            const int rebuilt = maybe_rebuild_for_hdmi_change(
                source, error, error_capacity);
            if (rebuilt != 0) {
                encoder->placeholder_frames = false;
                invalidate_stale_encoder(encoder);
                return -1;
            }
            const int codec = encoder->codec_type;
            source_lock.unlock();
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            packet->data = nullptr;
            packet->data_size = 0;
            packet->codec = public_codec(codec);
            packet->key_frame = 0;
            packet->pts_ns = monotonic_ns();
            return 0;
        }
    } else if (!missing_signal) {
        source->failures++;
        const int rebuilt = maybe_rebuild_for_hdmi_change(
            source, error, error_capacity);
        if (rebuilt != 0) {
            encoder->placeholder_frames = false;
            invalidate_stale_encoder(encoder);
            return -1;
        }
        encoder->placeholder_frames = false;
        encoder->placeholder_need_key = false;
        encoder->placeholder_idr_tries = 0;
        encoder->placeholder_logged = false;
        encoder->request_keyframe = true;
        {
            std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
            (void)mmf::capture_use_vi_frames();
        }
        if (bind_encoder_to_source_locked(
                encoder, source, error, error_capacity) != 0)
            return -1;
        const int codec = encoder->codec_type;
        source_lock.unlock();
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        packet->data = nullptr;
        packet->data_size = 0;
        packet->codec = public_codec(codec);
        packet->key_frame = 0;
        packet->pts_ns = monotonic_ns();
        return 0;
    }

    /* Host mode changes often stop both VI and VENC. Keep VPSS→VENC bound
       and feed the no-signal still into VPSS from userspace (VI unbound).
       Do not CVI_VENC_SendFrame: UnBind does not clear currBindMode and
       deadlocks the VPU. WAVE4 then emits a normal IDR/P stream. */
    source->failures++;
    const int rebuilt = maybe_rebuild_for_hdmi_change(
        source, error, error_capacity);
    if (rebuilt != 0) {
        encoder->placeholder_frames = false;
        {
            std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
            (void)mmf::capture_use_vi_frames();
        }
        invalidate_stale_encoder(encoder);
        return -1;
    }
    return encode_bound_placeholder(encoder, source, packet, error, error_capacity);
}

int32_t encoder_latency(void *opaque, onekvm_video_latency_v1 *latency) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr || latency == nullptr ||
        latency->struct_size < sizeof(*latency)) {
        return -1;
    }
    uint64_t capture_ns = encoder->last_capture_ns.load(std::memory_order_relaxed);
    uint64_t encode_ns = encoder->last_encode_ns.load(std::memory_order_relaxed);
    if (encoder->channel >= kFirstVENCChannel) {
        const uint64_t channel_encode = mmf::h26x_last_encode_ns(encoder->channel);
        if (channel_encode > 0)
            encode_ns = channel_encode;
        const uint64_t channel_capture = mmf::h26x_last_capture_ns(encoder->channel);
        if (channel_capture > 0)
            capture_ns = channel_capture;
    }
    latency->capture_ns = capture_ns;
    latency->encode_ns = encode_ns;
    return 0;
}

int32_t encoder_unbind_source(void *opaque, char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) {
        set_error(error, error_capacity, "encoder is null");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    if (!encoder->source_bound && encoder->bound_source == nullptr) {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        mmf::park_unbound_vpss_channels();
        return 0;
    }
    /* Keep the configured channel, VPSS binding, and vendor receive worker
       across reconnects, but let MMF stop/join its userspace reader while
       parked. Actually unbinding the producer here leaves WAVE4 input-starved;
       its later final StopRecvFrame can then block forever in the vendor VPU
       lock. Final Stop/Unbind/Destroy ordering belongs to close_h26x_encoder(). */
    const bool live = encoder->initialized && encoder->source_bound &&
        encoder->mmf_generation == g_mmf_generation.load(std::memory_order_acquire);
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    if (live && mmf::park_h26x_capture(encoder->channel) != 0) {
        set_error(error, error_capacity, "park VPSS/VENC capture failed");
        return -1;
    }
    encoder->source_bound = false;
    encoder->frame_pending = false;
    encoder->packet_borrowed = false;
    encoder->pending_pts_ns = 0;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
    encoder->bound_since_ns = 0;
    encoder->bound_source = nullptr;
    return 0;
}

int32_t encoder_encode(void *opaque, const onekvm_video_frame_v1 *frame,
                       onekvm_video_packet_v1 *packet,
                       char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    size_t required_frame_size = 0;
    if (encoder == nullptr || frame == nullptr || packet == nullptr ||
        frame->struct_size < sizeof(*frame) || packet->struct_size < sizeof(*packet) ||
        frame->data == nullptr || frame->data_size == 0 ||
        frame->pixel_format != ONEKVM_VIDEO_PIXEL_NV21 ||
        !nv21_size(frame->width, frame->height, &required_frame_size) ||
        frame->data_size < required_frame_size) {
        set_error(error, error_capacity, "invalid encoder frame");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    if (encoder->managed_allocation &&
        (encoder->allocation_input_mode != ONEKVM_VIDEO_ENCODER_INPUT_MANUAL ||
         frame->width != encoder->allocation_width ||
         frame->height != encoder->allocation_height ||
         frame->pixel_format != encoder->allocation_pixel_format)) {
        set_error(error, error_capacity,
                  "frame does not match managed manual encoder allocation");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    const int configure_result = configure_encoder(
        encoder, frame->width, frame->height, error, error_capacity);
    if (configure_result != 0)
        return configure_result;

    int result = 0;
    uint64_t output_pts_ns = frame->pts_ns;
    const uint8_t *output_data = nullptr;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->codec_type == 0) {
            /* JPEG uses a borrowed VI/VB frame without copying it. Keep the
               Encode call synchronous so the pipeline cannot return that
               frame to VI before VENC has produced the JPEG bitstream. */
            const uint64_t encode_start_ns = monotonic_ns();
            const int push_result = mmf::submit_jpeg_frame(
                encoder->channel, const_cast<uint8_t *>(frame->data),
                frame->width, frame->height, kMMFNV21,
                jpeg_quality(encoder->config.quality_factor));
            if (push_result != 0) {
                set_error(error, error_capacity, "submit MMF JPEG frame failed: %d",
                          push_result);
                return -1;
            }
            result = mmf::read_jpeg_packet(encoder->channel, encoder->output.data(),
                                          static_cast<int>(encoder->output.size()));
            if (result > 0 && mmf::release_jpeg_packet(encoder->channel) != 0) {
                set_error(error, error_capacity, "release MMF JPEG frame failed");
                return -1;
            }
            if (result > 0) {
                output_data = encoder->output.data();
                const uint64_t encode_ns = monotonic_ns() - encode_start_ns;
                if (encode_ns < 1000000000ull)
                    encoder->last_encode_ns.store(encode_ns, std::memory_order_relaxed);
            }
        } else if (encoder->managed_allocation &&
                   encoder->allocation_input_mode == ONEKVM_VIDEO_ENCODER_INPUT_MANUAL &&
                   encoder->allocation_purpose == ONEKVM_VIDEO_ENCODER_PURPOSE_BACKGROUND) {
            // A recording frame must have a matching completed access unit.
            // The realtime compatibility path below intentionally pipelines
            // frames, which would otherwise lose the final recording frame.
            if (encoder->request_keyframe) {
                if (mmf::request_h26x_idr(encoder->channel) != 0) {
                    set_error(error, error_capacity, "request recording IDR failed");
                    return -1;
                }
                encoder->request_keyframe = false;
            }
            result = mmf::submit_h26x_frame(encoder->channel,
                const_cast<uint8_t *>(frame->data), frame->width, frame->height,
                kMMFNV21);
            if (result == 0) {
                result = mmf::read_manual_h26x_frame(encoder->channel,
                    encoder->output.data(), static_cast<int>(encoder->output.size()), 1000);
            }
            if (result <= 0) {
                set_error(error, error_capacity, "complete recording frame failed: %d", result);
                recover_encoder_after_stream_error(encoder);
                return -1;
            }
            output_data = encoder->output.data();
        } else {
            /* The previous borrowed access unit is valid until this call. A
               v1 caller that does not use encoder_release_packet therefore
               still gets the documented "until next encoder call" lifetime. */
            if (encoder->packet_borrowed) {
                if (mmf::release_h26x_packet(encoder->channel) != 0) {
                    set_error(error, error_capacity, "release previous MMF packet failed");
                    return -1;
                }
                encoder->packet_borrowed = false;
            }
            /* New Core calls encoder_prepare before borrowing this source
               frame. The previous access unit is therefore already copied out
               and its VENC stream released. Keep the old in-call drain as the
               ABI-v1 fallback for callers that do not use the optional tail. */
            bool unused_key = false;
            if (encoder->frame_pending) {
                result = drain_venc_packet(
                    encoder, &output_data, &output_pts_ns, &unused_key);
            } else if (encoder->prepared_size != 0) {
                result = static_cast<int>(encoder->prepared_size);
                output_data = encoder->output.data();
                output_pts_ns = encoder->prepared_pts_ns;
            }
            if (result >= 0 && !encoder->frame_pending) {
                if (encoder->request_keyframe) {
                    if (mmf::request_h26x_idr(encoder->channel) != 0) {
                        if (encoder->packet_borrowed) {
                            mmf::release_h26x_packet(encoder->channel);
                            encoder->packet_borrowed = false;
                        }
                        set_error(error, error_capacity, "request MMF IDR failed");
                        return -1;
                    }
                    encoder->request_keyframe = false;
                }
                const int push_result = mmf::submit_h26x_frame(
                    encoder->channel, const_cast<uint8_t *>(frame->data),
                    frame->width, frame->height, kMMFNV21);
                if (push_result != 0) {
                    if (encoder->packet_borrowed) {
                        mmf::release_h26x_packet(encoder->channel);
                        encoder->packet_borrowed = false;
                    }
                    set_error(error, error_capacity, "submit MMF frame failed: %d",
                              push_result);
                    return -1;
                }
                encoder->frame_pending = true;
                encoder->pending_pts_ns = frame->pts_ns;
            }
        }
    }
    if (result == -2) {
        set_error(error, error_capacity, "encoded frame exceeds %zu bytes", encoder->output.size());
        return -1;
    }
    if (result < 0) {
        set_error(error, error_capacity, "MMF encode failed: %d", result);
        return -1;
    }
    if (result == 0) {
        packet->data = nullptr;
        packet->data_size = 0;
        packet->codec = public_codec(encoder->codec_type);
        packet->key_frame = 0;
        packet->pts_ns = frame->pts_ns;
        return 0;
    }
    size_t size = static_cast<size_t>(result);
    if (encoder->codec_type == 0) {
        size = complete_jpeg_size(encoder->output.data(), size);
        if (size == 0) {
            set_error(error, error_capacity, "incomplete JPEG frame");
            return -1;
        }
    }
    packet->data = output_data != nullptr ? output_data : encoder->output.data();
    packet->data_size = size;
    packet->codec = public_codec(encoder->codec_type);
    packet->key_frame = 0;
    packet->pts_ns = output_pts_ns;
    if (output_data == encoder->output.data()) {
        encoder->prepared_size = 0;
        encoder->prepared_pts_ns = 0;
    }
    return 0;
}

int32_t encoder_set_quality(void *opaque, double quality,
                            char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr || !std::isfinite(quality) || quality < 0 || quality > 1) {
        set_error(error, error_capacity, "quality factor must be between 0 and 1");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    // JPEG quality is supplied to every mmf::submit_jpeg_frame call.
    encoder->config.quality_factor = quality;
    if (encoder->codec_type == 0 || !encoder->initialized ||
        encoder->channel < 0 || encoder->width <= 0 || encoder->height <= 0)
        return 0;
    const int fps = normalized_output_fps(
        encoder->config.fps, encoder->width, encoder->height);
    const int gop = normalized_gop(encoder->config.gop, fps);
    const int bitrate_kbps = encoder->config.bitrate_kbps > 0
        ? encoder->config.bitrate_kbps
        : bitrate(encoder->width, encoder->height, quality, fps);
    if (mmf::set_h26x_rate_control(
            encoder->channel, fps, gop, bitrate_kbps,
            encoder->config.initial_qp, encoder->config.min_qp,
            encoder->config.max_qp) != 0) {
        set_error(error, error_capacity, "queue VENC quality bitrate failed");
        return -1;
    }
    return 0;
}

int32_t encoder_request_keyframe(void *opaque, char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) {
        set_error(error, error_capacity, "encoder is null");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    if (encoder->codec_type != 0) {
        encoder->request_keyframe = true;
    }
    return 0;
}

uint32_t encoder_codec(void *opaque) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) return ONEKVM_VIDEO_CODEC_AUTO;
    std::lock_guard<std::mutex> lock(encoder->mutex);
    return public_codec(encoder->codec_type);
}

void encoder_release_packet(void *opaque) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) return;
    std::lock_guard<std::mutex> lock(encoder->mutex);
    if (!encoder->initialized || encoder->codec_type == 0)
        return;
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    if (encoder->mmf_generation != g_mmf_generation.load(std::memory_order_acquire)) {
        encoder->packet_borrowed = false;
        encoder->prepared_size = 0;
        encoder->prepared_pts_ns = 0;
        return;
    }
    if (encoder->packet_borrowed) {
        if (mmf::release_h26x_packet(encoder->channel) == 0)
            encoder->packet_borrowed = false;
        return;
    }
    /* Bound packets are copied and their VENC stream is released before the
       ABI call returns.  Do not prefetch or collapse queued access units here:
       discarding a P frame and later forwarding a dependent P frame breaks
       the decoder reference chain. */
}

int32_t encoder_resources(onekvm_video_encoder_resources_v1 *resources,
                          char *error, uint32_t error_capacity) {
    if (resources == nullptr || resources->struct_size < sizeof(*resources)) {
        set_error(error, error_capacity, "invalid encoder resources output");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    resources->policy_h26x_capacity = 1;
    resources->policy_jpeg_capacity = 1;
    resources->active_h26x = 0;
    for (int channel = kFirstVENCChannel; channel <= kLastVENCChannel; ++channel) {
        if (mmf::g_runtime.h26x_encoders[channel].initialized)
            ++resources->active_h26x;
    }
    resources->active_jpeg = mmf::g_runtime.jpeg_initialized ? 1u : 0u;
    resources->hardware_channel_capacity = kLastVENCChannel + 1;
    resources->input_mode_mask = ONEKVM_VIDEO_ENCODER_INPUT_MASK_MANUAL |
        ONEKVM_VIDEO_ENCODER_INPUT_MASK_BOUND;
    resources->purpose_mask = ONEKVM_VIDEO_ENCODER_PURPOSE_MASK_REALTIME |
        ONEKVM_VIDEO_ENCODER_PURPOSE_MASK_BACKGROUND;
    resources->flags = 0;
    if (mmf::g_runtime.reference_count == 0) {
        set_error(error, error_capacity, "MMF runtime is not initialized");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    }
    return ONEKVM_VIDEO_RESOURCE_OK;
}

int32_t encoder_allocate(
    const onekvm_video_encoder_allocation_request_v1 *request,
    void **result, onekvm_video_encoder_allocation_v1 *allocation,
    char *error, uint32_t error_capacity) {
    if (result == nullptr)
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    *result = nullptr;
    if (request == nullptr || request->struct_size < sizeof(*request) ||
        request->config.struct_size < sizeof(request->config) ||
        allocation == nullptr || allocation->struct_size < sizeof(*allocation) ||
        request->flags != 0 || request->width <= 0 || request->height <= 0 ||
        (request->width & 1) != 0 || (request->height & 1) != 0 ||
        request->pixel_format != ONEKVM_VIDEO_PIXEL_NV21 ||
        (request->input_mode != ONEKVM_VIDEO_ENCODER_INPUT_MANUAL &&
         request->input_mode != ONEKVM_VIDEO_ENCODER_INPUT_BOUND) ||
        (request->purpose != ONEKVM_VIDEO_ENCODER_PURPOSE_REALTIME &&
         request->purpose != ONEKVM_VIDEO_ENCODER_PURPOSE_BACKGROUND) ||
        !valid_encoder_config(&request->config)) {
        set_error(error, error_capacity, "invalid encoder allocation request");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    const int type = codec_type(request->config.codec);
    if (type == 0 && request->input_mode == ONEKVM_VIDEO_ENCODER_INPUT_BOUND) {
        set_error(error, error_capacity, "bound JPEG allocation is unsupported");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }

    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    if (mmf::g_runtime.reference_count == 0) {
        set_error(error, error_capacity, "MMF runtime is not initialized");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    }
    if ((type == 0 && mmf::g_runtime.jpeg_initialized) ||
        (type != 0 && mmf::g_runtime.h26x_encoder_active)) {
        set_error(error, error_capacity, "requested encoder resource is busy");
        return ONEKVM_VIDEO_RESOURCE_BUSY;
    }

    void *opaque = nullptr;
    int result_code = encoder_create(
        &request->config, &opaque, error, error_capacity);
    if (result_code != 0)
        return result_code == -1 ? ONEKVM_VIDEO_RESOURCE_NO_MEMORY : result_code;
    auto *encoder = static_cast<Encoder *>(opaque);
    encoder->managed_allocation = true;
    encoder->allocation_id = next_allocation_id();
    if (encoder->allocation_id == 0) {
        set_error(error, error_capacity, "encoder allocation IDs are exhausted");
        encoder_destroy(encoder);
        return ONEKVM_VIDEO_RESOURCE_INTERNAL;
    }
    encoder->allocation_input_mode = request->input_mode;
    encoder->allocation_purpose = request->purpose;
    encoder->allocation_pixel_format = request->pixel_format;
    encoder->allocation_width = request->width;
    encoder->allocation_height = request->height;
    result_code = configure_encoder(
        encoder, request->width, request->height, error, error_capacity);
    if (result_code != 0) {
        encoder_destroy(encoder);
        return result_code;
    }
    fill_allocation(encoder, allocation);
    *result = encoder;
    return ONEKVM_VIDEO_RESOURCE_OK;
}

int32_t encoder_allocation(void *opaque,
                           onekvm_video_encoder_allocation_v1 *allocation,
                           char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr || allocation == nullptr ||
        allocation->struct_size < sizeof(*allocation)) {
        set_error(error, error_capacity, "invalid encoder allocation query");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    if (!encoder->managed_allocation) {
        set_error(error, error_capacity, "encoder was not explicitly allocated");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }
    if (!encoder->initialized || encoder->mmf_generation !=
        g_mmf_generation.load(std::memory_order_acquire)) {
        set_error(error, error_capacity, "encoder allocation is no longer initialized");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    }
    fill_allocation(encoder, allocation);
    return ONEKVM_VIDEO_RESOURCE_OK;
}

void encoder_destroy(void *opaque) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(encoder->mutex);
        close_encoder(encoder);
    }
    delete encoder;
}

const onekvm_video_format_v1 kFormats[] = {
    {sizeof(onekvm_video_format_v1), 2880, 1620, 30, ONEKVM_VIDEO_PIXEL_NV21},
    {sizeof(onekvm_video_format_v1), 2560, 1440, 30, ONEKVM_VIDEO_PIXEL_NV21},
    {sizeof(onekvm_video_format_v1), 1920, 1080, 60, ONEKVM_VIDEO_PIXEL_NV21},
    {sizeof(onekvm_video_format_v1), 1280, 720, 60, ONEKVM_VIDEO_PIXEL_NV21},
    {sizeof(onekvm_video_format_v1), 640, 480, 60, ONEKVM_VIDEO_PIXEL_NV21},
};

const onekvm_video_backend_v1 kBackend = {
    sizeof(onekvm_video_backend_v1),
    ONEKVM_VIDEO_BACKEND_ABI_V1,
    "nanokvm-mmf",
    ONEKVM_VIDEO_CODEC_MASK_AUTO | ONEKVM_VIDEO_CODEC_MASK_H264 |
        ONEKVM_VIDEO_CODEC_MASK_H265 | ONEKVM_VIDEO_CODEC_MASK_MJPEG,
    ONEKVM_VIDEO_FEATURE_SIGNAL_PRESENT | ONEKVM_VIDEO_FEATURE_KEYFRAME |
        ONEKVM_VIDEO_FEATURE_BORROWED_PACKET |
        ONEKVM_VIDEO_FEATURE_PREPARE_ENCODE |
        ONEKVM_VIDEO_FEATURE_BOUND_ENCODER |
        ONEKVM_VIDEO_FEATURE_LATENCY |
        ONEKVM_VIDEO_FEATURE_EDID |
        ONEKVM_VIDEO_FEATURE_ENCODER_ALLOCATION |
        ONEKVM_VIDEO_FEATURE_SOURCE_SNAPSHOT,
    kFormats,
    static_cast<uint32_t>(sizeof(kFormats) / sizeof(kFormats[0])),
    source_create,
    source_reset,
    source_read,
    source_release,
    source_signal_present,
    nullptr,
    source_destroy,
    encoder_create,
    encoder_reset,
    encoder_encode,
    encoder_set_quality,
    encoder_request_keyframe,
    encoder_codec,
    encoder_destroy,
    encoder_release_packet,
    encoder_prepare,
    encoder_bind_source,
    encoder_read_packet,
    encoder_unbind_source,
    source_input_format,
    source_latency,
    encoder_latency,
    edid_capabilities,
    edid_get,
    edid_set,
    encoder_resources,
    encoder_allocate,
    encoder_allocation,
    source_snapshot,
};


} // namespace onekvm::video_backend
#pragma GCC visibility pop
