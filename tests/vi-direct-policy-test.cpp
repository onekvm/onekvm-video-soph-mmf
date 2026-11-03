#include "vi_direct_policy.hpp"
#include <cassert>
#include <cstdint>
#include <initializer_list>

int main()
{
    // The VI DMA pitch differs from the common VB allocation's alignment at
    // 720 pixels. This boundary previously produced a diagonally torn image.
    assert(onekvm::vi_packed_stride(720, 480) == 1440);
    assert(onekvm::vi_packed_stride(1920, 1080) == 3840);
    assert(onekvm::vi_packed_stride(0, 1080) == 0);
    assert(onekvm::vi_packed_stride(1921, 1080) == 0);
    assert(onekvm::vi_packed_stride(UINT32_MAX - 1, UINT32_MAX - 1) == 0);
    for (int source : {60, 90, 120}) {
        for (int target : {15, 30, 45, 60, 90, 120}) {
            int accepted = 0;
            for (uint32_t ref = 0; ref < static_cast<uint32_t>(source * 10); ++ref)
                accepted += onekvm::keep_vi_frame(ref, source, target);
            assert(accepted == (target < source ? target : source) * 10);
        }
    }
    assert(onekvm::keep_vi_frame(UINT32_MAX, 90, 90));
    assert(onekvm::keep_vi_frame(0, 90, 90));
    assert(!onekvm::keep_vi_frame(1, 90, 0));
}
