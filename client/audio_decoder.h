
#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <cstdint>
#include <vector>
#include <mutex>

struct IMFTransform;

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    bool init(int sampleRate, int channels, const uint8_t* asc, int ascLen);
    void cleanup();

    bool decode(const uint8_t* aacData, int aacSize, std::vector<uint8_t>& pcmOut);

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }
    bool initialized() const { return initialized_; }

private:
    bool initDecoder();
    bool processOutput(std::vector<uint8_t>& pcmOut);

    IMFTransform* decoder_ = nullptr;
    int sampleRate_ = 0;
    int channels_ = 0;
    std::vector<uint8_t> asc_;
    bool initialized_ = false;
    std::mutex mtx_;
};

#endif
