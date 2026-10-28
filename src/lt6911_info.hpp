#pragma once

#include "lt6911_edid.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace onekvm::video_backend {

/* Publish only the completed probe's cache. Readers never access I2C or flash.
 * /run is per-boot RAM; retain the last probe after Core exits for ADB/SSH. */
inline bool publish_lt6911_info(const EdidBoardInfo &info,
                               const char *directory = "/run/onekvm/lt6911_info")
{
    if (mkdir(directory, 0755) != 0 && errno != EEXIST)
        return false;
    struct stat st;
    if (lstat(directory, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
        return false;
    const auto write_text = [directory](const char *name, const char *value) {
        const std::string target = std::string(directory) + "/" + name;
        const std::string pattern = target + ".XXXXXX";
        std::vector<char> temporary(pattern.begin(), pattern.end());
        temporary.push_back('\0');
        const int fd = mkstemp(temporary.data());
        if (fd < 0)
            return false;
        const std::string text = std::string(value) + "\n";
        size_t offset = 0;
        bool ok = fchmod(fd, 0444) == 0;
        while (ok && offset < text.size()) {
            const ssize_t n = write(fd, text.data() + offset, text.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) { ok = false; break; }
            offset += static_cast<size_t>(n);
        }
        if (close(fd) != 0)
            ok = false;
        if (ok && rename(temporary.data(), target.c_str()) != 0)
            ok = false;
        if (!ok)
            unlink(temporary.data());
        return ok;
    };
    const char *chip = "unknown";
    if (info.chip == Lt6911Chip::C) chip = "lt6911c";
    if (info.chip == Lt6911Chip::UXC) chip = "lt6911uxc";
    if (info.chip == Lt6911Chip::D) chip = "lt6911d";
    return write_text("chip_id", chip) &&
           write_text("board_id", info.board_id) &&
           write_text("driver", "soph-mmf");
}

} // namespace onekvm::video_backend
