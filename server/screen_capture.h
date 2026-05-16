
#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdint>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    bool init();
    void cleanup();
    
    const uint8_t* capture(bool& hasNew);
    
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    bool initDXGI();
    bool initGDI();

    void cleanupDXGIOnly();
    bool resetDXGI();

    // DXGI
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* stagingTexture_ = nullptr;
    bool frameAcquired_ = false;

    // GDI fallback
    HDC hdcScreen_ = nullptr;
    HDC hdcMem_ = nullptr;
    HBITMAP hBitmap_ = nullptr;
    void* gdiBits_ = nullptr;

    uint8_t* frameBuffer_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool useGDI_ = false;
    bool initialized_ = false;
};

#endif // SCREEN_CAPTURE_H
