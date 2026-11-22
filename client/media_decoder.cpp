
#include "media_decoder.h"
#include "nv12_layout.h"
#include "../common/video_diagnostics.h"
#define NOMINMAX
#include <iostream>
#include <algorithm>
#include <cstring>
#include <utility>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

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
        const uint8_t* uvRow0 = uvPlane + (size_t)(y / 2) * pitchUV;
        const uint8_t* uvRow1 = uvPlane + (size_t)std::min(y / 2 + 1, halfH - 1) * pitchUV;
        bool oddRow = (y & 1) != 0;

        for (int x = 0; x < w; x++) {
            uint8_t Y = yRow[x];

            const int ux = (x / 2) * 2;
            const int next = std::min(x / 2 + 1, halfW - 1) * 2;
            const int right = (x & 1) ? next : ux;
            const uint8_t* bottom = oddRow && y + 1 < h ? uvRow1 : uvRow0;
            int C = Y - 16;
            int D = (uvRow0[ux] + uvRow0[right] + bottom[ux] + bottom[right] + 2) / 4 - 128;
            int E = (uvRow0[ux + 1] + uvRow0[right + 1] + bottom[ux + 1] + bottom[right + 1] + 2) / 4 - 128;

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

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return false;
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
        const HRESULT lowLatencyResult = mftAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
        mftAttr->Release();
        std::cout << "[MediaDecoder] MF_LOW_LATENCY attribute hr=0x"
                  << std::hex << lowLatencyResult << std::dec << std::endl;
    }

    hr = MFCreateMediaType(&inputType_);
    if (FAILED(hr)) return false;
    inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType_, MF_MT_FRAME_SIZE, alignedW_, alignedH_);
    MFSetAttributeRatio(inputType_, MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(inputType_, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);

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
        if (!selectNv12Output()) {
            std::cerr << "[MediaDecoder] No usable NV12 output type" << std::endl;
            return false;
        }
    } else if (!reconfigureOutput()) {
        return false;
    }

    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;
    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return false;

    return true;
}

// 从解码器当前输出类型读取真实帧尺寸。
bool MediaDecoder::reconfigureOutput() {
    ComPtr<IMFMediaType> cur;
    if (FAILED(decoder_->GetOutputCurrentType(0, cur.GetAddressOf()))) return false;
    UINT32 fw = 0, fh = 0;
    if (FAILED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &fw, &fh)) ||
        fw == 0 || fh == 0 || fw > 8192 || fh > 8192 || (fw % 2) || (fh % 2)) return false;

    UINT32 stride = 0;
    cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
    frameW_ = static_cast<int>(fw);
    frameH_ = static_cast<int>(fh);
    linearStride_ = static_cast<INT32>(stride);
    loggedLayout_ = false;
    return true;
}

bool MediaDecoder::selectNv12Output() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(decoder_->GetOutputAvailableType(0, i, type.GetAddressOf()))) return false;
        GUID subtype = GUID_NULL;
        if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12 &&
            SUCCEEDED(decoder_->SetOutputType(0, type.Get(), 0)) && reconfigureOutput()) return true;
    }
}

bool MediaDecoder::resetStream() {
    std::lock_guard<std::mutex> lock(mtx_);
    discontinuity_ = true;
    nextSampleTime_ = 0;
    return initialized_ && SUCCEEDED(decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0)) &&
           SUCCEEDED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
}

MediaDecoder::DecodeResult MediaDecoder::decode(const uint8_t* data, int size, std::vector<uint8_t>& bgraOut) {
    std::lock_guard<std::mutex> lock(mtx_);
    bgraOut.clear();

    if (!initialized_ || size < 4) return DecodeResult::Error;

    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return DecodeResult::Error;

    IMFMediaBuffer* buf = nullptr;
    hr = MFCreateMemoryBuffer((DWORD)size, &buf);
    if (FAILED(hr)) { sample->Release(); return DecodeResult::Error; }

    BYTE* dataPtr = nullptr;
    hr = buf->Lock(&dataPtr, nullptr, nullptr);
    if (FAILED(hr)) {
        buf->Release();
        sample->Release();
        return DecodeResult::Error;
    }
    memcpy(dataPtr, data, size);
    buf->Unlock();
    hr = buf->SetCurrentLength((DWORD)size);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    buf->Release();
    if (FAILED(hr)) { sample->Release(); return DecodeResult::Error; }
    constexpr LONGLONG frameDuration = 10000000LL / 30;
    sample->SetSampleTime(nextSampleTime_);
    sample->SetSampleDuration(frameDuration);
    if (discontinuity_) sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);

    for (int attempt = 0; attempt < 3; ++attempt) {
        hr = decoder_->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING && processOutput(bgraOut)) continue;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE && selectNv12Output()) continue;
        break;
    }
    sample->Release();

    if (FAILED(hr)) {
        std::cerr << "[MediaDecoder] ProcessInput failed: 0x" << std::hex << hr << std::dec << std::endl;
        return DecodeResult::Error;
    }

    if (inputFrameLogs_ < 5) {
        ++inputFrameLogs_;
        std::cout << "[MediaDecoder] dbg h264-rx " << width_ << 'x' << height_
                  << " pts100ns=" << nextSampleTime_ << " bytes=" << size
                  << " hash=" << std::hex << videoFingerprint(data, size) << std::dec << std::endl;
    }
    nextSampleTime_ += frameDuration;
    discontinuity_ = false;
    if (!processOutput(bgraOut)) return DecodeResult::Error;
    return bgraOut.empty() ? DecodeResult::NeedInput : DecodeResult::FrameReady;
}

bool MediaDecoder::processOutput(std::vector<uint8_t>& bgraOut) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        MFT_OUTPUT_STREAM_INFO info = {};
        if (FAILED(decoder_->GetOutputStreamInfo(0, &info))) return false;

        ComPtr<IMFSample> callerSample;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            ComPtr<IMFMediaBuffer> buffer;
            const auto packed = nv12Layout(frameW_, frameH_, frameW_);
            const auto declared = nv12Layout(frameW_, frameH_, linearStride_);
            const auto size = std::max({std::size_t(info.cbSize), packed.size, declared.size});
            if (size > MAXDWORD || FAILED(MFCreateSample(callerSample.GetAddressOf())) ||
                FAILED(MFCreateAlignedMemoryBuffer(static_cast<DWORD>(size),
                       info.cbAlignment ? info.cbAlignment - 1 : 0, buffer.GetAddressOf())) ||
                FAILED(callerSample->AddBuffer(buffer.Get()))) return false;
        }

        MFT_OUTPUT_DATA_BUFFER output = {};
        output.pSample = callerSample.Get();
        DWORD status = 0;
        const HRESULT hr = decoder_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        ComPtr<IMFSample> providedSample;
        if (output.pSample != callerSample.Get()) providedSample.Attach(output.pSample);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE && selectNv12Output()) continue;
        if (FAILED(hr)) {
            std::cerr << "[MediaDecoder] ProcessOutput hr=0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }

        const DWORD sampleStatus = output.dwStatus & ~static_cast<DWORD>(MFT_OUTPUT_DATA_BUFFER_INCOMPLETE);
        if (output.pSample && sampleStatus != MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE &&
            sampleStatus != MFT_OUTPUT_DATA_BUFFER_STREAM_END && !readFrame(output.pSample, bgraOut)) return false;
        if (!(output.dwStatus & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)) return true;
    }
    return false;
}

bool MediaDecoder::readFrame(IMFSample* sample, std::vector<uint8_t>& bgraOut) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) return false;

    BYTE* data = nullptr;
    DWORD bytes = 0;
    Nv12Layout layout = {};
    ComPtr<IMF2DBuffer> surface;
    const bool is2D = SUCCEEDED(buffer.As(&surface));
    bool linearLocked = false;
    if (is2D) {
        // Native pitch and contiguous length describe different layouts. Let MF
        // remove surface padding before calculating the NV12 plane offsets.
        const auto packed = nv12Layout(frameW_, frameH_, frameW_);
        if (SUCCEEDED(surface->GetContiguousLength(&bytes)) && packed.size && bytes == packed.size) {
            packedNv12_.resize(bytes);
            if (SUCCEEDED(surface->ContiguousCopyTo(packedNv12_.data(), bytes))) {
                layout = packed;
                data = packedNv12_.data();
            }
        }
    } else if (SUCCEEDED(buffer->Lock(&data, nullptr, &bytes))) {
        linearLocked = true;
        layout = linearNv12Layout(frameW_, frameH_, linearStride_, bytes);
    }

    const bool complete = data && layout.size && frameW_ >= width_ && frameH_ >= height_;
    if (complete) {
        if (lumaDiagnosticLogs_ < 5) {
            ++lumaDiagnosticLogs_;
            LONGLONG sampleTime = 0;
            if (FAILED(sample->GetSampleTime(&sampleTime))) sampleTime = -1;
            auto meanY = [&](int firstRow, int endRow) {
                uint64_t sum = 0;
                uint64_t count = 0;
                for (int y = firstRow; y < endRow; y += 3) {
                    for (int x = 0; x < width_; x += 3) {
                        sum += data[size_t(y) * layout.pitch + x];
                        ++count;
                    }
                }
                return count ? double(sum) / count : 0.0;
            };
            std::cout << "[MediaDecoder] dbg nv12-y " << width_ << 'x' << height_
                      << " pts100ns=" << sampleTime << " top=" << meanY(0, height_ / 2)
                      << " mid=" << meanY(height_ / 2, height_ * 3 / 4)
                      << " bottom=" << meanY(height_ * 3 / 4, height_) << std::endl;
        }
        if (!loggedLayout_) {
            loggedLayout_ = true;
            std::cout << "[MediaDecoder] output pitch=" << layout.pitch << " frame=" << frameW_ << 'x'
                      << frameH_ << " len=" << bytes << " uvOffset=" << layout.uvOffset
                      << " 2d=" << is2D << " display=" << width_ << 'x' << height_ << std::endl;
        }
        bgraOut.resize(std::size_t(width_) * height_ * 4);
        const int pitch = static_cast<int>(layout.pitch);
        nv12ToBgra(data, pitch, data + layout.uvOffset, pitch, width_, height_, bgraOut.data(), width_);
    } else if (++layoutErrors_ <= 5) {
        std::cerr << "[MediaDecoder] rejected NV12 frame=" << frameW_ << 'x' << frameH_
                  << " len=" << bytes << " stride=" << linearStride_ << " 2d=" << is2D
                  << " display=" << width_ << 'x' << height_ << std::endl;
    }
    if (linearLocked) buffer->Unlock();
    return complete;
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
    linearStride_ = 0;
    nextSampleTime_ = 0;
    layoutErrors_ = 0;
    lumaDiagnosticLogs_ = 0;
    inputFrameLogs_ = 0;
    loggedLayout_ = false;
    discontinuity_ = true;
    initialized_ = false;
}
