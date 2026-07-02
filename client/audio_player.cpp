
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "audio_player.h"
#include <iostream>
#include <algorithm>
#include <audioclient.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <avrt.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")

static const REFERENCE_TIME HNS_BUFFER_DURATION = 30 * 10000;

AudioPlayer::AudioPlayer() {}
AudioPlayer::~AudioPlayer() { cleanup(); }

bool AudioPlayer::init(int sampleRate, int channels) {
    sampleRate_ = sampleRate;
    channels_ = channels;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::cerr << "[AudioPlayer] CoInitializeEx failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] Create enumerator failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] GetDefaultAudioEndpoint failed: 0x" << std::hex << hr << std::dec << std::endl;
        enumerator->Release();
        return false;
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient_);
    device->Release();
    enumerator->Release();
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] Activate IAudioClient failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)channels_;
    wfx.nSamplesPerSec = (DWORD)sampleRate_;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels_ * 2);
    wfx.nAvgBytesPerSec = sampleRate_ * wfx.nBlockAlign;

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                  HNS_BUFFER_DURATION, 0, &wfx, nullptr);
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] Initialize failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = audioClient_->GetBufferSize(&bufferFrameCount_);
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] GetBufferSize failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = audioClient_->GetService(IID_PPV_ARGS(&renderClient_));
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] GetService IAudioRenderClient failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    UINT32 padding = 0;
    audioClient_->GetCurrentPadding(&padding);
    UINT32 avail = bufferFrameCount_ - padding;
    if (avail > 0) {
        BYTE* silenceData = nullptr;
        if (SUCCEEDED(renderClient_->GetBuffer(avail, &silenceData))) {
            memset(silenceData, 0, avail * wfx.nBlockAlign);
            renderClient_->ReleaseBuffer(avail, 0);
        }
    }

    hr = audioClient_->Start();
    if (FAILED(hr)) {
        std::cerr << "[AudioPlayer] Start failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    running_ = true;
    playThread_ = std::thread(&AudioPlayer::playLoop, this);

    std::cout << "[AudioPlayer] Initialized: " << sampleRate_ << "Hz "
              << channels_ << "ch, buffer=" << bufferFrameCount_ << " frames" << std::endl;
    return true;
}

void AudioPlayer::cleanup() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        queueCV_.notify_all();
    }
    if (playThread_.joinable()) playThread_.join();

    if (audioClient_) {
        audioClient_->Stop();
        audioClient_->Release();
        audioClient_ = nullptr;
    }
    if (renderClient_) {
        renderClient_->Release();
        renderClient_ = nullptr;
    }

    std::lock_guard<std::mutex> lock(queueMtx_);
    while (!pcmQueue_.empty()) pcmQueue_.pop();
}

bool AudioPlayer::play(const uint8_t* pcm16, int size) {
    if (!running_ || !renderClient_) return false;

    std::vector<uint8_t> data(pcm16, pcm16 + size);
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        if (pcmQueue_.size() > 10) {
            return false;
        }
        pcmQueue_.push(std::move(data));
    }
    queueCV_.notify_one();
    return true;
}

void AudioPlayer::playLoop() {
    DWORD taskIndex = 0;
    HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Playback", &taskIndex);

    int blockAlign = channels_ * 2;

    while (running_) {
        std::vector<uint8_t> data;
        {
            std::unique_lock<std::mutex> lock(queueMtx_);
            if (!queueCV_.wait_for(lock, std::chrono::milliseconds(50),
                                   [this] { return !pcmQueue_.empty() || !running_; })) {
                continue;
            }
            if (!running_) break;
            data = std::move(pcmQueue_.front());
            pcmQueue_.pop();
        }

        UINT32 totalFrames = (UINT32)(data.size() / blockAlign);
        UINT32 written = 0;

        while (written < totalFrames && running_) {
            UINT32 padding = 0;
            HRESULT hr = audioClient_->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;

            UINT32 avail = bufferFrameCount_ - padding;
            if (avail == 0) {
                Sleep(5);
                continue;
            }

            UINT32 writeFrames = (std::min)(avail, totalFrames - written);
            BYTE* dst = nullptr;
            hr = renderClient_->GetBuffer(writeFrames, &dst);
            if (FAILED(hr)) break;

            memcpy(dst, data.data() + (size_t)written * blockAlign, (size_t)writeFrames * blockAlign);
            renderClient_->ReleaseBuffer(writeFrames, 0);
            written += writeFrames;
        }
    }

    if (taskHandle) AvRevertMmThreadCharacteristics(taskHandle);
}
