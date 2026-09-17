#include "onekvm_video_backend_internal.hpp"
#include "lt6911_edid.hpp"

#include <exception>

#pragma GCC visibility push(hidden)
namespace onekvm::video_backend {

/* All VI/VPSS/VENC objects share one vendor MMF context. Resolution changes
 * tear that context down, so serialize every MMF operation across sources and
 * encoders. Recursive locking keeps the existing small helper boundaries. */
std::recursive_mutex g_mmf_mutex;
std::atomic<uint64_t> g_mmf_generation{1};

void set_error(char *error, uint32_t capacity, const char *format, ...) {
    if (error == nullptr || capacity == 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vsnprintf(error, capacity, format, args);
    va_end(args);
    error[capacity - 1] = '\0';
}

std::pair<int, int> resolution_size(int resolution) {
    const auto out = onekvm::target_output_resolution(resolution, {});
    return {static_cast<int>(out.width), static_cast<int>(out.height)};
}

std::pair<int, int> source_output_size(const Source *source) {
    if (source == nullptr)
        return {1920, 1080};
    if (source->capture_width > 0 && source->capture_height > 0)
        return {source->capture_width, source->capture_height};
    const auto out = onekvm::target_output_resolution(
        source->config.resolution, source->input_resolution.current());
    return {static_cast<int>(out.width), static_cast<int>(out.height)};
}

uint64_t monotonic_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool nv21_size(int width, int height, size_t *size) {
    if (size == nullptr || width <= 0 || height <= 0 ||
        (width & 1) != 0 || (height & 1) != 0) {
        return false;
    }
    const uint64_t pixels = static_cast<uint64_t>(width) *
        static_cast<uint64_t>(height);
    const uint64_t bytes = pixels + pixels / 2;
    if (bytes > static_cast<uint64_t>(SIZE_MAX) || bytes > INT32_MAX) {
        return false;
    }
    *size = static_cast<size_t>(bytes);
    return true;
}

bool read_hdmi_timing(onekvm::InputResolution *csi,
                      onekvm::InputResolution *hdmi) {
    onekvm_lt6911_input_timing timing{};
    if (csi == nullptr || hdmi == nullptr)
        return false;
    if (lt6911_get_input_timing(0, &timing) != 0)
        return false;
    *csi = {timing.csi_width, timing.csi_height};
    *hdmi = {timing.hdmi_width, timing.hdmi_height};
    return true;
}

bool read_hdmi_input(onekvm::InputResolution *resolution) {
    onekvm::InputResolution csi{};
    onekvm::InputResolution hdmi{};
    if (resolution == nullptr || !read_hdmi_timing(&csi, &hdmi))
        return false;
    const auto chosen = onekvm::choose_vi_receiver_size(csi, hdmi, {});
    if (chosen.width != 0) {
        *resolution = chosen;
        return true;
    }
    if (hdmi.width != 0) {
        *resolution = hdmi;
        return true;
    }
    *resolution = csi;
    return csi.width != 0;
}

bool read_supported_input_resolution(onekvm::InputResolution *resolution,
                                     int fps) {
    onekvm::InputResolution value{};
    if (!read_hdmi_input(&value) ||
        !onekvm::supported_input_rate(value, fps))
        return false;
    *resolution = value;
    return true;
}

onekvm::InputResolution initial_input_resolution(int fps) {
    onekvm::InputResolution first{};
    onekvm::InputResolution second{};
    if (read_supported_input_resolution(&first, fps)) {
        std::this_thread::sleep_for(kInitialResolutionSampleDelay);
        if (read_supported_input_resolution(&second, fps) && first == second)
            return first;
    }
    return {1920, 1080};
}

bool stable_hdmi_input(onekvm::InputResolution *resolution) {
    onekvm::InputResolution first{};
    onekvm::InputResolution second{};
    if (resolution == nullptr || !read_hdmi_input(&first))
        return false;
    std::this_thread::sleep_for(kInitialResolutionSampleDelay);
    if (!read_hdmi_input(&second) || first != second)
        return false;
    *resolution = first;
    return true;
}

bool stable_input_resolution(onekvm::InputResolution *resolution, int fps) {
    onekvm::InputResolution value{};
    if (!stable_hdmi_input(&value) ||
        !onekvm::supported_input_rate(value, fps))
        return false;
    *resolution = value;
    return true;
}

int cached_signal_present(Source *source) {
    if (source == nullptr) return 0;
    const uint64_t now = monotonic_ns();
    /* Live HDMI is VENC packets, not VI IntCnt. After a host mode change
       the encoder can keep repeating the old geometry; the packet path
       marks cached_signal=0 and the watcher then probes CSI. */

    const int cached = source->cached_signal.load(std::memory_order_relaxed);
    if (source->capture_live.load(std::memory_order_relaxed))
        return cached;

    const auto probe_window = cached == 0
        ? kNoSignalProbeInterval : kSignalProbeInterval;
    const uint64_t probe_interval = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            probe_window).count());
    const uint64_t last_probe = source->last_signal_probe_ns.load(std::memory_order_relaxed);
    if (last_probe != 0 && now >= last_probe && now - last_probe < probe_interval) {
        return cached;
    }
    if (source->signal_probe_running.test_and_set(std::memory_order_acquire)) {
        return source->cached_signal.load(std::memory_order_relaxed);
    }
    onekvm::InputResolution csi{};
    onekvm::InputResolution hdmi{};
    const int present = read_hdmi_timing(&csi, &hdmi)
        ? (onekvm::csi_timing_present(csi, hdmi) ? 1 : 0) : -1;
    source->cached_signal.store(present, std::memory_order_relaxed);
    source->last_signal_probe_ns.store(monotonic_ns(), std::memory_order_relaxed);
    source->signal_probe_running.clear(std::memory_order_release);
    return present;
}

void publish_input_format(Source *source) {
    if (source == nullptr)
        return;
    onekvm::InputResolution input = source->reported_input;
    if (input.width == 0 || input.height == 0)
        input = source->input_resolution.current();
    const uint64_t packed =
        (static_cast<uint64_t>(input.width) << 32) |
        static_cast<uint64_t>(input.height);
    source->cached_input_size.store(packed, std::memory_order_release);
    source->cached_input_fps.store(source->config.fps, std::memory_order_relaxed);
}

void reset_signal_cache(Source *source) {
    source->last_frame_ns.store(0, std::memory_order_relaxed);
    source->last_signal_probe_ns.store(0, std::memory_order_relaxed);
    source->cached_signal.store(-1, std::memory_order_relaxed);
    source->last_vi_int_cnt.store(0, std::memory_order_relaxed);
    source->signal_probe_running.clear(std::memory_order_release);
    /* Give VENC a chance to produce the first access unit before the bound
       path treats empty reads as a missing HDMI mode and touches LT6911. */
    source->last_hdmi_probe = std::chrono::steady_clock::now();
}

void release_source_frame(Source *source) {
    if (!source->frame_pending || source->channel < 0) {
        return;
    }
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    mmf::release_capture_frame(source->channel);
    source->frame_pending = false;
}

void close_source(Source *source) {
    if (!source->initialized) {
        return;
    }
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    release_source_frame(source);
    /* mmf::shutdown() owns the complete dependency-ordered teardown: bound VENC
       consumers are stopped before their VPSS/VI producers.  Removing the VI
       channel here first leaves the VENC worker running without userspace
       draining its stream, which can fill the 12-pack queue and drop the
       cached SPS/PPS needed by reconnecting H.264 decoders.  It also made the
       later global teardown destroy the same VI resources twice. */
    mmf::shutdown();
    g_mmf_generation.fetch_add(1, std::memory_order_acq_rel);
    source->channel = -1;
    source->initialized = false;
    source->capture_width = 0;
    source->capture_height = 0;
    source->capture_live.store(false, std::memory_order_relaxed);
    source->no_signal_frame.clear();
}

int open_source(Source *source, const onekvm_video_source_config_v1 *config,
                char *error, uint32_t error_capacity,
                const onekvm::InputResolution *requested_input = nullptr) {
    if (config == nullptr || config->struct_size < sizeof(*config)) {
        set_error(error, error_capacity, "invalid source configuration");
        return -1;
    }
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    if (pcie_hdmi_startup_reset() != 0) {
        set_error(error, error_capacity, "reset PCIe HDMI bridge failed: %s",
                  std::strerror(errno));
        return -1;
    }
    /* After reboot the LT6911 HDMI RX stays idle until D283 is written
       once. Repeating that from the watcher disrupts CSI. */
    (void)lt6911_kick_hdmi();
    const int requested_fps = config->fps > 0 ? static_cast<int>(config->fps) : 60;
    const onekvm::InputResolution input = requested_input != nullptr &&
        onekvm::supported_input_rate(*requested_input, requested_fps)
        ? *requested_input : initial_input_resolution(requested_fps);
    const auto output = onekvm::target_output_resolution(
        config->resolution, input);
    const int width = static_cast<int>(output.width);
    const int height = static_cast<int>(output.height);
    std::fprintf(stderr, "OneKVM: configuring HDMI input %ux%u, output %dx%d\n",
                 input.width, input.height, width, height);
    if (onekvm_lt6911_set_active_size(input.width, input.height) != 0) {
        set_error(error, error_capacity, "invalid LT6911 input size %ux%u",
                  input.width, input.height);
        return -1;
    }
    if (mmf::initialize() != 0) {
        set_error(error, error_capacity, "initialize failed");
        return -1;
    }
    /* initialize() also reclaims stale MMF owners. Arm the LT6911 only after
       that teardown, but before StartViChn: programming CSI while an old VI
       generation is still active can deadlock the vendor frontend on a Core
       restart. */
    if (lt6911_start_csi() != 0) {
        mmf::shutdown();
        set_error(error, error_capacity, "LT6911 CSI arm before VI failed");
        return -1;
    }
    if (mmf::start_capture_pipeline() != 0) {
        mmf::shutdown();
        set_error(error, error_capacity, "start MMF capture pipeline failed");
        return -1;
    }
    const int channel = onekvm::mmf::vpss_phy_channel(width);
    if (channel < 0 || mmf::capture_channel_open(channel)) {
        mmf::stop_capture_pipeline();
        mmf::shutdown();
        set_error(error, error_capacity, "no free MMF VI channel");
        return -1;
    }
    mmf::set_capture_mirror(channel, false);
    mmf::set_capture_flip(channel, false);
    const int fps = onekvm::mmf::clamp_pipeline_fps(
        requested_fps, static_cast<int>(input.width),
        static_cast<int>(input.height), width, height);
    int result = mmf::open_capture_channel(channel, width, height, kMMFNV21, fps);
    if (result != 0) {
        mmf::stop_capture_pipeline();
        mmf::shutdown();
        set_error(error, error_capacity, "open MMF capture channel failed: %d", result);
        return -1;
    }

    source->channel = channel;
    source->initialized = true;
    source->capture_width = width;
    source->capture_height = height;
    source->config = *config;
    source->config.device = nullptr;
    source->failures = 0;
    source->last_recovery = {};
    source->input_resolution.set_current(input);
    source->reported_input = input;
    source->hdmi_blanking_samples = 0;
    source->hdmi_oor_samples = 0;
    source->hdmi_follow_samples = 0;
    source->hdmi_rearm_samples = 0;
    source->csi_half_rate_samples = 0;
    source->last_half_rate_check_ns = 0;
    source->out_of_range.store(false, std::memory_order_relaxed);
    onekvm::InputResolution probed{};
    if (read_hdmi_input(&probed)) {
        source->reported_input = probed;
        if (onekvm::classify_hdmi_input(probed, requested_fps) ==
            onekvm::HdmiInputClass::OutOfRange) {
            source->out_of_range.store(true, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "OneKVM: HDMI input %ux%u is out of range\n",
                         probed.width, probed.height);
        }
    }
    publish_input_format(source);
    source->no_signal_frame.clear();
    reset_signal_cache(source);
    return 0;
}

int recover_source(Source *source, char *error, uint32_t error_capacity) {
    if (source == nullptr || source->initialized)
        return 0;
    const auto want = source->pending_receiver.width != 0
        ? source->pending_receiver
        : source->input_resolution.current();
    const onekvm::InputResolution *requested =
        want.width != 0 ? &want : nullptr;
    return open_source(source, &source->config, error, error_capacity,
                       requested);
}

int reopen_source_for_input(Source *source, onekvm::InputResolution input,
                            char *error, uint32_t error_capacity) {
    const onekvm_video_source_config_v1 config = source->config;
    source->pending_receiver = input;
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    close_source(source);
    if (open_source(source, &config, error, error_capacity, &input) != 0)
        return -1;
    source->pending_receiver = {};
    std::fprintf(stderr, "OneKVM: HDMI input resolution changed to %ux%u\n",
                 input.width, input.height);
    return 0;
}

int rebuild_for_hdmi_timing(Source *source, bool force_probe, char *error,
                            uint32_t error_capacity) {
    if (source == nullptr)
        return 0;

    /* VENC last_frame_ns is not HDMI liveness. GetStream can stall while
       CSI is still running; probing LT6911 then interrupts a live frontend.
       Idle has no VI DMA, so CSI timing is the presence signal. A live
       bound path skips I2C while cached_signal stays 1. */
    const bool capture_live = source->capture_live.load(
        std::memory_order_relaxed);
    const bool recent_frames = capture_live &&
        source->cached_signal.load(std::memory_order_relaxed) == 1;
    source->last_signal_probe_ns.store(monotonic_ns(), std::memory_order_relaxed);

    /* A live supported mode does not need I2C. Touching 80ee here is what
       produced 490x404 "out of range" samples and flashed the no-signal
       artwork. 1440p is the same LT6911 as 1080p. */
    if (!force_probe && recent_frames &&
        onekvm::supported_input_resolution(source->input_resolution.current()))
        return 0;

    const auto now = std::chrono::steady_clock::now();
    const auto probe_interval = recent_frames
        ? kHDMIChangeProbeInterval
        : kHDMIFollowProbeInterval;
    const bool interval_elapsed =
        force_probe ||
        source->last_hdmi_probe.time_since_epoch().count() == 0 ||
        now - source->last_hdmi_probe >= probe_interval;
    if (!onekvm::hdmi_resolution_probe_due(
            static_cast<unsigned>(std::max(0, source->failures)),
            recent_frames, interval_elapsed))
        return 0;

    source->last_hdmi_probe = now;

    onekvm::InputResolution csi{};
    onekvm::InputResolution hdmi{};
    if (!read_hdmi_timing(&csi, &hdmi))
        return 0;

    hdmi = onekvm::infer_hdmi_mode(hdmi);
    if (!capture_live)
        source->cached_signal.store(
            onekvm::csi_timing_present(csi, hdmi) ? 1 : 0,
            std::memory_order_relaxed);
    if (hdmi.width == 0)
        source->hdmi_blanking_samples++;
    else
        source->hdmi_blanking_samples = 0;
    const auto current = source->input_resolution.current();
    const auto reported = hdmi.width != 0 ? hdmi : csi;
    if (reported.width != 0) {
        source->reported_input = reported;
        publish_input_format(source);
    }

    const int configured_fps = source->config.fps > 0
        ? static_cast<int>(source->config.fps) : 60;
    const auto kind = onekvm::classify_hdmi_input(reported, configured_fps);
    if (kind == onekvm::HdmiInputClass::OutOfRange) {
        if (onekvm::supported_input_rate(csi, configured_fps)) {
            source->hdmi_oor_samples = 0;
            source->out_of_range.store(false, std::memory_order_relaxed);
            return 0;
        }
        if (++source->hdmi_oor_samples < 3)
            return 0;
        if (!source->out_of_range.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "OneKVM: HDMI input %ux%u is out of range\n",
                         reported.width, reported.height);
        }
        return 0;
    } else {
        source->hdmi_oor_samples = 0;
    }

    onekvm::InputResolution receiver =
        onekvm::choose_vi_receiver_size(csi, hdmi, current, recent_frames);
    if (onekvm::should_grow_to_max_vi_receiver(
            current, hdmi, source->hdmi_blanking_samples))
        receiver = onekvm::kMaxViReceiver;
    source->hdmi_follow_samples = onekvm::next_hdmi_follow_samples(
        recent_frames, current, hdmi, source->hdmi_follow_samples);
    /* CSI 0x0 while HDMI still reports the current 1080 is normal when
       VPSS is parked (idle) and after placeholder unbinds VI. Do not reopen. */
    source->hdmi_rearm_samples = onekvm::next_hdmi_csi_rearm_samples(
        recent_frames, csi, hdmi, current, source->hdmi_rearm_samples,
        false);
    if (!onekvm::should_rebuild_vi_receiver(
            current, receiver, csi, hdmi, recent_frames)) {
        if (!onekvm::hdmi_csi_rearm_ready(source->hdmi_rearm_samples)) {
            if (kind == onekvm::HdmiInputClass::Supported)
                source->out_of_range.store(false, std::memory_order_relaxed);
            return 0;
        }
        if (source->last_recovery.time_since_epoch().count() != 0 &&
            now - source->last_recovery < kRecoveryInterval)
            return 0;
        receiver = current;
    } else if (onekvm::resolution_pixels(receiver) <
                   onekvm::resolution_pixels(current) &&
               !onekvm::hdmi_stalled_follow_ready(
                   csi, receiver, source->hdmi_follow_samples)) {
        if (kind == onekvm::HdmiInputClass::Supported)
            source->out_of_range.store(false, std::memory_order_relaxed);
        return 0;
    }
    source->out_of_range.store(false, std::memory_order_relaxed);

    source->failures = 0;
    if (receiver == current) {
        std::fprintf(stderr,
                     "OneKVM: HDMI timing CSI %ux%u HDMI %ux%u; rearm VI %ux%u\n",
                     csi.width, csi.height, hdmi.width, hdmi.height,
                     current.width, current.height);
    } else {
        std::fprintf(stderr,
                     "OneKVM: HDMI timing CSI %ux%u HDMI %ux%u; grow/shrink VI %ux%u -> %ux%u\n",
                     csi.width, csi.height, hdmi.width, hdmi.height,
                     current.width, current.height,
                     receiver.width, receiver.height);
    }
    const int reopen = reopen_source_for_input(
        source, receiver, error, error_capacity);
    if (reopen != 0)
        return -1;
    /* open_source clears last_recovery. Stamp after reopen so a stale
       I2C 1080 cannot HPD-loop every three watcher samples. */
    source->last_recovery = std::chrono::steady_clock::now();
    set_error(error, error_capacity,
              "HDMI input changed to %ux%u; pipeline rebuilt",
              receiver.width, receiver.height);
    return 1;
}

int maybe_rebuild_for_hdmi_change(Source *source, char *error,
                                  uint32_t error_capacity) {
    return rebuild_for_hdmi_timing(source, false, error, error_capacity);
}

int maybe_rebuild_for_hdmi_change_now(Source *source, char *error,
                                      uint32_t error_capacity) {
    return rebuild_for_hdmi_timing(source, true, error, error_capacity);
}

int maybe_rearm_csi_half_rate(Encoder *encoder, Source *source,
                              char *error, uint32_t error_capacity) {
    if (encoder == nullptr || source == nullptr)
        return 0;
    if (encoder->placeholder_frames || !encoder->initialized ||
        encoder->channel < kFirstVENCChannel)
        return 0;
    if (!source->capture_live.load(std::memory_order_relaxed))
        return 0;
    const uint64_t now_ns = monotonic_ns();
    const uint64_t interval_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            onekvm::kCsiHalfRateSampleInterval).count());
    if (source->last_half_rate_check_ns != 0 &&
        now_ns >= source->last_half_rate_check_ns &&
        now_ns - source->last_half_rate_check_ns < interval_ns)
        return 0;
    source->last_half_rate_check_ns = now_ns;

    const int fps = encoder->config.fps > 0 ? encoder->config.fps : 60;
    const int target = mmf::clamp_output_fps(
        fps, encoder->width, encoder->height);
    const int enc_fps = static_cast<int>(mmf::h26x_last_enc_fps(encoder->channel));
    if (target != source->csi_half_rate_target) {
        source->csi_half_rate_target = target;
        source->csi_half_rate_rearmed = false;
        source->csi_half_rate_samples = 0;
    }
    if (onekvm::csi_half_rate_recovered(target, enc_fps)) {
        source->csi_half_rate_samples = 0;
        source->csi_half_rate_rearmed = false;
        return 0;
    }
    source->csi_half_rate_samples = onekvm::next_csi_half_rate_samples(
        target, enc_fps, source->csi_half_rate_samples,
        source->csi_half_rate_rearmed);
    if (!onekvm::csi_half_rate_rearm_ready(
            source->csi_half_rate_samples, source->csi_half_rate_rearmed))
        return 0;

    const auto now = std::chrono::steady_clock::now();
    if (source->last_recovery.time_since_epoch().count() != 0 &&
        now - source->last_recovery < kRecoveryInterval)
        return 0;

    onekvm::InputResolution receiver = source->input_resolution.current();
    if (receiver.width == 0 || receiver.height == 0)
        receiver = {1920, 1080};
    std::fprintf(stderr,
                 "OneKVM: CSI half-rate EncFramePerSec=%d target=%d; rearm VI %ux%u\n",
                 enc_fps, target, receiver.width, receiver.height);
    const int reopen = reopen_source_for_input(
        source, receiver, error, error_capacity);
    source->last_recovery = std::chrono::steady_clock::now();
    if (reopen != 0)
        return -1;
    source->csi_half_rate_rearmed = true;
    set_error(error, error_capacity,
              "CSI half-rate %d/%d fps; pipeline rebuilt", enc_fps, target);
    return 1;
}

void hdmi_watch_sleep(Source *source, std::chrono::milliseconds total)
{
    auto remaining = total;
    while (remaining > std::chrono::milliseconds::zero() &&
           !source->hdmi_watch_stop.load(std::memory_order_relaxed)) {
        const auto step = remaining < kHDMIChangeGrowProbeInterval
            ? remaining : kHDMIChangeGrowProbeInterval;
        std::this_thread::sleep_for(step);
        remaining -= step;
    }
}

void hdmi_watch_loop(Source *source) {
    while (!source->hdmi_watch_stop.load(std::memory_order_relaxed)) {
        const uint64_t packed = source->cached_input_size.load(
            std::memory_order_acquire);
        const onekvm::InputResolution current{
            static_cast<uint32_t>(packed >> 32),
            static_cast<uint32_t>(packed & 0xffffffffu),
        };
        hdmi_watch_sleep(source, onekvm::hdmi_watch_interval(
            source->cached_signal.load(std::memory_order_relaxed),
            current,
            source->capture_live.load(std::memory_order_relaxed)));
        if (source->hdmi_watch_stop.load(std::memory_order_relaxed))
            break;

        /* At 1080p and above there is no receiver-grow transition to catch.
           Once a bound encoder has proved the path live, stay completely off
           LT6911: even a 1 Hz 80ee access eventually stalls CSI. Idle probes
           CSI I2C instead of /proc/cvitek/vi. A bound encoder detects a real
           stop and changes cached_signal before asking for recovery. */
        const int cached_signal = source->cached_signal.load(
            std::memory_order_relaxed);
        if (!onekvm::hdmi_watch_probe_due(
                cached_signal, current,
                source->capture_live.load(std::memory_order_relaxed)))
            continue;

        char error[256];
        std::lock_guard<std::mutex> lock(source->mutex);
        if (source->hdmi_watch_stop.load(std::memory_order_relaxed))
            continue;
        if (!source->initialized) {
            const auto now = std::chrono::steady_clock::now();
            if (source->last_recovery.time_since_epoch().count() != 0 &&
                now - source->last_recovery < kHDMIChangeIdleWindow)
                continue;
            source->last_recovery = now;
            if (recover_source(source, error, sizeof(error)) != 0) {
                std::fprintf(stderr,
                             "OneKVM: HDMI watch recover failed: %s\n",
                             error);
            } else if (source->initialized) {
                source->pending_receiver = {};
            }
            continue;
        }
        /* Idle CSI probes belong here. maybe_rebuild also reads LT6911;
           do not fopen /proc/cvitek/vi in the same tick. */
        const bool need_fast =
            !onekvm::supported_input_resolution(
                source->input_resolution.current()) &&
            source->cached_signal.load(std::memory_order_relaxed) != 1;
        if (need_fast)
            (void)maybe_rebuild_for_hdmi_change_now(
                source, error, sizeof(error));
        else
            (void)maybe_rebuild_for_hdmi_change(
                source, error, sizeof(error));
    }
}

void stop_hdmi_watch(Source *source) {
    if (source == nullptr)
        return;
    source->hdmi_watch_stop.store(true, std::memory_order_relaxed);
    if (source->hdmi_watch.joinable())
        source->hdmi_watch.join();
}

void start_hdmi_watch(Source *source) {
    if (source == nullptr)
        return;
    stop_hdmi_watch(source);
    source->hdmi_watch_stop.store(false, std::memory_order_relaxed);
    source->hdmi_watch = std::thread(hdmi_watch_loop, source);
}

int32_t source_create(const onekvm_video_source_config_v1 *config, void **result,
                      char *error, uint32_t error_capacity) {
    if (result == nullptr) {
        set_error(error, error_capacity, "source output is null");
        return -1;
    }
    *result = nullptr;
    Source *source = new (std::nothrow) Source();
    if (source == nullptr) {
        set_error(error, error_capacity, "allocate source: out of memory");
        return -1;
    }
    (void)restore_active_edid_if_needed();
    if (open_source(source, config, error, error_capacity) != 0) {
        delete source;
        return -1;
    }
    try {
        start_hdmi_watch(source);
    } catch (const std::exception &ex) {
        std::fprintf(stderr, "OneKVM: HDMI watch thread failed: %s\n",
                     ex.what());
    }
    *result = source;
    return 0;
}

int32_t source_reset(void *opaque, const onekvm_video_source_config_v1 *config,
                     char *error, uint32_t error_capacity) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || config == nullptr || config->struct_size < sizeof(*config)) {
        set_error(error, error_capacity, "invalid source reset");
        return -1;
    }
    std::lock_guard<std::mutex> lock(source->mutex);
    if (!source->initialized) {
        return open_source(source, config, error, error_capacity);
    }
    release_source_frame(source);
    const auto output = onekvm::target_output_resolution(
        config->resolution, source->input_resolution.current());
    const int width = static_cast<int>(output.width);
    const int height = static_cast<int>(output.height);
    const int fps = onekvm::mmf::clamp_pipeline_fps(
        config->fps > 0 ? static_cast<int>(config->fps) : 60,
        static_cast<int>(source->input_resolution.current().width),
        static_cast<int>(source->input_resolution.current().height),
        width, height);
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    int result = mmf::reset_capture_channel(
        source->channel, width, height, kMMFNV21, fps);
    if (result != 0) {
        set_error(error, error_capacity, "reset MMF capture channel failed: %d", result);
        return -1;
    }
    source->channel = onekvm::mmf::vpss_phy_channel(width);
    source->config = *config;
    source->config.device = nullptr;
    source->capture_width = width;
    source->capture_height = height;
    source->failures = 0;
    source->last_recovery = {};
    source->no_signal_frame.clear();
    publish_input_format(source);
    reset_signal_cache(source);
    return 0;
}

int ensure_no_signal_nv21(Source *source, int width, int height,
                          char *error, uint32_t error_capacity) {
    size_t bytes = 0;
    if (source == nullptr || !nv21_size(width, height, &bytes)) {
        set_error(error, error_capacity, "invalid no-signal frame size %dx%d",
                  width, height);
        return -1;
    }
    if (source->no_signal_frame.size() == bytes &&
        source->no_signal_width == width && source->no_signal_height == height)
        return 0;
    try {
        source->no_signal_frame.resize(bytes);
    } catch (const std::bad_alloc &) {
        set_error(error, error_capacity, "allocate no-signal frame: out of memory");
        return -1;
    }
    int written = render_no_signal_nv21(source->no_signal_frame.data(),
                                        static_cast<int>(bytes), width, height);
    if (written != static_cast<int>(bytes)) {
        uint8_t *plane = source->no_signal_frame.data();
        const size_t luma = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::memset(plane, 16, luma);
        std::memset(plane + luma, 128, bytes - luma);
    }
    source->no_signal_width = width;
    source->no_signal_height = height;
    return 0;
}

int no_signal_frame(Source *source, onekvm_video_frame_v1 *frame,
                    char *error, uint32_t error_capacity) {
    const auto [width, height] = source_output_size(source);
    if (ensure_no_signal_nv21(source, width, height, error, error_capacity) != 0)
        return -1;
    frame->data = source->no_signal_frame.data();
    frame->data_size = source->no_signal_frame.size();
    frame->width = width;
    frame->height = height;
    frame->pixel_format = ONEKVM_VIDEO_PIXEL_NV21;
    frame->pts_ns = monotonic_ns();
    frame->token = 0;
    return 0;
}

int32_t source_read(void *opaque, onekvm_video_frame_v1 *frame,
                    char *error, uint32_t error_capacity) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || frame == nullptr || frame->struct_size < sizeof(*frame)) {
        set_error(error, error_capacity, "invalid source read");
        return -1;
    }
    std::lock_guard<std::mutex> lock(source->mutex);
    if (!source->initialized) {
        set_error(error, error_capacity, "source is not initialized");
        return -1;
    }
    if (source->frame_pending) {
        set_error(error, error_capacity, "previous source frame was not released");
        return -1;
    }

    void *data = nullptr;
    int length = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    const uint64_t capture_start_ns = monotonic_ns();
    int result = mmf::acquire_capture_frame(source->channel, &data, &length, &width, &height, &format);
    if (result == 0 && data != nullptr && length > 0) {
        const uint64_t capture_ns = monotonic_ns() - capture_start_ns;
        if (capture_ns < 1000000000ull)
            source->last_capture_ns.store(capture_ns, std::memory_order_relaxed);
    }
    if (result != 0 || data == nullptr || length <= 0) {
        if (result == 0) {
            mmf::release_capture_frame(source->channel);
        }
        source->failures++;
        const int rebuilt = maybe_rebuild_for_hdmi_change(
            source, error, error_capacity);
        if (rebuilt != 0)
            return -1;
        if (source->out_of_range.load(std::memory_order_relaxed) ||
            cached_signal_present(source) == 0)
            return no_signal_frame(source, frame, error, error_capacity);
        const auto now = std::chrono::steady_clock::now();
        const bool due = source->failures >= kRecoveryFailureThreshold &&
            (source->last_recovery.time_since_epoch().count() == 0 ||
             now - source->last_recovery >= kRecoveryInterval);
        if (due) {
            source->failures = 0;
            source->last_recovery = now;
            const auto [reset_width, reset_height] = source_output_size(source);
            const int reset_fps = source->config.fps > 0
                ? static_cast<int>(source->config.fps) : 60;
            int reset = mmf::reset_capture_channel(
                source->channel, reset_width, reset_height, kMMFNV21, reset_fps);
            source->channel = onekvm::mmf::vpss_phy_channel(reset_width);
            set_error(error, error_capacity, "MMF VI read failed; channel reset returned %d", reset);
        } else {
            set_error(error, error_capacity, "MMF VI read failed: %d", result);
        }
        return -1;
    }

    source->failures = 0;
    source->last_frame_ns.store(monotonic_ns(), std::memory_order_relaxed);
    source->cached_signal.store(1, std::memory_order_relaxed);
    source->frame_pending = true;
    source->frame_token++;
    if (source->frame_token == 0) {
        source->frame_token++;
    }
    frame->data = static_cast<const uint8_t *>(data);
    frame->data_size = static_cast<uint64_t>(length);
    frame->width = width;
    frame->height = height;
    frame->pixel_format = ONEKVM_VIDEO_PIXEL_NV21;
    frame->pts_ns = monotonic_ns();
    frame->token = source->frame_token;
    return 0;
}

void source_release(void *opaque, uint64_t token) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || token == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(source->mutex);
    if (source->frame_pending && source->frame_token == token) {
        release_source_frame(source);
    }
}

int32_t source_signal_present(void *opaque) {
    auto *source = static_cast<Source *>(opaque);
    if (source != nullptr && source->out_of_range.load(std::memory_order_relaxed))
        return 1;
    return cached_signal_present(source) > 0 ? 1 : 0;
}

int32_t source_latency(void *opaque, onekvm_video_latency_v1 *latency) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || latency == nullptr ||
        latency->struct_size < sizeof(*latency)) {
        return -1;
    }
    latency->capture_ns = source->last_capture_ns.load(std::memory_order_relaxed);
    latency->encode_ns = 0;
    return 0;
}

int32_t source_input_format(void *opaque, onekvm_video_format_v1 *format) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || format == nullptr ||
        format->struct_size < ONEKVM_VIDEO_FORMAT_V1_BASE_SIZE) {
        return -1;
    }
    const uint64_t packed =
        source->cached_input_size.load(std::memory_order_acquire);
    const auto width = static_cast<int32_t>(packed >> 32);
    const auto height = static_cast<int32_t>(packed & 0xffffffffu);
    if (width <= 0 || height <= 0)
        return -1;
    format->width = width;
    format->height = height;
    format->fps = source->cached_input_fps.load(std::memory_order_relaxed);
    format->pixel_format = ONEKVM_VIDEO_PIXEL_UNKNOWN;
    if (format->struct_size >= offsetof(onekvm_video_format_v1, flags) +
            sizeof(format->flags)) {
        format->flags = source->out_of_range.load(std::memory_order_relaxed)
            ? ONEKVM_VIDEO_FORMAT_OUT_OF_RANGE : 0;
    }
    return 0;
}

void source_destroy(void *opaque) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr) {
        return;
    }
    stop_hdmi_watch(source);
    {
        std::lock_guard<std::mutex> lock(source->mutex);
        close_source(source);
    }
    delete source;
}


} // namespace onekvm::video_backend
#pragma GCC visibility pop
