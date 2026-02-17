
#include "display_buffer.h"

DisplayBuffer::DisplayBuffer() {
    InitializeCriticalSection(&cs_);
}

DisplayBuffer::~DisplayBuffer() {
    cleanup();
    DeleteCriticalSection(&cs_);
}

void DisplayBuffer::cleanup() {
    EnterCriticalSection(&cs_);
    if (memBitmap_) {
        if (memDC_ && oldBitmap_) SelectObject(memDC_, oldBitmap_);
        DeleteObject(memBitmap_);
        memBitmap_ = nullptr;
    }
    if (memDC_) {
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    width_ = height_ = 0;
    bitmapBits_ = nullptr;
    LeaveCriticalSection(&cs_);
}

bool DisplayBuffer::updateFrame(const uint8_t* data, int width, int height) {
    EnterCriticalSection(&cs_);

    if (width != width_ || height != height_) {
        if (memBitmap_) {
            if (memDC_ && oldBitmap_) SelectObject(memDC_, oldBitmap_);
            DeleteObject(memBitmap_);
        }
        if (memDC_) DeleteDC(memDC_);

        HDC sdc = GetDC(nullptr);
        memDC_ = CreateCompatibleDC(sdc);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        memBitmap_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bitmapBits_, nullptr, 0);
        if (!memBitmap_) {
            ReleaseDC(nullptr, sdc);
            LeaveCriticalSection(&cs_);
            return false;
        }

        oldBitmap_ = (HBITMAP)SelectObject(memDC_, memBitmap_);
        width_ = width;
        height_ = height;
        ReleaseDC(nullptr, sdc);
    }

    if (bitmapBits_ && data) {
        memcpy(bitmapBits_, data, width * height * 4);
    }

    LeaveCriticalSection(&cs_);
    return true;
}

RECT DisplayBuffer::calculateDisplayRect(int ww, int wh) {
    RECT r = {0, 0, ww, wh};
    if (width_ == 0 || height_ == 0) return r;

    float ar = (float)width_ / height_;
    float war = (float)ww / wh;

    if (ar > war) {
        int nh = (int)(ww / ar);
        r.top = (wh - nh) / 2;
        r.bottom = r.top + nh;
    } else {
        int nw = (int)(wh * ar);
        r.left = (ww - nw) / 2;
        r.right = r.left + nw;
    }

    return r;
}

void DisplayBuffer::draw(HWND hwnd, HDC hdc) {
    EnterCriticalSection(&cs_);

    if (!memDC_ || !memBitmap_ || width_ == 0 || height_ == 0) {
        LeaveCriticalSection(&cs_);
        return;
    }

    RECT cr;
    GetClientRect(hwnd, &cr);
    int ww = cr.right, wh = cr.bottom;

    HDC bdc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, ww, wh);
    HBITMAP ob = (HBITMAP)SelectObject(bdc, bb);

    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(bdc, &cr, bg);
    DeleteObject(bg);

    RECT dr = calculateDisplayRect(ww, wh);
    SetStretchBltMode(bdc, HALFTONE);
    StretchBlt(bdc, dr.left, dr.top, dr.right - dr.left, dr.bottom - dr.top,
               memDC_, 0, 0, width_, height_, SRCCOPY);

    BitBlt(hdc, 0, 0, ww, wh, bdc, 0, 0, SRCCOPY);

    SelectObject(bdc, ob);
    DeleteObject(bb);
    DeleteDC(bdc);

    LeaveCriticalSection(&cs_);
}
