
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

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

// {62CE7E72-4C71-4D20-B15D-45283A99B03B}
static const GUID CLSID_H264DecoderMFT =
    {0x62CE7E72, 0x4C71, 0x4D20, {0xB1, 0x5D, 0x45, 0x28, 0x3A, 0x99, 0xB0, 0x3B}};

void MediaDecoder::nv12ToBgra(const uint8_t* nv12, uint8_t* bgra, int w, int h, int strideY, int strideUV, int alignedH) {
    const uint8_t* yPlane = nv12;
    const uint8_t* uvPlane = nv12 + strideY * alignedH;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t Y = yPlane[y * strideY + x];
            int uvi = (y / 2) * strideUV + (x / 2) * 2;
            uint8_t U = uvPlane[uvi];
            uint8_t V = uvPlane[uvi + 1];

            int C = Y - 16;
            int D = U - 128;
            int E = V - 128;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            R = std::max(0, std::min(255, R));
            G = std::max(0, std::min(255, G));
            B = std::max(0, std::min(255, B));

            bgra[(y * w + x) * 4] = (uint8_t)B;
            bgra[(y * w + x) * 4 + 1] = (uint8_t)G;
            bgra[(y * w + x) * 4 + 2] = (uint8_t)R;
            bgra[(y * w + x) * 4 + 3] = 255;
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
    stride_ = alignedW_ * 4;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return false;

    if (!initDecoder()) {
        std::cerr << "[MediaDecoder] Decoder init failed" << std::endl;
        cleanup();
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
    outputType_->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M);
    outputType_->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);

    hr = decoder_->SetOutputType(0, outputType_, 0);
    if (FAILED(hr)) {
        std::cerr << "[MediaDecoder] SetOutputType failed: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

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
    if (SUCCEEDED(hr)) {
        memcpy(dataPtr, data, size);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)size);
    }

    sample->AddBuffer(buf);
    buf->Release();
    sample->SetSampleTime(0);

    hr = decoder_->ProcessInput(0, sample, 0);
    sample->Release();

    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        std::cerr << "[MediaDecoder] ProcessInput failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    bool ok = processOutput(bgraOut);
    return ok;
}

bool MediaDecoder::processOutput(std::vector<uint8_t>& bgraOut) {
    bool gotOutput = false;
    int loopCount = 0;

    while (loopCount < 16) {
        loopCount++;

        IMFSample* userSample = nullptr;
        MFCreateSample(&userSample);
        IMFMediaBuffer* userBuf = nullptr;
        MFCreateMemoryBuffer(4 * 1024 * 1024, &userBuf);
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

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (userSample) userSample->Release();
            break;
        }

        if (hr == MF_E_BUFFERTOOSMALL) {
            if (userSample) userSample->Release();
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
            DWORD maxLen = 0, curLen = 0;
            hr = buf->Lock(&bufPtr, &maxLen, &curLen);
            if (SUCCEEDED(hr) && curLen > 0) {
                int strideY, strideUV, actualH;
                int expectedAligned = alignedW_ * alignedH_ * 3 / 2;
                int expectedUnaligned = width_ * height_ * 3 / 2;

                if ((int)curLen >= expectedAligned) {
                    strideY = strideUV = alignedW_;
                    actualH = alignedH_;
                } else if ((int)curLen >= expectedUnaligned) {
                    strideY = strideUV = width_;
                    actualH = height_;
                } else {
                    buf->Unlock();
                    buf->Release();
                    resultSample->Release();
                    if (userSample && userSample != resultSample) userSample->Release();
                    break;
                }

                bgraOut.resize(width_ * height_ * 4);
                nv12ToBgra(bufPtr, bgraOut.data(), width_, height_, strideY, strideUV, actualH);

                gotOutput = true;
                buf->Unlock();
            }
            buf->Release();
        }

        if (userSample && userSample != resultSample)
            userSample->Release();
        resultSample->Release();

        if (outputData.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) {
            break;
        }
    }

    return gotOutput;
}

void MediaDecoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);

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
    initialized_ = false;
}
