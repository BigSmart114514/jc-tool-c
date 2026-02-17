#include "desktop_window.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <iostream>

#pragma comment(lib, "dwmapi.lib")

#define WM_UPDATE_DISPLAY  (WM_USER + 100)
#define WM_CONNECTION_LOST (WM_USER + 101)

static DesktopWindow* g_desktopWindow = nullptr;

DesktopWindow::DesktopWindow() {}

DesktopWindow::~DesktopWindow() {
    if (g_desktopWindow == this) g_desktopWindow = nullptr;
}

void DesktopWindow::init(ITransport* transport) {
    transport_ = transport;
    // 不设置传输层回调！回调由 ControlPanel 管理
}

void DesktopWindow::requestStream() {
    // 发送 ClientReady，告诉服务端"我准备好了，给我推流"
    if (transport_ && transport_->isConnected()) {
        std::cout << "[Desktop] Requesting stream..." << std::endl;
        auto ready = MessageBuilder::ClientReady();
        transport_->send(ready);
    }
}

void DesktopWindow::handleMessage(const BinaryData& data) {
    if (data.empty()) return;

    auto type = static_cast<Desktop::MsgType>(data[0]);

    switch (type) {
        case Desktop::MsgType::ScreenInfo:
            handleScreenInfo(data);
            break;

        case Desktop::MsgType::VideoFrame:
            if (data.size() > 2 && decoderReady_) {
                bool isKeyframe = data[1] != 0;
                handleVideoFrame(data.data() + 2, data.size() - 2, isKeyframe);
            }
            break;

        default:
            break;
    }
}

void DesktopWindow::handleScreenInfo(const BinaryData& data) {
    if (data.size() < 1 + sizeof(Desktop::ScreenInfo)) return;

    auto* info = reinterpret_cast<const Desktop::ScreenInfo*>(data.data() + 1);
    
    std::cout << "[Desktop] Screen: " << info->width << "x" << info->height << std::endl;

    screenWidth_ = info->width;
    screenHeight_ = info->height;

    // 初始化或重新初始化解码器
    decoder_.cleanup();
    if (decoder_.init(info->width, info->height)) {
        decoderReady_ = true;
        std::cout << "[Desktop] Decoder initialized" << std::endl;
    } else {
        std::cerr << "[Desktop] Decoder init failed" << std::endl;
        decoderReady_ = false;
    }
}

void DesktopWindow::handleVideoFrame(const uint8_t* data, size_t size, bool isKeyframe) {
    std::vector<uint8_t> rgbData;
    if (decoder_.decode(data, static_cast<int>(size), rgbData)) {
        int w = decoder_.getWidth();
        int h = decoder_.getHeight();
        screenWidth_ = w;
        screenHeight_ = h;
        display_.updateFrame(rgbData.data(), w, h);

        DWORD now = GetTickCount();
        if (now - lastFrameTime_ >= 16) {
            if (hwnd_) PostMessage(hwnd_, WM_UPDATE_DISPLAY, 0, 0);
            lastFrameTime_ = now;
        }
    }
}

void DesktopWindow::sendInput(const Desktop::InputEvent& ev) {
    if (transport_ && transport_->isConnected()) {
        auto msg = MessageBuilder::InputEvent(ev);
        transport_->send(msg);
    }
}

bool DesktopWindow::convertToImageCoords(int cx, int cy, int& ix, int& iy) {
    int ow = screenWidth_, oh = screenHeight_;
    if (ow == 0 || oh == 0 || !hwnd_) return false;

    RECT cr;
    GetClientRect(hwnd_, &cr);
    RECT dr = display_.calculateDisplayRect(cr.right, cr.bottom);

    if (cx < dr.left || cx >= dr.right || cy < dr.top || cy >= dr.bottom) return false;

    float sx = (float)ow / (dr.right - dr.left);
    float sy = (float)oh / (dr.bottom - dr.top);
    ix = std::clamp((int)((cx - dr.left) * sx), 0, ow - 1);
    iy = std::clamp((int)((cy - dr.top) * sy), 0, oh - 1);
    return true;
}

bool DesktopWindow::create(HINSTANCE hInstance, const char* title) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "RemoteDesktopWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;

    static bool registered = false;
    if (!registered) {
        RegisterClassA(&wc);
        registered = true;
    }

    g_desktopWindow = this;

    int ww = GetSystemMetrics(SM_CXSCREEN) * 3 / 4;
    int wh = GetSystemMetrics(SM_CYSCREEN) * 3 / 4;

    hwnd_ = CreateWindowExA(WS_EX_COMPOSITED, "RemoteDesktopWindow", title,
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
                            nullptr, nullptr, hInstance, nullptr);

    if (!hwnd_) return false;

    BOOL dt = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &dt, sizeof(dt));

    return true;
}

void DesktopWindow::destroy() {
    decoderReady_ = false;
    
    // 先清回调，防止 WM_DESTROY 触发
    auto savedOnClosed = onClosed_;
    onClosed_ = nullptr;
    onOpenFileManager_ = nullptr;

    if (hwnd_) {
        g_desktopWindow = nullptr; // 先清指针，防止 WndProc 访问
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    decoder_.cleanup();
    
    // 恢复（供外部使用）
    onClosed_ = savedOnClosed;
    
    if (g_desktopWindow == this) g_desktopWindow = nullptr;
}

LRESULT CALLBACK DesktopWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = g_desktopWindow;
    if (!self || self->hwnd_ != hwnd) return DefWindowProcA(hwnd, msg, wParam, lParam);

    bool mouseMove = self->pEnableMouseMove_ ? self->pEnableMouseMove_->load() : true;
    bool mouseClick = self->pEnableMouseClick_ ? self->pEnableMouseClick_->load() : true;
    bool keyboard = self->pEnableKeyboard_ ? self->pEnableKeyboard_->load() : true;

    switch (msg) {
        case WM_CLOSE:
            // 不销毁窗口，通知控制面板处理
            if (self->onClosed_) {
                self->onClosed_();
            } else {
                DestroyWindow(hwnd);
            }
            return 0;
            
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            self->display_.draw(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_UPDATE_DISPLAY:
            if (self->hwnd_) {
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_F12 && self->onOpenFileManager_) {
                self->onOpenFileManager_();
                return 0;
            }
            if (keyboard) self->sendInput({1, 0, 0, static_cast<int>(wParam), 0});
            break;

        case WM_KEYUP:
            if (keyboard) self->sendInput({1, 0, 0, static_cast<int>(wParam), 1});
            break;

        case WM_LBUTTONDOWN:
            if (mouseClick) {
                SetCapture(hwnd);
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 1, 0});
            }
            break;

        case WM_LBUTTONUP:
            if (mouseClick) {
                ReleaseCapture();
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 2, 0});
            }
            break;

        case WM_RBUTTONDOWN:
            if (mouseClick) {
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 3, 0});
            }
            break;

        case WM_RBUTTONUP:
            if (mouseClick) {
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 4, 0});
            }
            break;

        case WM_MBUTTONDOWN:
            if (mouseClick) {
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 5, 0});
            }
            break;

        case WM_MBUTTONUP:
            if (mouseClick) {
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 6, 0});
            }
            break;

        case WM_MOUSEWHEEL:
            if (mouseClick) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                int x, y;
                if (self->convertToImageCoords(pt.x, pt.y, x, y))
                    self->sendInput({0, x, y, 7, delta});
            }
            break;

        case WM_MOUSEMOVE:
            if (mouseMove) {
                int x, y;
                if (self->convertToImageCoords(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    self->sendInput({0, x, y, 0, 0});
            }
            break;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}