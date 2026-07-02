
#define NOMINMAX
#include "audio_decoder.h"
#include <iostream>
#include <algorithm>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

AudioDecoder::AudioDecoder() {}
AudioDecoder::~AudioDecoder() { cleanup(); }

bool AudioDecoder::init(int sampleRate, int channels, const uint8_t* asc, int ascLen) {
    std::lock_guard<std::mutex> lock(mtx_);

    sampleRate_ = sampleRate;
    channels_ = channels;
    asc_.assign(asc, asc + ascLen);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[AudioDecoder] CoInitializeEx failed" << std::endl;
        return false;
    }

    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[AudioDecoder] MFStartup failed" << std::endl;
        return false;
    }

    if (!initDecoder()) {
        std::cerr << "[AudioDecoder] initDecoder failed" << std::endl;
        cleanup();
        return false;
    }

    initialized_ = true;
    std::cout << "[AudioDecoder] AAC decoder initialized: "
              << sampleRate_ << "Hz " << channels_ << "ch" << std::endl;
    return true;
}

bool AudioDecoder::initDecoder() {
    HRESULT hr;

    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Audio, MFAudioFormat_AAC };
    MFT_REGISTER_TYPE_INFO outputInfo = { MFMediaType_Audio, MFAudioFormat_PCM };

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
                   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                   &inputInfo, &outputInfo, &activates, &count);
    if (SUCCEEDED(hr) && count > 0) {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&decoder_));
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);

    if (!decoder_) {
        std::cerr << "[AudioDecoder] No AAC decoder found" << std::endl;
        return false;
    }
    std::cout << "[AudioDecoder] AAC decoder MFT created" << std::endl;

    IMFAttributes* mftAttr = nullptr;
    if (SUCCEEDED(decoder_->GetAttributes(&mftAttr))) {
        mftAttr->SetUINT32(MF_LOW_LATENCY, TRUE);
        mftAttr->Release();
    }

    IMFMediaType* inputType = nullptr;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;
    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate_);
    inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels_);
    if (!asc_.empty()) {
        inputType->SetBlob(MF_MT_USER_DATA, asc_.data(), (UINT32)asc_.size());
    }
    hr = decoder_->SetInputType(0, inputType, 0);
    inputType->Release();
    if (FAILED(hr)) {
        std::cerr << "[AudioDecoder] SetInputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    IMFMediaType* outputType = nullptr;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return false;
    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate_);
    outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels_);
    UINT32 blockAlign = channels_ * 2;
    outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, blockAlign);
    outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate_ * blockAlign);
    hr = decoder_->SetOutputType(0, outputType, 0);
    outputType->Release();
    if (FAILED(hr)) {
        std::cerr << "[AudioDecoder] SetOutputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return false;
    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return false;

    return true;
}

bool AudioDecoder::decode(const uint8_t* aacData, int aacSize, std::vector<uint8_t>& pcmOut) {
    std::lock_guard<std::mutex> lock(mtx_);
    pcmOut.clear();
    if (!initialized_ || aacSize <= 0) return false;

    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return false;

    IMFMediaBuffer* buf = nullptr;
    hr = MFCreateMemoryBuffer((DWORD)aacSize, &buf);
    if (FAILED(hr)) { sample->Release(); return false; }

    BYTE* ptr = nullptr;
    hr = buf->Lock(&ptr, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(ptr, aacData, aacSize);
        buf->Unlock();
        buf->SetCurrentLength((DWORD)aacSize);
    }

    sample->AddBuffer(buf);
    buf->Release();
    sample->SetSampleTime(0);

    hr = decoder_->ProcessInput(0, sample, 0);
    sample->Release();

    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
        std::cerr << "[AudioDecoder] ProcessInput failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    return processOutput(pcmOut);
}

bool AudioDecoder::processOutput(std::vector<uint8_t>& pcmOut) {
    bool gotOutput = false;

    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    decoder_->GetOutputStreamInfo(0, &streamInfo);

    for (int loop = 0; loop < 8; loop++) {
        MFT_OUTPUT_DATA_BUFFER outputData = {};

        bool needSample = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0;
        IMFSample* userSample = nullptr;
        if (needSample) {
            MFCreateSample(&userSample);
            IMFMediaBuffer* buf = nullptr;
            DWORD bufSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : (DWORD)(1024 * channels_ * 2);
            if (SUCCEEDED(MFCreateMemoryBuffer(bufSize, &buf))) {
                userSample->AddBuffer(buf);
                buf->Release();
            }
            outputData.pSample = userSample;
        }

        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &outputData, &status);

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
                    size_t oldSz = pcmOut.size();
                    pcmOut.resize(oldSz + curLen);
                    memcpy(pcmOut.data() + oldSz, ptr, curLen);
                    gotOutput = true;
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

    return gotOutput;
}

void AudioDecoder::cleanup() {
    std::lock_guard<std::mutex> lock(mtx_);

    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder_->Release();
        decoder_ = nullptr;
    }

    MFShutdown();
    sampleRate_ = 0;
    channels_ = 0;
    asc_.clear();
    initialized_ = false;
}
