
#ifndef DISPLAY_BUFFER_H
#define DISPLAY_BUFFER_H

#include <windows.h>
#include <cstdint>

class DisplayBuffer {
public:
    DisplayBuffer();
    ~DisplayBuffer();

    bool updateFrame(const uint8_t* data, int width, int height);
    void draw(HWND hwnd, HDC hdc);
    RECT calculateDisplayRect(int windowWidth, int windowHeight);
    
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    void cleanup();

    CRITICAL_SECTION cs_;
    HDC memDC_ = nullptr;
    HBITMAP memBitmap_ = nullptr;
    HBITMAP oldBitmap_ = nullptr;
    void* bitmapBits_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

#endif // DISPLAY_BUFFER_H
