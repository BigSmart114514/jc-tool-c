
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
    // 从解码器的实际输出类型重新读取帧尺寸。
    // 解码器的输出尺寸不一定等于我们请求的尺寸（H.264 有宏块/对齐 padding），
    // 以前用请求尺寸去算 stride，行宽一错整帧就滑坡。
    bool reconfigureOutput();
    void cleanupLocked();

    static void nv12ToBgra(const uint8_t* yPlane, int pitchY,
                           const uint8_t* uvPlane, int pitchUV,
                           int w, int h, uint8_t* bgra, int outW);

    IMFTransform* decoder_ = nullptr;
    IMFMediaType* inputType_ = nullptr;
    IMFMediaType* outputType_ = nullptr;

    int width_ = 0;          // 显示/裁剪尺寸（客户端实际要的尺寸）
    int height_ = 0;
    int alignedW_ = 0;
    int alignedH_ = 0;
    int stride_ = 0;

    int frameW_ = 0;         // 解码器真实输出帧尺寸
    int frameH_ = 0;
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif // MEDIA_DECODER_H
