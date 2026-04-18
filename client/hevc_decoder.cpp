
#include "hevc_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

HEVCDecoder::HEVCDecoder() {}

HEVCDecoder::~HEVCDecoder() {
    cleanup();
}

bool HEVCDecoder::init(int width, int height) {
    av_log_set_level(AV_LOG_FATAL); // 只显示致命错误
    std::lock_guard<std::mutex> lock(mtx_);
    
    width_ = width;
    height_ = height;

    codec_ = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec_) return false;

    parser_ = av_parser_init(codec_->id);
    if (!parser_) return false;

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) return false;

    ctx_->width = width_;
    ctx_->height = height_;
    ctx_->thread_count = 4;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (avcodec_open2(ctx_, codec_, nullptr) < 0) return false;

    frame_ = av_frame_alloc();
    rgbFrame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();

    if (!frame_ || !rgbFrame_ || !pkt_) return false;

    rgbFrame_->format = AV_PIX_FMT_BGRA;
    rgbFrame_->width = width_;
    rgbFrame_->height = height_;
    av_frame_get_buffer(rgbFrame_, 32);

    initialized_ = true;
    return true;
}

void HEVCDecoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (parser_) { av_parser_close(parser_); parser_ = nullptr; }
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (pkt_) { av_packet_free(&pkt_); }
    if (rgbFrame_) { av_frame_free(&rgbFrame_); }
    if (frame_) { av_frame_free(&frame_); }
    if (ctx_) { avcodec_free_context(&ctx_); }
    
    initialized_ = false;
}

bool HEVCDecoder::decode(const uint8_t* data, int size, std::vector<uint8_t>& output) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (!initialized_) return false;

    output.clear();
    const uint8_t* p = data;
    int remaining = size;

    while (remaining > 0) {
        int parsed = av_parser_parse2(parser_, ctx_, &pkt_->data, &pkt_->size,
                                      p, remaining, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (parsed < 0) return false;
        p += parsed;
        remaining -= parsed;

        if (pkt_->size > 0) {
            int ret = avcodec_send_packet(ctx_, pkt_);
            if (ret < 0) continue;

            while (ret >= 0) {
                ret = avcodec_receive_frame(ctx_, frame_);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) return false;

                if (!swsCtx_ || frame_->width != width_ || frame_->height != height_) {
                    if (swsCtx_) sws_freeContext(swsCtx_);
                    width_ = frame_->width;
                    height_ = frame_->height;
                    swsCtx_ = sws_getContext(width_, height_, (AVPixelFormat)frame_->format,
                                             width_, height_, AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
                    av_frame_unref(rgbFrame_);
                    rgbFrame_->format = AV_PIX_FMT_BGRA;
                    rgbFrame_->width = width_;
                    rgbFrame_->height = height_;
                    rgbFrame_->linesize[0];
                    av_frame_get_buffer(rgbFrame_, 32);
                }

                sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, height_,
                          rgbFrame_->data, rgbFrame_->linesize);

                output.resize(width_ * height_ * 4);
                for (int y = 0; y < height_; y++) {
                    memcpy(output.data() + y * width_ * 4,
                           rgbFrame_->data[0] + y * rgbFrame_->linesize[0], width_ * 4);
                }

                av_frame_unref(frame_);
            }
        }
    }

    return !output.empty();
}
