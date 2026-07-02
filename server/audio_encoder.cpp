
#define NOMINMAX
#include "audio_encoder.h"
#include <iostream>
#include <algorithm>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <propvarutil.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

AudioEncoder::AudioEncoder() {}
AudioEncoder::~AudioEncoder() { cleanup(); }

bool AudioEncoder::init(int sampleRate, int channels, int bitrate) {
    std::lock_guard<std::mutex> lock(mtx_);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[AudioEncoder] CoInitializeEx failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[AudioEncoder] MFStartup failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    sampleRate_ = sampleRate;
    channels_ = channels;
    bitrate_ = bitrate;
    frameSize_ = FRAME_SAMPLES * channels * 2;

    if (!initEncoder()) {
        std::cerr << "[AudioEncoder] initEncoder failed" << std::endl;
        cleanup();
        return false;
    }

    initialized_ = true;
    std::cout << "[AudioEncoder] AAC encoder initialized: "
              << sampleRate_ << "Hz " << channels_ << "ch "
              << bitrate_ / 1000 << "kbps" << std::endl;
    return true;
}

bool AudioEncoder::initEncoder() {
    HRESULT hr;

    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Audio, MFAudioFormat_PCM };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Audio, MFAudioFormat_AAC };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &inputInfo, &outputInfo, &activates, &count);
    if (SUCCEEDED(hr) && count > 0) {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
        if (FAILED(hr)) {
            std::cerr << "[AudioEncoder] ActivateObject failed: 0x" << std::hex << hr << std::dec << std::endl;
        }
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (!encoder_) {
        std::cerr << "[AudioEncoder] No AAC encoder found" << std::endl;
        return false;
    }
    std::cout << "[AudioEncoder] AAC encoder MFT created" << std::endl;

    IMFAttributes* mftAttr = nullptr;
    if (SUCCEEDED(encoder_->GetAttributes(&mftAttr))) {
        mftAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
        mftAttr->Release();
    }

    ICodecAPI* codecApi = nullptr;
    if (SUCCEEDED(encoder_->QueryInterface(IID_PPV_ARGS(&codecApi)))) {
        VARIANT var;
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVEncCommonLowLatency, &var);
        codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &var);

        var.vt = VT_UI4;
        var.ulVal = eAVEncCommonRateControlMode_CBR;
        codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
        var.ulVal = bitrate_;
        codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
        codecApi->Release();
        std::cout << "[AudioEncoder] AAC CBR " << bitrate_ << "bps set via ICodecAPI" << std::endl;
    }

    IMFMediaType* outputType = nullptr;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return false;
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate_);
    outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels_);
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(bitrate_ / 8));
    outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    hr = encoder_->SetOutputType(0, outputType, 0);
    if (FAILED(hr)) {
        std::cerr << "[AudioEncoder] SetOutputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        outputType->Release();
        return false;
    }

    IMFMediaType* fullOutputType = nullptr;
    hr = encoder_->GetOutputCurrentType(0, &fullOutputType);
    if (SUCCEEDED(hr)) {
        BYTE* ascData = nullptr;
        UINT32 ascSize = 0;
        hr = fullOutputType->GetAllocatedBlob(MF_MT_USER_DATA, &ascData, &ascSize);
        if (SUCCEEDED(hr) && ascData && ascSize > 0) {
            asc_.assign(ascData, ascData + ascSize);
            CoTaskMemFree(ascData);
        }
        fullOutputType->Release();
    }

    std::cout << "[AudioEncoder] AAC output type set, ASC size=" << asc_.size() << std::endl;

    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate_);
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels_);
    UINT32 blockAlign = channels_ * 2;
    inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
    inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate_ * blockAlign);
    inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    hr = encoder_->SetInputType(0, inputType, 0);
    inputType->Release();
    if (FAILED(hr)) {
        std::cerr << "[AudioEncoder] SetInputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        outputType->Release();
        return false;
    }
    outputType->Release();

    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return false;

    return true;
}

bool AudioEncoder::encode(const uint8_t* pcm16, int numSamples, std::vector<uint8_t>& output) {
    std::lock_guard<std::mutex> lock(mtx_);
    output.clear();
    if (!initialized_) return false;

    int dataSize = numSamples * channels_ * 2;

    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* buf = nullptr;
    hr = MFCreateMemoryBuffer((DWORD)dataSize, &buf);
    if (FAILED(hr)) { sample->Release(); return false; }

    BYTE* ptr = nullptr;
    hr = buf->Lock(&ptr, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(ptr, pcm16, dataSize);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)dataSize);
    }

    sample->AddBuffer(buf);
    buf->Release();

    LONGLONG duration = (LONGLONG)(10000000LL * numSamples / sampleRate_);
    sample->SetSampleTime(0);
    sample->SetSampleDuration(duration);

    hr = encoder_->ProcessInput(0, sample, 0);
    sample->Release();

    if (hr == MF_E_NOTACCEPTING) {
        std::vector<uint8_t> drainBuf;
        for (int i = 0; i < 4; i++) {
            processOutput(drainBuf);
            if (!output.empty()) break;
        }
        return !output.empty();
    }

    if (FAILED(hr)) {
        std::cerr << "[AudioEncoder] ProcessInput failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    processOutput(output);
    return !output.empty();
}

bool AudioEncoder::processOutput(std::vector<uint8_t>& output) {
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    encoder_->GetOutputStreamInfo(0, &streamInfo);

    for (int loop = 0; loop < 8; loop++) {
        MFT_OUTPUT_DATA_BUFFER outputData = {};

        bool needSample = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
        IMFSample* userSample = nullptr;
        if (needSample) {
            MFCreateSample(&userSample);
            IMFMediaBuffer* buf = nullptr;
            if (SUCCEEDED(MFCreateMemoryBuffer(streamInfo.cbSize > 0 ? streamInfo.cbSize : 2048, &buf))) {
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
            if (userSample) userSample->Release();
            break;
        }

        IMFSample* resultSample = outputData.pSample;
        if (resultSample) {
            IMFMediaBuffer* buf = nullptr;
            hr = resultSample->GetBufferByIndex(0, &buf);
            if (SUCCEEDED(hr)) {
                BYTE* ptr = nullptr;
                DWORD curLen = 0;
                hr = buf->Lock(&ptr, nullptr, &curLen);
                if (SUCCEEDED(hr) && curLen > 0) {
                    size_t oldSz = output.size();
                    output.resize(oldSz + curLen);
                    memcpy(output.data() + oldSz, ptr, curLen);
                    buf->Unlock();
                }
                buf->Release();
            }
            resultSample->Release();
        }

        if (userSample && userSample != resultSample)
            userSample->Release();

        if (outputData.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE) break;
    }

    return !output.empty();
}

void AudioEncoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder_->Release();
        encoder_ = nullptr;
    }

    MFShutdown();
    sampleRate_ = 0;
    channels_ = 0;
    bitrate_ = 0;
    frameSize_ = 0;
    asc_.clear();
    initialized_ = false;
}
