
#ifndef MEDIA_ENCODER_H
#define MEDIA_ENCODER_H

#include <vector>
#include <mutex>
#include <cstdint>
#include <d3d11.h>

struct IMFTransform;
struct IMFMediaType;
struct IMFSample;

class MediaEncoder {
public:
    MediaEncoder();
    ~MediaEncoder();

    bool init(ID3D11Device* device, int srcW, int srcH, int dstW, int dstH, int fps, int bitrate = 3000000);
    void cleanup();

    bool encodeFromTexture(ID3D11Texture2D* bgraTex, int64_t pts, std::vector<uint8_t>& output, bool keyframe = false);
    bool encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& output, bool keyframe = false);

    bool initialized() const { return initialized_; }
    bool hasGPUPath() const { return hasGPUPath_; }
    int encodedWidth() const { return width_; }
    int encodedHeight() const { return height_; }

private:
    bool initEncoder();
    bool initVideoProcessor();
    bool processOutput(std::vector<uint8_t>& output);
    bool createInputSample(const uint8_t* nv12Data, int64_t pts, bool keyframe);
    bool flushEncoder(std::vector<uint8_t>& output);

    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;

    ID3D11VideoDevice* videoDevice_ = nullptr;
    ID3D11VideoContext* videoContext_ = nullptr;
    ID3D11VideoProcessorEnumerator* vpEnum_ = nullptr;
    ID3D11VideoProcessor* videoProcessor_ = nullptr;
    ID3D11Texture2D* nv12Texture_ = nullptr;
    ID3D11Texture2D* nv12Staging_ = nullptr;
    ID3D11VideoProcessorOutputView* vpOutputView_ = nullptr;

    IMFTransform* encoder_ = nullptr;
    IMFMediaType* inputType_ = nullptr;
    IMFMediaType* outputType_ = nullptr;

    int srcWidth_ = 0;
    int srcHeight_ = 0;
    int width_ = 0;
    int height_ = 0;
    int alignedW_ = 0;
    int alignedH_ = 0;
    int fps_ = 0;
    int bitrate_ = 3000000;

    bool initialized_ = false;
    bool hasGPUPath_ = false;
    std::mutex mtx_;
};

#endif // MEDIA_ENCODER_H
