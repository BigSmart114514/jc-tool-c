#include "../server/h264_nal.h"

#include <cassert>

int main() {
    constexpr uint8_t complete[] = {0, 0, 0, 1, 0x67, 0x01, 0, 0, 1, 0x68, 0x01, 0, 0, 0, 1, 0x65};
    const auto keyframe = annexBNalTypes(complete, sizeof(complete));
    assert(keyframe.idr && keyframe.sps && keyframe.pps);
    const auto header = annexBNalTypes(complete, sizeof(complete) - 1);
    assert(!header.idr && header.sps && header.pps);
    constexpr uint8_t delta[] = {0, 0, 1, 0x41, 0, 0, 3, 1, 0x65};
    const auto nonKey = annexBNalTypes(delta, sizeof(delta));
    assert(!nonKey.idr && !nonKey.sps && !nonKey.pps);
    const auto empty = annexBNalTypes(nullptr, 0);
    assert(!empty.idr && !empty.sps && !empty.pps);
}
