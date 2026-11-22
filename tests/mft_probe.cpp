// MFT H.264 编码器配置对照实验。
//
// 目的：把 MediaEncoder::initEncoder() 里的每一项配置单独拆开，喂同一段
// 合成画面，输出多份 Annex-B 码流，再用外部解码器检查哪一份是合法的。
//
// 用法： MftProbe.exe --out <dir>
//
// 产物：<dir>/v0_product.h264 ... 每份 30 帧。
// 全部使用 640x368（16 对齐），只关注码流合法性，不关注画质。

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

// {6CA50344-051A-4DED-9779-A43305165E35}
static const GUID CLSID_MSH264 = {
    0x6CA50344, 0x051A, 0x4DED, { 0x97, 0x79, 0xA4, 0x33, 0x05, 0x16, 0x5E, 0x35 }
};

namespace fs = std::filesystem;

static const int kW = 640;
static const int kH = 368;
static const int kFps = 30;
static const int kFrames = 30;
static const int kBitrate = 8000000;

// 高细节图案：棋盘 + 渐变 + 移动竖条，让编码器产生大量残差，
// 熵编码一旦参数不匹配就会立刻暴露。
static void makeNv12(int frame, std::vector<uint8_t>& out) {
    out.resize(std::size_t(kW) * kH * 3 / 2);
    uint8_t* y = out.data();
    uint8_t* uv = out.data() + std::size_t(kW) * kH;

    for (int py = 0; py < kH; ++py) {
        for (int px = 0; px < kW; ++px) {
            const int checker = ((px / 8) + (py / 8)) & 1;
            int v = checker ? 200 : 40;
            v += (px * 60) / kW;                 // 水平渐变
            v += (py * 20) / kH;                 // 垂直渐变
            const int bar = (px + frame * 6) % 160;
            if (bar < 24) v = 250;               // 移动竖条
            if (v < 16) v = 16;
            if (v > 235) v = 235;
            y[std::size_t(py) * kW + px] = static_cast<uint8_t>(v);
        }
    }
    for (int py = 0; py < kH / 2; ++py) {
        for (int px = 0; px < kW / 2; ++px) {
            uv[std::size_t(py) * kW + px * 2 + 0] = static_cast<uint8_t>(64 + (frame % 64));
            uv[std::size_t(py) * kW + px * 2 + 1] = static_cast<uint8_t>(192 - (frame % 64));
        }
    }
}

struct Variant {
    const char* name;
    bool cabac;
    bool lowLatency;
    bool qualityMode;
    bool maxRefFrames;
    bool forceKeyframe;
    bool apiBeforeTypes;   // true: 按微软文档在 SetOutputType 之前设置 ICodecAPI
    int quality = 100;     // eAVEncCommonQuality，仅 qualityMode 时生效
    bool unconstrainedVbr = false;  // true: 显式设 UnconstrainedVBR，仍保留 CABAC
    int qualityVsSpeed = -1;        // >=0: 设置 CODECAPI_AVEncCommonQualityVsSpeed 的值
    int qp = -1;                    // >=0: 改用 CODECAPI_AVEncVideoEncodeQP
};

static bool setOutputType(IMFTransform* mft) {
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    t->SetUINT32(MF_MT_AVG_BITRATE, kBitrate);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    t->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, kW, kH);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, kFps, 1);
    const HRESULT hr = mft->SetOutputType(0, t, 0);
    t->Release();
    return SUCCEEDED(hr);
}

static bool setInputType(IMFTransform* mft) {
    IMFMediaType* t = nullptr;
    if (FAILED(MFCreateMediaType(&t))) return false;
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    t->SetUINT32(MF_MT_DEFAULT_STRIDE, kW);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, kW, kH);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, kFps, 1);
    const HRESULT hr = mft->SetInputType(0, t, 0);
    t->Release();
    return SUCCEEDED(hr);
}

static void applyCodecApi(IMFTransform* mft, const Variant& v) {
    ICodecAPI* api = nullptr;
    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&api)))) return;
    VARIANT var = {};

    if (v.qualityMode) {
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_Quality;
        api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
        var.ulVal = static_cast<ULONG>(v.quality);
        api->SetValue(&CODECAPI_AVEncCommonQuality, &var);
        if (v.qualityVsSpeed >= 0) {
            var.ulVal = static_cast<ULONG>(v.qualityVsSpeed);
            api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &var);
        }
    }
    if (v.qp >= 0) {
        var = {};
        var.vt = VT_UI4;
        var.ulVal = static_cast<ULONG>(v.qp);
        api->SetValue(&CODECAPI_AVEncVideoEncodeQP, &var);
    }
    if (v.unconstrainedVbr) {
        var = {};
        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
        api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
    }
    if (v.lowLatency) {
        var = {};
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        api->SetValue(&CODECAPI_AVEncCommonLowLatency, &var);
        api->SetValue(&CODECAPI_AVEncCommonRealTime, &var);
        api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
    }
    if (v.maxRefFrames) {
        var = {};
        var.vt = VT_UI4;
        var.ulVal = 4;
        api->SetValue(&CODECAPI_AVEncVideoMaxNumRefFrame, &var);
    }
    if (v.cabac) {
        var = {};
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        api->SetValue(&CODECAPI_AVEncH264CABACEnable, &var);
    }
    api->Release();
}

static void forceKeyframe(IMFTransform* mft) {
    ICodecAPI* api = nullptr;
    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(&api)))) return;
    VARIANT var = {};
    var.vt = VT_UI4;
    var.ulVal = 1;
    api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
    api->Release();
}

// 抽干编码器输出，把 Annex-B 追加到 out。
static bool drain(IMFTransform* mft, std::vector<uint8_t>& out) {
    MFT_OUTPUT_STREAM_INFO info = {};
    if (FAILED(mft->GetOutputStreamInfo(0, &info))) return false;

    for (int guard = 0; guard < 64; ++guard) {
        IMFSample* mine = nullptr;
        const bool needSample =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0;
        if (needSample) {
            if (FAILED(MFCreateSample(&mine))) return false;
            IMFMediaBuffer* b = nullptr;
            HRESULT hr = MFCreateAlignedMemoryBuffer(
                info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &b);
            if (SUCCEEDED(hr)) {
                hr = mine->AddBuffer(b);
                b->Release();
            }
            if (FAILED(hr)) { mine->Release(); return false; }
        }

        MFT_OUTPUT_DATA_BUFFER odb = {};
        odb.pSample = mine;
        DWORD status = 0;
        const HRESULT hr = mft->ProcessOutput(0, 1, &odb, &status);
        if (odb.pEvents) odb.pEvents->Release();

        IMFSample* got = odb.pSample;
        if (mine && mine != got) mine->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (got) got->Release();
            return true;
        }
        if (FAILED(hr)) {
            std::cerr << "    ProcessOutput hr=0x" << std::hex << hr << std::dec << "\n";
            if (got) got->Release();
            return false;
        }

        if (got) {
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(got->ConvertToContiguousBuffer(&b))) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len)) && len > 0)
                    out.insert(out.end(), p, p + len);
                b->Unlock();
                b->Release();
            }
            got->Release();
        }
        if (!(status & MFT_OUTPUT_DATA_BUFFER_INCOMPLETE)) return true;
    }
    return true;
}

static bool runVariant(const Variant& v, const fs::path& dir) {
    IMFTransform* mft = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MSH264, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&mft)))) {
        std::cerr << "  no encoder MFT\n";
        return false;
    }

    IMFAttributes* attr = nullptr;
    if (SUCCEEDED(mft->GetAttributes(&attr))) {
        attr->SetUINT32(MF_LOW_LATENCY, TRUE);
        attr->Release();
    }

    if (v.apiBeforeTypes) applyCodecApi(mft, v);

    if (!setOutputType(mft) || !setInputType(mft)) {
        std::cerr << "  SetOutput/InputType failed\n";
        mft->Release();
        return false;
    }

    if (!v.apiBeforeTypes) applyCodecApi(mft, v);

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    std::vector<uint8_t> stream;
    std::vector<uint8_t> nv12;
    LONGLONG time = 0;
    const LONGLONG dur = 10000000LL / kFps;

    for (int f = 0; f < kFrames; ++f) {
        makeNv12(f, nv12);
        IMFSample* s = nullptr;
        IMFMediaBuffer* b = nullptr;
        if (FAILED(MFCreateSample(&s))) continue;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()), &b))) {
            s->Release();
            continue;
        }
        BYTE* p = nullptr;
        if (SUCCEEDED(b->Lock(&p, nullptr, nullptr))) {
            memcpy(p, nv12.data(), nv12.size());
            b->Unlock();
            b->SetCurrentLength(static_cast<DWORD>(nv12.size()));
            s->AddBuffer(b);
        }
        b->Release();
        s->SetSampleTime(time);
        s->SetSampleDuration(dur);

        if (v.forceKeyframe && (f % (kFps * 2) == 0)) forceKeyframe(mft);

        HRESULT hr = mft->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) {
            drain(mft, stream);
            hr = mft->ProcessInput(0, s, 0);
        }
        s->Release();
        if (FAILED(hr)) std::cerr << "    ProcessInput hr=0x" << std::hex << hr << std::dec << "\n";
        drain(mft, stream);
        time += dur;
    }

    // 报告码流里的 SPS/PPS，方便确认熵编码标志是否与数据一致。
    for (std::size_t i = 3; i + 1 < stream.size(); ++i) {
        if (stream[i - 3] || stream[i - 2] || stream[i - 1] != 1) continue;
        const int type = stream[i] & 0x1f;
        if (type == 8 && i + 5 < stream.size()) {
            std::printf("    PPS:");
            for (int k = 1; k <= 4 && i + k < stream.size(); ++k)
                std::printf(" %02x", stream[i + k]);
            std::printf("\n");
            break;
        }
    }

    mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft->Release();

    const fs::path p = dir / (std::string(v.name) + ".h264");
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(stream.data()),
              static_cast<std::streamsize>(stream.size()));
    out.close();
    std::printf("  %-18s %7zu bytes\n", v.name, stream.size());
    return stream.size() > 0;
}

int main(int argc, char** argv) {
    fs::path dir = "mft-probe";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) dir = argv[++i];
    }
    fs::create_directories(dir);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION))) {
        std::cerr << "MFStartup failed\n";
        return 1;
    }

    // 关键对照：QvS（QualityVsSpeed）与 CABAC 的组合矩阵。
    const Variant variants[] = {
        { "a_product_qvs100_cabac", true,  true,  true,  true,  true,  false, 100, false, 100 },
        { "b_qvs100_nocabac",       false, true,  true,  true,  true,  false, 100, false, 100 },
        { "c_qvs66_cabac",          true,  true,  true,  true,  true,  false, 100, false, 66  },
        { "d_qvs50_cabac",          true,  true,  true,  true,  true,  false, 100, false, 50  },
        { "e_qvs33_cabac",          true,  true,  true,  true,  true,  false, 100, false, 33  },
        { "f_qvs_unset_cabac",      true,  true,  true,  true,  true,  false, 100, false, -1  },
        { "g_qvs100_noquality",     true,  true,  false, true,  true,  false, 100, false, 100 },
        { "h_qvs100_unconstr",      true,  true,  false, true,  true,  false, 100, true,  100 },
        { "i_qvs100_api_first",     true,  true,  true,  true,  true,  true,  100, false, 100 },
    };

    for (const auto& v : variants) {
        std::printf("[probe] %s\n", v.name);
        runVariant(v, dir);
    }

    MFShutdown();
    std::printf("[probe] done -> %s\n", dir.string().c_str());
    return 0;
}
