#include "onekvm_video_backend_internal.hpp"
#include "lt6911_edid.hpp"

#include <exception>

#pragma GCC visibility push(hidden)
namespace onekvm::video_backend {

/* All VI/VPSS/VENC objects share one vendor MMF context. Resolution changes
 * tear that context down, so serialize every MMF operation across sources and
 * encoders. Recursive locking keeps the existing small helper boundaries. */
std::recursive_mutex g_mmf_mutex;
std::recursive_mutex g_mmf_transaction_mutex;
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
                      onekvm::InputResolution *hdmi,
                      bool silent = false,
                      bool accept_early_rx = false,
                      bool *receiver_locked = nullptr) {
    onekvm_lt6911_input_timing timing{};
    if (csi == nullptr || hdmi == nullptr)
        return false;
    const int result = silent
        ? lt6911_get_input_timing_silent(0, &timing)
        : pcie_hdmi_variant()
            ? lt6911_get_input_timing_pcie(0, &timing)
            : lt6911_get_input_timing(0, &timing);
    if (result != 0)
        return false;
    if (receiver_locked != nullptr)
        *receiver_locked = timing.uxc_rx_signal == 0x55;
    *csi = {timing.csi_width, timing.csi_height};
    *hdmi = {timing.hdmi_width, timing.hdmi_height};
    const onekvm::InputResolution rx{timing.uxc_rx_width,
                                     timing.uxc_rx_height};
    if (!silent && (timing.uxc_rx_signal == 0x55 || accept_early_rx) &&
        onekvm::supported_input_resolution(rx))
        *hdmi = rx;
    return true;
}

bool wait_for_pcie_timing(onekvm::InputResolution previous, int fps,
                          bool require_change,
                          std::chrono::steady_clock::time_point deadline,
                          std::chrono::milliseconds interval,
                          onekvm::InputResolution *input,
                          onekvm::InputResolution *last_csi,
                          onekvm::InputResolution *last_hdmi,
                          std::chrono::milliseconds same_mode_delay =
                              std::chrono::milliseconds::zero()) {
    const auto started = std::chrono::steady_clock::now();
    onekvm::InputResolution candidate{};
    unsigned samples = 0;
    bool old_timing_departed = false;
    do {
        /* 139 queries the HDMI receiver before restarting CIF. On PCIe the
           receiver's active size appears before CSI and before 0x86a3 says
           locked. Only use that early size while VI is stopped, and require
           two matching snapshots before arming the new receiver. */
        bool receiver_locked = false;
        if (read_hdmi_timing(last_csi, last_hdmi, false, true,
                             &receiver_locked)) {
            if (!receiver_locked || *last_hdmi != previous)
                old_timing_departed = true;
            const auto locked =
                onekvm::supported_input_rate(*last_hdmi, fps) &&
                *last_hdmi != previous
                    ? *last_hdmi
                    : onekvm::csi_reported_size(*last_csi, *last_hdmi);
            if (onekvm::supported_input_rate(locked, fps) &&
                (!require_change || locked != previous ||
                 (old_timing_departed && receiver_locked &&
                  same_mode_delay != std::chrono::milliseconds::zero() &&
                  std::chrono::steady_clock::now() - started >= same_mode_delay))) {
                samples = locked == candidate ? samples + 1 : 1;
                candidate = locked;
                if (samples >= 2) {
                    *input = locked;
                    return true;
                }
            } else {
                candidate = {};
                samples = 0;
            }
        } else {
            candidate = {};
            samples = 0;
        }
        std::this_thread::sleep_for(interval);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

bool pcie_hdmi_blank_after_stall(Source *source) {
    if (source == nullptr || !pcie_hdmi_variant())
        return false;
    onekvm::InputResolution csi{};
    onekvm::InputResolution hdmi{};
    if (!read_hdmi_timing(&csi, &hdmi, true)) {
        source->early_blank_samples = 0;
        return false;
    }
    if (csi.width != 0 || csi.height != 0 ||
        hdmi.width != 0 || hdmi.height != 0) {
        source->early_blank_samples = 0;
        return false;
    }
    return ++source->early_blank_samples >= 2;
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

constexpr const char *kLastHdmiInputPath = "/run/onekvm/last-hdmi-input";

onekvm::InputResolution recalled_hdmi_input() {
    std::FILE *file = std::fopen(kLastHdmiInputPath, "r");
    if (file == nullptr)
        return {};
    unsigned width = 0;
    unsigned height = 0;
    const int matched = std::fscanf(file, "%u %u", &width, &height);
    std::fclose(file);
    if (matched != 2)
        return {};
    const onekvm::InputResolution value{width, height};
    if (!onekvm::supported_input_resolution(value) ||
        onekvm::is_pcie_bootstrap_resolution(value))
        return {};
    return value;
}

void remember_hdmi_input(onekvm::InputResolution value) {
    if (!onekvm::supported_input_resolution(value) ||
        onekvm::is_pcie_bootstrap_resolution(value))
        return;
    std::FILE *file = std::fopen(kLastHdmiInputPath, "w");
    if (file == nullptr)
        return;
    std::fprintf(file, "%u %u\n", value.width, value.height);
    std::fclose(file);
}

onekvm::InputResolution initial_input_resolution(int fps) {
    const bool pcie = probe_edid_board().board == NanoKVMBoard::PCIe;
    const auto deadline = std::chrono::steady_clock::now() +
        kInitialResolutionProbeWindow;
    onekvm::InputResolution previous{};
    onekvm::InputResolution best{};
    unsigned stable_samples = 0;
    do {
        onekvm::InputResolution sample{};
        if (read_supported_input_resolution(&sample, fps)) {
            if (onekvm::resolution_pixels(sample) >
                onekvm::resolution_pixels(best))
                best = sample;
            if (sample == previous) {
                if (++stable_samples >= 2)
                    return sample;
            } else {
                previous = sample;
                stable_samples = 1;
            }
        } else {
            previous = {};
            stable_samples = 0;
        }
        std::this_thread::sleep_for(kInitialResolutionSampleDelay);
    } while (std::chrono::steady_clock::now() < deadline);

    const onekvm::InputResolution stable =
        (previous.width != 0 && stable_samples >= 2) ? previous
                                                     : onekvm::InputResolution{};
    /* The PCIe fixture is commonly fed by a splitter while the source is
       still in BIOS.  That source mode is 640x480; LT6911 can report zero
       counters until its CSI receiver is armed with the same geometry.  A
       1920x1080 fallback therefore leaves this cold-start case in a circular
       no-frame state.  Prefer a HDMI reading from this probe, then the last
       live mode from this boot, then 640x480.  Cube/Lite still default to
       1080p.  If a real timing appears later, the HDMI watcher rebuilds. */
    return onekvm::pick_initial_vi_resolution(
        pcie, stable, best, recalled_hdmi_input());
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
    /* Live HDMI is VENC packets, not VI IntCnt. After a host mode change
       the encoder can keep repeating the old geometry; the packet path
       marks cached_signal=0 and the watcher then probes CSI. Do not turn a
       status query into an idle LT6911 I2C transaction: the receiver remains
       armed while VPSS is parked, and low-clock CSI can stop on that read. */
    return source->cached_signal.load(std::memory_order_relaxed);
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
    remember_hdmi_input(input);
}

void reset_signal_cache(Source *source) {
    source->early_blank_samples = 0;
    source->last_frame_ns.store(0, std::memory_order_relaxed);
    source->last_signal_probe_ns.store(0, std::memory_order_relaxed);
    source->cached_signal.store(
        source->awaiting_hdmi_timing.load(std::memory_order_relaxed) ? 0 : -1,
        std::memory_order_relaxed);
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
    MmfControlLock global_lock;
    source->frame_release_pending = true;
    if (mmf::release_capture_frame(source->channel) == 0) {
        source->frame_pending = false;
        source->frame_release_pending = false;
    }
}

void retry_source_frame_release(Source *source) {
    if (source->frame_release_pending)
        release_source_frame(source);
}

int close_source(Source *source) {
    if (!source->initialized) {
        return 0;
    }
    MmfControlLock global_lock;
    release_source_frame(source);
    /* mmf::shutdown() owns the complete dependency-ordered teardown: bound VENC
       consumers are stopped before their VPSS/VI producers.  Removing the VI
       channel here first leaves the VENC worker running without userspace
       draining its stream, which can fill the 12-pack queue and drop the
       cached SPS/PPS needed by reconnecting H.264 decoders.  It also made the
       later global teardown destroy the same VI resources twice. */
    if (mmf::shutdown() != 0)
        return -1;
    g_mmf_generation.fetch_add(1, std::memory_order_acq_rel);
    source->channel = -1;
    source->frame_pending = false;
    source->frame_release_pending = false;
    source->initialized = false;
    source->capture_width = 0;
    source->capture_height = 0;
    source->capture_live.store(false, std::memory_order_relaxed);
    source->preserved_locked_csi = false;
    source->no_signal_frame.clear();
    return 0;
}

int open_source(Source *source, const onekvm_video_source_config_v1 *config,
                char *error, uint32_t error_capacity,
                const onekvm::InputResolution *requested_input = nullptr,
                bool skip_pcie_startup_pulse = false,
                bool force_software_rearm = false,
                const onekvm::InputResolution *placeholder_output = nullptr,
                bool reuse_measured_timing = false) {
    if (config == nullptr || config->struct_size < sizeof(*config)) {
        set_error(error, error_capacity, "invalid source configuration");
        return -1;
    }
    MmfControlLock global_lock;
    const int requested_fps = config->fps > 0 ? static_cast<int>(config->fps) : 60;
    onekvm::InputResolution locked_csi{};
    onekvm::InputResolution locked_hdmi{};
    onekvm::InputResolution locked_receiver{};
    bool preserve_locked_csi = false;
    /* Snapshot an already locked bridge before reclaiming a previous SoC VI
       owner.  This is read-only and happens after that process has exited;
       DestroyVi cannot erase the LT6911's valid 1080p geometry afterwards. */
    if (!force_software_rearm && pcie_hdmi_variant() &&
        read_hdmi_timing(&locked_csi, &locked_hdmi)) {
        /* On LT6911UXC the 0x85 active-size counters are returned through
           locked_hdmi; locked_csi is the LT6911C 0xc2 register family and
           remains zero. Select the valid receiver geometry across both
           families before deciding whether the bridge is already locked. */
        locked_receiver =
            onekvm::csi_reported_size(locked_csi, locked_hdmi);
        preserve_locked_csi =
            onekvm::supported_input_rate(locked_receiver, requested_fps);
    }
    /* The old VI generation must be gone before LT6911 is reprogrammed.  VI
       startup itself happens later, after the bridge has been armed. */
    if (mmf::reclaim_stale_runtime() != 0) {
        set_error(error, error_capacity, "reclaim stale MMF runtime failed");
        return -1;
    }
    if (preserve_locked_csi && requested_input != nullptr &&
        *requested_input != locked_receiver)
        preserve_locked_csi = false;
    if (!preserve_locked_csi) {
        /* Recovery first rearms the bridge in software. Neither that path
           nor a full GPIO reset should get an extra startup pulse here. */
        if (!skip_pcie_startup_pulse && pcie_hdmi_startup_reset() != 0) {
            set_error(error, error_capacity, "reset PCIe HDMI bridge failed: %s",
                      std::strerror(errno));
            return -1;
        }
        /* Cold start needs D283. A passive mode switch already has two
           matching receiver timings, so remeasuring would delay CSI. */
        if (!reuse_measured_timing)
            (void)lt6911_kick_hdmi();
    } else {
        std::fprintf(stderr,
                     "OneKVM: preserving locked LT6911 CSI %ux%u\n",
                     locked_receiver.width, locked_receiver.height);
    }
    const onekvm::InputResolution input = requested_input != nullptr &&
        onekvm::supported_input_rate(*requested_input, requested_fps)
        ? *requested_input
        : preserve_locked_csi ? locked_receiver
        : initial_input_resolution(requested_fps);
    const auto output = config->resolution == 0 && placeholder_output != nullptr &&
            onekvm::supported_output_resolution(*placeholder_output)
        ? *placeholder_output
        : onekvm::target_output_resolution(config->resolution, input);
    const int width = static_cast<int>(output.width);
    const int height = static_cast<int>(output.height);
    std::fprintf(stderr, "OneKVM: configuring HDMI input %ux%u, output %dx%d\n",
                 input.width, input.height, width, height);
    if (onekvm_lt6911_set_active_size(input.width, input.height) != 0) {
        set_error(error, error_capacity, "invalid LT6911 input size %ux%u",
                  input.width, input.height);
        return -1;
    }
    /* Arm the bridge after old-owner cleanup and before SAMPLE_PLAT_VI_INIT.
       Starting VI first lets an already-running 1080p CSI stream hit a 640p
       CSIBDG bootstrap and permanently trips the frontend width checker. */
    if (!preserve_locked_csi &&
        (reuse_measured_timing ? lt6911_start_csi_with_cached_timing()
                               : lt6911_start_csi()) != 0) {
        set_error(error, error_capacity, "LT6911 CSI arm before VI failed");
        return -1;
    }
    if (mmf::initialize() != 0) {
        set_error(error, error_capacity, "initialize failed");
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
    source->preserved_locked_csi = preserve_locked_csi;
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
    source->hdmi_blank_rearm_samples = 0;
    source->csi_half_rate_samples = 0;
    source->last_half_rate_check_ns = 0;
    source->out_of_range.store(false, std::memory_order_relaxed);
    /* input was selected before lt6911_start_csi()/StartViChn. Do not
       re-read LT6911 here: opening 80ee after CSI is armed can stop a valid
       low-clock stream before the first consumer has a chance to prove it
       through VENC packets. Later staleness-driven recovery publishes a new
       geometry or out-of-range state when the source actually changes. */
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
                            char *error, uint32_t error_capacity,
                            bool force_pcie_reset = false,
                            bool blank_mode_switch = false,
                            unsigned blank_reset_count = 0,
                            bool *did_pcie_reset = nullptr) {
    const onekvm_video_source_config_v1 config = source->config;
    const auto previous_input = source->input_resolution.current();
    MmfControlLock global_lock;
    bool fresh_timing = false;
    bool source_closed = false;
    const bool full_pcie_reset = force_pcie_reset && blank_reset_count > 0;
    const onekvm::InputResolution held_output{
        static_cast<uint32_t>(std::max(0, source->capture_width)),
        static_cast<uint32_t>(std::max(0, source->capture_height))};
    onekvm::InputResolution csi{};
    onekvm::InputResolution hdmi{};
    const int fps = config.fps > 0 ? static_cast<int>(config.fps) : 60;
    /* Stop the old capture before querying the new HDMI timing. */
    const bool passive_probe = force_pcie_reset && blank_mode_switch &&
        !full_pcie_reset && !source->hdmi_blank_rearm_attempted;
    if (passive_probe) {
        const auto started = std::chrono::steady_clock::now();
        if (close_source(source) != 0) {
            set_error(error, error_capacity, "stop old MMF source failed");
            return -1;
        }
        source_closed = true;
        /* Match 139's restart path: compare new timings with the geometry
           actually used by the old capture, not the watcher's next guess. */
        fresh_timing = wait_for_pcie_timing(
            previous_input, fps, true,
            std::chrono::steady_clock::now() + kPcieHDMIPassiveProbeWindow,
            std::chrono::milliseconds(100), &input, &csi, &hdmi,
            std::chrono::milliseconds(1200));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr,
                     "OneKVM: PCIe passive timing CSI %ux%u HDMI %ux%u fresh=%d elapsed=%lldms\n",
                     csi.width, csi.height, hdmi.width, hdmi.height,
                     fresh_timing ? 1 : 0, static_cast<long long>(elapsed));
    }
    /* Keep the last output and wait for a source to return. Resetting a
       bridge while HDMI is genuinely absent just replaces the placeholder
       with a 640x480 probe and delays the next lock. */
    const bool wait_for_signal = passive_probe &&
        !fresh_timing && csi.width == 0 && csi.height == 0 &&
        hdmi.width == 0 && hdmi.height == 0;
    /* A blank HDMI input can need an HPD reset before the receiver will
       publish the new BIOS/OS timing. Only reset after passive recovery
       failed, or on a later retry. */
    if (force_pcie_reset && !fresh_timing && !wait_for_signal &&
        (full_pcie_reset ? pcie_hdmi_reset() : pcie_hdmi_startup_reset()) != 0) {
        const int reset_errno = errno;
        if (source_closed) {
            source->pending_receiver = input;
            if (open_source(source, &config, error, error_capacity, &input,
                            true, false, &held_output) == 0)
                source->pending_receiver = {};
        }
        set_error(error, error_capacity, "reset PCIe HDMI bridge failed: %s",
                  std::strerror(reset_errno));
        return -1;
    }
    if (force_pcie_reset && !fresh_timing && !wait_for_signal &&
        did_pcie_reset != nullptr)
        *did_pcie_reset = true;
    if (force_pcie_reset && !fresh_timing && !wait_for_signal)
        std::fprintf(stderr, "OneKVM: PCIe HDMI %s reset before CSI rearm\n",
                     full_pcie_reset ? "full" : "short");
    if (force_pcie_reset && !fresh_timing && !wait_for_signal) {
        const auto reset_done = std::chrono::steady_clock::now();
        const auto deadline = reset_done + kPcieHDMIResetProbeWindow;
        std::this_thread::sleep_for(kPcieHDMIResetEarliestProbe);
        /* The read-only counters remain 0x0 when the placeholder has
           detached VI.  The receiver is already being reset here, so a
           gated timing snapshot cannot interrupt a live capture. */
        fresh_timing = wait_for_pcie_timing(
            previous_input, fps, blank_mode_switch, deadline,
            std::chrono::milliseconds(200), &input, &csi, &hdmi);
        const auto probe_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - reset_done).count();
        std::fprintf(stderr, "OneKVM: PCIe reset timing CSI %ux%u HDMI %ux%u fresh=%d probe=%lldms\n",
                     csi.width, csi.height, hdmi.width, hdmi.height,
                     fresh_timing ? 1 : 0, static_cast<long long>(probe_ms));
        /* The old 1080 counters can survive a reset while BIOS has already
           switched modes. Try the common VGA and 720x400 BIOS geometries on
           successive blank retries; then revisit the previous receiver.
           A fresh, different timing always wins over this fallback. */
        if (blank_mode_switch && !fresh_timing) {
            switch (blank_reset_count % 3) {
            case 0: input = {640, 480}; break;
            case 1: input = {720, 400}; break;
            default: break;
            }
        }
    }
    /* A fallback receiver probes BIOS geometries while HDMI is absent. Keep
       the last encoded size for its placeholder; otherwise every retry
       changes the WebRTC video dimensions even though the image is static. */
    const auto *placeholder_output = blank_mode_switch && !fresh_timing &&
            onekvm::supported_output_resolution(held_output)
        ? &held_output : nullptr;
    source->pending_receiver = input;
    if (!source_closed && close_source(source) != 0) {
        set_error(error, error_capacity, "stop old MMF source failed");
        return -1;
    }
    /* A reappearing HDMI timing may have the same geometry as the one that
       stalled. Its active-size counter alone does not prove CSI is armed. */
    const bool rearm_same_mode = force_pcie_reset && fresh_timing &&
        input == previous_input;
    if (open_source(source, &config, error, error_capacity, &input,
                    true, !force_pcie_reset || rearm_same_mode || wait_for_signal,
                    placeholder_output, passive_probe && fresh_timing) != 0)
        return -1;
    source->pending_receiver = {};
    source->hdmi_waiting_for_signal = wait_for_signal;
    /* The silent LT6911 counters can stay zero until VI is consuming CSI.
       Give the rebuilt receiver one first-AU window before returning to
       placeholder. awaiting_hdmi_timing still rejects old encoded packets. */
    if (source->awaiting_hdmi_timing.load(std::memory_order_relaxed))
        source->cached_signal.store(1, std::memory_order_relaxed);
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
    const bool awaiting_live_au = source->awaiting_hdmi_timing.load(
        std::memory_order_relaxed);
    const bool consumer_waiting = capture_live || awaiting_live_au ||
        source->hdmi_waiting_for_signal;
    const bool recent_frames = capture_live && !awaiting_live_au &&
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
    /* After a true no-signal interval the UXC active-size bank can remain
       zero in silent reads even after HDMI returns. The placeholder has
       detached VI, so a gated snapshot is safe until capture resumes. */
    if (!read_hdmi_timing(&csi, &hdmi,
                          !source->hdmi_waiting_for_signal))
        return 0;

    hdmi = onekvm::infer_hdmi_mode(hdmi);
    if (source->awaiting_hdmi_timing.load(std::memory_order_relaxed)) {
        if (onekvm::supported_input_resolution(hdmi)) {
            if (hdmi == source->hdmi_resume_candidate)
                source->hdmi_resume_samples++;
            else {
                source->hdmi_resume_candidate = hdmi;
                source->hdmi_resume_samples = 1;
            }
            if (source->hdmi_resume_samples >= 2) {
                /* This only starts a live capture trial. The packet path
                   alone can confirm fresh frames and release the IDR. */
                source->cached_signal.store(1, std::memory_order_relaxed);
                std::fprintf(stderr, "OneKVM: HDMI signal confirmed %ux%u\n",
                             hdmi.width, hdmi.height);
            }
        } else {
            source->hdmi_resume_candidate = {};
            source->hdmi_resume_samples = 0;
            source->cached_signal.store(0, std::memory_order_relaxed);
        }
    } else if (!capture_live)
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
    /* Blank counters alone cannot justify replacing a geometry obtained
       after a full bridge reset. The LT6911UXC reports 0x0 when VI is
       detached for the placeholder, even with a valid 640/720 input. */
    const bool allow_blank_bootstrap_grow =
        source->hdmi_blank_reset_count == 0 &&
        !source->hdmi_blank_rearm_attempted &&
        onekvm::is_pcie_bootstrap_resolution(current);
    if (!source->preserved_locked_csi &&
        (hdmi.width != 0 || allow_blank_bootstrap_grow ||
         !pcie_hdmi_variant()) &&
        onekvm::should_grow_to_max_vi_receiver(
            current, hdmi, source->hdmi_blanking_samples))
        receiver = onekvm::kMaxViReceiver;
    source->hdmi_follow_samples = onekvm::next_hdmi_follow_samples(
        recent_frames, current, hdmi, source->hdmi_follow_samples);
    /* CSI 0x0 while HDMI still reports the current 1080 is normal when
       VPSS is parked (idle) and after placeholder unbinds VI. Do not reopen. */
    source->hdmi_rearm_samples = onekvm::next_hdmi_csi_rearm_samples(
        recent_frames, csi, hdmi, current, source->hdmi_rearm_samples,
        capture_live && mmf::vi_dma_running());
    /* A live splitter source can leave LT6911 reporting 0x0 for both the
       upstream HDMI and downstream CSI counters. The geometry-based rearm
       gate cannot fire in that state. Poll a short window first: a normal
       mode switch will publish its new timing without an HPD reset. If the
       window stays blank, rearm in software first. Restrict this to a stale
       bound stream so an idle/no-source device does not rebuild forever. */
    const bool blank_live_loss = consumer_waiting &&
        source->cached_signal.load(std::memory_order_relaxed) == 0 &&
        csi.width == 0 && csi.height == 0 &&
        hdmi.width == 0 && hdmi.height == 0;
    source->hdmi_blank_rearm_samples =
        onekvm::next_hdmi_blank_rearm_samples(
            blank_live_loss, source->hdmi_blank_rearm_samples);
    /* A timing counter can briefly recover without producing an AU. Keep
       recovery state until an encoded packet proves the new lock. */
    /* A reset during a BIOS/OS mode transition can lock the bridge to the
       departing timing and still produce no live AU. Keep retrying at a
       bounded interval while a consumer is waiting, including if the silent
       counters remain 0x0 after the source returns. */
    const bool restored_timing_ready = consumer_waiting &&
        source->hdmi_waiting_for_signal &&
        onekvm::supported_input_rate(hdmi, configured_fps) &&
        source->last_recovery.time_since_epoch().count() != 0 &&
        now - source->last_recovery >= kHDMIFollowProbeInterval;
    const bool unproved_retry_ready = consumer_waiting && awaiting_live_au &&
        source->hdmi_blank_rearm_attempted &&
        !source->hdmi_waiting_for_signal &&
        source->last_recovery.time_since_epoch().count() != 0 &&
        now - source->last_recovery >= kHDMIBlankRetryInterval;
    const bool blank_rearm_ready = onekvm::hdmi_blank_rearm_ready(
        source->hdmi_blank_rearm_samples,
        source->hdmi_blank_rearm_attempted) || unproved_retry_ready;
    if (!onekvm::should_rebuild_vi_receiver(
            current, receiver, csi, hdmi, recent_frames)) {
        if (!onekvm::hdmi_csi_rearm_ready(source->hdmi_rearm_samples) &&
            !blank_rearm_ready && !restored_timing_ready) {
            if (kind == onekvm::HdmiInputClass::Supported)
                source->out_of_range.store(false, std::memory_order_relaxed);
            return 0;
        }
        if (!restored_timing_ready &&
            source->last_recovery.time_since_epoch().count() != 0 &&
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
    const bool force_pcie_reset = pcie_hdmi_variant() &&
        !restored_timing_ready &&
        receiver == current && blank_rearm_ready;
    std::fprintf(stderr,
                 "OneKVM: HDMI recovery blank=%u attempted=%d live=%d\n",
                 source->hdmi_blank_rearm_samples,
                 source->hdmi_blank_rearm_attempted ? 1 : 0,
                 capture_live ? 1 : 0);
    /* Growing the bootstrap receiver can coincide with the blank-reset
       threshold, but that rebuild does not pulse GPIO451. Mark a reset only
       when this reopen actually performs it. */
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
    bool did_pcie_reset = false;
    const int reopen = reopen_source_for_input(
        source, receiver, error, error_capacity, force_pcie_reset,
        blank_rearm_ready, source->hdmi_blank_reset_count, &did_pcie_reset);
    if (reopen != 0) {
        /* GPIO/reset failures leave the original source running. Retry on a
           later probe instead of marking a completed reset. */
        source->last_recovery = std::chrono::steady_clock::now();
        std::fprintf(stderr, "OneKVM: HDMI recovery failed: %s\n", error);
        return -1;
    }
    /* Track each blank recovery so persistent loss retries at a bounded
       interval. PCIe uses a short pulse first, then a full GPIO reset;
       Cube only rearms CSI in software. */
    if (blank_rearm_ready && receiver == current) {
        source->hdmi_blank_rearm_attempted = true;
        if (did_pcie_reset)
            source->hdmi_blank_reset_count++;
    }
    /* open_source clears last_recovery. Stamp after reopen so a stale
       I2C 1080 cannot HPD-loop every three watcher samples. */
    source->last_recovery = std::chrono::steady_clock::now();
    const auto recovered_input = source->input_resolution.current();
    set_error(error, error_capacity,
              "HDMI input changed to %ux%u; pipeline rebuilt",
              recovered_input.width, recovered_input.height);
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

void hdmi_watch_sleep(Source *source, std::chrono::milliseconds total,
                      int observed_signal)
{
    auto remaining = total;
    while (remaining > std::chrono::milliseconds::zero() &&
           !source->hdmi_watch_stop.load(std::memory_order_relaxed)) {
        const auto step = remaining < kHDMIChangeGrowProbeInterval
            ? remaining : kHDMIChangeGrowProbeInterval;
        std::this_thread::sleep_for(step);
        if (source->cached_signal.load(std::memory_order_relaxed) != observed_signal)
            return;
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
        const int observed_signal = source->cached_signal.load(
            std::memory_order_relaxed);
        const bool consumer_waiting = source->capture_live.load(
            std::memory_order_relaxed) || source->awaiting_hdmi_timing.load(
            std::memory_order_relaxed) || source->hdmi_waiting_for_signal.load(
            std::memory_order_relaxed);
        hdmi_watch_sleep(source, onekvm::hdmi_watch_interval(
            observed_signal,
            current,
            consumer_waiting),
            observed_signal);
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
                source->awaiting_hdmi_timing.load(std::memory_order_relaxed)
                    ? 0 : cached_signal, current,
                source->capture_live.load(std::memory_order_relaxed) ||
                source->awaiting_hdmi_timing.load(std::memory_order_relaxed) ||
                source->hdmi_waiting_for_signal.load(
                    std::memory_order_relaxed)))
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
        /* Only a bound consumer which has observed VENC staleness may reach
           the recovery probe. Idle polling can stop low-clock CSI before the
           first consumer arrives. */
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

void ensure_hdmi_watch(Source *source) {
    if (source == nullptr || source->hdmi_watch.joinable())
        return;
    source->hdmi_watch_stop.store(false, std::memory_order_relaxed);
    try {
        source->hdmi_watch = std::thread(hdmi_watch_loop, source);
    } catch (const std::exception &ex) {
        source->hdmi_watch_stop.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "OneKVM: HDMI watch thread failed: %s\n",
                     ex.what());
    } catch (...) {
        source->hdmi_watch_stop.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "OneKVM: HDMI watch thread failed\n");
    }
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
    /* EDID writes already program the bridge and persist the same bytes in
       /var/lib. Rewriting that file on every source creation performs two
       needless PCIe resets and can destroy a low-clock lock before VI opens. */
    if (open_source(source, config, error, error_capacity) != 0) {
        delete source;
        return -1;
    }
    ensure_hdmi_watch(source);
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
    MmfControlLock global_lock;
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
    /* Output scaling does not repair a stalled HDMI input. Keep the recovery
       cooldown so an unproved passive restart can still reach its fallback. */
    if (!source->awaiting_hdmi_timing.load(std::memory_order_relaxed))
        source->last_recovery = {};
    source->no_signal_frame.clear();
    publish_input_format(source);
    reset_signal_cache(source);
    return 0;
}

int ensure_no_signal_nv21(Source *source, int width, int height,
                          char *error, uint32_t error_capacity,
                          int art_width, int art_height) {
    size_t bytes = 0;
    if (source == nullptr || !nv21_size(width, height, &bytes)) {
        set_error(error, error_capacity, "invalid no-signal frame size %dx%d",
                  width, height);
        return -1;
    }
    if (art_width <= 0 || art_height <= 0) {
        art_width = width;
        art_height = height;
    }
    if (source->no_signal_frame.size() == bytes &&
        source->no_signal_width == width && source->no_signal_height == height &&
        source->no_signal_art_width == art_width &&
        source->no_signal_art_height == art_height)
        return 0;
    try {
        source->no_signal_frame.resize(bytes);
    } catch (const std::bad_alloc &) {
        set_error(error, error_capacity, "allocate no-signal frame: out of memory");
        return -1;
    }
    int written = render_no_signal_nv21_for_output(source->no_signal_frame.data(),
        static_cast<int>(bytes), width, height, art_width, art_height);
    if (written != static_cast<int>(bytes)) {
        uint8_t *plane = source->no_signal_frame.data();
        const size_t luma = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::memset(plane, 16, luma);
        std::memset(plane + luma, 128, bytes - luma);
    }
    source->no_signal_width = width;
    source->no_signal_height = height;
    source->no_signal_art_width = art_width;
    source->no_signal_art_height = art_height;
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
    retry_source_frame_release(source);
    if (source->frame_pending) {
        set_error(error, error_capacity, "previous source frame was not released");
        return -1;
    }

    void *data = nullptr;
    int length = 0;
    int width = 0;
    int height = 0;
    int format = 0;
    MmfControlLock global_lock;
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
