
#ifndef HEVC_DECODER_H
#define HEVC_DECODER_H

#include <vector>
#include <mutex>
#include <cstdint>

struct AVCodec;
struct AVCodecContext;
struct AVCodecParserContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class HEVCDecoder {
public:
    HEVCDecoder();
    ~HEVCDecoder();

    bool init(int width, int height);
    void cleanup();
    
    bool decode(const uint8_t* data, int size, std::vector<uint8_t>& output);
    
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    int getStride() const { return stride_; }

private:
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVCodecParserContext* parser_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* rgbFrame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif // HEVC_DECODER_H
