
#include "screen_capture.h"
#include <iostream>

ScreenCapture::ScreenCapture() {}

ScreenCapture::~ScreenCapture() {
    cleanup();
}

bool ScreenCapture::init() {
    if (!initDXGI()) {
        return initGDI();
    }
    return true;
}

bool ScreenCapture::initDXGI() {
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                    nullptr, 0, D3D11_SDK_VERSION, &device_, &fl, &context_);
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDev = nullptr;
    hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    if (FAILED(hr)) { device_->Release(); device_ = nullptr; return false; }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    if (FAILED(hr)) { device_->Release(); device_ = nullptr; return false; }

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(0, &output);
    adapter->Release();
    if (FAILED(hr)) { device_->Release(); device_ = nullptr; return false; }

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();
    if (FAILED(hr)) { device_->Release(); device_ = nullptr; return false; }

    hr = output1->DuplicateOutput(device_, &duplication_);
    output1->Release();
    if (FAILED(hr)) { device_->Release(); device_ = nullptr; return false; }

    width_ = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device_->CreateTexture2D(&td, nullptr, &stagingTexture_);
    if (FAILED(hr)) {
        duplication_->Release();
        device_->Release();
        duplication_ = nullptr;
        device_ = nullptr;
        return false;
    }

    frameBuffer_ = new uint8_t[size_t(width_) * height_ * 4];
    memset(frameBuffer_, 0, size_t(width_) * height_ * 4);

    useGDI_ = false;
    initialized_ = true;

    std::cout << "[Capture] DXGI initialized: "
            << width_ << "x" << height_
            << ", bufferSize=" << size_t(width_) * height_ * 4
            << std::endl;
    return true;
}

bool ScreenCapture::initGDI() {
    hdcScreen_ = GetDC(nullptr);
    hdcMem_ = CreateCompatibleDC(hdcScreen_);
    
    width_ = GetDeviceCaps(hdcScreen_, DESKTOPHORZRES);
    height_ = GetDeviceCaps(hdcScreen_, DESKTOPVERTRES);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -height_;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBitmap_ = CreateDIBSection(hdcMem_, &bmi, DIB_RGB_COLORS, &gdiBits_, nullptr, 0);
    if (!hBitmap_) {
        DeleteDC(hdcMem_);
        ReleaseDC(nullptr, hdcScreen_);
        return false;
    }

    SelectObject(hdcMem_, hBitmap_);
    frameBuffer_ = static_cast<uint8_t*>(gdiBits_);
    useGDI_ = true;
    initialized_ = true;

    std::cout << "[Capture] GDI: " << width_ << "x" << height_ << std::endl;
    return true;
}

void ScreenCapture::cleanup() {
    if (frameAcquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }

    if (stagingTexture_) { stagingTexture_->Release(); stagingTexture_ = nullptr; }
    if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }

    if (hBitmap_) { DeleteObject(hBitmap_); hBitmap_ = nullptr; }
    if (hdcMem_) { DeleteDC(hdcMem_); hdcMem_ = nullptr; }
    if (hdcScreen_) { ReleaseDC(nullptr, hdcScreen_); hdcScreen_ = nullptr; }

    if (!useGDI_ && frameBuffer_) { delete[] frameBuffer_; }
    frameBuffer_ = nullptr;
    initialized_ = false;
}

const uint8_t* ScreenCapture::capture(bool& hasNew) {
    hasNew = false;
    if (!initialized_) return nullptr;

    if (useGDI_) {
        BitBlt(hdcMem_, 0, 0, width_, height_, hdcScreen_, 0, 0, SRCCOPY);
        hasNew = true;
        return frameBuffer_;
    }

    if (frameAcquired_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource* res = nullptr;
    HRESULT hr = duplication_->AcquireNextFrame(16, &fi, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return frameBuffer_;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        std::cerr << "[Capture] DXGI access lost, recreating duplication" << std::endl;
        resetDXGI();
        return nullptr;
    }

    if (FAILED(hr)) {
        static int failCount = 0;
        if (++failCount % 60 == 0) {
            std::cerr << "[Capture] DXGI AcquireNextFrame failed, hr=0x"
                    << std::hex << hr << std::dec << std::endl;
        }

        return nullptr;
    }

    frameAcquired_ = true;
    hasNew = true;

    ID3D11Texture2D* tex = nullptr;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr)) return frameBuffer_;

    context_->CopyResource(stagingTexture_, tex);
    tex->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context_->Map(stagingTexture_, 0, D3D11_MAP_READ, 0, &mapped))) {
        if (mapped.RowPitch == width_ * 4) {
            memcpy(frameBuffer_, mapped.pData, size_t(width_) * height_ * 4);
        } else {
            for (int y = 0; y < height_; y++) {
                memcpy(frameBuffer_ + y * width_ * 4,
                       (uint8_t*)mapped.pData + y * mapped.RowPitch, width_ * 4);
            }
        }
        context_->Unmap(stagingTexture_, 0);
    }

    return frameBuffer_;
}

void ScreenCapture::cleanupDXGIOnly() {
    if (frameAcquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }

    if (stagingTexture_) {
        stagingTexture_->Release();
        stagingTexture_ = nullptr;
    }

    if (duplication_) {
        duplication_->Release();
        duplication_ = nullptr;
    }

    if (context_) {
        context_->Release();
        context_ = nullptr;
    }

    if (device_) {
        device_->Release();
        device_ = nullptr;
    }

    if (!useGDI_ && frameBuffer_) {
        delete[] frameBuffer_;
        frameBuffer_ = nullptr;
    }

    initialized_ = false;
}

bool ScreenCapture::resetDXGI() {
    std::cerr << "[Capture] Resetting DXGI duplication..." << std::endl;

    cleanupDXGIOnly();

    if (initDXGI()) {
        std::cout << "[Capture] DXGI reset OK" << std::endl;
        return true;
    }

    std::cerr << "[Capture] DXGI reset failed, falling back to GDI" << std::endl;
    return initGDI();
}
