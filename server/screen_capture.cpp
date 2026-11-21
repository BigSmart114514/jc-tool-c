
#include "screen_capture.h"
#include <windows.h>
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
    if (!device_) {
        D3D_FEATURE_LEVEL fl;
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                        nullptr, 0, D3D11_SDK_VERSION, &device_, &fl, &context_);
        if (FAILED(hr)) return false;
    }

    // If a specific output name is set, find its index
    int outputIndex = 0;
    if (!captureOutputName_.empty()) {
        outputIndex = -1;
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                for (int i = 0;; ++i) {
                    IDXGIOutput* output = nullptr;
                    if (FAILED(adapter->EnumOutputs(i, &output))) break;
                    DXGI_OUTPUT_DESC desc;
                    output->GetDesc(&desc);
                    if (captureOutputName_ == desc.DeviceName) {
                        outputIndex = i;
                        output->Release();
                        break;
                    }
                    output->Release();
                }
                adapter->Release();
            }
            dxgiDev->Release();
        }
        if (outputIndex < 0) {
            std::cerr << "[Capture] Output device not found: ";
            std::wcerr << captureOutputName_ << std::endl;
            // Find the virtual display: prefer non-primary output
            outputIndex = -1;
            int totalOutputs = 0;
            IDXGIAdapter* adapter2 = nullptr;
            IDXGIDevice* dxgiDev2 = nullptr;
            if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev2)) &&
                SUCCEEDED(dxgiDev2->GetAdapter(&adapter2))) {
                for (int i = 0;; ++i) {
                    IDXGIOutput* output = nullptr;
                    if (FAILED(adapter2->EnumOutputs(i, &output))) break;
                    ++totalOutputs;
                    DXGI_OUTPUT_DESC desc;
                    output->GetDesc(&desc);
                    MONITORINFOEXW mi = { sizeof(mi) };
                    if (GetMonitorInfoW(desc.Monitor, &mi)) {
                        if (!(mi.dwFlags & MONITORINFOF_PRIMARY)) {
                            outputIndex = i;
                            output->Release();
                            break;
                        }
                    }
                    output->Release();
                }
                adapter2->Release();
            }
            if (dxgiDev2) dxgiDev2->Release();
            if (outputIndex < 0 && totalOutputs > 0)
                outputIndex = totalOutputs - 1; // fallback: highest index
            if (outputIndex < 0)
                outputIndex = 0;
            std::cerr << "[Capture] Using output index: " << outputIndex << std::endl;
        }
    }

    return initDuplication(outputIndex);
}

bool ScreenCapture::initDuplication(int outputIndex) {
    if (!device_) return false;

    IDXGIDevice* dxgiDev = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    if (FAILED(hr)) return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(outputIndex, &output);
    adapter->Release();
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC desc;
    output->GetDesc(&desc);

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(device_, &duplication_);
    output1->Release();
    if (FAILED(hr)) return false;

    width_ = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    desktopOriginX_ = desc.DesktopCoordinates.left;
    desktopOriginY_ = desc.DesktopCoordinates.top;
    currentOutputIndex_ = outputIndex;   // 记住当前抓的是哪个输出，DXGI reset 时复用

    // 抓取分辨率可能变化（虚拟显示器切换 / 显示模式改变）。
    // 以前这里只在第一次创建纹理，之后一直沿用旧尺寸，
    // CopyResource 尺寸不匹配 → 花屏。现在尺寸不一致就重建。
    auto recreateIfSizeChanged = [&](ID3D11Texture2D*& tex, const D3D11_TEXTURE2D_DESC& wanted) {
        if (tex) {
            D3D11_TEXTURE2D_DESC cur = {};
            tex->GetDesc(&cur);
            if (cur.Width == wanted.Width && cur.Height == wanted.Height
                && cur.Format == wanted.Format) {
                return true;
            }
            tex->Release();
            tex = nullptr;
        }
        return SUCCEEDED(device_->CreateTexture2D(&wanted, nullptr, &tex));
    };

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!recreateIfSizeChanged(stagingTexture_, td)) {
        duplication_->Release(); duplication_ = nullptr;
        return false;
    }

    D3D11_TEXTURE2D_DESC gpuDesc = td;
    gpuDesc.Usage = D3D11_USAGE_DEFAULT;
    gpuDesc.CPUAccessFlags = 0;
    gpuDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!recreateIfSizeChanged(gpuCopyTexture_, gpuDesc)) {
        stagingTexture_->Release(); stagingTexture_ = nullptr;
        duplication_->Release(); duplication_ = nullptr;
        return false;
    }

    // 记录 frameBuffer_ 当前对应的宽高，避免分辨率变化后仍按旧 buffer 写入
    if (!frameBuffer_ || frameBufferW_ != width_ || frameBufferH_ != height_) {
        // GDI 模式下的 frameBuffer_ 是 DIB 内部指针，不能 delete
        if (!useGDI_) delete[] frameBuffer_;
        frameBuffer_ = new uint8_t[size_t(width_) * height_ * 4];
        memset(frameBuffer_, 0, size_t(width_) * height_ * 4);
        frameBufferW_ = width_;
        frameBufferH_ = height_;
    }

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
    cleanupDuplicationOnly();

    if (gpuCopyTexture_) { gpuCopyTexture_->Release(); gpuCopyTexture_ = nullptr; }
    if (stagingTexture_) { stagingTexture_->Release(); stagingTexture_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }

    if (hBitmap_) { DeleteObject(hBitmap_); hBitmap_ = nullptr; }
    if (hdcMem_) { DeleteDC(hdcMem_); hdcMem_ = nullptr; }
    if (hdcScreen_) { ReleaseDC(nullptr, hdcScreen_); hdcScreen_ = nullptr; }

    if (!useGDI_ && frameBuffer_) { delete[] frameBuffer_; }
    frameBuffer_ = nullptr;
    frameBufferW_ = frameBufferH_ = 0;
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

bool ScreenCapture::captureTexture(ID3D11Texture2D** outTex) {
    if (!outTex) return false;
    *outTex = nullptr;
    if (!initialized_ || useGDI_) return false;

    if (frameAcquired_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource* res = nullptr;
    HRESULT hr = duplication_->AcquireNextFrame(16, &fi, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        resetDXGI();
        return false;
    }

    if (FAILED(hr)) {
        return false;
    }

    frameAcquired_ = true;

    ID3D11Texture2D* tex = nullptr;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr)) return false;

    context_->CopyResource(gpuCopyTexture_, tex);
    tex->Release();

    *outTex = gpuCopyTexture_;
    return true;
}

void ScreenCapture::cleanupDuplicationOnly() {
    if (frameAcquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frameAcquired_ = false;
    }
    if (duplication_) {
        duplication_->Release();
        duplication_ = nullptr;
    }
}

void ScreenCapture::cleanupDXGIOnly() {
    cleanupDuplicationOnly();

    if (gpuCopyTexture_) {
        gpuCopyTexture_->Release();
        gpuCopyTexture_ = nullptr;
    }

    if (stagingTexture_) {
        stagingTexture_->Release();
        stagingTexture_ = nullptr;
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
    frameBufferW_ = frameBufferH_ = 0;

    initialized_ = false;
}

bool ScreenCapture::resetDXGI() {
    std::cerr << "[Capture] Resetting DXGI duplication..." << std::endl;

    cleanupDuplicationOnly();

    // 抓的是同一个输出，不能回落到默认的 0 号（通常是主屏），
    // 否则虚拟显示器会莫名其妙变成主屏画面。
    if (initDuplication(currentOutputIndex_)) {
        std::cout << "[Capture] DXGI reset OK" << std::endl;
        return true;
    }

    std::cerr << "[Capture] DXGI reset failed, falling back to GDI" << std::endl;
    cleanupDXGIOnly();
    return initGDI();
}
