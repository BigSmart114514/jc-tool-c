
#include "media_decoder.h"
#define NOMINMAX
#include <iostream>
#include <algorithm>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <mfobjects.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

// {62CE7E72-4C71-4D20-B15D-45283A99B03B}
static const GUID CLSID_H264DecoderMFT =
    {0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x3A, 0x99, 0xB0, 0x3B}};

void MediaDecoder::nv12ToBgra(const uint8_t* yPlane, int pitchY,
                              const uint8_t* uvPlane, int pitchUV,
                              int w, int h, uint8_t* bgra, int outW) {
    int halfW = (w + 1) / 2;
    int halfH = (h + 1) / 2;

    for (int y = 0; y < h; y++) {
        const uint8_t* yRow = yPlane + (size_t)y * pitchY;
        const uint8_t* uvRow0 = uvPlane ? uvPlane + (size_t)(y / 2) * pitchUV : nullptr;
        const uint8_t* uvRow1 = uvPlane
            ? uvPlane + (size_t)std::min(y / 2 + 1, halfH - 1) * pitchUV : nullptr;
        bool oddRow = (y & 1) != 0;

        for (int x = 0; x < w; x++) {
            uint8_t Y = yRow[x];

            // UV 平面不可用（buffer 不完整）时按中性色处理，只显示亮度，
            // 绝不越界读。
            float U = 128.0f, V = 128.0f;
            if (uvPlane) {
                int ux = x / 2;
                int ux1 = std::min(ux + 1, halfW - 1);
                float fx = (x & 1) * 0.5f;
                int o00 = ux * 2, o10 = ux1 * 2;

                // 偶数行用当前 UV 行，奇数行与下一 UV 行做垂直插值
                const uint8_t* ra = oddRow ? uvRow1 : uvRow0;
                U = (1.0f - fx) * ra[o00] + fx * ra[o10];
                V = (1.0f - fx) * ra[o00 + 1] + fx * ra[o10 + 1];
                if (oddRow && y + 1 < h) {
                    float U0 = (1.0f - fx) * uvRow0[o00] + fx * uvRow0[o10];
                    float V0 = (1.0f - fx) * uvRow0[o00 + 1] + fx * uvRow0[o10 + 1];
                    U = (U0 + U) * 0.5f;
                    V = (V0 + V) * 0.5f;
                }
            }

            int C = Y - 16;
            int D = (int)(U + 0.5f) - 128;
            int E = (int)(V + 0.5f) - 128;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            R = std::max(0, std::min(255, R));
            G = std::max(0, std::min(255, G));
            B = std::max(0, std::min(255, B));

            uint8_t* px = bgra + ((size_t)y * outW + x) * 4;
            px[0] = (uint8_t)B;
            px[1] = (uint8_t)G;
            px[2] = (uint8_t)R;
            px[3] = 255;
        }
    }
}

MediaDecoder::MediaDecoder() {}

MediaDecoder::~MediaDecoder() {
    cleanup();
}

bool MediaDecoder::init(int width, int height) {
    std::lock_guard<std::mutex> lock(mtx_);

    width_ = width;
    height_ = height;
    alignedW_ = (width + 15) & ~15;
    alignedH_ = (height + 15) & ~15;
    frameW_ = alignedW_;
    frameH_ = alignedH_;
    // 输出 BGRA 是紧凑排列（每行恰好 width*4），不是 aligned 宽度。
    // 以前返回 alignedW_*4，任何用 getStride() 的地方都会读错行。
    stride_ = width_ * 4;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return false;

    if (!initDecoder()) {
        std::cerr << "[MediaDecoder] Decoder init failed" << std::endl;
        cleanupLocked();
        return false;
    }

    initialized_ = true;
    std::cout << "[MediaDecoder] H.264 decoder initialized: "
              << width_ << "x" << height_ << std::endl;
    return true;
}

bool MediaDecoder::initDecoder() {
    HRESULT hr;
    decoder_ = nullptr;

    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &inputInfo, &outputInfo, &activates, &count);

    if (SUCCEEDED(hr) && count > 0) {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&decoder_));
        if (SUCCEEDED(hr))
            std::cout << "[MediaDecoder] Found H.264 decoder via MFTEnumEx" << std::endl;
    }

    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (!decoder_) {
        hr = CoCreateInstance(CLSID_H264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&decoder_));
    }

    if (!decoder_) {
        std::cerr << "[MediaDecoder] No H.264 decoder found" << std::endl;
        return false;
    }

    IMFAttributes* mftAttr = nullptr;
    if (SUCCEEDED(decoder_->GetAttributes(&mftAttr))) {
        mftAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
        mftAttr->Release();
        std::cout << "[MediaDecoder] MF_LOW_LATENCY set on MFT attributes" << std::endl;
    }

    hr = MFCreateMediaType(&inputType_);
    if (FAILED(hr)) return false;
    inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType_, MF_MT_FRAME_SIZE, alignedW_, alignedH_);
    inputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = decoder_->SetInputType(0, inputType_, 0);
    if (FAILED(hr)) {
        std::cerr << "[MediaDecoder] SetInputType failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = MFCreateMediaType(&outputType_);
    if (FAILED(hr)) return false;
    outputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(outputType_, MF_MT_FRAME_SIZE, alignedW_, alignedH_);
    outputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType_->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    outputType_->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);

    hr = decoder_->SetOutputType(0, outputType_, 0);
    if (FAILED(hr)) {
        // 部分硬件解码器不接受我们指定的尺寸，退而用解码器给出的类型
        std::cerr << "[MediaDecoder] SetOutputType(aligned) failed: 0x"
                  << std::hex << hr << std::dec << ", falling back" << std::endl;
        IMFMediaType* avail = nullptr;
        for (DWORD i = 0; SUCCEEDED(decoder_->GetOutputAvailableType(0, i, &avail)); ++i) {
            GUID sub = GUID_NULL;
            avail->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                hr = decoder_->SetOutputType(0, avail, 0);
                avail->Release();
                if (SUCCEEDED(hr)) break;
            } else {
                avail->Release();
            }
            avail = nullptr;
        }
        if (FAILED(hr)) {
            std::cerr << "[MediaDecoder] No usable NV12 output type" << std::endl;
            return false;
        }
    }

    // 读取解码器实际接受的帧尺寸，不要相信我们自己请求的那个值
    reconfigureOutput();

    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(decoder_->QueryInterface(IID_PPV_ARGS(&codecApi)))) {
        VARIANT var;
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        codecApi->SetValue(&CODECAPI_AVDecVideoAcceleration_H264, &var);
        var.vt = VT_UI4;
        var.ulVal = (ULONG)alignedW_;
        codecApi->SetValue(&CODECAPI_AVDecVideoMaxCodedWidth, &var);
        var.ulVal = (ULONG)alignedH_;
        codecApi->SetValue(&CODECAPI_AVDecVideoMaxCodedHeight, &var);
        codecApi->Release();
        std::cout << "[MediaDecoder] Low-latency + HW acceleration enabled" << std::endl;
    } else {
        std::cout << "[MediaDecoder] ICodecAPI not supported" << std::endl;
    }

    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;
    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return false;

    return true;
}

// 从解码器当前输出类型读取真实帧尺寸。
bool MediaDecoder::reconfigureOutput() {
    if (!decoder_) return false;

    IMFMediaType* cur = nullptr;
    if (FAILED(decoder_->GetOutputCurrentType(0, &cur)) || !cur) {
        // 还没协商出输出类型，保持请求值
        return false;
    }

    UINT32 fw = 0, fh = 0;
    if (SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &fw, &fh)) && fw > 0 && fh > 0) {
        if ((int)fw != frameW_ || (int)fh != frameH_) {
            std::cout << "[MediaDecoder] actual output frame size: "
                      << fw << "x" << fh
                      << " (requested " << alignedW_ << "x" << alignedH_ << ")"
                      << std::endl;
        }
        frameW_ = (int)fw;
        frameH_ = (int)fh;
    }
    cur->Release();
    return true;
}

bool MediaDecoder::decode(const uint8_t* data, int size, std::vector<uint8_t>& bgraOut) {
    std::lock_guard<std::mutex> lock(mtx_);
    bgraOut.clear();

    if (!initialized_ || size < 4) return false;

    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* buf = nullptr;
    hr = MFCreateMemoryBuffer((DWORD)size, &buf);
    if (FAILED(hr)) { sample->Release(); return false; }

    BYTE* dataPtr = nullptr;
    hr = buf->Lock(&dataPtr, nullptr, nullptr);
    if (FAILED(hr)) {
        buf->Release();
        sample->Release();
        return false;
    }
    memcpy(dataPtr, data, size);
    buf->Unlock();
    hr = buf->SetCurrentLength((DWORD)size);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    buf->Release();
    if (FAILED(hr)) { sample->Release(); return false; }
    sample->SetSampleTime(0);

    hr = decoder_->ProcessInput(0, sample, 0);
    bool gotOutput = false;
    if (hr == MF_E_NOTACCEPTING) {
        gotOutput = processOutput(bgraOut);
        hr = decoder_->ProcessInput(0, sample, 0);
    }
    sample->Release();

    // STREAM_CHANGE 表示解码器要从 SPS/PPS 重新协商输出格式，
    // 以前直接当错误返回，于是客户端只会疯狂发关键帧请求、永远黑屏。
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        std::cout << "[MediaDecoder] stream change requested, renegotiating output type" << std::endl;
        IMFMediaType* avail = nullptr;
        for (DWORD i = 0; SUCCEEDED(decoder_->GetOutputAvailableType(0, i, &avail)); ++i) {
            GUID sub = GUID_NULL;
            avail->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12 && SUCCEEDED(decoder_->SetOutputType(0, avail, 0))) {
                avail->Release();
                break;
            }
            avail->Release();
            avail = nullptr;
        }
        reconfigureOutput();
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        return false;
    }

    if (FAILED(hr)) {
        std::cerr << "[MediaDecoder] ProcessInput failed: 0x" << std::hex << hr << std::dec << std::endl;
        return gotOutput;
    }

    return processOutput(bgraOut) || gotOutput;
}

bool MediaDecoder::processOutput(std::vector<uint8_t>& bgraOut) {
    bool gotOutput = false;
    int loopCount = 0;
    int tooSmallCount = 0;

    while (loopCount < 16) {
        loopCount++;

        IMFSample* userSample = nullptr;
        MFCreateSample(&userSample);
        IMFMediaBuffer* userBuf = nullptr;
        // 按解码器真实帧尺寸申请，别用请求尺寸（可能偏小导致 BUFFERTOOSMALL 死循环）
        size_t bufSize = (size_t)frameW_ * frameH_ * 3 / 2 + frameW_ * 64;
        MFCreateMemoryBuffer((DWORD)bufSize, &userBuf);
        if (userSample && userBuf) {
            userSample->AddBuffer(userBuf);
            userBuf->Release();
        }

        MFT_OUTPUT_DATA_BUFFER outputData = {};
        outputData.dwStreamID = 0;
        outputData.pSample = userSample;
        outputData.dwStatus = 0;

        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &outputData, &status);
        if (outputData.pEvents) outputData.pEvents->Release();

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (userSample) userSample->Release();
            std::cout << "[MediaDecoder] STREAM_CHANGE during ProcessOutput" << std::endl;
            IMFMediaType* avail = nullptr;
            bool set = false;
            for (DWORD i = 0; SUCCEEDED(decoder_->GetOutputAvailableType(0, i, &avail)); ++i) {
                GUID sub = GUID_NULL;
                avail->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12 && SUCCEEDED(decoder_->SetOutputType(0, avail, 0))) {
                    avail->Release();
                    set = true;
                    break;
                }
                avail->Release();
                avail = nullptr;
            }
            reconfigureOutput();
            if (!set) break;
            continue;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (userSample) userSample->Release();
            break;
        }

        if (hr == MF_E_BUFFERTOOSMALL) {
            if (userSample) userSample->Release();
            reconfigureOutput();
            if (++tooSmallCount > 3) {
                // 连续三次都装不下：解码器要的尺寸和 MF_MT_FRAME_SIZE 报的不一致，
                // 继续重试只会空转。
                std::cerr << "[MediaDecoder] MF_E_BUFFERTOOSMALL persists, giving up this frame" << std::endl;
                break;
            }
            continue;
        }

        if (FAILED(hr)) {
            std::cerr << "[MediaDecoder] ProcessOutput hr=0x" << std::hex << hr << std::dec << std::endl;
            if (userSample) userSample->Release();
            break;
        }

        IMFSample* resultSample = outputData.pSample;
        if (!resultSample) {
            if (userSample) userSample->Release();
            break;
        }

        IMFMediaBuffer* buf = nullptr;
        hr = resultSample->GetBufferByIndex(0, &buf);
        if (SUCCEEDED(hr)) {
            BYTE* bufPtr = nullptr;
            LONG pitch = 0;
            DWORD curLen = 0;
            bool locked = false;
            bool got2d = false;

            // 关键：优先用 IMF2DBuffer 拿真实行 pitch。
            // 硬件/网络解码器输出的行宽通常是 128/256 对齐，
            // 跟 width 16 对齐推算出来的值根本不一样，
            // 一旦用错，越往下偏移越大，整帧斜着滑出去 —— 就是花屏。
            IMF2DBuffer* buf2d = nullptr;
            if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d))) && buf2d) {
                const HRESULT lockResult = buf2d->Lock2D(&bufPtr, &pitch);
                if (SUCCEEDED(lockResult) && pitch > 0) {
                    locked = true;
                    got2d = true;
                    DWORD contig = 0;
                    if (SUCCEEDED(buf2d->GetContiguousLength(&contig)) && contig > 0) {
                        curLen = contig;
                    } else {
                        curLen = (DWORD)((size_t)pitch * frameH_ * 3 / 2);
                    }
                } else {
                    if (SUCCEEDED(lockResult)) buf2d->Unlock2D();
                    buf2d->Release();
                    buf2d = nullptr;
                }
            }

            if (!locked) {
                // 空样本不包含像素，不能拿分配容量冒充有效长度。
                hr = buf->Lock(&bufPtr, nullptr, &curLen);
                locked = SUCCEEDED(hr);
            }

            // GetContiguousLength 有时返回的是整个分配容量（我们自己
            // MFCreateMemoryBuffer 时多要了余量），夹到 NV12 最大可能长度。
            if (got2d && pitch > 0 && frameH_ > 0) {
                size_t maxA = (size_t)pitch * (size_t)frameH_ * 3 / 2;
                if ((size_t)curLen > maxA) curLen = (DWORD)maxA;
            }

            // 用“有效数据长度”校正行跨距。
            // NV12 只有两种常见排布：
            //   A) Y、UV 共用同一 pitch  → 总长 = pitch * H * 3/2
            //   B) UV 紧凑（pitch=帧宽）  → 总长 = pitch * H + ceil(H/2) * W
            // 两种候选都算出来，取与实际长度吻合的那个。2D 给的 pitch 也要
            // 通过这道校验，否则布局判断会全盘跑偏（连 UV 都会被判成不存在）。
            if (locked && curLen > 0 && frameH_ > 0 && frameW_ > 0) {
                size_t L = (size_t)curLen;
                size_t h = (size_t)frameH_;
                size_t w = (size_t)frameW_;
                auto fits = [&](int p) -> bool {
                    if (p <= 0 || (size_t)p < w) return false;
                    size_t a = (size_t)p * h * 3 / 2;
                    size_t b = (size_t)p * h + ((h + 1) / 2) * w;
                    // 不能叫 near：windows.h 里有历史遗留的 near/far 宏
                    auto lenIsClose = [&](size_t v) {
                        size_t d = v > L ? v - L : L - v;
                        return d <= 16;
                    };
                    return lenIsClose(a) || lenIsClose(b);
                };
                int candA = (int)(L * 2 / (h * 3));
                int candB = (int)((L > ((h + 1) / 2) * w
                                   ? L - ((h + 1) / 2) * w : 0) / h);
                int chosen = (int)pitch;
                if (!fits(chosen)) {
                    if (fits(candA))      chosen = candA;
                    else if (fits(candB)) chosen = candB;
                    else if (candA >= (int)w) chosen = candA;
                    else if (candB >= (int)w) chosen = candB;
                }
                if (chosen < (int)w) chosen = (int)w;
                if (chosen != (int)pitch) {
                    // 只打印前几次，避免逐帧刷屏（1D 缓冲每帧都没有 pitch 信息）
                    static int correctedLogs = 0;
                    if (correctedLogs < 3) {
                        correctedLogs++;
                        std::cout << "[MediaDecoder] pitch derived " << chosen
                                  << " (len=" << curLen
                                  << " frame=" << frameW_ << "x" << frameH_
                                  << ", 2d=" << (got2d ? 1 : 0) << ")"
                                  << std::endl;
                    }
                }
                pitch = chosen;
            }

            if (locked && curLen > 0) {
                int pitchY = (int)pitch;
                if (pitchY < frameW_) pitchY = frameW_;   // 防御：pitch 不得小于帧宽

                // 已知：Y 的行跨距 = pitchY，帧高 = frameH_。
                //
                // 决策顺序（重要）：
                //   1) 优先按完整帧高处理。Y 平面够长就保留全部行 ——
                //      "Y 完整但缺 UV"只损失颜色，不该连下半屏一起丢。
                //   2) Y 平面都不够长时，才按总长反推真实行数。
                //   3) UV 布局按 A（共用 pitch）→ B（紧凑）依次尝试，
                //      剩余空间不足则传 nullptr，只显示亮度。
                int yHeight = frameH_;
                int pitchUV = pitchY;
                size_t yEnd = (size_t)pitchY * (size_t)yHeight;

                if (yEnd > curLen) {
                    // Y 不完整：NV12 总长 ≈ pitch * H * 3/2，据此反推行数
                    int h = (int)((curLen * 2) / ((size_t)pitchY * 3));
                    h &= ~1;
                    if (h >= 2 && h < yHeight) yHeight = h;
                    yEnd = (size_t)pitchY * (size_t)yHeight;
                }

                int uvOffset = -1;
                if (yEnd <= curLen) {
                    size_t remain = curLen - yEnd;
                    size_t uvRows = (size_t)((yHeight + 1) / 2);
                    if (remain >= uvRows * (size_t)pitchY) {
                        uvOffset = (int)yEnd;          // 布局 A：Y/UV 同 pitch
                        pitchUV = pitchY;
                    } else if (remain >= uvRows * (size_t)frameW_) {
                        uvOffset = (int)yEnd;          // 布局 B：UV 紧凑
                        pitchUV = frameW_;
                    }
                }

                static bool loggedOnce = false;
                if (!loggedOnce) {
                    loggedOnce = true;
                    std::cout << "[MediaDecoder] output pitch=" << pitchY
                              << " uvPitch=" << pitchUV
                              << " frame=" << frameW_ << "x" << frameH_
                              << " yHeight=" << yHeight
                              << " len=" << curLen
                              << " uvOffset=" << uvOffset
                              << " (display " << width_ << "x" << height_ << ")"
                              << std::endl;
                }

                // 真实帧可能小于请求尺寸：超出部分填黑，绝不越界读。
                // uvOffset < 0 表示 UV 不可用，只输出亮度（灰度），同样不越界。
                int drawW = std::min(width_, frameW_);
                int drawH = std::min(height_, yHeight);
                bgraOut.assign((size_t)width_ * height_ * 4, 0);
                for (size_t i = 3; i < bgraOut.size(); i += 4) bgraOut[i] = 255;

                if (drawW > 0 && drawH > 0) {
                    const uint8_t* uvPtr =
                        (uvOffset >= 0 && (size_t)uvOffset < curLen)
                            ? (bufPtr + uvOffset) : nullptr;
                    nv12ToBgra(bufPtr, pitchY, uvPtr, pitchUV,
                               drawW, drawH, bgraOut.data(), width_);
                }
                gotOutput = true;
            }

            if (buf2d) {
                // 2D 锁定的 buffer 只能用 Unlock2D 解锁，1D 用 Unlock
                if (locked) buf2d->Unlock2D();
                buf2d->Release();
            } else if (locked) {
                buf->Unlock();
            }
            buf->Release();
        }

        if (userSample && userSample != resultSample)
            userSample->Release();
        resultSample->Release();

        if (outputData.dwStatus == MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) {
            break;
        }
    }

    return gotOutput;
}

void MediaDecoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);
    cleanupLocked();
}

void MediaDecoder::cleanupLocked() {
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_->Release();
        decoder_ = nullptr;
    }

    if (inputType_) { inputType_->Release(); inputType_ = nullptr; }
    if (outputType_) { outputType_->Release(); outputType_ = nullptr; }

    MFShutdown();

    width_ = height_ = stride_ = 0;
    frameW_ = frameH_ = 0;
    initialized_ = false;
}
