#include "../src/lt6911_info.hpp"

#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

using namespace onekvm::video_backend;

static std::string read_text(const std::string &path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int main()
{
    char root[] = "/tmp/lt6911-info-test.XXXXXX";
    assert(mkdtemp(root));
    const std::string directory = std::string(root) + "/lt6911_info";
    EdidBoardInfo info;
    assert(publish_lt6911_info(info, directory.c_str()));
    assert(read_text(directory + "/chip_id") == "unknown\n");
    info.chip = Lt6911Chip::UXC;
    info.board_id = "pcie";
    assert(publish_lt6911_info(info, directory.c_str()));
    assert(read_text(directory + "/chip_id") == "lt6911uxc\n");
    assert(read_text(directory + "/board_id") == "pcie\n");
    assert(read_text(directory + "/driver") == "soph-mmf\n");
    info.chip = Lt6911Chip::C;
    info.board_id = "cube";
    assert(publish_lt6911_info(info, directory.c_str()));
    assert(read_text(directory + "/chip_id") == "lt6911c\n");
    info.chip = Lt6911Chip::D;
    assert(publish_lt6911_info(info, directory.c_str()));
    assert(read_text(directory + "/chip_id") == "lt6911d\n");
    struct stat st;
    assert(stat((directory + "/chip_id").c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0444);
    /* Refuse redirected directories; never modify a symlink's target. */
    const std::string link = std::string(root) + "/link";
    assert(symlink(directory.c_str(), link.c_str()) == 0);
    assert(!publish_lt6911_info(info, link.c_str()));
    unlink(link.c_str());
    for (const char *name : {"chip_id", "board_id", "driver"})
        assert(unlink((directory + "/" + name).c_str()) == 0);
    assert(rmdir(directory.c_str()) == 0 && rmdir(root) == 0);
    puts("LT6911 cached diagnostic info tests passed");
}
