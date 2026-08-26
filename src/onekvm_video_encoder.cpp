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
    const int output_fps = mmf::clamp_output_fps(fps);
    const double fps_scale = output_fps > mmf::kDefaultInputFps
        ? static_cast<double>(output_fps) / mmf::kDefaultInputFps
        : 1.0;
    return std::max(100, static_cast<int>(static_cast<double>(base) * quality * fps_scale));
}

static int normalized_output_fps(int fps)
{
    return mmf::clamp_output_fps(fps);
}

static int normalized_gop(int gop, int fps)
{
    if (gop > 0)
        return gop;
    return std::max(1, fps);
}

static bool encoder_rc_unchanged(const onekvm_video_encoder_config_v1 &cur,
                                 const onekvm_video_encoder_config_v1 &next)
{
    return cur.quality_factor == next.quality_factor &&
           cur.bitrate_kbps == next.bitrate_kbps &&
           cur.initial_qp == next.initial_qp &&
           cur.min_qp == next.min_qp &&
           cur.max_qp == next.max_qp;
}

void close_encoder(Encoder *encoder) {
    if (!encoder->initialized) {
        return;
    }
    /* Join the VENC reader before taking g_mmf_mutex or DestroyChn.
       GetStream holds EnterVcodecLock; detaching then destroying deadlocks.
       Skip stale handles: the channel number may already belong to a
       newer encoder after an MMF generation bump. */
    if (encoder->codec_type != 0 && encoder->channel >= 0 &&
        encoder->mmf_generation ==
            g_mmf_generation.load(std::memory_order_acquire))
        mmf::stop_h26x_reader(encoder->channel);
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
    encoder->bound_since_ns = 0;
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
        return -1;
    }
    int fps = normalized_output_fps(encoder->config.fps);
    // The capture/VPSS path can continue delivering frames at the HDMI input
    // cadence even when the requested output is lower.  Tell VENC the
    // expected input cadence separately from the requested destination rate;
    // using the target for both fields makes the driver assume every submitted
    // frame is already at the target rate and it will not perform rate
    // conversion (the stream then remains close to the source FPS).
    const int venc_input_fps = mmf::venc_src_fps(fps);
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
    const auto next = normalized_encoder_config(config);
    const int next_codec = codec_type(config->codec);
    /* Cube cannot close/recreate a live VENC channel from this thread:
       GetStream holds EnterVcodecLock on the reader. Only fps/gop changes
       are posted for that thread. Suspend and quality/bitrate/QP still
       close the channel — after stop_h26x_reader joins. */
    if (encoder->initialized && encoder->source_bound &&
        encoder->codec_type != 0 && next_codec != encoder->codec_type) {
        set_error(error, error_capacity,
                  "cannot change bound VENC codec; VPSS would fill waitq");
        return -1;
    }
    if (encoder->initialized && encoder->codec_type != 0 &&
        encoder->codec_type == next_codec && encoder->channel >= 0 &&
        encoder_rc_unchanged(encoder->config, next)) {
        const int fps = normalized_output_fps(next.fps);
        const int gop = normalized_gop(next.gop, fps);
        const int cur_fps = normalized_output_fps(encoder->config.fps);
        const int cur_gop = normalized_gop(encoder->config.gop, cur_fps);
        if (fps != cur_fps || gop != cur_gop) {
            if (mmf::set_h26x_output_fps(encoder->channel, fps, gop) != 0) {
                set_error(error, error_capacity,
                          "queue VENC output fps %d failed", fps);
                return -1;
            }
            encoder->config = next;
            encoder->codec_type = next_codec;
            return 0;
        }
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

int encode_bound_placeholder(Encoder *encoder, Source *source,
                             onekvm_video_packet_v1 *packet,
                             char *error, uint32_t error_capacity) {
    const bool entering = !encoder->placeholder_frames;
    encoder->placeholder_frames = true;
    if (encoder->channel < 0 || !encoder->initialized) {
        set_error(error, error_capacity, "placeholder requires an open VENC channel");
        return -1;
    }
    /* CloseFd/UnBind while the reader is in GetStream races EnterVcodecLock.
       Join first, same order as close_encoder. Do not take g_mmf_mutex until
       GetStream has returned. */
    if (encoder->source_bound) {
        mmf::stop_h26x_reader(encoder->channel);
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        if (mmf::unbind_h26x_from_capture(encoder->channel) != 0) {
            set_error(error, error_capacity,
                      "unbind VPSS from VENC for placeholder failed");
            return -1;
        }
        encoder->source_bound = false;
    }
    const auto [width, height] = source_output_size(source);
    if (ensure_no_signal_nv21(source, width, height, error, error_capacity) != 0)
        return -1;
    {
        std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
        /* The vendor GetStream and SendFrame ioctls share EnterVcodecLock.
           Submit the first still while the reader is stopped; starting the
           reader first makes SendFrame intermittently return VENC_BUSY. */
        if (entering)
            (void)mmf::request_h26x_idr(encoder->channel);
        const int push = mmf::submit_h26x_frame(
            encoder->channel, source->no_signal_frame.data(),
            width, height, kMMFNV21);
        if (push != 0) {
            mmf::start_h26x_reader(encoder->channel, false);
            set_error(error, error_capacity,
                      "submit no-signal frame to VENC failed: %d", push);
            return -1;
        }
        /* request_h26x_idr above already marked the reader as waiting for an
           IDR.  Suppress a second ioctl while the submitted still encodes. */
        mmf::start_h26x_reader(encoder->channel, false);
    }
    const uint8_t *output_data = nullptr;
    uint64_t output_pts_ns = 0;
    bool key_frame = false;
    int result = drain_venc_packet(
        encoder, &output_data, &output_pts_ns, &key_frame);
    if (result == 0) {
        (void)mmf::wait_ready_h26x_packet(encoder->channel, 40);
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
    packet->data = output_data;
    packet->data_size = static_cast<uint64_t>(result);
    packet->codec = public_codec(encoder->codec_type);
    packet->key_frame = key_frame ? 1 : 0;
    packet->pts_ns = monotonic_ns();
    encoder->last_encode_ns.store(mmf::h26x_last_encode_ns(encoder->channel),
                                 std::memory_order_relaxed);
    return 0;
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
            mmf::h26x_reader_want_idr(encoder->channel);
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
    const bool missing_signal =
        source->out_of_range.load(std::memory_order_relaxed) ||
        cached_signal_present(source) == 0;
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
        if (result > 0 && !missing_signal)
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
            const bool still_missing =
                source->out_of_range.load(std::memory_order_relaxed) ||
                cached_signal_present(source) == 0;
            if (result > 0)
                return fill_bound_live_packet(
                    encoder, source, packet, output_data, result, key_frame,
                    !still_missing);
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
        encoder->request_keyframe = true;
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

    /* Host mode changes often stop both VI and VENC. Keep probing/rebuilding
       the source, but do not turn this already-started bound channel into a
       userspace SendFrame channel. VPSS_UnBind does not clear the vendor
       driver's currBindMode flag; SendFrame after that point can retain the
       global VPU lock while venc-handler waits for it, pinning every VPSS VB
       block and making StopRecvFrame hang in kthread_stop. Keep the producer
       relationship intact and return an empty packet while HDMI is absent.
       The bound worker resumes without a channel-mode transition when input
       frames return. */
    source->failures++;
    const int rebuilt = maybe_rebuild_for_hdmi_change(
        source, error, error_capacity);
    if (rebuilt != 0) {
        encoder->placeholder_frames = false;
        invalidate_stale_encoder(encoder);
        return -1;
    }
    return fill_bound_empty_packet(encoder, packet);
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
    if (!encoder->source_bound && encoder->bound_source == nullptr)
        return 0;
    /* Keep the configured channel and its receive worker across reconnects.
       This CVITEK driver cannot reliably restart a stopped VENC worker:
       StartRecvFrame may succeed without recreating it, after which VPSS
       fills the input waitq forever.  mmf::unbind_h26x_from_capture disconnects only
       the producer; close_encoder performs the worker's single Stop when the
       encoder is reset or finally released. Join our GetStream reader before
       CloseFd so the two never share EnterVcodecLock. */
    const bool live = encoder->initialized && encoder->source_bound &&
        encoder->mmf_generation == g_mmf_generation.load(std::memory_order_acquire);
    if (live && encoder->codec_type != 0 && encoder->channel >= 0)
        mmf::stop_h26x_reader(encoder->channel);
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
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
        ONEKVM_VIDEO_FEATURE_BOUND_ENCODER |
        ONEKVM_VIDEO_FEATURE_LATENCY,
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
};


} // namespace onekvm::video_backend
#pragma GCC visibility pop
