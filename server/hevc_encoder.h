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

    // 【修改】增加源宽高的参数
    bool init(int srcWidth, int srcHeight, int dstWidth, int dstHeight, int fps, int crf);
    void cleanup();
    
    bool encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& output, bool keyframe = false);

private:
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* swsCtx_ = nullptr;
    
    // 【新增】保存原始输入宽高的变量
    int srcWidth_ = 0;
    int srcHeight_ = 0;

    int width_ = 0; // 目标编码宽度
    int height_ = 0; // 目标编码高度
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif // HEVC_ENCODER_H