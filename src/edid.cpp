#include "lt6911_edid.hpp"
#include "onekvm_video_backend_internal.hpp"

#include <cstdlib>
#include <cstring>

namespace onekvm::video_backend {
namespace {

constexpr uint32_t kEDIDBytes = 256;

bool valid_edid(const uint8_t *data, uint32_t size)
{
    if (data == nullptr || size < 128 || size % 128 != 0 || size > ONEKVM_VIDEO_EDID_MAX_BYTES)
        return false;
    static const uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (std::memcmp(data, header, sizeof(header)) != 0)
        return false;
    for (uint32_t block = 0; block < size / 128; block++) {
        uint8_t sum = 0;
        for (uint32_t i = 0; i < 128; i++)
            sum += data[block * 128 + i];
        if (sum != 0)
            return false;
    }
    return true;
}

EdidBoardInfo &board()
{
    static EdidBoardInfo info = probe_edid_board();
    return info;
}

} // namespace

int32_t edid_capabilities(onekvm_video_edid_caps_v1 *caps)
{
    if (caps == nullptr || caps->struct_size < sizeof(*caps))
        return -1;
    const EdidBoardInfo info = board();
    caps->min_bytes = 128;
    caps->max_bytes = kEDIDBytes;
    caps->chip_id = info.chip_id;
    caps->board_id = info.board_id;
    caps->flags = 0;
    if (info.writable)
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_WRITABLE;
    if (info.board == NanoKVMBoard::PCIe) {
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_HOTPLUG;
        caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    } else {
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_PERSISTENT;
        caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_REBOOT;
    }
    return 0;
}

int32_t edid_get(onekvm_video_edid_blob_v1 *edid, char *error, uint32_t error_capacity)
{
    if (edid == nullptr || edid->struct_size < sizeof(*edid)) {
        set_error(error, error_capacity, "invalid EDID buffer");
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mmf_mutex);
    if (lt6911_edid_read(edid->data, kEDIDBytes) != 0) {
        set_error(error, error_capacity, "read HDMI EDID");
        return -1;
    }
    edid->size = kEDIDBytes;
    return 0;
}

int32_t edid_set(const onekvm_video_edid_blob_v1 *edid,
                 onekvm_video_edid_apply_result_v1 *apply,
                 char *error, uint32_t error_capacity)
{
    if (edid == nullptr || apply == nullptr ||
        edid->struct_size < sizeof(*edid) ||
        apply->struct_size < sizeof(*apply)) {
        set_error(error, error_capacity, "invalid EDID request");
        return -1;
    }
    if (!valid_edid(edid->data, edid->size)) {
        set_error(error, error_capacity, "EDID is invalid");
        return -1;
    }
    const EdidBoardInfo info = board();
    if (!info.writable) {
        set_error(error, error_capacity, "EDID is read-only on this capture chip");
        return -1;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mmf_mutex);
    if (info.board == NanoKVMBoard::PCIe)
        (void)pcie_hdmi_reset();
    if (lt6911_edid_write(edid->data, edid->size) != 0) {
        set_error(error, error_capacity, "write HDMI EDID");
        return -1;
    }
    (void)persist_active_edid(edid->data, edid->size);
    if (info.board == NanoKVMBoard::PCIe) {
        (void)pcie_hdmi_reset();
        apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    } else {
        apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_REBOOT;
    }
    return 0;
}

} // namespace onekvm::video_backend
