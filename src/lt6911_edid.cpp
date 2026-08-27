#include "lt6911_edid.hpp"

#include "mmf.hpp"
#include "onekvm_video_backend_internal.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace onekvm::video_backend {
namespace {

constexpr const char *kI2CDevice = "/dev/i2c-4";
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
uint8_t g_bank = 0xff;

int write_raw(const uint8_t *data, size_t len)
{
    return write(g_fd, data, len) == (ssize_t)len ? 0 : -1;
}

int set_bank(uint8_t bank)
{
    if (bank == g_bank)
        return 0;
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
    return read(g_fd, data, len) == (ssize_t)len ? 0 : -1;
}

int open_bus()
{
    g_fd = open(kI2CDevice, O_RDWR | O_CLOEXEC);
    if (g_fd < 0)
        return -1;
    if (ioctl(g_fd, I2C_SLAVE_FORCE, kI2CAddress) < 0) {
        close(g_fd);
        g_fd = -1;
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

int uxc_read(uint8_t *edid, size_t size)
{
    const uint8_t chunks = static_cast<uint8_t>(size / kUXCChunk);
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x84) || write_reg(kSys, 0x5a, 0x80))
        return -1;
    for (uint8_t i = 0; i < chunks; i++) {
        if (write_reg(kSys, 0x5e, 0x5f) || write_reg(kSys, 0x5a, 0xa0) ||
            write_reg(kSys, 0x5a, 0x80) || write_reg(kSys, 0x5b, 0x01) ||
            write_reg(kSys, 0x5c, 0x80) ||
            write_reg(kSys, 0x5d, static_cast<uint8_t>(kUXCChunk * i)) ||
            write_reg(kSys, 0x5a, 0x90) || write_reg(kSys, 0x5a, 0x80) ||
            write_reg(kSys, 0x58, 0x21) ||
            read_regs(kSys, 0x5f, edid + kUXCChunk * i, kUXCChunk))
            return -1;
    }
    return 0;
}

int c_write(const uint8_t *edid, size_t size)
{
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
    }
    if (write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5a, 0x8a) ||
        write_reg(kSys, 0x5a, 0x82))
        return -1;
    if (read_regs(kSys2, 0x02, &status, 1) != 0 || status != 0xff)
        return -1;
    return write_reg(kSys2, 0x02, 0xdf) || write_reg(kSys2, 0x02, 0xff) ? -1 : 0;
}

int c_read(uint8_t *edid, size_t size)
{
    const uint8_t chunks = static_cast<uint8_t>(size / kCChunk);
    if (write_reg(kSys, 0xff, 0x80) || enable() ||
        write_reg(kSys, 0x5a, 0x86) || write_reg(kSys, 0x5a, 0x82))
        return -1;
    for (uint8_t i = 0; i < chunks; i++) {
        if (write_reg(kSys, 0x5e, 0x6f) || write_reg(kSys, 0x5a, 0xa2) ||
            write_reg(kSys, 0x5a, 0x82) || write_reg(kSys, 0x5b, 0x01) ||
            write_reg(kSys, 0x5c, 0x80) ||
            write_reg(kSys, 0x5d, static_cast<uint8_t>(kCChunk * i)) ||
            write_reg(kSys, 0x5a, 0x92) || write_reg(kSys, 0x5a, 0x82) ||
            write_reg(kSys, 0x58, 0x01) ||
            read_regs(kSys, 0x5f, edid + kCChunk * i, kCChunk))
            return -1;
    }
    return 0;
}

Lt6911Chip detect_chip()
{
    uint8_t id[2] = {};
    if (write_reg(kSys, 0xff, 0x80) || enable())
        return Lt6911Chip::Unknown;
    if (read_regs(kSys4, 0x00, id, 2) == 0 && id[0] == 0x16 && id[1] == 0x05)
        return Lt6911Chip::C;
    return Lt6911Chip::UXC;
}

int gpio_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    const int ok = write(fd, value, std::strlen(value)) > 0 ? 0 : -1;
    close(fd);
    return ok;
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
    case Lt6911Chip::UXC:
        info.chip_id = "LT6911UXC";
        info.writable = info.board != NanoKVMBoard::Unknown;
        break;
    default:
        info.chip_id = "";
        info.writable = false;
        break;
    }
    return info;
}

int lt6911_edid_read(uint8_t *data, size_t size)
{
    if (data == nullptr || size != 256)
        return -1;
    onekvm_lt6911_i2c_lock();
    int result = -1;
    if (open_bus() == 0) {
        const Lt6911Chip chip = detect_chip();
        result = chip == Lt6911Chip::C ? c_read(data, size) : uxc_read(data, size);
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
        result = chip == Lt6911Chip::C ? c_write(data, size) : uxc_write(data, size);
        close_bus();
    }
    onekvm_lt6911_i2c_unlock();
    return result;
}

int pcie_hdmi_reset()
{
    (void)gpio_write("/sys/class/gpio/export", "451");
    (void)gpio_write("/sys/class/gpio/gpio451/direction", "out");
    if (gpio_write("/sys/class/gpio/gpio451/value", "0") != 0)
        return -1;
    usleep(100000);
    if (gpio_write("/sys/class/gpio/gpio451/value", "1") != 0)
        return -1;
    usleep(100000);
    return 0;
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
    (void)pcie_hdmi_reset();
    const int result = lt6911_edid_write(data.data(), data.size());
    (void)pcie_hdmi_reset();
    return result;
}

} // namespace onekvm::video_backend
