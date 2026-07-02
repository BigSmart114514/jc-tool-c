
#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>

struct IAudioClient;
struct IAudioCaptureClient;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool init();
    void cleanup();
    void start();
    void stop();

    bool initialized() const { return audioClient_ != nullptr; }
    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

    using DataCallback = std::function<void(const uint8_t* pcm16, int numSamples)>;
    void setDataCallback(DataCallback cb) { callback_ = std::move(cb); }

    static constexpr int FRAME_SAMPLES = 1024;

private:
    void captureLoop();
    void convertAndAccumulate(const BYTE* src, UINT32 frames);

    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    HANDLE audioEvent_ = nullptr;
    void* mixFormat_ = nullptr;

    std::thread captureThread_;
    std::atomic<bool> running_{false};
    DataCallback callback_;

    int sampleRate_ = 0;
    int channels_ = 0;
    int bitsPerSample_ = 0;

    std::vector<uint8_t> accBuffer_;
    int accSamples_ = 0;
};

#endif
