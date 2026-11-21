#pragma once

#include <cstddef>
#include <cstdint>

struct H264NalTypes {
    bool idr = false;
    bool sps = false;
    bool pps = false;
};

inline H264NalTypes annexBNalTypes(const uint8_t* data, std::size_t size) {
    H264NalTypes types;
    // Both three- and four-byte start codes end in 00 00 01.
    for (std::size_t i = 3; i < size; ++i) {
        if (data[i - 3] != 0 || data[i - 2] != 0 || data[i - 1] != 1) continue;
        const auto type = data[i] & 0x1f;
        types.idr |= type == 5;
        types.sps |= type == 7;
        types.pps |= type == 8;
        if (types.idr && types.sps && types.pps) break;
    }
    return types;
}
