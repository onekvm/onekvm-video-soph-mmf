#include "lt6911_edid.hpp"
#include "onekvm_video_backend_internal.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>

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
    /* First probe can run before HDMI/I2C is ready (status polls start early).
       Retry while chip is still unknown so chip_id is not stuck empty forever. */
    static std::mutex mu;
    static EdidBoardInfo info;
    static bool initialized = false;
    static int chip_retries = 0;
    constexpr int kMaxChipRetries = 20;
    std::lock_guard<std::mutex> lock(mu);
    if (!initialized) {
        info = probe_edid_board();
        initialized = true;
    } else if (info.chip == Lt6911Chip::Unknown && chip_retries < kMaxChipRetries) {
        ++chip_retries;
        EdidBoardInfo again = probe_edid_board();
        if (again.chip != Lt6911Chip::Unknown ||
            (info.board == NanoKVMBoard::Unknown &&
             again.board != NanoKVMBoard::Unknown)) {
            info = again;
        }
    }
    return info;
}

} // namespace

int32_t edid_capabilities(onekvm_video_edid_caps_v1 *caps)
{
    if (caps == nullptr || caps->struct_size < sizeof(*caps))
        return -1;
    const EdidBoardInfo info = board();
    caps->min_bytes = kEDIDBytes;
    caps->max_bytes = kEDIDBytes;
    caps->chip_id = info.chip_id;
    caps->board_id = info.board_id;
    caps->flags = 0;
    if (info.writable)
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_WRITABLE | ONEKVM_VIDEO_EDID_CAP_PERSISTENT;
    if (lt6911_shadow_supported())
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_TEMPORARY;
    if (info.board == NanoKVMBoard::PCIe ||
        (info.chip == Lt6911Chip::C && lt6911_shadow_supported())) {
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_HOTPLUG;
        caps->apply_policy = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    } else {
        caps->flags |= ONEKVM_VIDEO_EDID_CAP_PERSISTENT;
        caps->apply_policy = (info.chip == Lt6911Chip::C || info.chip == Lt6911Chip::D)
            ? ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE : ONEKVM_VIDEO_EDID_APPLY_REBOOT;
    }
    return 0;
}

int32_t edid_get(onekvm_video_edid_blob_v1 *edid, char *error, uint32_t error_capacity)
{
    if (edid == nullptr || edid->struct_size < sizeof(*edid)) {
        set_error(error, error_capacity, "invalid EDID buffer");
        return -1;
    }
    MmfControlLock lock;
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
    if (info.chip == Lt6911Chip::C && lt6911_shadow_supported()) {
        // Save first, then explicitly apply the same payload to RAM. HPD alone
        // does not reload C Flash. Keep both stages inside one capture pause.
        struct Context {
            const onekvm_video_edid_blob_v1 *edid;
            onekvm_video_edid_apply_result_v1 *apply;
            char *error;
            uint32_t capacity;
            bool saved = false;
        } context{edid, apply, error, error_capacity};
        const int result = with_edid_capture_stopped([](void *ptr) {
            auto &c = *static_cast<Context *>(ptr);
            if (lt6911_edid_write(c.edid->data, c.edid->size)) {
                set_error(c.error, c.capacity, "save permanent HDMI EDID");
                return -1;
            }
            c.saved = true;
            c.apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE;
            (void)persist_active_edid(c.edid->data, c.edid->size);
            const int live = lt6911_shadow_write(c.edid->data, c.edid->size);
            if (!live) {
                c.apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
            } else {
                std::fprintf(stderr, "EDID saved permanently; RAM apply failed (%d); power cycle required\n", live);
            }
            return 0;
        }, &context, error, error_capacity);
        if (!context.saved) return result;
        // A failure after verified persistence must not invite another Flash
        // write. Existing callers understand success + POWER_CYCLE as saved,
        // awaiting activation, including a failure to rebuild capture.
        if (result) {
            apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE;
            std::fprintf(stderr, "EDID saved permanently; capture recovery failed; power cycle required\n");
        }
        return 0;
    }
    if (info.chip == Lt6911Chip::D) {
        struct Context {
            const onekvm_video_edid_blob_v1 *edid;
            bool pcie;
            bool saved = false;
            uint32_t required = ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE;
        } context{edid, info.board == NanoKVMBoard::PCIe};
        const int result = with_edid_capture_stopped([](void *ptr) {
            auto &c = *static_cast<Context *>(ptr);
            if (c.pcie && pcie_hdmi_reset()) return -1;
            if (lt6911_edid_write(c.edid->data, c.edid->size)) return -1;
            c.saved = true;
            (void)persist_active_edid(c.edid->data, c.edid->size);
            if (c.pcie && !pcie_hdmi_reset())
                c.required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
            return 0;
        }, &context, error, error_capacity);
        if (!context.saved) {
            set_error(error, error_capacity, "write LT6911D EDID while capture stopped");
            return result;
        }
        apply->apply_required = result
            ? static_cast<uint32_t>(ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE) : context.required;
        return 0; // Persistence succeeded; do not invite another Flash erase.
    }
    MmfControlLock lock;
    if (info.board == NanoKVMBoard::PCIe && pcie_hdmi_reset() != 0) {
        set_error(error, error_capacity, "reset PCIe HDMI before EDID write");
        return -1;
    }
    if (lt6911_edid_write(edid->data, edid->size) != 0) {
        set_error(error, error_capacity, "write HDMI EDID");
        return -1;
    }
    (void)persist_active_edid(edid->data, edid->size);
    if (info.board == NanoKVMBoard::PCIe) {
        if (pcie_hdmi_reset() != 0) {
            set_error(error, error_capacity, "reset PCIe HDMI after EDID write");
            return -1;
        }
        apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    } else {
        apply->apply_required = (info.chip == Lt6911Chip::C || info.chip == Lt6911Chip::D)
            ? ONEKVM_VIDEO_EDID_APPLY_POWER_CYCLE : ONEKVM_VIDEO_EDID_APPLY_REBOOT;
    }
    return 0;
}

int32_t edid_set_mode(const onekvm_video_edid_blob_v1 *edid, uint32_t mode,
                       onekvm_video_edid_apply_result_v1 *apply,
                       char *error, uint32_t error_capacity)
{
    if (mode == ONEKVM_VIDEO_EDID_WRITE_PERMANENT)
        return edid_set(edid, apply, error, error_capacity);
    if (mode != ONEKVM_VIDEO_EDID_WRITE_TEMPORARY || !lt6911_shadow_supported()) {
        set_error(error, error_capacity, "temporary EDID writes are not supported by this firmware");
        return -1;
    }
    if (!edid || !apply || edid->struct_size < sizeof(*edid) ||
        apply->struct_size < sizeof(*apply) || edid->size != kEDIDBytes ||
        !valid_edid(edid->data, edid->size) || edid->data[126] != 1) {
        set_error(error, error_capacity, "temporary EDID requires a valid 256-byte EDID");
        return -1;
    }
    struct Context { const onekvm_video_edid_blob_v1 *edid; char *error; uint32_t capacity; };
    Context context{edid, error, error_capacity};
    const int result = with_edid_capture_stopped([](void *ptr) {
        auto &context = *static_cast<Context *>(ptr);
        const int result = lt6911_shadow_write(context.edid->data, context.edid->size);
        if (result) set_error(context.error, context.capacity,
            "write EDID shadow RAM (error %d)", result);
        return result;
    }, &context, error, error_capacity);
    if (result) return result;
    apply->apply_required = ONEKVM_VIDEO_EDID_APPLY_HOTPLUG;
    return 0;
}

} // namespace onekvm::video_backend
