#include "onekvm_video_backend_internal.hpp"

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

int jpeg_quality(double quality) {
    if (quality <= 0) return 80;
    quality = std::min(quality, 1.0);
    return 55 + static_cast<int>(quality * 35);
}

int bitrate(int width, int height, double quality) {
    if (quality <= 0) quality = 1.0;
    quality = std::min(quality, 1.0);
    /* NanoKVM's highest H.264 quality preset is 5 Mbps.  OneKVM's maximum
       quality doubles that ceiling for sharper text and motion at 1080p60. */
    /* Cast before multiplying: 1920 * 1080 * 10000 exceeds signed int32 and
       previously wrapped before division, silently falling through to the
       100 kbps safety floor. */
    const int64_t base = static_cast<int64_t>(width) * height * 10000 /
                         (static_cast<int64_t>(1920) * 1080);
    return std::max(100, static_cast<int>(static_cast<double>(base) * quality));
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
    if (live && encoder->source_bound && encoder->codec_type != 0) {
        mmf::unbind_h26x_from_capture(encoder->channel);
        encoder->source_bound = false;
    }
    if (live) {
        if (encoder->codec_type == 0) {
            mmf::close_jpeg_encoder(encoder->channel);
        } else {
            mmf::close_h26x_encoder(encoder->channel);
        }
    }
    encoder->initialized = false;
    encoder->channel = -1;
    encoder->width = 0;
    encoder->height = 0;
    encoder->frame_pending = false;
    encoder->source_bound = false;
    encoder->placeholder_frames = false;
    encoder->packet_borrowed = false;
    encoder->pending_pts_ns = 0;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
    encoder->mmf_generation = 0;
    encoder->bound_source = nullptr;
}

void invalidate_stale_encoder(Encoder *encoder) {
    if (!encoder->initialized || encoder->mmf_generation ==
        g_mmf_generation.load(std::memory_order_acquire))
        return;
    encoder->initialized = false;
    encoder->channel = -1;
    encoder->width = 0;
    encoder->height = 0;
    encoder->source_bound = false;
    encoder->placeholder_frames = false;
    encoder->request_keyframe = true;
    encoder->frame_pending = false;
    encoder->packet_borrowed = false;
    encoder->pending_pts_ns = 0;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
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
        return -1;
    }
    int fps = encoder->config.fps > 0 ? encoder->config.fps : 30;
    // The capture/VPSS path can continue delivering frames at the HDMI input
    // cadence even when the requested output is lower.  Tell VENC the
    // expected input cadence separately from the requested destination rate;
    // using the target for both fields makes the driver assume every submitted
    // frame is already at the target rate and it will not perform rate
    // conversion (the stream then remains close to the source FPS).
    constexpr int kVencInputFPS = 60;
    /* Keep the default GOP at one second. This avoids spending excessive VBR
       budget on IDR frames while still bounding decoder recovery latency. */
    int gop = encoder->config.gop > 0 ? encoder->config.gop : std::max(1, fps);
    int result = 0;
    int channel = kJPEGChannel;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->codec_type == 0) {
            channel = kJPEGChannel;
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
            config.input_fps = kVencInputFPS;
            config.output_fps = fps;
            config.bitrate_kbps = encoder->config.bitrate_kbps > 0
                ? encoder->config.bitrate_kbps
                : bitrate(width, height, encoder->config.quality_factor);
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
                return -1;
            }
        }
    }
    if (result != 0) {
        set_error(error, error_capacity, "initialize MMF encoder failed: %d", result);
        return -1;
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
    close_encoder(encoder);
    encoder->config = normalized_encoder_config(config);
    encoder->codec_type = codec_type(config->codec);
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
                      uint64_t *output_pts_ns) {
    /* Do not expose VENC-owned pack addresses to Go/Pion.  RTP packetization
       and batched socket writes can outlive the driver's safe borrow window;
       keeping the stream held during that work also lets the producer fill
       its 12-pack queue, at which point cached SPS/PPS headers are dropped.

       Copy the encoded access unit into the encoder's reusable staging buffer
       and release the driver stream before crossing the ABI.  This is only a
       copy of the compressed bitstream (not the multi-megabyte raw frame), and
       performs no allocation in the steady state. */
    const int result = mmf::read_latest_h26x_packet(
        encoder->channel, encoder->output.data(),
        static_cast<int>(encoder->output.size()));
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
    const auto [width, height] = resolution_size(source->config.resolution);
    if (configure_encoder(encoder, width, height, error, error_capacity) != 0)
        return -1;
    /* Core binds every iteration. Stay unbound while showing the no-signal
       artwork or the next Bind would reattach VPSS and drop placeholder
       submits on packet_pending. */
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

int encode_bound_placeholder(Encoder *encoder, Source *source,
                             onekvm_video_packet_v1 *packet,
                             char *error, uint32_t error_capacity) {
    if (encoder->source_bound) {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (mmf::unbind_h26x_from_capture(encoder->channel) != 0) {
            set_error(error, error_capacity,
                      "unbind capture for no-signal frame failed");
            return -1;
        }
        encoder->source_bound = false;
        encoder->request_keyframe = true;
    }
    encoder->placeholder_frames = true;

    onekvm_video_frame_v1 frame{};
    frame.struct_size = sizeof(frame);
    if (no_signal_frame(source, &frame, error, error_capacity) != 0)
        return -1;

    const uint8_t *output_data = nullptr;
    uint64_t output_pts_ns = frame.pts_ns;
    int result = 0;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->request_keyframe) {
            if (mmf::request_h26x_idr(encoder->channel) != 0) {
                set_error(error, error_capacity, "request no-signal IDR failed");
                return -1;
            }
            encoder->request_keyframe = false;
        }
        const int push = mmf::submit_h26x_frame(
            encoder->channel, const_cast<uint8_t *>(frame.data),
            frame.width, frame.height, kMMFNV21);
        if (push != 0) {
            set_error(error, error_capacity,
                      "submit no-signal frame failed: %d", push);
            return -1;
        }
    }
    result = drain_venc_packet(encoder, &output_data, &output_pts_ns);
    if (result < 0) {
        set_error(error, error_capacity,
                  "encode no-signal frame failed: %d", result);
        return -1;
    }
    packet->data = result > 0 ? output_data : nullptr;
    packet->data_size = result > 0 ? static_cast<uint64_t>(result) : 0;
    packet->codec = public_codec(encoder->codec_type);
    packet->key_frame = 0;
    packet->pts_ns = output_pts_ns;
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
    std::lock_guard<std::mutex> lock(encoder->mutex);
    Source *source = encoder->bound_source;
    if (source == nullptr || encoder->codec_type == 0) {
        set_error(error, error_capacity, "encoder is not bound to a source");
        return -1;
    }
    std::lock_guard<std::mutex> source_lock(source->mutex);
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
            /* Binding starts the vendor receive worker before Core can issue
               SetKeyFrame. At 60 fps that short window can fill the fixed
               12-pack queue with stale P frames, leaving no slot for the
               requested SPS/PPS. Empty only the already-ready queue, then
               request the decoder's new random-access point. */
            encoder->prepared_size = 0;
            encoder->prepared_pts_ns = 0;
            const int drained = mmf::drain_h26x_packets(
                encoder->channel, encoder->output.data(),
                static_cast<int>(encoder->output.size()));
            if (drained < 0) {
                set_error(error, error_capacity,
                          "drain bound MMF frames before IDR failed: %d", drained);
                return -1;
            }
            if (mmf::request_h26x_idr(encoder->channel) != 0) {
                set_error(error, error_capacity, "request bound MMF IDR failed");
                return -1;
            }
            encoder->request_keyframe = false;
        }
        if (encoder->prepared_size != 0) {
            result = static_cast<int>(encoder->prepared_size);
            output_data = encoder->output.data();
            output_pts_ns = encoder->prepared_pts_ns;
            encoder->prepared_size = 0;
            encoder->prepared_pts_ns = 0;
        }
    }
    /* Wait on the VENC fd outside g_mmf_mutex. GetStream/select while
       holding that lock blocked bind, HDMI rebuild, and shutdown. */
    if (result == 0 && output_data == nullptr)
        result = drain_venc_packet(encoder, &output_data, &output_pts_ns);
    if (result < 0) {
        set_error(error, error_capacity, "read bound MMF frame failed: %d", result);
        return -1;
    }
    if (result > 0) {
        source->failures = 0;
        if (!encoder->placeholder_frames) {
            source->last_frame_ns.store(monotonic_ns(), std::memory_order_relaxed);
            source->cached_signal.store(1, std::memory_order_relaxed);
        }
    } else {
        /* Default H.264/H.265 never calls source_read. Resolution changes
           and the no-signal placeholder must be handled here. */
        source->failures++;
        const int rebuilt = maybe_rebuild_for_hdmi_change(
            source, error, error_capacity);
        if (rebuilt != 0) {
            encoder->placeholder_frames = false;
            invalidate_stale_encoder(encoder);
            return -1;
        }
        if (source->out_of_range.load(std::memory_order_relaxed) ||
            cached_signal_present(source) == 0)
            return encode_bound_placeholder(
                encoder, source, packet, error, error_capacity);
        if (encoder->placeholder_frames) {
            encoder->placeholder_frames = false;
            encoder->request_keyframe = true;
            if (bind_encoder_to_source_locked(
                    encoder, source, error, error_capacity) != 0)
                return -1;
        }
    }
    packet->data = result > 0 ? output_data : nullptr;
    packet->data_size = result > 0 ? static_cast<uint64_t>(result) : 0;
    packet->codec = public_codec(encoder->codec_type);
    packet->key_frame = 0;
    packet->pts_ns = monotonic_ns();
    return 0;
}

int32_t encoder_unbind_source(void *opaque, char *error, uint32_t error_capacity) {
    auto *encoder = static_cast<Encoder *>(opaque);
    if (encoder == nullptr) {
        set_error(error, error_capacity, "encoder is null");
        return -1;
    }
    std::lock_guard<std::mutex> lock(encoder->mutex);
    if (!encoder->source_bound && encoder->bound_source == nullptr)
        return 0;
    /* Keep the configured channel and its receive worker across reconnects.
       This CVITEK driver cannot reliably restart a stopped VENC worker:
       StartRecvFrame may succeed without recreating it, after which VPSS
       fills the input waitq forever.  mmf::unbind_h26x_from_capture disconnects only
       the producer; close_encoder performs the worker's single Stop when the
       encoder is reset or finally released. */
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    const bool live = encoder->initialized && encoder->source_bound &&
        encoder->mmf_generation == g_mmf_generation.load(std::memory_order_acquire);
    if (live && mmf::unbind_h26x_from_capture(encoder->channel) != 0) {
        set_error(error, error_capacity, "unbind VPSS from VENC failed");
        return -1;
    }
    encoder->source_bound = false;
    encoder->frame_pending = false;
    encoder->packet_borrowed = false;
    encoder->pending_pts_ns = 0;
    encoder->prepared_size = 0;
    encoder->prepared_pts_ns = 0;
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
    if (configure_encoder(encoder, frame->width, frame->height,
                          error, error_capacity) != 0) {
        return -1;
    }

    int result = 0;
    uint64_t output_pts_ns = frame->pts_ns;
    const uint8_t *output_data = nullptr;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (encoder->codec_type == 0) {
            /* JPEG uses a borrowed VI/VB frame without copying it. Keep the
               Encode call synchronous so the pipeline cannot return that
               frame to VI before VENC has produced the JPEG bitstream. */
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
            if (result > 0) output_data = encoder->output.data();
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
            if (encoder->frame_pending) {
                result = drain_venc_packet(encoder, &output_data, &output_pts_ns);
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
    // JPEG quality is supplied to every mmf::submit_jpeg_frame call,
    // so changing it does not require tearing down channel 0. NanoKVM's
    // vendor JPEG teardown also disturbs the VPSS path shared by H.26x.
    if (encoder->codec_type != 0) {
        close_encoder(encoder);
    }
    encoder->config.quality_factor = quality;
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
        ONEKVM_VIDEO_FEATURE_BOUND_ENCODER,
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
};


} // namespace onekvm::video_backend
#pragma GCC visibility pop
