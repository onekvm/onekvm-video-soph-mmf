#pragma once

#include <cstdint>
#include <limits>

namespace onekvm {

// The bypass DMA writes packed 422 with a 16-byte line alignment. Never
// derive this stride from an NV21 descriptor supplied by older VI drivers.
inline uint32_t vi_packed_stride(uint32_t width, uint32_t height)
{
    const uint64_t stride = (uint64_t(width) * 2u + 15u) & ~uint64_t(15u);
    if (!width || !height || (width & 1u) || (height & 1u) ||
        stride > std::numeric_limits<uint32_t>::max() / height)
        return 0;
    return static_cast<uint32_t>(stride);
}

// Decimate raw capture frames before encoding, preserving every encoded
// reference frame. TimeRef is the VI capture sequence, not an AU sequence.
inline bool keep_vi_frame(uint32_t reference, int source_fps, int target_fps)
{
    if (target_fps <= 0) return false;
    if (source_fps <= 0 || target_fps >= source_fps) return true;
    const uint64_t phase = reference % static_cast<uint32_t>(source_fps);
    return phase == 0 || phase * target_fps / source_fps !=
        (phase - 1u) * target_fps / source_fps;
}

} // namespace onekvm
