
#define NOMINMAX
#include "media_encoder.h"
#include <iostream>
#include <algorithm>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <propvarutil.h>
#include <vector>
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

// {6CA50344-051A-4DED-9779-A43305165E35}
static const GUID CLSID_H264EncoderMFT =
    {0x6CA50344, 0x051A, 0x4DED, {0x97, 0x79, 0xA4, 0x33, 0x05, 0x16, 0x5E, 0x35}};

static void bgraToNv12(const uint8_t* bgra, int sw, int sh,
                        uint8_t* yPlane, uint8_t* uvPlane,
                        int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int srcY = std::min(y * sh / dh, sh - 1);
        for (int x = 0; x < dw; x++) {
            int srcX = std::min(x * sw / dw, sw - 1);
            const uint8_t* p = bgra + (srcY * sw + srcX) * 4;
            uint8_t b = p[0], g = p[1], r = p[2];

            int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            Y = std::max(16, std::min(235, Y));
            yPlane[y * dw + x] = (uint8_t)Y;

            if ((x & 1) == 0 && (y & 1) == 0) {
                int sumR = r, sumG = g, sumB = b;
                int count = 1;
                if (x + 1 < dw) {
                    auto* p2 = bgra + (srcY * sw + std::min(srcX + 1, sw - 1)) * 4;
                    sumR += p2[2]; sumG += p2[1]; sumB += p2[0]; count++;
                }
                if (y + 1 < dh) {
                    auto* p3 = bgra + (std::min(srcY + 1, sh - 1) * sw + srcX) * 4;
                    sumR += p3[2]; sumG += p3[1]; sumB += p3[0]; count++;
                }
                if (x + 1 < dw && y + 1 < dh) {
                    auto* p4 = bgra + (std::min(srcY + 1, sh - 1) * sw + std::min(srcX + 1, sw - 1)) * 4;
                    sumR += p4[2]; sumG += p4[1]; sumB += p4[0]; count++;
                }
                int avgR = sumR / count, avgG = sumG / count, avgB = sumB / count;
                int U = ((-38 * avgR - 74 * avgG + 112 * avgB + 128) >> 8) + 128;
                int V = ((112 * avgR - 94 * avgG - 18 * avgB + 128) >> 8) + 128;
                U = std::max(16, std::min(240, U));
                V = std::max(16, std::min(240, V));
                int uvi = (y / 2) * (dw / 2) * 2 + (x / 2) * 2;
                uvPlane[uvi] = (uint8_t)U;
                uvPlane[uvi + 1] = (uint8_t)V;
            }
        }
    }
}

MediaEncoder::MediaEncoder() {}

MediaEncoder::~MediaEncoder() {
    cleanup();
}

bool MediaEncoder::init(ID3D11Device* device, int srcW, int srcH, int dstW, int dstH, int fps, int bitrate) {
    std::lock_guard<std::mutex> lock(mtx_);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[MediaEncoder] CoInitializeEx failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] MFStartup failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    srcWidth_ = srcW;
    srcHeight_ = srcH;

    int encW = dstW;
    int encH = dstH;
    const int kMaxEncWidth = 2560;
    if (encW > kMaxEncWidth) {
        encH = encH * kMaxEncWidth / encW;
        encW = kMaxEncWidth;
        std::cout << "[MediaEncoder] Clamping encoding resolution: "
                  << dstW << "x" << dstH << " -> " << encW << "x" << encH << std::endl;
    }

    width_ = encW;
    height_ = encH;
    alignedW_ = (encW + 15) & ~15;
    alignedH_ = (encH + 15) & ~15;
    if (alignedW_ != encW || alignedH_ != encH)
        std::cout << "[MediaEncoder] Aligned to " << alignedW_ << "x" << alignedH_ << std::endl;
    fps_ = fps;
    bitrate_ = bitrate;

    if (device) {
        d3dDevice_ = device;
        d3dDevice_->AddRef();
        d3dDevice_->GetImmediateContext(&d3dContext_);

        if (!initVideoProcessor()) {
            std::cerr << "[MediaEncoder] VideoProcessor init failed, using CPU path" << std::endl;
        }
    }

    if (!initEncoder()) {
        std::cerr << "[MediaEncoder] Encoder init failed" << std::endl;
        cleanup();
        return false;
    }

    initialized_ = true;
    std::cout << "[MediaEncoder] H.264 encoder initialized: "
              << width_ << "x" << height_ << " @ " << fps_ << "fps"
              << (hasGPUPath_ ? " +GPU(VP)" : " +CPU")
              << std::endl;
    return true;
}

bool MediaEncoder::initVideoProcessor() {
    HRESULT hr;

    ID3D11VideoDevice* videoDev = nullptr;
    hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&videoDev));
    if (FAILED(hr)) return false;

    ID3D11VideoContext* videoCtx = nullptr;
    hr = d3dContext_->QueryInterface(IID_PPV_ARGS(&videoCtx));
    if (FAILED(hr)) { videoDev->Release(); return false; }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpDesc = {};
    vpDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    vpDesc.InputWidth = srcWidth_;
    vpDesc.InputHeight = srcHeight_;
    vpDesc.OutputWidth = alignedW_;
    vpDesc.OutputHeight = alignedH_;
    vpDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;

    ID3D11VideoProcessorEnumerator* vpEnum = nullptr;
    hr = videoDev->CreateVideoProcessorEnumerator(&vpDesc, &vpEnum);
    if (FAILED(hr)) {
        videoDev->Release(); videoCtx->Release();
        return false;
    }

    UINT fmtSupported = FALSE;
    vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &fmtSupported);
    if (!fmtSupported) {
        vpEnum->Release(); videoDev->Release(); videoCtx->Release();
        return false;
    }
    vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &fmtSupported);
    if (!fmtSupported) {
        vpEnum->Release(); videoDev->Release(); videoCtx->Release();
        return false;
    }

    ID3D11VideoProcessor* vp = nullptr;
    hr = videoDev->CreateVideoProcessor(vpEnum, 0, &vp);
    if (FAILED(hr)) {
        vpEnum->Release(); videoDev->Release(); videoCtx->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC nv12Desc = {};
    nv12Desc.Width = alignedW_;
    nv12Desc.Height = alignedH_;
    nv12Desc.Format = DXGI_FORMAT_NV12;
    nv12Desc.MipLevels = 1;
    nv12Desc.ArraySize = 1;
    nv12Desc.SampleDesc.Count = 1;
    nv12Desc.Usage = D3D11_USAGE_DEFAULT;
    nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* nv12Tex = nullptr;
    hr = d3dDevice_->CreateTexture2D(&nv12Desc, nullptr, &nv12Tex);
    if (FAILED(hr)) {
        vp->Release(); vpEnum->Release(); videoDev->Release(); videoCtx->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = nv12Desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.BindFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    hr = d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        nv12Tex->Release(); vp->Release(); vpEnum->Release();
        videoDev->Release(); videoCtx->Release();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovDesc.Texture2D.MipSlice = 0;
    ID3D11VideoProcessorOutputView* outputView = nullptr;
    hr = videoDev->CreateVideoProcessorOutputView(nv12Tex, vpEnum, &ovDesc, &outputView);
    if (FAILED(hr)) {
        staging->Release(); nv12Tex->Release(); vp->Release(); vpEnum->Release();
        videoDev->Release(); videoCtx->Release();
        return false;
    }

    videoDevice_ = videoDev;
    videoContext_ = videoCtx;
    vpEnum_ = vpEnum;
    videoProcessor_ = vp;
    nv12Texture_ = nv12Tex;
    nv12Staging_ = staging;
    vpOutputView_ = outputView;

    hasGPUPath_ = true;
    std::cout << "[MediaEncoder] VideoProcessor ready: "
              << srcWidth_ << "x" << srcHeight_ << " -> "
              << alignedW_ << "x" << alignedH_ << std::endl;
    return true;
}

bool MediaEncoder::encodeFromTexture(ID3D11Texture2D* bgraTex, int64_t pts,
                                      std::vector<uint8_t>& output, bool keyframe) {
    std::lock_guard<std::mutex> lock(mtx_);
    output.clear();
    if (!initialized_ || !hasGPUPath_) return false;

    HRESULT hr;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice = 0;
    ID3D11VideoProcessorInputView* inputView = nullptr;
    hr = videoDevice_->CreateVideoProcessorInputView(bgraTex, vpEnum_, &ivDesc, &inputView);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateInputView: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_STREAM streamData = {};
    streamData.Enable = TRUE;
    streamData.pInputSurface = inputView;

    hr = videoContext_->VideoProcessorBlt(videoProcessor_, vpOutputView_,
                                           0, 1, &streamData);
    inputView->Release();
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] VideoProcessorBlt: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    d3dContext_->CopyResource(nv12Staging_, nv12Texture_);

    size_t ySize = size_t(alignedW_) * alignedH_;
    size_t uvSize = ySize / 2;
    std::vector<uint8_t> nv12Buf(ySize + uvSize);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3dContext_->Map(nv12Staging_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] Map: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    uint8_t* base = (uint8_t*)mapped.pData;
    for (int y = 0; y < alignedH_; y++)
        memcpy(nv12Buf.data() + y * alignedW_, base + y * mapped.RowPitch, alignedW_);

    uint8_t* uvSrc = base + mapped.RowPitch * alignedH_;
    UINT uvRowPitch = (alignedW_ / 2) * 2;
    for (int y = 0; y < alignedH_ / 2; y++)
        memcpy(nv12Buf.data() + ySize + y * uvRowPitch,
               uvSrc + y * mapped.RowPitch, uvRowPitch);

    d3dContext_->Unmap(nv12Staging_, 0);

    if (!createInputSample(nv12Buf.data(), pts, keyframe)) {
        std::cerr << "[MediaEncoder] createInputSample failed" << std::endl;
        return false;
    }

    output.clear();
    processOutput(output);

    return true;
}

bool MediaEncoder::initEncoder() {
    HRESULT hr;

    hr = CoCreateInstance(CLSID_MSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&encoder_));
    if (FAILED(hr)) {
        hr = CoCreateInstance(CLSID_H264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&encoder_));
    }

    if (!encoder_) {
        MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       &inputInfo, &outputInfo, &activates, &count);
        if (SUCCEEDED(hr) && count > 0) {
            hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
        }
        for (UINT32 i = 0; i < count; i++) activates[i]->Release();
        CoTaskMemFree(activates);
    }

    if (!encoder_) {
        std::cerr << "[MediaEncoder] No H.264 encoder found" << std::endl;
        return false;
    }
    std::cout << "[MediaEncoder] Encoder created" << std::endl;

    IMFAttributes* mftAttr = nullptr;
    if (SUCCEEDED(encoder_->GetAttributes(&mftAttr))) {
        mftAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
        mftAttr->Release();
        std::cout << "[MediaEncoder] MF_LOW_LATENCY set on MFT attributes" << std::endl;
    }

    IMFMediaType* outputType = nullptr;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return false;
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate_);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outputType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M);
    outputType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, alignedW_, alignedH_);
    MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE, fps_, 1);
    hr = encoder_->SetOutputType(0, outputType, 0);
    outputType->Release();
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] SetOutputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[MediaEncoder] Output type set (" << alignedW_ << "x" << alignedH_ << ")" << std::endl;

    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, alignedW_, alignedH_);
    MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, fps_, 1);
    hr = encoder_->SetInputType(0, inputType, 0);
    inputType->Release();
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] SetInputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[MediaEncoder] Input type set" << std::endl;

    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(encoder_->QueryInterface(IID_PPV_ARGS(&codecApi)))) {
        VARIANT var;
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVEncCommonLowLatency, &var);
        codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &var);
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        var.vt = VT_UI4;
        var.ulVal = 100;
        codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);
        var.ulVal = 1;
        codecApi->SetValue(&CODECAPI_AVEncVideoMaxNumRefFrame, &var);
        codecApi->Release();
        std::cout << "[MediaEncoder] Low-latency mode enabled via ICodecAPI" << std::endl;
    } else {
        std::cout << "[MediaEncoder] ICodecAPI not supported, encoder may have latency" << std::endl;
    }

    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return false;

    return true;
}

bool MediaEncoder::createInputSample(const uint8_t* nv12Data, int64_t pts, bool keyframe) {
    size_t bufSize = size_t(alignedW_) * alignedH_ * 3 / 2;

    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* buf = nullptr;
    hr = MFCreateMemoryBuffer((DWORD)bufSize, &buf);
    if (FAILED(hr)) { sample->Release(); return false; }

    BYTE* dataPtr = nullptr;
    hr = buf->Lock(&dataPtr, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(dataPtr, nv12Data, bufSize);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)bufSize);
    }

    sample->AddBuffer(buf);
    buf->Release();

    sample->SetSampleTime((LONGLONG)(pts * 10000000LL / fps_));
    sample->SetSampleDuration((LONGLONG)(10000000LL / fps_));

    if (keyframe) {
        sample->SetUINT32(CODECAPI_AVEncVideoForceKeyFrame, TRUE);
    }

    hr = encoder_->ProcessInput(0, sample, 0);

    if (hr == MF_E_NOTACCEPTING) {
        std::vector<uint8_t> drainBuf;
        int retries = 0;
        while (hr == MF_E_NOTACCEPTING && retries < 16) {
            processOutput(drainBuf);
            hr = encoder_->ProcessInput(0, sample, 0);
            retries++;
        }
    }

    sample->Release();

    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] ProcessInput final: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    return true;
}

bool MediaEncoder::encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& output, bool keyframe) {
    std::lock_guard<std::mutex> lock(mtx_);
    output.clear();
    if (!initialized_) return false;

    size_t ySize = size_t(alignedW_) * alignedH_;
    size_t uvSize = ySize / 2;
    std::vector<uint8_t> nv12Buf(ySize + uvSize);
    uint8_t* yPlane = nv12Buf.data();
    uint8_t* uvPlane = nv12Buf.data() + ySize;

    bgraToNv12(bgra, srcWidth_, srcHeight_, yPlane, uvPlane, alignedW_, alignedH_);

    if (!createInputSample(nv12Buf.data(), pts, keyframe)) {
        return false;
    }

    processOutput(output);

    return true;
}

bool MediaEncoder::processOutput(std::vector<uint8_t>& output) {
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    encoder_->GetOutputStreamInfo(0, &streamInfo);
    while (true) {
        MFT_OUTPUT_DATA_BUFFER outputData = {};

        bool needSample = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
        IMFSample* userSample = nullptr;
        if (needSample) {
            MFCreateSample(&userSample);
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(MFCreateMemoryBuffer(streamInfo.cbSize, &buf))) {
                userSample->AddBuffer(buf);
                buf->Release();
            }
            outputData.pSample = userSample;
        }

        DWORD status = 0;
        HRESULT hr = encoder_->ProcessOutput(0, 1, &outputData, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (userSample) userSample->Release();
            break;
        }

        if (FAILED(hr)) {
            static int poErrCount = 0;
            if (++poErrCount <= 3)
                std::cerr << "[MediaEncoder] ProcessOutput hr=0x" << std::hex << hr
                          << " status=" << status << std::dec << std::endl;
            if (userSample) userSample->Release();
            if (hr == MF_E_BUFFERTOOSMALL) {
                continue;
            }
            break;
        }

        IMFSample* resultSample = outputData.pSample;
        if (resultSample) {
            IMFMediaBuffer* buf = nullptr;
            hr = resultSample->GetBufferByIndex(0, &buf);
            if (SUCCEEDED(hr)) {
                BYTE* dataPtr = nullptr;
                DWORD curLen = 0;
                hr = buf->Lock(&dataPtr, nullptr, &curLen);
                if (SUCCEEDED(hr) && curLen > 0) {
                    size_t oldSize = output.size();
                    output.resize(oldSize + curLen);
                    memcpy(output.data() + oldSize, dataPtr, curLen);
                    buf->Unlock();
                }
                buf->Release();
            }
            resultSample->Release();
        }

        if (outputData.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) break;
        if (outputData.dwStatus == MFT_OUTPUT_DATA_BUFFER_INCOMPLETE) continue;
        break;
    }

    return !output.empty();
}

bool MediaEncoder::flushEncoder(std::vector<uint8_t>& output) {
    if (!encoder_) return false;
    encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    processOutput(output);
    return true;
}

void MediaEncoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder_->Release();
        encoder_ = nullptr;
    }

    if (inputType_) { inputType_->Release(); inputType_ = nullptr; }
    if (outputType_) { outputType_->Release(); outputType_ = nullptr; }

    if (vpOutputView_) { vpOutputView_->Release(); vpOutputView_ = nullptr; }
    if (nv12Staging_) { nv12Staging_->Release(); nv12Staging_ = nullptr; }
    if (nv12Texture_) { nv12Texture_->Release(); nv12Texture_ = nullptr; }
    if (videoProcessor_) { videoProcessor_->Release(); videoProcessor_ = nullptr; }
    if (vpEnum_) { vpEnum_->Release(); vpEnum_ = nullptr; }
    if (videoContext_) { videoContext_->Release(); videoContext_ = nullptr; }
    if (videoDevice_) { videoDevice_->Release(); videoDevice_ = nullptr; }

    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }

    MFShutdown();

    srcWidth_ = srcHeight_ = width_ = height_ = 0;
    hasGPUPath_ = false;
    initialized_ = false;
}
