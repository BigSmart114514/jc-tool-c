
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "audio_capture.h"
#include <iostream>
#include <algorithm>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")

static const REFERENCE_TIME HNS_BUFFER_DURATION = 10 * 10000;

AudioCapture::AudioCapture() {}
AudioCapture::~AudioCapture() { cleanup(); }

bool AudioCapture::init() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[AudioCapture] CoInitializeEx failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] CoCreateInstance enumerator failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetDefaultAudioEndpoint failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
    device->Release();
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Activate IAudioClient failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }

    WAVEFORMATEX* mixFmt = nullptr;
    hr = audioClient_->GetMixFormat(&mixFmt);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetMixFormat failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }
    mixFormat_ = mixFmt;

    sampleRate_ = mixFmt->nSamplesPerSec;
    channels_ = mixFmt->nChannels;
    bitsPerSample_ = mixFmt->wBitsPerSample;

    audioEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent_) {
        std::cerr << "[AudioCapture] CreateEvent failed" << std::endl;
        enumerator->Release();
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                  HNS_BUFFER_DURATION, 0, mixFmt, nullptr);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] Initialize loopback failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }

    hr = audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] GetService IAudioCaptureClient failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }

    hr = audioClient_->SetEventHandle(audioEvent_);
    if (FAILED(hr)) {
        std::cerr << "[AudioCapture] SetEventHandle failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }

    accBuffer_.resize((FRAME_SAMPLES + 480) * channels_ * 2);
    accSamples_ = 0;

    enumerator->Release();
    std::cout << "[AudioCapture] Initialized: " << sampleRate_ << "Hz "
              << channels_ << "ch " << bitsPerSample_ << "bit" << std::endl;
    return true;
}

void AudioCapture::cleanup() {
    stop();
    if (audioEvent_) { CloseHandle(audioEvent_); audioEvent_ = nullptr; }
    if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
    if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
    if (mixFormat_) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }
}

void AudioCapture::start() {
    if (running_) return;
    if (!audioClient_ || !captureClient_) return;

    running_ = true;
    audioClient_->Start();
    captureThread_ = std::thread(&AudioCapture::captureLoop, this);
    std::cout << "[AudioCapture] Started" << std::endl;
}

void AudioCapture::stop() {
    running_ = false;
    if (audioEvent_) SetEvent(audioEvent_);
    if (captureThread_.joinable()) captureThread_.join();
    if (audioClient_) {
        audioClient_->Stop();
        audioClient_->Reset();
    }
    accSamples_ = 0;
}

void AudioCapture::convertAndAccumulate(const BYTE* src, UINT32 frames) {
    int totalSamples = (int)frames * channels_;
    int newCount = accSamples_ + frames;
    size_t needed = (size_t)newCount * channels_ * 2;
    if (needed > accBuffer_.size()) {
        accBuffer_.resize(needed + FRAME_SAMPLES * channels_ * 2);
    }

    size_t dstOffset = (size_t)accSamples_ * channels_ * 2;

    if (bitsPerSample_ == 32) {
        const float* floatSrc = (const float*)src;
        for (int i = 0; i < totalSamples; i++) {
            float val = std::clamp(floatSrc[i], -1.0f, 1.0f);
            int16_t pcm = (int16_t)(val * 32767.0f);
            accBuffer_[dstOffset + i * 2] = (uint8_t)(pcm & 0xFF);
            accBuffer_[dstOffset + i * 2 + 1] = (uint8_t)((pcm >> 8) & 0xFF);
        }
    } else if (bitsPerSample_ == 16) {
        size_t bytes = (size_t)frames * channels_ * 2;
        if (dstOffset + bytes <= accBuffer_.size()) {
            memcpy(accBuffer_.data() + dstOffset, src, bytes);
        }
    } else if (bitsPerSample_ == 24) {
        for (UINT32 f = 0; f < frames; f++) {
            for (UINT32 c = 0; c < (UINT32)channels_; c++) {
                int val = src[(f * channels_ + c) * 3] |
                          (src[(f * channels_ + c) * 3 + 1] << 8) |
                          (src[(f * channels_ + c) * 3 + 2] << 16);
                if (val & 0x800000) val |= ~0xFFFFFF;
                int16_t pcm = (int16_t)(val >> 8);
                size_t pos = dstOffset + (size_t)f * channels_ * 2 + (size_t)c * 2;
                if (pos + 1 < accBuffer_.size()) {
                    accBuffer_[pos] = (uint8_t)(pcm & 0xFF);
                    accBuffer_[pos + 1] = (uint8_t)((pcm >> 8) & 0xFF);
                }
            }
        }
    }

    accSamples_ = newCount;
}

void AudioCapture::captureLoop() {
    DWORD taskIndex = 0;
    HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);

    while (running_) {
        DWORD wait = WaitForSingleObject(audioEvent_, 1000);
        if (wait == WAIT_OBJECT_0 && running_) {
            UINT32 nextSize = 0;
            HRESULT hr = captureClient_->GetNextPacketSize(&nextSize);
            if (FAILED(hr)) continue;

            while (nextSize > 0 && running_) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                hr = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr) && data && frames > 0) {
                    convertAndAccumulate(data, frames);
                    captureClient_->ReleaseBuffer(frames);

                    while (accSamples_ >= FRAME_SAMPLES && running_) {
                        if (callback_) {
                            callback_(accBuffer_.data(), FRAME_SAMPLES);
                        }
                        int remaining = accSamples_ - FRAME_SAMPLES;
                        if (remaining > 0) {
                            size_t bytesPerFrame = (size_t)channels_ * 2;
                            memmove(accBuffer_.data(),
                                    accBuffer_.data() + FRAME_SAMPLES * bytesPerFrame,
                                    (size_t)remaining * bytesPerFrame);
                        }
                        accSamples_ = remaining;
                    }
                } else if (FAILED(hr)) {
                    captureClient_->ReleaseBuffer(0);
                }

                hr = captureClient_->GetNextPacketSize(&nextSize);
                if (FAILED(hr)) break;
            }
        }
    }

    if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
}
