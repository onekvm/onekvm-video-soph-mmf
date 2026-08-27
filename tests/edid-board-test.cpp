#include "lt6911_edid.hpp"

int main() {
    using onekvm::video_backend::parse_nanokvm_board;
    using onekvm::video_backend::NanoKVMBoard;
    if (parse_nanokvm_board("{\"variant\":\"pcie\"}") != NanoKVMBoard::PCIe)
        return 1;
    if (parse_nanokvm_board("{\"variant\":\"cube\"}") != NanoKVMBoard::Cube)
        return 2;
    if (parse_nanokvm_board("{\"variant\":\"lite\"}") != NanoKVMBoard::Lite)
        return 3;
    if (parse_nanokvm_board("{}") != NanoKVMBoard::Unknown)
        return 4;
    return 0;
}
