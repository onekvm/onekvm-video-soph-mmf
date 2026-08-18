#include "onekvm_video_backend_internal.hpp"

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
    switch (resolution) {
    case 720:
        return {1280, 720};
    case 480:
        return {640, 480};
    default:
        return {1920, 1080};
    }
}

bool read_vi_fps(Source *source, double *fps) {
    if (source == nullptr || fps == nullptr)
        return false;
    FILE *file = std::fopen(kVideoStatusPath, "r");
    if (file == nullptr) {
        return false;
    }
    char line[256];
    bool in_chn_status = false;
    bool found = false;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (parse_vi_chn_status_header(line)) {
            in_chn_status = true;
            continue;
        }
        ViChnStatus status{};
        if (in_chn_status && parse_vi_chn_status(line, &status)) {
            const int last = source->last_vi_int_cnt.exchange(
                status.int_cnt, std::memory_order_relaxed);
            if (!status.enabled)
                *fps = 0;
            else if (status.frame_rate > 0)
                *fps = static_cast<double>(status.frame_rate);
            else if (status.int_cnt > last)
                *fps = 60;
            else
                *fps = 0;
            found = true;
            break;
        }
        if (parse_vi_fps_line(line, fps)) {
            found = true;
            break;
        }
    }
    std::fclose(file);
    return found;
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

bool read_hdmi_input(onekvm::InputResolution *resolution) {
    uint32_t width = 0;
    uint32_t height = 0;
    if (resolution == nullptr || lt6911_get_input_size(0, &width, &height) != 0)
        return false;
    *resolution = {width, height};
    return true;
}

bool read_supported_input_resolution(onekvm::InputResolution *resolution) {
    onekvm::InputResolution value{};
    if (!read_hdmi_input(&value) ||
        !onekvm::supported_input_resolution(value))
        return false;
    *resolution = value;
    return true;
}

onekvm::InputResolution initial_input_resolution() {
    onekvm::InputResolution first{};
    onekvm::InputResolution second{};
    if (read_supported_input_resolution(&first)) {
        std::this_thread::sleep_for(kInitialResolutionSampleDelay);
        if (read_supported_input_resolution(&second) && first == second)
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

bool stable_input_resolution(onekvm::InputResolution *resolution) {
    onekvm::InputResolution value{};
    if (!stable_hdmi_input(&value) ||
        !onekvm::supported_input_resolution(value))
        return false;
    *resolution = value;
    return true;
}

int cached_signal_present(Source *source) {
    if (source == nullptr) return 0;
    const uint64_t now = monotonic_ns();
    /* Do not treat recent VENC packets as HDMI. After a host mode change
       the encoder can keep repeating the old geometry while VI IntCnt
       is already frozen. */

    const int cached = source->cached_signal.load(std::memory_order_relaxed);
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
    double fps = 0;
    const int present = read_vi_fps(source, &fps) ? (fps > 0 ? 1 : 0) : -1;
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
    const onekvm::InputResolution input = requested_input != nullptr &&
        onekvm::supported_input_resolution(*requested_input)
        ? *requested_input : initial_input_resolution();
    std::fprintf(stderr, "OneKVM: configuring HDMI input %ux%u\n",
                 input.width, input.height);
    if (onekvm_lt6911_set_active_size(input.width, input.height) != 0) {
        set_error(error, error_capacity, "invalid LT6911 input size %ux%u",
                  input.width, input.height);
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
    int channel = mmf::find_free_capture_channel();
    if (channel < 0) {
        mmf::stop_capture_pipeline();
        mmf::shutdown();
        set_error(error, error_capacity, "no free MMF VI channel");
        return -1;
    }
    const auto [width, height] = resolution_size(config->resolution);
    mmf::set_capture_mirror(channel, false);
    mmf::set_capture_flip(channel, false);
    const int fps = config->fps > 0 ? static_cast<int>(config->fps) : 60;
    int result = mmf::open_capture_channel(channel, width, height, kMMFNV21, fps);
    if (result != 0) {
        mmf::stop_capture_pipeline();
        mmf::shutdown();
        set_error(error, error_capacity, "open MMF capture channel failed: %d", result);
        return -1;
    }
    source->channel = channel;
    source->initialized = true;
    source->config = *config;
    source->config.device = nullptr;
    source->failures = 0;
    source->last_recovery = {};
    source->input_resolution.set_current(input);
    source->reported_input = input;
    source->out_of_range.store(false, std::memory_order_relaxed);
    onekvm::InputResolution probed{};
    if (read_hdmi_input(&probed)) {
        source->reported_input = probed;
        if (onekvm::classify_hdmi_input(probed) ==
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

int reopen_source_for_input(Source *source, onekvm::InputResolution input,
                            char *error, uint32_t error_capacity) {
    const onekvm_video_source_config_v1 config = source->config;
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    close_source(source);
    if (open_source(source, &config, error, error_capacity, &input) != 0)
        return -1;
    std::fprintf(stderr, "OneKVM: HDMI input resolution changed to %ux%u\n",
                 input.width, input.height);
    return 0;
}

int maybe_rebuild_for_hdmi_change(Source *source, char *error,
                                  uint32_t error_capacity) {
    if (source == nullptr)
        return 0;

    /* VENC last_frame_ns is not HDMI liveness. GetStream can stall while
       CSI is still running; probing LT6911 then interrupts a live frontend.
       After a host mode change IntCnt freezes, so a fresh VI sample is the
       only safe gate. */
    double fps = 0;
    const bool recent_frames = read_vi_fps(source, &fps) && fps > 0;
    source->cached_signal.store(recent_frames ? 1 : 0, std::memory_order_relaxed);
    source->last_signal_probe_ns.store(monotonic_ns(), std::memory_order_relaxed);

    const auto now = std::chrono::steady_clock::now();
    const bool interval_elapsed =
        source->last_hdmi_probe.time_since_epoch().count() == 0 ||
        now - source->last_hdmi_probe >= kHDMIChangeProbeInterval;
    if (!onekvm::hdmi_resolution_probe_due(
            static_cast<unsigned>(std::max(0, source->failures)),
            recent_frames, interval_elapsed))
        return 0;

    source->last_hdmi_probe = now;

    /* LT6911C internal-register access can interrupt live CSI output on
       NanoKVM Cube.  Probe only after VI/VENC has already gone idle. */
    onekvm::InputResolution observed{};
    if (!stable_hdmi_input(&observed))
        return 0;
    source->reported_input = observed;
    publish_input_format(source);

    const auto kind = onekvm::classify_hdmi_input(observed);
    if (kind == onekvm::HdmiInputClass::OutOfRange) {
        if (!source->out_of_range.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "OneKVM: HDMI input %ux%u is out of range\n",
                         observed.width, observed.height);
        }
        return 0;
    }
    if (kind != onekvm::HdmiInputClass::Supported)
        return 0;

    const auto current = source->input_resolution.current();
    const bool was_out_of_range =
        source->out_of_range.exchange(false, std::memory_order_relaxed);
    if (!was_out_of_range && observed == current)
        return 0;

    source->failures = 0;
    source->last_recovery = now;
    const int reopen = reopen_source_for_input(
        source, observed, error, error_capacity);
    if (reopen != 0)
        return -1;
    set_error(error, error_capacity,
              "HDMI input changed to %ux%u; pipeline rebuilt",
              observed.width, observed.height);
    return 1;
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
    if (open_source(source, config, error, error_capacity) != 0) {
        delete source;
        return -1;
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
    const auto [width, height] = resolution_size(config->resolution);
    const int fps = config->fps > 0 ? static_cast<int>(config->fps) : 60;
    std::lock_guard<std::recursive_mutex> global_lock(g_mmf_mutex);
    int result = mmf::reset_capture_channel(
        source->channel, width, height, kMMFNV21, fps);
    if (result != 0) {
        set_error(error, error_capacity, "reset MMF capture channel failed: %d", result);
        return -1;
    }
    source->config = *config;
    source->config.device = nullptr;
    source->failures = 0;
    source->last_recovery = {};
    source->no_signal_frame.clear();
    publish_input_format(source);
    reset_signal_cache(source);
    return 0;
}

int no_signal_frame(Source *source, onekvm_video_frame_v1 *frame,
                    char *error, uint32_t error_capacity) {
    const auto [width, height] = resolution_size(source->config.resolution);
    size_t bytes = 0;
    if (!nv21_size(width, height, &bytes)) {
        set_error(error, error_capacity, "invalid no-signal frame size %dx%d",
                  width, height);
        return -1;
    }
    if (source->no_signal_frame.size() != bytes ||
        source->no_signal_width != width || source->no_signal_height != height) {
        try {
            source->no_signal_frame.resize(bytes);
        } catch (const std::bad_alloc &) {
            set_error(error, error_capacity, "allocate no-signal frame: out of memory");
            return -1;
        }
        int written = render_no_signal_nv21(source->no_signal_frame.data(),
                                               static_cast<int>(bytes), width, height);
        if (written != static_cast<int>(bytes)) {
            set_error(error, error_capacity,
                      "render no-signal frame returned %d, want %zu", written, bytes);
            return -1;
        }
        source->no_signal_width = width;
        source->no_signal_height = height;
    }
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
    int result = mmf::acquire_capture_frame(source->channel, &data, &length, &width, &height, &format);
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
            const auto [reset_width, reset_height] = resolution_size(source->config.resolution);
            const int reset_fps = source->config.fps > 0
                ? static_cast<int>(source->config.fps) : 60;
            int reset = mmf::reset_capture_channel(
                source->channel, reset_width, reset_height, kMMFNV21, reset_fps);
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

int32_t source_input_format(void *opaque, onekvm_video_format_v1 *format) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr || format == nullptr ||
        format->struct_size < sizeof(*format)) {
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
    return 0;
}

void source_destroy(void *opaque) {
    auto *source = static_cast<Source *>(opaque);
    if (source == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(source->mutex);
        close_source(source);
    }
    delete source;
}


} // namespace onekvm::video_backend
#pragma GCC visibility pop
