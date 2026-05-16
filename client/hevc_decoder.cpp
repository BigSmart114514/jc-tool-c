
#include "hevc_decoder.h"
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/macros.h>
#include <libswscale/swscale.h>
}

HEVCDecoder::HEVCDecoder() {}

HEVCDecoder::~HEVCDecoder() {
    cleanup();
}

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    int hw_fmt = (int)(intptr_t)ctx->opaque;
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == (AVPixelFormat)hw_fmt)
            return *p;
    }
    return pix_fmts[0];
}

bool HEVCDecoder::initHwAccel() {
    const AVHWDeviceType types[] = {
        AV_HWDEVICE_TYPE_D3D11VA,
        AV_HWDEVICE_TYPE_DXVA2,
        AV_HWDEVICE_TYPE_NONE
    };

    for (int i = 0; types[i] != AV_HWDEVICE_TYPE_NONE; i++) {
        AVBufferRef* hw_dev = nullptr;
        if (av_hwdevice_ctx_create(&hw_dev, types[i], nullptr, nullptr, 0) < 0)
            continue;

        const AVCodecHWConfig* config = nullptr;
        for (int j = 0;; j++) {
            config = avcodec_get_hw_config(codec_, j);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                config->device_type == types[i]) {
                hwDeviceCtx_ = hw_dev;
                hwPixFmt_ = config->pix_fmt;
                ctx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
                ctx_->opaque = (void*)(intptr_t)hwPixFmt_;
                ctx_->get_format = get_hw_format;
                std::cout << "[Decoder] HW device created: "
                          << av_hwdevice_get_type_name(types[i])
                          << " pix_fmt=" << config->pix_fmt << std::endl;
                return true;
            }
        }
        av_buffer_unref(&hw_dev);
    }
    return false;
}

bool HEVCDecoder::init(int width, int height) {
    av_log_set_level(AV_LOG_WARNING);
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
    ctx_->coded_width = FFALIGN(width_, 16);
    ctx_->coded_height = FFALIGN(height_, 16);
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

    bool hwOk = initHwAccel();
    if (!hwOk) {
        ctx_->get_format = nullptr;
    }
    ctx_->thread_count = 4;

    if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
        if (hwOk) {
            std::cout << "[Decoder] HW codec open failed, falling back to software" << std::endl;
            avcodec_free_context(&ctx_);
            ctx_ = avcodec_alloc_context3(codec_);
            ctx_->width = width_;
            ctx_->height = height_;
            ctx_->coded_width = FFALIGN(width_, 16);
            ctx_->coded_height = FFALIGN(height_, 16);
            ctx_->thread_count = 4;
            ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
            av_buffer_unref(&hwDeviceCtx_);
            hwDeviceCtx_ = nullptr;
            hwPixFmt_ = 0;
            if (avcodec_open2(ctx_, codec_, nullptr) < 0) return false;
        } else {
            return false;
        }
    }

    if (hwOk)
        std::cout << "[Decoder] Using HW pixel format (ctx->pix_fmt="
                  << ctx_->pix_fmt << ")" << std::endl;
    else
        std::cout << "[Decoder] Using software decoder" << std::endl;

    frame_ = av_frame_alloc();
    rgbFrame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    swFrame_ = av_frame_alloc();

    if (!frame_ || !rgbFrame_ || !pkt_ || !swFrame_) return false;

    rgbFrame_->format = AV_PIX_FMT_BGRA;
    rgbFrame_->width = width_;
    rgbFrame_->height = height_;
    av_frame_get_buffer(rgbFrame_, 32);

    stride_ = rgbFrame_->linesize[0];
    if (stride_ == 0) stride_ = width_ * 4;

    initialized_ = true;
    return true;
}

void HEVCDecoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (swFrame_) { av_frame_free(&swFrame_); }
    if (parser_) { av_parser_close(parser_); parser_ = nullptr; }
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (pkt_) { av_packet_free(&pkt_); }
    if (rgbFrame_) { av_frame_free(&rgbFrame_); }
    if (frame_) { av_frame_free(&frame_); }
    if (ctx_) { avcodec_free_context(&ctx_); }
    if (hwDeviceCtx_) { av_buffer_unref(&hwDeviceCtx_); }

    hwPixFmt_ = 0;
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

                AVFrame* decoded = frame_;
                if (hwDeviceCtx_ && hwPixFmt_ && frame_->format == (AVPixelFormat)hwPixFmt_) {
                    av_frame_unref(swFrame_);
                    if (av_hwframe_transfer_data(swFrame_, frame_, 0) < 0)
                        return false;
                    decoded = swFrame_;
                }

                if (!swsCtx_ || decoded->width != width_ || decoded->height != height_) {
                    if (swsCtx_) sws_freeContext(swsCtx_);
                    width_ = decoded->width;
                    height_ = decoded->height;
                    swsCtx_ = sws_getContext(width_, height_, (AVPixelFormat)decoded->format,
                                             width_, height_, AV_PIX_FMT_BGRA,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
                    av_frame_unref(rgbFrame_);
                    rgbFrame_->format = AV_PIX_FMT_BGRA;
                    rgbFrame_->width = width_;
                    rgbFrame_->height = height_;
                    av_frame_get_buffer(rgbFrame_, 32);
                }

                sws_scale(swsCtx_, decoded->data, decoded->linesize, 0, height_,
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
