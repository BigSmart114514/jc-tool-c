
#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool init();
    void cleanup();
    
    const uint8_t* capture(bool& hasNew);
    bool captureTexture(ID3D11Texture2D** outTex);
    
    ID3D11Device* getDevice() const { return device_; }
    ID3D11DeviceContext* getContext() const { return context_; }
    
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    int getOriginX() const { return desktopOriginX_; }
    int getOriginY() const { return desktopOriginY_; }

    // Select a specific output by DeviceName (e.g. L"\\\\.\\DISPLAY4")
    // If not set, defaults to output 0 (primary)
    void setCaptureOutput(const std::wstring& deviceName) { captureOutputName_ = deviceName; }
    const std::wstring& getCaptureOutput() const { return captureOutputName_; }

private:
    bool initDXGI();
    bool initDuplication(int outputIndex = 0);
    bool initGDI();

    void cleanupDuplicationOnly();
    void cleanupDXGIOnly();
    bool resetDXGI();

    // DXGI
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* stagingTexture_ = nullptr;
    ID3D11Texture2D* gpuCopyTexture_ = nullptr;
    bool frameAcquired_ = false;

    // GDI fallback
    HDC hdcScreen_ = nullptr;
    HDC hdcMem_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    void* gdiBits_ = nullptr;

    uint8_t* frameBuffer_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int desktopOriginX_ = 0;
    int desktopOriginY_ = 0;
    bool useGDI_ = false;
    bool initialized_ = false;
    std::wstring captureOutputName_;  // blank means output 0
};

#endif // SCREEN_CAPTURE_H
