
#ifndef AUDIO_ENCODER_H
#define AUDIO_ENCODER_H

#include <cstdint>
#include <vector>
#include <mutex>

struct IMFTransform;

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    bool init(int sampleRate, int channels, int bitrate);
    void cleanup();

    bool encode(const uint8_t* pcm16, int numSamples, std::vector<uint8_t>& output);

    const std::vector<uint8_t>& audioSpecificConfig() const { return asc_; }
    bool initialized() const { return initialized_; }

    static constexpr int FRAME_SAMPLES = 1024;

private:
    bool initEncoder();
    bool processOutput(std::vector<uint8_t>& output);

    IMFTransform* encoder_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;
    int bitrate_ = 0;
    int frameSize_ = 0;
    std::vector<uint8_t> asc_;
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif
