#include "onekvm_video_backend_internal.hpp"
#include "lt6911_edid.hpp"

#include <cassert>
#include <cstdarg>
#include <string>
#include <vector>

using namespace onekvm::video_backend;
namespace {
bool supported = true, stopped = false;
int flash_error = 0, ram_error = 0, capture_error = 0;
std::vector<std::string> events;
EdidBoardInfo info{NanoKVMBoard::Cube, Lt6911Chip::C, "cube", "LT6911C", true};
void reset() {
    flash_error = ram_error = capture_error = 0;
    events.clear();
}
}
namespace onekvm::video_backend {
std::recursive_mutex g_mmf_mutex, g_mmf_transaction_mutex;
EdidBoardInfo probe_edid_board() { return info; }
bool lt6911_shadow_supported() { return supported; }
void set_error(char *error, uint32_t size, const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    if (error && size) vsnprintf(error, size, fmt, args);
    va_end(args);
}
int with_edid_capture_stopped(int (*operation)(void *), void *context, char *, uint32_t) {
    events.push_back("stop");
    if (capture_error == 1) return -1;
    stopped = true;
    MmfControlLock lock;
    int rc = operation(context);
    stopped = false;
    events.push_back("reopen");
    return capture_error == 2 ? -1 : rc;
}
int lt6911_edid_write(const uint8_t *, size_t) {
    if ((info.chip == Lt6911Chip::C && supported) || info.chip == Lt6911Chip::D) assert(stopped);
    events.push_back("flash"); return flash_error;
}
int lt6911_shadow_write(const uint8_t *, size_t) {
    assert(stopped); events.push_back("ram"); return ram_error;
}
int persist_active_edid(const uint8_t *, size_t) { events.push_back("persist"); return 0; }
int lt6911_edid_read(uint8_t *, size_t) { return 0; }
int pcie_hdmi_reset() { events.push_back("reset"); return 0; }
}

int main(int argc, char **argv) {
    const std::string profile = argc > 1 ? argv[1] : "cube";
    if (profile == "unverified") supported = false;
    if (profile == "pcie") info = {NanoKVMBoard::PCIe, Lt6911Chip::UXC, "pcie", "LT6911UXC", true};
    if (profile == "d-cube" || profile == "d-pcie") {
        supported = false;
        info = {profile == "d-cube" ? NanoKVMBoard::Cube : NanoKVMBoard::PCIe,
                Lt6911Chip::D, "test", "LT6911D", true};
    }
    onekvm_video_edid_caps_v1 caps{}; caps.struct_size = sizeof(caps);
    assert(edid_capabilities(&caps) == 0);
    assert(caps.apply_policy == (supported || info.board == NanoKVMBoard::PCIe
        ? ONEKVM_VIDEO_EDID_APPLY_HOTPLUG : ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE));
    onekvm_video_edid_blob_v1 data{}; data.struct_size = sizeof(data); data.size = 256;
    for (int i = 1; i < 7; ++i) data.data[i] = 255;
    data.data[126] = 1; data.data[127] = 5;
    onekvm_video_edid_apply_result_v1 apply{}; apply.struct_size = sizeof(apply);
    char error[256]{};
    auto put = [&](uint32_t mode) { return edid_set_mode(&data, mode, &apply, error, sizeof(error)); };
    assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) == 0);
    if (info.chip == Lt6911Chip::D) {
        assert(!(caps.flags & ONEKVM_VIDEO_EDID_CAP_TEMPORARY));
        if (profile == "d-cube") {
            assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE);
            assert((events == std::vector<std::string>{"stop", "flash", "persist", "reopen"}));
        } else {
            assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_HOTPLUG);
            assert((events == std::vector<std::string>{"stop", "reset", "flash", "persist", "reset", "reopen"}));
        }
        reset(); assert(put(ONEKVM_VIDEO_EDID_WRITE_TEMPORARY) != 0 && events.empty());
        reset(); flash_error = -1;
        assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) != 0);
        reset(); capture_error = 2;
        assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) == 0);
        assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE);
        return 0;
    }
    if (profile == "unverified") {
        assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE);
        assert((events == std::vector<std::string>{"flash", "persist"}));
        reset(); assert(put(ONEKVM_VIDEO_EDID_WRITE_TEMPORARY) != 0); assert(events.empty());
        return 0;
    }
    if (profile == "pcie") {
        assert((events == std::vector<std::string>{"reset", "flash", "persist", "reset"}));
        return 0;
    }
    assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_HOTPLUG);
    assert((events == std::vector<std::string>{"stop", "flash", "persist", "ram", "reopen"}));
    reset(); ram_error = -1;
    assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) == 0);
    assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE);
    assert((events == std::vector<std::string>{"stop", "flash", "persist", "ram", "reopen"}));
    reset(); flash_error = -1;
    assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) != 0);
    assert((events == std::vector<std::string>{"stop", "flash", "reopen"}));
    reset(); capture_error = 1;
    assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) != 0);
    assert((events == std::vector<std::string>{"stop"}));
    reset(); capture_error = 2;
    assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) == 0);
    assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE);
    reset(); assert(edid_set(&data, &apply, error, sizeof(error)) == 0);
    assert(apply.apply_required == ONEKVM_VIDEO_EDID_APPLY_HOTPLUG);
    reset(); assert(put(ONEKVM_VIDEO_EDID_WRITE_TEMPORARY) == 0);
    assert((events == std::vector<std::string>{"stop", "ram", "reopen"}));
    reset(); data.data[127] ^= 1;
    assert(put(ONEKVM_VIDEO_EDID_WRITE_PERMANENT) != 0); assert(events.empty());
}
