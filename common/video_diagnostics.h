#pragma once

#include <cstddef>
#include <cstdint>

// Log-only fingerprint for matching the same compressed frame at both ends.
inline std::uint64_t videoFingerprint(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
