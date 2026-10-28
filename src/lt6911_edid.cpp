#include "lt6911_edid.hpp"
#include "lt6911_info.hpp"
#include "lt6911_uxc_shadow.hpp"
#include "lt6911_c_shadow.hpp"
#include "lt6911_receiver.h"
#include "lt6911_d_flash.hpp"
#include <atomic>

#include "mmf.hpp"
#include "onekvm_video_backend_internal.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
#include <vector>

namespace onekvm::video_backend {
namespace {

constexpr const char *kI2CDevice = "/dev/i2c-4";
constexpr const char *kKernelDriver = "/sys/bus/i2c/devices/4-002b/driver";
constexpr uint8_t kI2CAddress = 0x2b;
constexpr uint8_t kBank = 0xff;
constexpr uint8_t kSys = 0x80;
constexpr uint8_t kSys2 = 0x90;
constexpr uint8_t kSys3 = 0x81;
constexpr uint8_t kSys4 = 0xa0;
constexpr uint8_t kUXCChunk = 32;
constexpr uint8_t kCChunk = 16;
constexpr const char *kActivePath = "/var/lib/onekvm/edid/active.bin";
constexpr const char *kHardwarePath = "/run/onekvm/hardware.json";

int g_fd = -1;
int g_bus_lock_fd = -1;
uint8_t g_bank = 0xff;
int g_hdmi_gpio_fd = -1;
std::mutex g_hdmi_gpio_mutex;
std::atomic<bool> g_shadow_supported{false};
Lt6911Chip g_shadow_chip = Lt6911Chip::Unknown;
uint8_t g_active_edid[256]{};
bool g_active_valid = false; // guarded by the shared LT6911 I2C mutex

int write_raw(const uint8_t *data, size_t len)
{
    const ssize_t count = write(g_fd, data, len);
    if (count == (ssize_t)len) return 0;
    g_bank = 0xff;
    if (count >= 0) errno = EIO;
    return -1;
}

int set_bank(uint8_t bank)
{
    const uint8_t cmd[] = {kBank, bank};
    if (write_raw(cmd, sizeof(cmd)) != 0)
        return -1;
    g_bank = bank;
    return 0;
}

int write_reg(uint8_t bank, uint8_t reg, uint8_t value)
{
    if (set_bank(bank) != 0)
        return -1;
    const uint8_t cmd[] = {reg, value};
    return write_raw(cmd, sizeof(cmd));
}

int write_regs(uint8_t bank, uint8_t reg, const uint8_t *data, size_t len)
{
    if (set_bank(bank) != 0)
        return -1;
    std::vector<uint8_t> cmd(1 + len);
    cmd[0] = reg;
    std::memcpy(cmd.data() + 1, data, len);
    return write_raw(cmd.data(), cmd.size());
}

int read_regs(uint8_t bank, uint8_t reg, uint8_t *data, size_t len)
{
    if (set_bank(bank) != 0)
        return -1;
    if (write_raw(&reg, 1) != 0)
        return -1;
    const ssize_t count = read(g_fd, data, len);
    if (count == (ssize_t)len) return 0;
    g_bank = 0xff;
    if (count >= 0) errno = EIO;
    return -1;
}

int open_bus()
{
    g_bus_lock_fd = open("/run/lock/onekvm-lt6911-flash.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_bus_lock_fd < 0) return -1;
    if (flock(g_bus_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_bus_lock_fd); g_bus_lock_fd = -1; return -1;
    }
    g_fd = open(kI2CDevice, O_RDWR | O_CLOEXEC);
    if (g_fd < 0) {
        close(g_bus_lock_fd); g_bus_lock_fd = -1; return -1;
    }
    if (ioctl(g_fd, I2C_SLAVE_FORCE, kI2CAddress) < 0) {
        close(g_fd);
        g_fd = -1;
        close(g_bus_lock_fd); g_bus_lock_fd = -1;
        return -1;
    }
    g_bank = 0xff;
    return 0;
}

void close_bus()
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_bank = 0xff;
    if (g_bus_lock_fd >= 0) { close(g_bus_lock_fd); g_bus_lock_fd = -1; }
}

int enable()
{
    return write_reg(kSys, 0xee, 0x01);
}

int disable()
{
    return write_reg(kSys, 0xee, 0x00);
}

int uxc_write(const uint8_t *edid, size_t size)
{
    uint8_t chip = 0;
    uint8_t version[32] = {};
    const uint8_t chunks = static_cast<uint8_t>(size / kUXCChunk + 1);
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5e, 0xdf) || write_reg(kSys, 0x58, 0x00) ||
        write_reg(kSys, 0x59, 0x51) || write_reg(kSys, 0x5a, 0x10) ||
        write_reg(kSys, 0x5a, 0x00) || write_reg(kSys, 0x58, 0x21) ||
        write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x80) || write_reg(kSys, 0x5a, 0x84) ||
        write_reg(kSys, 0x5a, 0x80) || write_reg(kSys, 0x5b, 0x01) ||
        write_reg(kSys, 0x5c, 0x80) || write_reg(kSys, 0x5d, 0x00) ||
        write_reg(kSys, 0x5a, 0x81) || write_reg(kSys, 0x5a, 0x80))
        return -1;
    usleep(500000);
    if (read_regs(kSys3, 0x08, &chip, 1) != 0 || chip != 0xee)
        return -1;
    if (write_reg(kSys3, 0x08, 0xae) || write_reg(kSys3, 0x08, 0xee) ||
        write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x84) || write_reg(kSys, 0x5a, 0x80) ||
        write_reg(kSys, 0x5a, 0x84) || write_reg(kSys, 0x5a, 0x80))
        return -1;
    for (uint8_t i = 0; i < chunks; i++) {
        const uint8_t *payload = i + 1 == chunks ? version : edid + kUXCChunk * i;
        if (write_reg(kSys, 0x5e, 0xdf) || write_reg(kSys, 0x5a, 0x20) ||
            write_reg(kSys, 0x5a, 0x00) || write_reg(kSys, 0x58, 0x21) ||
            write_regs(kSys, 0x59, payload, kUXCChunk) ||
            write_reg(kSys, 0x5b, 0x01))
            return -1;
        if (i + 1 != chunks) {
            if (write_reg(kSys, 0x5c, 0x80) ||
                write_reg(kSys, 0x5d, static_cast<uint8_t>(kUXCChunk * i)))
                return -1;
        } else if (write_reg(kSys, 0x5c, 0x81) || write_reg(kSys, 0x5d, 0x00)) {
            return -1;
        }
        if (write_reg(kSys, 0x5e, 0xc0) || write_reg(kSys, 0x5a, 0x90) ||
            write_reg(kSys, 0x5a, 0x80))
            return -1;
        if (i + 1 != chunks) {
            if (write_reg(kSys, 0x5a, 0x84))
                return -1;
        } else if (write_reg(kSys, 0x5a, 0x88)) {
            return -1;
        }
        if (write_reg(kSys, 0x5a, 0x80))
            return -1;
    }
    if (read_regs(kSys3, 0x08, &chip, 1) != 0 || chip != 0xee)
        return -1;
    return write_reg(kSys3, 0x08, 0xae) || write_reg(kSys3, 0x08, 0xee) ? -1 : 0;
}

int uxc_read_at(uint8_t *edid, size_t size, uint32_t address)
{
    const uint8_t chunks = static_cast<uint8_t>(size / kUXCChunk);
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x84) || write_reg(kSys, 0x5a, 0x80))
        return -1;
    for (uint8_t i = 0; i < chunks; i++) {
        if (write_reg(kSys, 0x5e, 0x5f) || write_reg(kSys, 0x5a, 0xa0) ||
            write_reg(kSys, 0x5a, 0x80) || write_reg(kSys, 0x5b, (address + kUXCChunk * i) >> 16) ||
            write_reg(kSys, 0x5c, (address + kUXCChunk * i) >> 8) ||
            write_reg(kSys, 0x5d, (address + kUXCChunk * i) & 0xff) ||
            write_reg(kSys, 0x5a, 0x90))
            return -1;
        // Flash-to-FIFO is asynchronous. Match the maintained UXC flash reader's
        // completion interval before stopping the transfer or reading the FIFO.
        usleep(5000);
        if (write_reg(kSys, 0x5a, 0x80) ||
            write_reg(kSys, 0x58, 0x21) ||
            read_regs(kSys, 0x5f, edid + kUXCChunk * i, kUXCChunk))
            return -1;
    }
    return 0;
}

int uxc_read(uint8_t *edid, size_t size) { return uxc_read_at(edid, size, 0x18000); }

int c_read(uint8_t *edid, size_t size);

int c_write(const uint8_t *edid, size_t size)
{
    uint8_t before[256]{}, again[256]{};
    if (size != sizeof(before) || c_read(before, size) || c_read(again, size) ||
        std::memcmp(before, again, size)) return -1;
    // A retry of an already saved payload only needs the RAM/HPD stage.
    if (!std::memcmp(before, edid, size)) return 0;
    uint8_t chip[2] = {};
    uint8_t status = 0;
    const uint8_t chunks = static_cast<uint8_t>(size / kCChunk);
    if (enable() || read_regs(kSys4, 0x00, chip, 2) != 0 ||
        chip[0] != 0x16 || chip[1] != 0x05 || disable())
        return -1;
    usleep(100000);
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        read_regs(kSys4, 0x00, chip, 2) != 0 ||
        chip[0] != 0x16 || chip[1] != 0x05 || disable())
        return -1;
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5e, 0xc0) ||
        write_reg(kSys, 0x58, 0x00) || write_reg(kSys, 0x59, 0x51) ||
        write_reg(kSys, 0x5a, 0x92) || write_reg(kSys, 0x5a, 0x82) ||
        write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5a, 0x86) ||
        write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5b, 0x01) ||
        write_reg(kSys, 0x5c, 0x80) || write_reg(kSys, 0x5d, 0x00) ||
        write_reg(kSys, 0x5a, 0x83) || write_reg(kSys, 0x5a, 0x82))
        return -1;
    usleep(500000);
    if (read_regs(kSys2, 0x02, &status, 1) != 0 || status != 0xff)
        return -1;
    if (write_reg(kSys2, 0x02, 0xdf) || write_reg(kSys2, 0x02, 0xff) ||
        write_reg(kSys, 0xff, 0x80) || enable() || write_reg(kSys, 0x5a, 0x86))
        return -1;
    for (uint8_t i = 0; i < chunks; i++) {
        if (write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5a, 0x86) ||
            write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5e, 0xef) ||
            write_reg(kSys, 0x5a, 0xa2) || write_reg(kSys, 0x5a, 0x82) ||
            write_reg(kSys, 0x58, 0x01) ||
            write_regs(kSys, 0x59, edid + kCChunk * i, kCChunk) ||
            write_reg(kSys, 0x5b, 0x01) || write_reg(kSys, 0x5c, 0x80) ||
            write_reg(kSys, 0x5d, static_cast<uint8_t>(kCChunk * i)) ||
            write_reg(kSys, 0x5e, 0xe0) || write_reg(kSys, 0x5a, 0x92))
            return -1;
        // Match the maintained C programmer: wait for each FIFO page, close
        // its write-enable window and acknowledge completion before the next.
        usleep(10000);
        if (write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5a, 0x8a) ||
            write_reg(kSys, 0x5a, 0x82) ||
            read_regs(kSys2, 0x02, &status, 1) || status != 0xff ||
            write_reg(kSys2, 0x02, 0xdf) || write_reg(kSys2, 0x02, 0xff))
            return -1;
    }
    if (write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5a, 0x8a) ||
        write_reg(kSys, 0x5a, 0x82))
        return -1;
    if (read_regs(kSys2, 0x02, &status, 1) != 0 || status != 0xff)
        return -1;
    if (write_reg(kSys2, 0x02, 0xdf) || write_reg(kSys2, 0x02, 0xff)) return -1;
    // Do not report "saved" or apply RAM until persistent bytes are verified.
    return c_read(before, size) || c_read(again, size) ||
        std::memcmp(before, edid, size) || std::memcmp(again, edid, size) ? -1 : 0;
}

int c_read_at(uint8_t *edid, size_t size, uint32_t address)
{
    const uint8_t chunks = static_cast<uint8_t>(size / kCChunk);
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x86) || write_reg(kSys, 0x5a, 0x82))
        return -1;
    for (uint8_t i = 0; i < chunks; i++) {
        if (write_reg(kSys, 0x5e, 0x6f) || write_reg(kSys, 0x5a, 0xa2) ||
            write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5b, (address + kCChunk * i) >> 16) ||
            write_reg(kSys, 0x5c, (address + kCChunk * i) >> 8) ||
            write_reg(kSys, 0x5d, (address + kCChunk * i) & 0xff) ||
            write_reg(kSys, 0x5a, 0x92)) return -1;
        usleep(1000); // Flash-to-FIFO completion, as in the maintained C reader.
        if (write_reg(kSys, 0x5a, 0x82) ||
            write_reg(kSys, 0x58, 0x01) ||
            read_regs(kSys, 0x5f, edid + kCChunk * i, kCChunk))
            return -1;
    }
    return 0;
}

int c_read(uint8_t *edid, size_t size) { return c_read_at(edid, size, 0x18000); }

int identity_read(void *, uint16_t address, uint8_t *data, size_t size)
{
    return read_regs(address >> 8, address & 0xff, data, size);
}
int identity_write(void *, uint16_t address, uint8_t value)
{
    return write_reg(address >> 8, address & 0xff, value);
}
Lt6911Chip detect_chip()
{
    int chip = onekvm_lt6911_cached_chip();
    if (chip != ONEKVM_LT6911_UNKNOWN) return static_cast<Lt6911Chip>(chip);
    const onekvm_lt6911_io io{nullptr, identity_read, identity_write};
    if (onekvm_lt6911_identify(&io, &chip)) return Lt6911Chip::Unknown;
    onekvm_lt6911_cache_chip(chip);
    return static_cast<Lt6911Chip>(chip);
}
void d_wait(unsigned ms) { usleep(ms * 1000); }
const DFlashIO kDFlashIO{write_reg, read_regs, write_regs, d_wait};
int read_edid(Lt6911Chip chip, uint8_t *data, size_t size)
{
    switch (chip) {
    case Lt6911Chip::C: return c_read(data, size);
    case Lt6911Chip::UXC: return uxc_read(data, size);
    case Lt6911Chip::D: return d_flash_read(kDFlashIO, 0x8000, data, size);
    default: return -ENODEV;
    }
}
int finish_edid(Lt6911Chip chip, int result)
{
    // D helpers already attempt every cleanup step, including E0:EE close.
    if (chip == Lt6911Chip::D || chip == Lt6911Chip::Unknown) return result;
    const int cleanup = disable();
    return result ? result : cleanup;
}

bool pcie_variant()
{
    const char *path = std::getenv("ONEKVM_MACHINE_HARDWARE_PATH");
    if (path == nullptr || path[0] == '\0')
        path = kHardwarePath;
    std::ifstream in(path);
    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return parse_nanokvm_board(json.c_str()) == NanoKVMBoard::PCIe;
}

int ensure_hdmi_gpio()
{
    if (g_hdmi_gpio_fd >= 0)
        return 0;
    if (!pcie_variant()) {
        errno = ENODEV;
        return -1;
    }
    const int chip = open("/dev/gpiochip1", O_RDONLY | O_CLOEXEC);
    if (chip < 0)
        return -1;
    gpio_v2_line_request request{};
    request.offsets[0] = 3;
    request.num_lines = 1;
    std::strncpy(request.consumer, "onekvm-mmf-hdmi", sizeof(request.consumer) - 1);
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    request.config.num_attrs = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].attr.values = 1;
    request.config.attrs[0].mask = 1;
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &request) != 0) {
        close(chip);
        return -1;
    }
    close(chip);
    g_hdmi_gpio_fd = request.fd;
    return 0;
}

int set_hdmi_gpio(bool high)
{
    if (ensure_hdmi_gpio() != 0)
        return -1;
    gpio_v2_line_values values{};
    values.mask = 1;
    values.bits = high ? 1 : 0;
    return ioctl(g_hdmi_gpio_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
}

int pcie_hdmi_reset_us(useconds_t low_us, useconds_t settle_us)
{
    std::lock_guard<std::mutex> lock(g_hdmi_gpio_mutex);
    if (set_hdmi_gpio(false) != 0)
        return -1;
    usleep(low_us);
    if (set_hdmi_gpio(true) != 0)
        return -1;
    usleep(settle_us);
    return 0;
}

} // namespace

EdidBoardInfo probe_edid_board()
{
    EdidBoardInfo info;
    const char *path = std::getenv("ONEKVM_MACHINE_HARDWARE_PATH");
    if (path == nullptr || path[0] == '\0')
        path = kHardwarePath;
    std::ifstream in(path);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    info.board = parse_nanokvm_board(json.c_str());
    switch (info.board) {
    case NanoKVMBoard::PCIe:
        info.board_id = "pcie";
        break;
    case NanoKVMBoard::Lite:
        info.board_id = "lite";
        break;
    case NanoKVMBoard::Cube:
        info.board_id = "cube";
        break;
    default:
        info.board_id = "";
        break;
    }
    onekvm_lt6911_i2c_lock();
    if (open_bus() == 0) {
        info.chip = detect_chip();
        close_bus();
    }
    onekvm_lt6911_i2c_unlock();
    switch (info.chip) {
    case Lt6911Chip::C:
        info.chip_id = "LT6911C";
        info.writable = info.board != NanoKVMBoard::Unknown;
        break;
    case Lt6911Chip::D:
        info.chip_id = "LT6911D";
        info.writable = info.board != NanoKVMBoard::Unknown;
        break;
    case Lt6911Chip::UXC:
        info.chip_id = "LT6911UXC";
        info.writable = info.board != NanoKVMBoard::Unknown;
        break;
    default:
        info.chip_id = "";
        info.writable = false;
        break;
    }
    (void)publish_lt6911_info(info);
    return info;
}

bool pcie_hdmi_variant()
{
    return pcie_variant();
}

namespace {
int shadow_read(void *, uint8_t bank, uint8_t reg, uint8_t *value) {
    return read_regs(bank, reg, value, 1);
}
int shadow_write(void *, uint8_t bank, uint8_t reg, uint8_t value) {
    return write_reg(bank, reg, value);
}
void shadow_wait(unsigned duration) { usleep(duration); }
const UxcShadowIO kShadowIO{nullptr, shadow_read, shadow_write, shadow_wait};
struct ShadowProfile { uint32_t address; uint8_t bytes[32]; };
// Read-only 32-byte signatures from the verified UXC image: shadow writer,
// HPD bit update and MCU watchdog feed. Never infer support from a chip name.
const ShadowProfile kShadowProfile[] = {
    {0x5300, {0x90,0x02,0x40,0xe0,0x60,0x09,0x7b,0x01,0x7a,0x01,0x79,0x6e,0x12,0x38,0x02,0x7b,0x01,0x7a,0x01,0x79,0x38,0x12,0x4e,0x17,0xe4,0xff,0xfe,0x90,0xb0,0x0a,0xef,0xf0}},
    {0x5320, {0x24,0x38,0xf5,0x82,0x74,0x01,0x3e,0xf5,0x83,0xe0,0x90,0xb0,0xb0,0xf0,0x0f,0xbf,0x00,0x01,0x0e,0xe4,0xb5,0x07,0xe4,0xee,0xb4,0x01,0xe0,0x90,0x85,0x9c,0xe0,0x60}},
    {0x5680, {0x39,0xf0,0x22,0xef,0x90,0xb0,0x00,0x60,0x0b,0xe0,0x44,0x01,0xf0,0x90,0x02,0x66,0x74,0x01,0xf0,0x22,0xe0,0x54,0xfe,0xf0,0xe4,0x90,0x02,0x66,0xf0,0x22,0x90,0x82}},
    {0x5720, {0x8c,0x22,0xef,0x90,0x82,0x58,0x60,0x04,0x74,0x01,0xf0,0x22,0x74,0xef,0xf0,0x22,0x90,0x80,0x10,0x74,0x07,0xf0,0x74,0x05,0xf0,0x22,0xff,0xff,0xff,0xff,0xff,0xff}},
};
struct CShadowProfile { uint32_t address; uint8_t bytes[16]; };
// Verified C startup HPD, RAM writer and register helper. No UXC registers.
const CShadowProfile kCShadowProfile[] = {
    {0x2810, {0xe0,0xf4,0x60,0x03,0x02,0x28,0x9f,0xa3,0xe0,0xf4,0x60,0x03,0x02,0x28,0x9f,0xa3}},
    {0x2820, {0xe0,0xf4,0x70,0x7b,0xa3,0xe0,0xf4,0x70,0x76,0xa3,0xe0,0xf4,0x70,0x71,0xa3,0xe0}},
    {0x2830, {0xf4,0x70,0x6c,0xa3,0xe0,0x70,0x68,0x90,0x00,0x84,0xe0,0x64,0x02,0x70,0x60,0xa3}},
    {0x2840, {0xe0,0x64,0x03,0x70,0x5a,0x90,0x00,0x83,0xe0,0x90,0x00,0x02,0xf0,0x90,0x01,0x03}},
    {0x28b0, {0xf5,0x82,0x74,0x21,0x3e,0xf5,0x83,0xe4,0x93,0xfd,0x74,0x09,0x2f,0xf5,0x82,0x74}},
    {0x07a0, {0x12,0x34,0xcc,0xe4,0xfd,0x7f,0x06,0x7e,0xc0,0x12,0x34,0x42,0x12,0x30,0x32,0x12}},
    {0x07f0, {0x7f,0x64,0x7e,0x00,0x12,0x34,0xa0,0x7d,0x08,0x7f,0x06,0x7e,0xc0,0x12,0x34,0x42}},
    {0x30b0, {0x8e,0x7e,0xc0,0x12,0x34,0x42,0xe4,0x90,0x00,0x00,0xf0,0xa3,0xf0,0x90,0x00,0x00}},
    {0x30c0, {0xe0,0xfe,0xa3,0xe0,0xff,0xc3,0xee,0x94,0x01,0x50,0x23,0x74,0x09,0x2f,0xf5,0x82}},
    {0x30d0, {0x74,0x01,0x3e,0xf5,0x83,0xe0,0xfd,0x7f,0x90,0x7e,0xc0,0x12,0x34,0x42,0x90,0x00}},
    {0x30e0, {0x01,0xe0,0x04,0xf0,0x70,0xd7,0x90,0x00,0x00,0xe0,0x04,0xf0,0x80,0xcf,0x7d,0x02}},
    {0x30f0, {0x7f,0x8e,0x7e,0xc0,0x02,0x34,0x42,0xe4,0xfd,0x7f,0x27,0x7e,0xb8,0x12,0x34,0x42}},
    {0x3440, {0xfd,0x0f,0x8f,0x82,0x8e,0x83,0xed,0xf0,0x22,0xe4,0xfd,0xff,0x7e,0xd8,0x12,0x34}},
};

// RAM-only journal: survives a Core restart, never a power cycle. It is the
// last known payload, not a chip RAM readback. A pending record restores the
// previous EDID before VI starts after an interrupted transaction.
constexpr const char *kCRuntimePath = "/run/onekvm/lt6911c-shadow.bin";
struct CShadowState {
    uint8_t magic[8] = {'O','K','C','S',1,0,0,0};
    uint8_t baseline[256]{};
    uint8_t active[256]{};
    uint8_t hpd = 0, control = 2, pending = 0, reserved[5]{};
};
CShadowState g_c_state;
int save_c_state(const CShadowState &state) {
    const std::string temporary = std::string(kCRuntimePath) + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    const bool written = write(fd, &state, sizeof(state)) == static_cast<ssize_t>(sizeof(state));
    const bool closed = close(fd) == 0;
    if (!written || !closed || rename(temporary.c_str(), kCRuntimePath) != 0) {
        unlink(temporary.c_str()); return -1;
    }
    return 0;
}
int c_prepare(void *, const CShadowControl &control) {
    auto next = g_c_state;
    next.hpd = control.hpd; next.control = control.control; next.pending = 1;
    if (save_c_state(next)) return -1;
    g_c_state = next;
    return 0;
}
int c_commit(const uint8_t *active) {
    auto next = g_c_state;
    std::memcpy(next.active, active, 256); next.pending = 0;
    if (save_c_state(next)) return -1;
    g_c_state = next;
    return 0;
}
bool c_initialize_shadow() {
    bool supported = true;
    for (const auto &profile : kCShadowProfile) {
        uint8_t actual[16]{}, again[16]{};
        if (c_read_at(actual, 16, profile.address) || c_read_at(again, 16, profile.address) ||
            std::memcmp(actual, again, 16) || std::memcmp(actual, profile.bytes, 16)) {
            supported = false; break;
        }
    }
    uint8_t baseline[256]{}, again[256]{};
    if (supported) {
        supported = c_read(baseline, 256) == 0 && c_read(again, 256) == 0 &&
            std::memcmp(baseline, again, 256) == 0;
        if (supported) {
            bool blank = true;
            for (auto value : baseline) if (value != 255) blank = false;
            // The verified firmware uses its embedded EDID when Flash is blank.
            // An invalid nonblank override is ambiguous: do not guess its RAM.
            if (blank) supported = c_read_at(baseline, 256, 0x2100) == 0 &&
                c_read_at(again, 256, 0x2100) == 0 && std::memcmp(baseline, again, 256) == 0;
            supported = supported && valid_shadow_edid(baseline) &&
                baseline[128] == 2 && baseline[129] == 3;
        }
    }
    if (disable()) supported = false;
    if (!supported) return false;
    CShadowState state;
    int fd = open(kCRuntimePath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        uint8_t extra;
        const bool complete = read(fd, &state, sizeof(state)) == static_cast<ssize_t>(sizeof(state)) && read(fd, &extra, 1) == 0;
        close(fd);
        if (!complete || std::memcmp(state.magic, g_c_state.magic, 8) || state.pending > 1 ||
            state.control != 2 || !valid_shadow_edid(state.active) ||
            std::memcmp(state.baseline, baseline, 256)) return false;
    } else {
        if (errno != ENOENT) return false;
        std::memcpy(state.baseline, baseline, 256);
        std::memcpy(state.active, baseline, 256);
    }
    g_c_state = state;
    if (state.pending) {
        const CShadowControl control{state.hpd, state.control};
        if (c_shadow_apply(kShadowIO, state.active, state.active, nullptr, &control)) return false;
    } else {
        // Check the streaming writer is idle without pulsing HPD or reading C090.
        uint8_t control = 0;
        const bool idle = enable() == 0 && read_regs(0xc0, 0x8e, &control, 1) == 0 && control == 2;
        const bool closed = disable() == 0;
        if (!idle || !closed) return false;
    }
    if (c_commit(state.active)) return false;
    std::memcpy(g_active_edid, state.active, 256);
    return true;
}
int c_apply_shadow(const uint8_t *desired) {
    if (!g_active_valid || !valid_shadow_edid(g_active_edid)) return -EUCLEAN;
    uint8_t before[256];
    std::memcpy(before, g_active_edid, 256);
    int result = c_shadow_apply(kShadowIO, before, desired, c_prepare);
    if (!result && c_commit(desired)) {
        // A failed commit must not report success with an unjournaled RAM value.
        const CShadowControl control{g_c_state.hpd, g_c_state.control};
        result = c_shadow_apply(kShadowIO, desired, before, nullptr, &control) ? -EUCLEAN : -EIO;
    }
    if (result && result != -EUCLEAN && g_c_state.pending && c_commit(before)) result = -EUCLEAN;
    return result;
}

}

bool lt6911_shadow_supported() { return g_shadow_supported.load(std::memory_order_acquire); }

void lt6911_edid_initialize()
{
    // Called before VI starts. Capability queries and GET never probe shadow RAM.
    static std::once_flag once;
    std::call_once(once, [] {
        const auto info = probe_edid_board();
        onekvm_lt6911_i2c_lock();
        if (open_bus() == 0) {
            bool supported = false;
            if (info.board == NanoKVMBoard::PCIe && info.chip == Lt6911Chip::UXC &&
                access(kKernelDriver, F_OK) != 0 && errno == ENOENT) {
                uint8_t watchdog = 0;
                bool saved = enable() == 0 && read_regs(0x80, 0x10, &watchdog, 1) == 0;
                supported = saved && write_reg(0x80, 0x10, 0) == 0;
                for (const auto &profile : kShadowProfile) {
                    uint8_t actual[32]{};
                    if (!supported || uxc_read_at(actual, 32, profile.address) ||
                        std::memcmp(actual, profile.bytes, 32)) { supported = false; break; }
                }
                if (saved && write_reg(0x80, 0x10, watchdog)) supported = false;
                if (disable()) supported = false;
                if (supported)
                    supported = uxc_shadow_apply(kShadowIO, nullptr, g_active_edid) == 0;
            } else if (info.board == NanoKVMBoard::Cube && info.chip == Lt6911Chip::C &&
                       access(kKernelDriver, F_OK) != 0 && errno == ENOENT) {
                supported = c_initialize_shadow();
            }
            g_shadow_chip = supported ? info.chip : Lt6911Chip::Unknown;
            g_shadow_supported.store(supported, std::memory_order_release);
            g_active_valid = supported;
            if (!g_active_valid && info.chip != Lt6911Chip::Unknown) {
                g_active_valid = finish_edid(info.chip,
                    read_edid(info.chip, g_active_edid, 256)) == 0;
            }
            close_bus();
        }
        onekvm_lt6911_i2c_unlock();
    });
}

int lt6911_shadow_write(const uint8_t *data, size_t size)
{
    if (!lt6911_shadow_supported()) return -EOPNOTSUPP;
    if (access(kKernelDriver, F_OK) == 0 || errno != ENOENT) return -EBUSY;
    if (!data || size != 256) return -EINVAL;
    onekvm_lt6911_i2c_lock();
    int result = -EIO;
    if (open_bus() == 0) {
        uint8_t actual[256]{};
        result = g_shadow_chip == Lt6911Chip::C
            ? c_apply_shadow(data) : uxc_shadow_apply(kShadowIO, data, actual);
        if (!result) {
            std::memcpy(g_active_edid, g_shadow_chip == Lt6911Chip::C ? data : actual, 256);
            g_active_valid = true;
        } else if (result == -EUCLEAN) {
            g_active_valid = false;
            g_shadow_supported.store(false, std::memory_order_release);
        }
        close_bus();
    }
    onekvm_lt6911_i2c_unlock();
    return result;
}

int lt6911_edid_read(uint8_t *data, size_t size)
{
    if (data == nullptr || size != 256)
        return -1;
    onekvm_lt6911_i2c_lock();
    int result = -1;
    if (g_active_valid) {
        std::memcpy(data, g_active_edid, 256);
        result = 0;
    } else if (onekvm_lt6911_cached_chip() == ONEKVM_LT6911_D) {
        // D's internal window belongs to stopped-capture maintenance only.
        // Cache failure must not turn an HTTP GET into a live Flash session.
        result = -EAGAIN;
    } else if (open_bus() == 0) {
        const Lt6911Chip chip = detect_chip();
        result = chip == Lt6911Chip::D ? -EAGAIN
            : finish_edid(chip, read_edid(chip, data, size));
        if (!result) {
            std::memcpy(g_active_edid, data, 256);
            g_active_valid = true;
        }
        close_bus();
    }
    onekvm_lt6911_i2c_unlock();
    return result;
}

int lt6911_edid_write(const uint8_t *data, size_t size)
{
    if (data == nullptr || size != 256)
        return -1;
    onekvm_lt6911_i2c_lock();
    int result = -1;
    if (open_bus() == 0) {
        const Lt6911Chip chip = detect_chip();
        switch (chip) {
        case Lt6911Chip::C: result = c_write(data, size); break;
        case Lt6911Chip::UXC: result = uxc_write(data, size); break;
        case Lt6911Chip::D: result = d_flash_write_edid(kDFlashIO, data, size); break;
        default: result = -ENODEV; break;
        }
        result = finish_edid(chip, result);
        // C retains its previous RAM until power-cycled. Keep that value separate
        // from the newly saved permanent EDID, including across Core restarts.
        if (!result && chip == Lt6911Chip::C && g_shadow_chip == Lt6911Chip::C && g_active_valid) {
            std::memcpy(g_c_state.baseline, data, 256);
            if (!valid_shadow_edid(data) || save_c_state(g_c_state))
                g_shadow_supported.store(false, std::memory_order_release);
        } else if (!result && chip == Lt6911Chip::D) {
            // PCIe explicitly resets to the verified Flash value afterwards.
            // Cube/Lite keeps serving its old loaded value until power-cycled.
            if (pcie_variant()) {
                std::memcpy(g_active_edid, data, 256);
                g_active_valid = true;
            }
        } else {
            g_active_valid = false;
            if (chip == Lt6911Chip::C)
                g_shadow_supported.store(false, std::memory_order_release);
        }
        close_bus();
    }
    onekvm_lt6911_i2c_unlock();
    return result;
}

void invalidate_edid_after_reset(bool succeeded)
{
    // D has no RAM override. A successful reset reloads its verified Flash
    // baseline; retain that cache without another internal-register session.
    if (!succeeded || onekvm_lt6911_cached_chip() != ONEKVM_LT6911_D)
        g_active_valid = false;
}

int pcie_hdmi_reset()
{
    if (!pcie_variant())
        return 0;
    /* Match NanoKVM's Reset HDMI operation: GPIO451 is held low for a full
       second before it is enabled again. A 100 ms pulse is sufficient for
       HPD renegotiation but does not reliably reset the LT6911UXC MCU. */
    const int result = pcie_hdmi_reset_us(1000000,
        onekvm_lt6911_cached_chip() == ONEKVM_LT6911_UXC ? 250000 : 2500000);
    {
        onekvm_lt6911_i2c_lock();
        invalidate_edid_after_reset(result == 0);
        onekvm_lt6911_i2c_unlock();
    }
    return result;
}

int pcie_hdmi_startup_reset()
{
    if (!pcie_variant()) return 0;
    if (pcie_hdmi_reset_us(10000,
        onekvm_lt6911_cached_chip() == ONEKVM_LT6911_UXC ? 250000 : 2500000) == 0) {
        onekvm_lt6911_i2c_lock();
        invalidate_edid_after_reset(true);
        onekvm_lt6911_i2c_unlock();
        return 0;
    }
    return errno == EBUSY ? 0 : -1;
}

int persist_active_edid(const uint8_t *data, size_t size)
{
    if (data == nullptr || size == 0)
        return -1;
    (void)mkdir("/var/lib/onekvm", 0755);
    (void)mkdir("/var/lib/onekvm/edid", 0755);
    std::ofstream out(kActivePath, std::ios::binary | std::ios::trunc);
    if (!out)
        return -1;
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return out ? 0 : -1;
}

int restore_active_edid_if_needed()
{
    std::ifstream in(kActivePath, std::ios::binary);
    if (!in)
        return 0;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (data.size() != 256)
        return 0;
    const EdidBoardInfo info = probe_edid_board();
    if (info.board != NanoKVMBoard::PCIe || !info.writable)
        return 0;
    if (pcie_hdmi_reset() != 0)
        return -1;
    const int result = lt6911_edid_write(data.data(), data.size());
    if (result != 0)
        return result;
    return pcie_hdmi_reset();
}

} // namespace onekvm::video_backend
