#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "lt6911_receiver.h"

namespace onekvm::video_backend {

enum class Lt6911Chip {
    Unknown,
    UXC,
    C,
    D,
};
static_assert(static_cast<int>(Lt6911Chip::Unknown) == ONEKVM_LT6911_UNKNOWN &&
              static_cast<int>(Lt6911Chip::UXC) == ONEKVM_LT6911_UXC &&
              static_cast<int>(Lt6911Chip::C) == ONEKVM_LT6911_C &&
              static_cast<int>(Lt6911Chip::D) == ONEKVM_LT6911_D);

enum class NanoKVMBoard {
    Unknown,
    Cube,
    Lite,
    PCIe,
};

struct EdidBoardInfo {
    NanoKVMBoard board = NanoKVMBoard::Unknown;
    Lt6911Chip chip = Lt6911Chip::Unknown;
    const char *board_id = "";
    const char *chip_id = "";
    bool writable = false;
};

inline NanoKVMBoard parse_nanokvm_board(const char *hardware_json)
{
    if (hardware_json == nullptr)
        return NanoKVMBoard::Unknown;
    if (std::strstr(hardware_json, "\"pcie\"") != nullptr)
        return NanoKVMBoard::PCIe;
    if (std::strstr(hardware_json, "\"lite\"") != nullptr)
        return NanoKVMBoard::Lite;
    if (std::strstr(hardware_json, "\"cube\"") != nullptr)
        return NanoKVMBoard::Cube;
    return NanoKVMBoard::Unknown;
}
EdidBoardInfo probe_edid_board();
bool pcie_hdmi_variant();
void lt6911_edid_initialize();
bool lt6911_shadow_supported();
int lt6911_shadow_write(const uint8_t *data, size_t size);
int with_edid_capture_stopped(int (*operation)(void *), void *context,
                              char *error, uint32_t error_capacity);
int lt6911_edid_read(uint8_t *data, size_t size);
int lt6911_edid_write(const uint8_t *data, size_t size);
int pcie_hdmi_reset();
int pcie_hdmi_startup_reset();
int persist_active_edid(const uint8_t *data, size_t size);
int restore_active_edid_if_needed();

} // namespace onekvm::video_backend
