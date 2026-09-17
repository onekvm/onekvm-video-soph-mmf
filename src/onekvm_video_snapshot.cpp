#include "onekvm_video_backend_internal.hpp"
#include "mmf_internal.hpp"
#include "snapshot_admission.hpp"

#include <chrono>

namespace onekvm::mmf {
int acquire_capture_frame_timeout(int channel, void **data, int *length,
    int *width, int *height, int *format, int timeout_ms);
int read_jpeg_packet_timeout(
    int channel, uint8_t *destination, int capacity, int timeout_ms);
int submit_jpeg_frame_timeout(int channel, uint8_t *data, int width, int height,
    int pixel_format, int quality, int timeout_ms);
}

namespace onekvm::video_backend {
namespace {

int remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return remaining > 0 ? static_cast<int>(remaining) : 0;
}

int snapshot_jpeg_quality(double quality)
{
    if (quality <= 0.0)
        return 80;
    return 55 + static_cast<int>(quality * 35.0);
}

} // namespace

int32_t source_snapshot(
    void *opaque, const onekvm_video_snapshot_request_v1 *request,
    uint8_t *data, uint64_t capacity, onekvm_video_snapshot_result_v1 *result,
    char *error, uint32_t error_capacity)
{
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || request == nullptr || result == nullptr ||
        request->struct_size < sizeof(*request) ||
        result->struct_size < sizeof(*result) || request->flags != 0 ||
        request->width < 0 || request->height < 0 ||
        ((request->width == 0) != (request->height == 0)) ||
        (request->width != 0 && ((request->width & 1) != 0 ||
                                  (request->height & 1) != 0)) ||
        !std::isfinite(request->quality_factor) ||
        request->quality_factor < 0.0 || request->quality_factor > 1.0 ||
        request->timeout_ms > 1000 || (data == nullptr && capacity != 0)) {
        set_error(error, error_capacity, "invalid snapshot request");
        return ONEKVM_VIDEO_RESOURCE_INVALID;
    }
    result->data_size = 0;
    result->width = 0;
    result->height = 0;
    result->pts_ns = 0;

    const uint32_t timeout_ms = request->timeout_ms == 0 ? 250 : request->timeout_ms;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    std::lock_guard<std::mutex> source_lock(source->mutex);
    if (!source->initialized) {
        set_error(error, error_capacity, "snapshot source is not initialized");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    }
    if (source->frame_pending) {
        set_error(error, error_capacity, "snapshot source frame is already borrowed");
        return ONEKVM_VIDEO_RESOURCE_BUSY;
    }
    switch (onekvm::snapshot_admission(
        source->cached_signal.load(std::memory_order_relaxed),
        source->last_signal_probe_ns.load(std::memory_order_relaxed),
        source->out_of_range.load(std::memory_order_relaxed))) {
    case onekvm::SnapshotAdmission::no_signal:
        set_error(error, error_capacity,
                  "snapshot unavailable without an HDMI signal");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    case onekvm::SnapshotAdmission::signal_unavailable:
        set_error(error, error_capacity,
                  "snapshot HDMI signal state is unavailable");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    case onekvm::SnapshotAdmission::out_of_range:
        set_error(error, error_capacity,
                  "snapshot unavailable for unsupported HDMI input");
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    case onekvm::SnapshotAdmission::allow:
        break;
    }
    const auto [expected_width, expected_height] = source_output_size(source);
    if (request->width != 0 &&
        (request->width != expected_width || request->height != expected_height)) {
        set_error(error, error_capacity,
            "snapshot size %dx%d does not match source output %dx%d",
            request->width, request->height, expected_width, expected_height);
        return ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
    }

    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    const int channel = source->channel;
    if (channel < 0 || channel >= MMF_VI_MAX_CHN ||
        !mmf::g_runtime.vi_chn_is_inited[channel]) {
        set_error(error, error_capacity, "snapshot capture channel is unavailable");
        return ONEKVM_VIDEO_RESOURCE_UNINITIALIZED;
    }
    const bool channel_was_running = mmf::g_runtime.vi_chn_running[channel];
    const bool vi_was_running = mmf::g_runtime.vi_dma_running;

    /* Creating JPEG allocates its VENC buffers and can take several frame
       periods. Reserve it before borrowing the depth-1 VPSS output so the
       live scaler is not left without a reusable output block during setup. */
    onekvm_video_encoder_allocation_request_v1 allocation_request{};
    allocation_request.struct_size = sizeof(allocation_request);
    allocation_request.config.struct_size = sizeof(allocation_request.config);
    allocation_request.config.codec = ONEKVM_VIDEO_CODEC_MJPEG;
    allocation_request.config.quality_factor = request->quality_factor;
    allocation_request.config.fps = 1;
    allocation_request.width = expected_width;
    allocation_request.height = expected_height;
    allocation_request.pixel_format = ONEKVM_VIDEO_PIXEL_NV21;
    allocation_request.input_mode = ONEKVM_VIDEO_ENCODER_INPUT_MANUAL;
    allocation_request.purpose = ONEKVM_VIDEO_ENCODER_PURPOSE_BACKGROUND;
    onekvm_video_encoder_allocation_v1 allocation{};
    allocation.struct_size = sizeof(allocation);
    void *encoder_opaque = nullptr;
    int32_t status = encoder_allocate(
        &allocation_request, &encoder_opaque, &allocation,
        error, error_capacity);
    if (status != ONEKVM_VIDEO_RESOURCE_OK)
        return status;
    auto *encoder = static_cast<Encoder *>(encoder_opaque);

    int idle_h26x_channel = -1;
    if (!channel_was_running &&
        mmf::begin_idle_h26x_drain(channel, &idle_h26x_channel) != 0) {
        encoder_destroy(encoder_opaque);
        set_error(error, error_capacity,
                  "snapshot could not start idle H.26x drain");
        return ONEKVM_VIDEO_RESOURCE_INTERNAL;
    }

    void *frame_data = nullptr;
    int frame_length = 0;
    int frame_width = 0;
    int frame_height = 0;
    int frame_format = 0;
    const int capture_timeout = remaining_ms(deadline);
    int capture_result = capture_timeout == 0 ? -1 :
        mmf::acquire_capture_frame_timeout(channel, &frame_data, &frame_length,
            &frame_width, &frame_height, &frame_format, capture_timeout);
    if (capture_result != 0) {
        const bool timed_out = remaining_ms(deadline) == 0;
        encoder_destroy(encoder_opaque);
        if (idle_h26x_channel >= 0) {
            if (mmf::finish_idle_h26x_drain(idle_h26x_channel) != 0) {
                set_error(error, error_capacity,
                          "snapshot could not drain idle H.26x encoder");
                return ONEKVM_VIDEO_RESOURCE_INTERNAL;
            }
        } else if (!channel_was_running) {
            if (mmf::pause_vpss_channel(channel) != 0) {
                set_error(error, error_capacity,
                          "snapshot could not park idle capture channel");
                return ONEKVM_VIDEO_RESOURCE_INTERNAL;
            }
            if (!vi_was_running && mmf::pause_vi_dma() != 0) {
                set_error(error, error_capacity,
                          "snapshot could not park idle VI DMA");
                return ONEKVM_VIDEO_RESOURCE_INTERNAL;
            }
        }
        set_error(error, error_capacity, timed_out
            ? "snapshot capture timed out" : "snapshot capture failed: %d",
            capture_result);
        return timed_out ? ONEKVM_VIDEO_RESOURCE_TIMEOUT
                         : ONEKVM_VIDEO_RESOURCE_INTERNAL;
    }

    /* This is the time the completed VPSS frame became observable to the
       backend. The vendor frame timestamp has no documented clock unit. */
    const uint64_t capture_pts_ns = monotonic_ns();
    status = ONEKVM_VIDEO_RESOURCE_INTERNAL;
    do {
        if (frame_data == nullptr || frame_length <= 0 ||
            frame_width != expected_width || frame_height != expected_height ||
            frame_format != kMMFNV21) {
            set_error(error, error_capacity,
                "snapshot frame shape changed during capture");
            status = ONEKVM_VIDEO_RESOURCE_UNSUPPORTED;
            break;
        }
        const int encode_timeout = remaining_ms(deadline);
        if (encode_timeout == 0 ||
            mmf::submit_jpeg_frame_timeout(encoder->channel,
                static_cast<uint8_t *>(frame_data), frame_width, frame_height,
                kMMFNV21, snapshot_jpeg_quality(request->quality_factor),
                encode_timeout) != 0) {
            set_error(error, error_capacity, "snapshot JPEG submission failed");
            status = encode_timeout == 0 ? ONEKVM_VIDEO_RESOURCE_TIMEOUT
                                         : ONEKVM_VIDEO_RESOURCE_INTERNAL;
            break;
        }
        const int jpeg_size = mmf::read_jpeg_packet_timeout(
            encoder->channel, encoder->output.data(),
            static_cast<int>(encoder->output.size()),
            remaining_ms(deadline));
        if (jpeg_size <= 0) {
            set_error(error, error_capacity, "snapshot JPEG timed out");
            status = remaining_ms(deadline) == 0 ? ONEKVM_VIDEO_RESOURCE_TIMEOUT
                                                : ONEKVM_VIDEO_RESOURCE_INTERNAL;
            break;
        }
        if (mmf::release_jpeg_packet(encoder->channel) != 0) {
            set_error(error, error_capacity, "snapshot JPEG release failed");
            status = ONEKVM_VIDEO_RESOURCE_INTERNAL;
            break;
        }
        result->data_size = static_cast<uint64_t>(jpeg_size);
        result->width = frame_width;
        result->height = frame_height;
        result->pts_ns = capture_pts_ns;
        if (capacity < result->data_size) {
            set_error(error, error_capacity,
                "snapshot buffer needs %llu bytes",
                static_cast<unsigned long long>(result->data_size));
            status = ONEKVM_VIDEO_RESOURCE_BUFFER_TOO_SMALL;
            break;
        }
        std::memcpy(data, encoder->output.data(), static_cast<size_t>(jpeg_size));
        status = ONEKVM_VIDEO_RESOURCE_OK;
    } while (false);

    /* Stop/reset/destroy JPEG before returning the VPSS block so hardware can
       no longer reference the borrowed frame. */
    encoder_destroy(encoder_opaque);
    mmf::release_capture_frame(channel);
    if (idle_h26x_channel >= 0) {
        if (mmf::finish_idle_h26x_drain(idle_h26x_channel) != 0) {
            set_error(error, error_capacity,
                      "snapshot could not drain idle H.26x encoder");
            status = ONEKVM_VIDEO_RESOURCE_INTERNAL;
        }
    } else if (!channel_was_running) {
        if (mmf::pause_vpss_channel(channel) != 0) {
            set_error(error, error_capacity,
                      "snapshot could not park idle capture channel");
            status = ONEKVM_VIDEO_RESOURCE_INTERNAL;
        }
        if (!vi_was_running && mmf::pause_vi_dma() != 0) {
            set_error(error, error_capacity,
                      "snapshot could not park idle VI DMA");
            status = ONEKVM_VIDEO_RESOURCE_INTERNAL;
        }
    }
    return status;
}

} // namespace onekvm::video_backend
