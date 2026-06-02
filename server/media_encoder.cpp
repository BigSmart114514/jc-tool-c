
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
#include <d3dcompiler.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "d3dcompiler")

// {6CA50344-051A-4DED-9779-A43305165E35}
static const GUID CLSID_H264EncoderMFT =
    {0x6CA50344, 0x051A, 0x4DED, {0x97, 0x79, 0xA4, 0x33, 0x05, 0x16, 0x5E, 0x35}};

static const char* g_csHLSL = R"(
Texture2D<float4> g_input : register(t0);
RWTexture2D<uint> g_yOut : register(u0);
RWTexture2D<uint2> g_uvOut : register(u1);

cbuffer Constants : register(b0) {
    uint g_srcW, g_srcH, g_dstW, g_dstH;
};

[numthreads(16, 16, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint dx = dtid.x, dy = dtid.y;
    if (dx >= g_dstW || dy >= g_dstH)
        return;

    float sx = ((float)dx + 0.5f) * (float)g_srcW / (float)g_dstW;
    float sy = ((float)dy + 0.5f) * (float)g_srcH / (float)g_dstH;

    uint ix = (uint)max(sx - 0.5f, 0.0f);
    uint iy = (uint)max(sy - 0.5f, 0.0f);
    float fx = sx - ((float)ix + 0.5f);
    float fy = sy - ((float)iy + 0.5f);

    uint ix1 = min(ix + 1, g_srcW - 1);
    uint iy1 = min(iy + 1, g_srcH - 1);

    float4 c00 = g_input[uint2(ix, iy)];
    float4 c10 = g_input[uint2(ix1, iy)];
    float4 c01 = g_input[uint2(ix, iy1)];
    float4 c11 = g_input[uint2(ix1, iy1)];

    float4 c = lerp(lerp(c00, c10, fx), lerp(c01, c11, fx), fy);

    float Yf = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
    Yf = Yf * (219.0f / 255.0f) + (16.0f / 255.0f);
    uint Y = (uint)round(Yf * 255.0f);
    Y = min(max(Y, 16u), 235u);
    g_yOut[uint2(dx, dy)] = Y;

    if ((dx & 1) == 0 && (dy & 1) == 0) {
        float uv_sx = ((float)dx + 0.5f) * (float)g_srcW / (float)g_dstW;
        float uv_sy = ((float)dy + 0.5f) * (float)g_srcH / (float)g_dstH;

        uint uix = (uint)max(uv_sx - 0.5f, 0.0f);
        uint uiy = (uint)max(uv_sy - 0.5f, 0.0f);
        uix = min(uix, g_srcW - 1);
        uiy = min(uiy, g_srcH - 1);
        uint uix1 = min(uix + 1, g_srcW - 1);
        uint uiy1 = min(uiy + 1, g_srcH - 1);

        float4 uv00 = g_input[uint2(uix, uiy)];
        float4 uv10 = g_input[uint2(uix1, uiy)];
        float4 uv01 = g_input[uint2(uix, uiy1)];
        float4 uv11 = g_input[uint2(uix1, uiy1)];

        float3 avg = (uv00.xyz + uv10.xyz + uv01.xyz + uv11.xyz) * 0.25f;

        float Uf = -0.169f * avg.r - 0.331f * avg.g + 0.500f * avg.b;
        float Vf = 0.500f * avg.r - 0.419f * avg.g - 0.081f * avg.b;

        Uf = Uf * (224.0f / 255.0f) + (128.0f / 255.0f);
        Vf = Vf * (224.0f / 255.0f) + (128.0f / 255.0f);

        uint U = (uint)round(Uf * 255.0f);
        uint V = (uint)round(Vf * 255.0f);
        U = min(max(U, 16u), 240u);
        V = min(max(V, 16u), 240u);

        g_uvOut[uint2(dx >> 1, dy >> 1)] = uint2(U, V);
    }
}
)";

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

        if (!initComputeShader()) {
            std::cerr << "[MediaEncoder] Compute shader init failed, using CPU path" << std::endl;
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
              << (hasComputeShader_ ? " +GPU(CS)" : " +CPU")
              << std::endl;
    return true;
}

bool MediaEncoder::initComputeShader() {
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(g_csHLSL, strlen(g_csHLSL), nullptr, nullptr, nullptr,
                            "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] D3DCompile failed: 0x" << std::hex << hr << std::dec << std::endl;
        if (errorBlob) {
            std::cerr << "  Errors: " << (const char*)errorBlob->GetBufferPointer() << std::endl;
            errorBlob->Release();
        }
        return false;
    }

    hr = d3dDevice_->CreateComputeShader(shaderBlob->GetBufferPointer(),
                                         shaderBlob->GetBufferSize(), nullptr, &cs_);
    shaderBlob->Release();
    if (errorBlob) errorBlob->Release();
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateComputeShader failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(UINT) * 4;
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, &csConstants_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateBuffer (cb) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC yDesc = {};
    yDesc.Width = alignedW_;
    yDesc.Height = alignedH_;
    yDesc.MipLevels = 1;
    yDesc.ArraySize = 1;
    yDesc.Format = DXGI_FORMAT_R8_UINT;
    yDesc.SampleDesc.Count = 1;
    yDesc.Usage = D3D11_USAGE_DEFAULT;
    yDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = d3dDevice_->CreateTexture2D(&yDesc, nullptr, &yTexture_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateTexture2D (Y) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC uvDesc = {};
    uvDesc.Width = alignedW_ / 2;
    uvDesc.Height = alignedH_ / 2;
    uvDesc.MipLevels = 1;
    uvDesc.ArraySize = 1;
    uvDesc.Format = DXGI_FORMAT_R8G8_UINT;
    uvDesc.SampleDesc.Count = 1;
    uvDesc.Usage = D3D11_USAGE_DEFAULT;
    uvDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    hr = d3dDevice_->CreateTexture2D(&uvDesc, nullptr, &uvTexture_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateTexture2D (UV) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC yStagingDesc = yDesc;
    yStagingDesc.Usage = D3D11_USAGE_STAGING;
    yStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    yStagingDesc.BindFlags = 0;
    hr = d3dDevice_->CreateTexture2D(&yStagingDesc, nullptr, &yStaging_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateTexture2D (Y staging) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_TEXTURE2D_DESC uvStagingDesc = uvDesc;
    uvStagingDesc.Usage = D3D11_USAGE_STAGING;
    uvStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    uvStagingDesc.BindFlags = 0;
    hr = d3dDevice_->CreateTexture2D(&uvStagingDesc, nullptr, &uvStaging_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateTexture2D (UV staging) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    hr = d3dDevice_->CreateUnorderedAccessView(yTexture_, &uavDesc, &yUAV_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateUAV (Y) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    uavDesc.Format = DXGI_FORMAT_R8G8_UINT;
    hr = d3dDevice_->CreateUnorderedAccessView(uvTexture_, &uavDesc, &uvUAV_);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] CreateUAV (UV) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hasComputeShader_ = true;
    std::cout << "[MediaEncoder] Compute shader ready (" << alignedW_ << "x" << alignedH_ << ")" << std::endl;
    return true;
}

bool MediaEncoder::encodeFromTexture(ID3D11Texture2D* bgraTex, int64_t pts,
                                      std::vector<uint8_t>& output, bool keyframe) {
    std::lock_guard<std::mutex> lock(mtx_);
    output.clear();
    if (!initialized_ || !hasComputeShader_) return false;

    HRESULT hr;

    if (bgraTex != cachedInputTex_) {
        if (inputSRV_) { inputSRV_->Release(); inputSRV_ = nullptr; }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = d3dDevice_->CreateShaderResourceView(bgraTex, &srvDesc, &inputSRV_);
        if (FAILED(hr)) {
            std::cerr << "[MediaEncoder] CreateSRV failed: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        cachedInputTex_ = bgraTex;
    }

    UINT csData[] = { (UINT)srcWidth_, (UINT)srcHeight_,
                      (UINT)alignedW_, (UINT)alignedH_ };
    d3dContext_->UpdateSubresource(csConstants_, 0, nullptr, csData, 0, 0);

    d3dContext_->CSSetShader(cs_, nullptr, 0);
    d3dContext_->CSSetConstantBuffers(0, 1, &csConstants_);
    d3dContext_->CSSetShaderResources(0, 1, &inputSRV_);

    ID3D11UnorderedAccessView* uavs[2] = { yUAV_, uvUAV_ };
    UINT uavCounts[2] = { 0, 0 };
    d3dContext_->CSSetUnorderedAccessViews(0, 2, uavs, uavCounts);

    UINT gx = (alignedW_ + 15) / 16;
    UINT gy = (alignedH_ + 15) / 16;
    d3dContext_->Dispatch(gx, gy, 1);

    ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
    d3dContext_->CSSetShaderResources(0, 1, nullSRV);
    ID3D11UnorderedAccessView* nullUAV[2] = { nullptr, nullptr };
    UINT nullCount[2] = { 0, 0 };
    d3dContext_->CSSetUnorderedAccessViews(0, 2, nullUAV, nullCount);
    d3dContext_->CSSetShader(nullptr, nullptr, 0);

    d3dContext_->CopyResource(yStaging_, yTexture_);
    d3dContext_->CopyResource(uvStaging_, uvTexture_);

    D3D11_MAPPED_SUBRESOURCE mappedY = {}, mappedUV = {};
    hr = d3dContext_->Map(yStaging_, 0, D3D11_MAP_READ, 0, &mappedY);
    if (FAILED(hr)) {
        std::cerr << "[MediaEncoder] Map(Y) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    hr = d3dContext_->Map(uvStaging_, 0, D3D11_MAP_READ, 0, &mappedUV);
    if (FAILED(hr)) {
        d3dContext_->Unmap(yStaging_, 0);
        std::cerr << "[MediaEncoder] Map(UV) failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    size_t ySize = size_t(alignedW_) * alignedH_;
    size_t uvSize = ySize / 2;
    std::vector<uint8_t> nv12Buf(ySize + uvSize);

    if (mappedY.RowPitch == (UINT)alignedW_) {
        memcpy(nv12Buf.data(), mappedY.pData, ySize);
    } else {
        for (int y = 0; y < alignedH_; y++) {
            memcpy(nv12Buf.data() + y * alignedW_,
                   (uint8_t*)mappedY.pData + y * mappedY.RowPitch, alignedW_);
        }
    }

    UINT uvRowPitch = (alignedW_ / 2) * 2;
    if (mappedUV.RowPitch == uvRowPitch) {
        memcpy(nv12Buf.data() + ySize, mappedUV.pData, uvSize);
    } else {
        for (int y = 0; y < alignedH_ / 2; y++) {
            memcpy(nv12Buf.data() + ySize + y * uvRowPitch,
                   (uint8_t*)mappedUV.pData + y * mappedUV.RowPitch, uvRowPitch);
        }
    }

    d3dContext_->Unmap(uvStaging_, 0);
    d3dContext_->Unmap(yStaging_, 0);

    if (!createInputSample(nv12Buf.data(), pts, keyframe)) {
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

    if (cs_) { cs_->Release(); cs_ = nullptr; }
    if (csConstants_) { csConstants_->Release(); csConstants_ = nullptr; }
    if (inputSRV_) { inputSRV_->Release(); inputSRV_ = nullptr; }
    if (yUAV_) { yUAV_->Release(); yUAV_ = nullptr; }
    if (uvUAV_) { uvUAV_->Release(); uvUAV_ = nullptr; }
    if (yTexture_) { yTexture_->Release(); yTexture_ = nullptr; }
    if (uvTexture_) { uvTexture_->Release(); uvTexture_ = nullptr; }
    if (yStaging_) { yStaging_->Release(); yStaging_ = nullptr; }
    if (uvStaging_) { uvStaging_->Release(); uvStaging_ = nullptr; }
    if (cachedInputTex_) { cachedInputTex_ = nullptr; }

    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }

    MFShutdown();

    srcWidth_ = srcHeight_ = width_ = height_ = 0;
    hasComputeShader_ = false;
    initialized_ = false;
}
