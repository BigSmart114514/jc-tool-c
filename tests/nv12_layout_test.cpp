#include "../client/nv12_layout.h"

#include <cassert>

int main() {
    constexpr int width = 1920;
    constexpr int height = 1088;
    constexpr int paddedPitch = 2048;
    constexpr std::size_t packedBytes = 3133440;
    constexpr std::size_t paddedBytes = 3342336;

    const auto packed = linearNv12Layout(width, height, 0, packedBytes);
    assert(packed.pitch == width && packed.uvOffset == 2088960 && packed.size == packedBytes);
    const auto padded = linearNv12Layout(width, height, paddedPitch, paddedBytes);
    assert(padded.pitch == paddedPitch && padded.uvOffset == 2228224 && padded.size == paddedBytes);

    // Never show a partial Y plane or a chroma plane read from the wrong offset.
    assert(!linearNv12Layout(width, height, 0, 0).size);
    assert(!linearNv12Layout(width, height, 0, packed.uvOffset - 1).size);
    assert(!linearNv12Layout(width, height, 0, packed.uvOffset).size);
    assert(!linearNv12Layout(width, height, 0, packedBytes - 1).size);
    assert(!linearNv12Layout(width, height, paddedPitch, packedBytes).size);
    assert(!linearNv12Layout(width, height, 0, paddedBytes).size);
    assert(!linearNv12Layout(width, height, 0, packedBytes + width * 64).size);
    assert(!linearNv12Layout(width, height, -width, packedBytes).size);
    assert(!linearNv12Layout(width, height, width - 2, packedBytes).size);
    assert(!nv12Layout(1439, 816, 1440).size);
    assert(!nv12Layout(1440, 815, 1440).size);

    const auto resized = linearNv12Layout(1440, 816, 1440, 1762560);
    assert(resized.pitch == 1440 && resized.uvOffset == 1175040 && resized.size == 1762560);
}
