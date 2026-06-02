
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
    bool hasGPUPath() const { return hasComputeShader_; }
    int encodedWidth() const { return width_; }
    int encodedHeight() const { return height_; }

private:
    bool initEncoder();
    bool initComputeShader();
    bool processOutput(std::vector<uint8_t>& output);
    bool createInputSample(const uint8_t* nv12Data, int64_t pts, bool keyframe);
    bool flushEncoder(std::vector<uint8_t>& output);

    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;

    ID3D11ComputeShader* cs_ = nullptr;
    ID3D11Buffer* csConstants_ = nullptr;
    ID3D11ShaderResourceView* inputSRV_ = nullptr;
    ID3D11UnorderedAccessView* yUAV_ = nullptr;
    ID3D11UnorderedAccessView* uvUAV_ = nullptr;
    ID3D11Texture2D* yTexture_ = nullptr;
    ID3D11Texture2D* uvTexture_ = nullptr;
    ID3D11Texture2D* yStaging_ = nullptr;
    ID3D11Texture2D* uvStaging_ = nullptr;
    ID3D11Texture2D* cachedInputTex_ = nullptr;

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
    bool hasComputeShader_ = false;
    std::mutex mtx_;
};

#endif // MEDIA_ENCODER_H
