#ifndef DESKTOP_WINDOW_H
#define DESKTOP_WINDOW_H

#include "../common/protocol.h"
#include "../common/transport.h"
#include "hevc_decoder.h"
#include "display_buffer.h"
#include <atomic>
#include <functional>

class DesktopWindow {
public:
    DesktopWindow();
    ~DesktopWindow();

    // 只存储传输指针（用于发送），不设置回调
    void init(ITransport* transport);
    
    bool create(HINSTANCE hInstance, const char* title);
    void destroy();

    // 由 ControlPanel 从传输回调中调用
    void handleMessage(const BinaryData& data);

    HWND getHwnd() const { return hwnd_; }

    void setInputToggles(std::atomic<bool>* mouseMove,
                         std::atomic<bool>* mouseClick,
                         std::atomic<bool>* keyboard) {
        pEnableMouseMove_ = mouseMove;
        pEnableMouseClick_ = mouseClick;
        pEnableKeyboard_ = keyboard;
    }

    void setOnClosed(std::function<void()> callback) { onClosed_ = callback; }
    void setOnOpenFileManager(std::function<void()> callback) { onOpenFileManager_ = callback; }

    // 请求服务端开始推流
    void requestStream();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void handleVideoFrame(const uint8_t* data, size_t size, bool isKeyframe);
    void handleScreenInfo(const BinaryData& data);
    void sendInput(const Desktop::InputEvent& ev);
    bool convertToImageCoords(int cx, int cy, int& ix, int& iy);

    HWND hwnd_ = nullptr;
    ITransport* transport_ = nullptr;
    HEVCDecoder decoder_;
    DisplayBuffer display_;

    std::atomic<int> screenWidth_{0};
    std::atomic<int> screenHeight_{0};
    DWORD lastFrameTime_ = 0;
    bool decoderReady_ = false;

    std::atomic<bool>* pEnableMouseMove_ = nullptr;
    std::atomic<bool>* pEnableMouseClick_ = nullptr;
    std::atomic<bool>* pEnableKeyboard_ = nullptr;

    std::function<void()> onClosed_;
    std::function<void()> onOpenFileManager_;
};

#endif