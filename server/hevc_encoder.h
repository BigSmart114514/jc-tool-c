
#ifndef HEVC_ENCODER_H
#define HEVC_ENCODER_H

#include <vector>
#include <mutex>
#include <cstdint>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class HEVCEncoder {
public:
    HEVCEncoder();
    ~HEVCEncoder();

    bool init(int width, int height, int fps, int crf);
    void cleanup();
    
    bool encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& output, bool keyframe = false);

private:
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    
    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif // HEVC_ENCODER_H
