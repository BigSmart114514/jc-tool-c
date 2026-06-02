
#ifndef MEDIA_DECODER_H
#define MEDIA_DECODER_H

#include <vector>
#include <mutex>
#include <cstdint>

struct IMFTransform;
struct IMFMediaType;
struct IMFSample;

class MediaDecoder {
public:
    MediaDecoder();
    ~MediaDecoder();

    bool init(int width, int height);
    void cleanup();
    bool decode(const uint8_t* data, int size, std::vector<uint8_t>& bgraOut);

    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    int getStride() const { return stride_; }

private:
    bool initDecoder();
    bool processOutput(std::vector<uint8_t>& bgraOut);
    static void nv12ToBgra(const uint8_t* nv12, uint8_t* bgra, int w, int h, int strideY, int strideUV, int alignedH);

    IMFTransform* decoder_ = nullptr;
    IMFMediaType* inputType_ = nullptr;
    IMFMediaType* outputType_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int alignedW_ = 0;
    int alignedH_ = 0;
    int stride_ = 0;
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif // MEDIA_DECODER_H
