
#include "hevc_encoder.h"
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

HEVCEncoder::HEVCEncoder() {}

HEVCEncoder::~HEVCEncoder() {
    cleanup();
}

bool HEVCEncoder::init(int width, int height, int fps, int crf) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    width_ = width;
    height_ = height;

    const char* encoders[] = {"hevc_nvenc", "hevc_qsv", "hevc_amf", "libx265", nullptr};

    for (int i = 0; encoders[i]; i++) {
        codec_ = avcodec_find_encoder_by_name(encoders[i]);
        if (!codec_) continue;

        ctx_ = avcodec_alloc_context3(codec_);
        if (!ctx_) continue;

        ctx_->width = width_;
        ctx_->height = height_;
        ctx_->time_base = {1, fps};
        ctx_->framerate = {fps, 1};
        ctx_->pix_fmt = (strstr(encoders[i], "qsv") || strstr(encoders[i], "amf"))
                        ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        ctx_->gop_size = 120;
        ctx_->max_b_frames = 0;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (strcmp(encoders[i], "hevc_nvenc") == 0) {
            av_opt_set(ctx_->priv_data, "preset", "p1", 0);
            av_opt_set(ctx_->priv_data, "tune", "ll", 0);
            av_opt_set(ctx_->priv_data, "rc", "constqp", 0);
            av_opt_set_int(ctx_->priv_data, "qp", crf, 0);
        } else if (strcmp(encoders[i], "hevc_qsv") == 0) {
            av_opt_set(ctx_->priv_data, "preset", "veryfast", 0);
            ctx_->global_quality = crf;
        } else if (strcmp(encoders[i], "hevc_amf") == 0) {
            av_opt_set(ctx_->priv_data, "quality", "speed", 0);
            av_opt_set(ctx_->priv_data, "rc", "cqp", 0);
            av_opt_set_int(ctx_->priv_data, "qp_i", crf, 0);
        } else {
            ctx_->thread_count = 2;
            av_opt_set(ctx_->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx_->priv_data, "tune", "zerolatency", 0);
        }

        if (avcodec_open2(ctx_, codec_, nullptr) >= 0) {
            std::cout << "[Encoder] " << encoders[i] << std::endl;
            break;
        }
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
    }

    if (!ctx_) {
        std::cerr << "[Encoder] No HEVC encoder found" << std::endl;
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = ctx_->pix_fmt;
    frame_->width = width_;
    frame_->height = height_;
    av_frame_get_buffer(frame_, 32);

    pkt_ = av_packet_alloc();

    swsCtx_ = sws_getContext(width_, height_, AV_PIX_FMT_BGRA,
                              width_, height_, ctx_->pix_fmt,
                              SWS_POINT, nullptr, nullptr, nullptr);

    initialized_ = true;
    return true;
}

void HEVCEncoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (pkt_) { av_packet_free(&pkt_); }
    if (frame_) { av_frame_free(&frame_); }
    if (ctx_) { avcodec_free_context(&ctx_); }
    
    initialized_ = false;
}

bool HEVCEncoder::encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& output, bool keyframe) {
    std::lock_guard<std::mutex> lock(mtx_);
    
    if (!initialized_) return false;

    output.clear();
    if (av_frame_make_writable(frame_) < 0) return false;

    const uint8_t* src[1] = {bgra};
    int srcStride[1] = {width_ * 4};
    sws_scale(swsCtx_, src, srcStride, 0, height_, frame_->data, frame_->linesize);

    frame_->pts = pts;
    frame_->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (keyframe) frame_->flags |= AV_FRAME_FLAG_KEY;

    int ret = avcodec_send_frame(ctx_, frame_);
    if (ret < 0) return false;

    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) return false;
        output.insert(output.end(), pkt_->data, pkt_->data + pkt_->size);
        av_packet_unref(pkt_);
    }

    return true;
}
