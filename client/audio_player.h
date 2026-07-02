
#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

struct IAudioClient;
struct IAudioRenderClient;

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    bool init(int sampleRate, int channels);
    void cleanup();

    bool play(const uint8_t* pcm16, int size);

private:
    void playLoop();

    IAudioClient* audioClient_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;

    std::thread playThread_;
    std::atomic<bool> running_{false};
    std::mutex queueMtx_;
    std::condition_variable queueCV_;
    std::queue<std::vector<uint8_t>> pcmQueue_;

    int sampleRate_ = 0;
    int channels_ = 0;
    UINT32 bufferFrameCount_ = 0;
};

#endif
