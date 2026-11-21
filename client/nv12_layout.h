#pragma once

#include <cstddef>
#include <limits>

struct Nv12Layout {
    std::size_t pitch;
    std::size_t uvOffset;
    std::size_t size;
};

inline Nv12Layout nv12Layout(int width, int height, int pitch) {
    if (width <= 0 || height <= 0 || (width % 2) || (height % 2) ||
        pitch < width || (pitch % 2)) return {};
    const auto rows = std::size_t(height) + std::size_t(height) / 2;
    const auto stride = std::size_t(pitch);
    if (stride > (std::numeric_limits<std::size_t>::max)() / rows) return {};
    return {stride, stride * std::size_t(height), stride * rows};
}

inline Nv12Layout linearNv12Layout(int width, int height, int declaredStride, std::size_t bytes) {
    // With no declared stride the standard linear NV12 layout is tightly packed.
    // In particular, a truncated padded sample must not be reinterpreted as packed.
    const auto expected = nv12Layout(width, height, declaredStride ? declaredStride : width);
    return bytes == expected.size ? expected : Nv12Layout{};
}
