#define UNICODE
#define _UNICODE

#include "control_panel.h"
#include <iostream>

#define WM_DESKTOP_CLOSED  (WM_USER + 300)
#define WM_FILE_CLOSED     (WM_USER + 301)

static ControlPanel* g_controlPanel = nullptr;

ControlPanel::ControlPanel() {
    g_controlPanel = this;
}

ControlPanel::~ControlPanel() {
    destroy();
    g_controlPanel = nullptr;
}

void ControlPanel::setConfig(const ControlPanelConfig& config) {
    config_ = config;
}

void ControlPanel::setupTransportCallbacks() {
    // 桌面传输回调 —— 转发给 DesktopWindow（如果存在）
    if (config_.desktopTransport) {
        TransportCallbacks cb;
        cb.onConnected = []() {
            std::cout << "[Desktop Transport] Connected" << std::endl;
        };
        cb.onDisconnected = []() {
            std::cout << "[Desktop Transport] Disconnected" << std::endl;
        };
        cb.onMessage = [this](const BinaryData& data) {
            std::lock_guard<std::mutex> lock(windowMtx_);
            if (desktopWindow_) {
                desktopWindow_->handleMessage(data);
            }
        };
        config_.desktopTransport->setCallbacks(cb);
    }

    // 文件传输回调 —— 转发给 FileWindow（如果存在）
    if (config_.fileTransport) {
        TransportCallbacks cb;
        cb.onConnected = []() {
            std::cout << "[File Transport] Connected" << std::endl;
        };
        cb.onDisconnected = []() {
            std::cout << "[File Transport] Disconnected" << std::endl;
        };
        cb.onMessage = [this](const BinaryData& data) {
            std::lock_guard<std::mutex> lock(windowMtx_);
            if (fileWindow_) {
                fileWindow_->handleMessage(data);
            }
        };
        config_.fileTransport->setCallbacks(cb);
    }
}

bool ControlPanel::create(HINSTANCE hInstance) {
    hInstance_ = hInstance;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RemoteControlPanel";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(0, L"RemoteControlPanel", L"Remote Control",
                            WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                            CW_USEDEFAULT, CW_USEDEFAULT, 320, 420,
                            nullptr, nullptr, hInstance, nullptr);

    if (!hwnd_) return false;

    hFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    hFontBold_ = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    createControls();
    
    // 设置传输回调（只做一次，永不更换）
    setupTransportCallbacks();

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    return true;
}

void ControlPanel::createControls() {
    int x = 15, y = 10, w = 270, h = 22;
    int btnH = 32;

    auto hLabel = CreateWindowW(L"STATIC", L"Connection",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 x, y, w, h, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)hFontBold_, TRUE);
    y += 24;

    std::wstring modeW(config_.modeText.begin(), config_.modeText.end());
    std::wstring modeLabel = L"Mode: " + modeW;
    auto hMode = CreateWindowW(L"STATIC", modeLabel.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                x, y, w, h, hwnd_, (HMENU)IDC_STATIC_MODE, hInstance_, nullptr);
    SendMessage(hMode, WM_SETFONT, (WPARAM)hFont_, TRUE);
    y += 20;

    std::wstring infoW(config_.connectInfo.begin(), config_.connectInfo.end());
    auto hInfo = CreateWindowW(L"STATIC", infoW.c_str(),
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                x, y, w, h * 2, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(hInfo, WM_SETFONT, (WPARAM)hFont_, TRUE);
    y += 40;

    hStaticStatus_ = CreateWindowW(L"STATIC", L"Status: Ready",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    x, y, w, h, hwnd_, (HMENU)IDC_STATIC_STATUS, hInstance_, nullptr);
    SendMessage(hStaticStatus_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    y += 30;

    CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                  x, y, w, 2, hwnd_, nullptr, hInstance_, nullptr);
    y += 10;

    hLabel = CreateWindowW(L"STATIC", L"Windows",
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            x, y, w, h, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)hFontBold_, TRUE);
    y += 26;

    hBtnDesktop_ = CreateWindowW(L"BUTTON", L"Open Desktop",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  x, y, w, btnH, hwnd_, (HMENU)IDC_BTN_DESKTOP, hInstance_, nullptr);
    SendMessage(hBtnDesktop_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    y += btnH + 6;

    hBtnFileManager_ = CreateWindowW(L"BUTTON", L"Open File Manager",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      x, y, w, btnH, hwnd_, (HMENU)IDC_BTN_FILEMANAGER, hInstance_, nullptr);
    SendMessage(hBtnFileManager_, WM_SETFONT, (WPARAM)hFont_, TRUE);
    y += btnH + 14;

    CreateWindowW(L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                  x, y, w, 2, hwnd_, nullptr, hInstance_, nullptr);
    y += 10;

    hLabel = CreateWindowW(L"STATIC", L"Input Control",
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            x, y, w, h, hwnd_, nullptr, hInstance_, nullptr);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)hFontBold_, TRUE);
    y += 26;

    auto hChk = CreateWindowW(L"BUTTON", L"Mouse Move",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               x, y, w, h, hwnd_, (HMENU)IDC_CHECK_MOUSE_MOVE, hInstance_, nullptr);
    SendMessage(hChk, WM_SETFONT, (WPARAM)hFont_, TRUE);
    CheckDlgButton(hwnd_, IDC_CHECK_MOUSE_MOVE, BST_CHECKED);
    y += 24;

    hChk = CreateWindowW(L"BUTTON", L"Mouse Click",
                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                          x, y, w, h, hwnd_, (HMENU)IDC_CHECK_MOUSE_CLICK, hInstance_, nullptr);
    SendMessage(hChk, WM_SETFONT, (WPARAM)hFont_, TRUE);
    CheckDlgButton(hwnd_, IDC_CHECK_MOUSE_CLICK, BST_CHECKED);
    y += 24;

    hChk = CreateWindowW(L"BUTTON", L"Keyboard",
                          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                          x, y, w, h, hwnd_, (HMENU)IDC_CHECK_KEYBOARD, hInstance_, nullptr);
    SendMessage(hChk, WM_SETFONT, (WPARAM)hFont_, TRUE);
    CheckDlgButton(hwnd_, IDC_CHECK_KEYBOARD, BST_CHECKED);
    y += 30;

    hBtnDisconnect_ = CreateWindowW(L"BUTTON", L"Disconnect && Exit",
                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     x, y, w, btnH, hwnd_, (HMENU)IDC_BTN_DISCONNECT, hInstance_, nullptr);
    SendMessage(hBtnDisconnect_, WM_SETFONT, (WPARAM)hFont_, TRUE);
}

void ControlPanel::updateStatus(const wchar_t* text) {
    if (hStaticStatus_) {
        std::wstring s = L"Status: ";
        s += text;
        SetWindowTextW(hStaticStatus_, s.c_str());
    }
}

void ControlPanel::toggleDesktop() {
    if (desktopOpen_) {
        // 关闭桌面 —— 先从锁里移除指针
        DesktopWindow* toDelete = nullptr;
        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            toDelete = desktopWindow_;
            desktopWindow_ = nullptr;
        }
        if (toDelete) {
            toDelete->destroy();
            delete toDelete;
        }
        desktopOpen_ = false;
        SetWindowTextW(hBtnDesktop_, L"Open Desktop");
        updateStatus(L"Desktop closed");
    } else {
        // 打开桌面
        auto* dw = new DesktopWindow();
        dw->init(config_.desktopTransport);
        dw->setInputToggles(&enableMouseMove_, &enableMouseClick_, &enableKeyboard_);

        dw->setOnClosed([this]() {
            // 从桌面窗口的 X 按钮关闭 → 通知控制面板
            PostMessage(hwnd_, WM_DESKTOP_CLOSED, 0, 0);
        });

        dw->setOnOpenFileManager([this]() {
            // F12
            if (!fileManagerOpen_) {
                PostMessage(hwnd_, WM_COMMAND, MAKEWPARAM(IDC_BTN_FILEMANAGER, 0), 0);
            }
        });

        std::string title = "Remote Desktop [" + config_.modeText + "]";
        if (!dw->create(hInstance_, title.c_str())) {
            delete dw;
            MessageBoxW(hwnd_, L"Failed to create desktop window", L"Error", MB_ICONERROR);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            desktopWindow_ = dw;
        }

        desktopOpen_ = true;
        SetWindowTextW(hBtnDesktop_, L"Close Desktop");
        updateStatus(L"Desktop opened");

        // 请求服务端推流
        dw->requestStream();
    }
}

void ControlPanel::toggleFileManager() {
    if (fileManagerOpen_) {
        FileWindow* toDelete = nullptr;
        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            toDelete = fileWindow_;
            fileWindow_ = nullptr;
        }
        if (toDelete) {
            toDelete->destroy();
            delete toDelete;
        }
        fileManagerOpen_ = false;
        SetWindowTextW(hBtnFileManager_, L"Open File Manager");
        updateStatus(L"File Manager closed");
    } else {
        auto* fw = new FileWindow();
        fw->init(config_.fileTransport);

        fw->setOnClosed([this]() {
            PostMessage(hwnd_, WM_FILE_CLOSED, 0, 0);
        });

        if (!fw->create(hInstance_)) {
            delete fw;
            MessageBoxW(hwnd_, L"Failed to create file manager", L"Error", MB_ICONERROR);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            fileWindow_ = fw;
        }

        fw->show();
        fw->navigateTo(L"");

        fileManagerOpen_ = true;
        SetWindowTextW(hBtnFileManager_, L"Close File Manager");
        updateStatus(L"File Manager opened");
    }
}

void ControlPanel::onDesktopWindowClosed() {
    DesktopWindow* toDelete = nullptr;
    {
        std::lock_guard<std::mutex> lock(windowMtx_);
        toDelete = desktopWindow_;
        desktopWindow_ = nullptr;
    }
    if (toDelete) {
        toDelete->destroy();
        delete toDelete;
    }
    desktopOpen_ = false;
    SetWindowTextW(hBtnDesktop_, L"Open Desktop");
    updateStatus(L"Desktop closed");
}

void ControlPanel::onFileWindowClosed() {
    FileWindow* toDelete = nullptr;
    {
        std::lock_guard<std::mutex> lock(windowMtx_);
        toDelete = fileWindow_;
        fileWindow_ = nullptr;
    }
    if (toDelete) {
        toDelete->destroy();
        delete toDelete;
    }
    fileManagerOpen_ = false;
    SetWindowTextW(hBtnFileManager_, L"Open File Manager");
    updateStatus(L"File Manager closed");
}

void ControlPanel::onDisconnect() {
    // 关闭所有窗口
    {
        std::lock_guard<std::mutex> lock(windowMtx_);
        if (desktopWindow_) {
            desktopWindow_->destroy();
            delete desktopWindow_;
            desktopWindow_ = nullptr;
        }
        if (fileWindow_) {
            fileWindow_->destroy();
            delete fileWindow_;
            fileWindow_ = nullptr;
        }
    }

    if (config_.desktopTransport) config_.desktopTransport->disconnect();
    if (config_.fileTransport) config_.fileTransport->disconnect();

    DestroyWindow(hwnd_);
}

void ControlPanel::destroy() {
    {
        std::lock_guard<std::mutex> lock(windowMtx_);
        if (desktopWindow_) {
            desktopWindow_->destroy();
            delete desktopWindow_;
            desktopWindow_ = nullptr;
        }
        if (fileWindow_) {
            fileWindow_->destroy();
            delete fileWindow_;
            fileWindow_ = nullptr;
        }
    }
    if (hFont_) { DeleteObject(hFont_); hFont_ = nullptr; }
    if (hFontBold_) { DeleteObject(hFontBold_); hFontBold_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

LRESULT CALLBACK ControlPanel::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_controlPanel) return DefWindowProcW(hwnd, msg, wParam, lParam);
    auto* self = g_controlPanel;

    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            switch (id) {
                case IDC_BTN_DESKTOP:     self->toggleDesktop(); break;
                case IDC_BTN_FILEMANAGER: self->toggleFileManager(); break;
                case IDC_BTN_DISCONNECT:  self->onDisconnect(); break;
                case IDC_CHECK_MOUSE_MOVE:
                    if (code == BN_CLICKED)
                        self->enableMouseMove_ = (IsDlgButtonChecked(hwnd, IDC_CHECK_MOUSE_MOVE) == BST_CHECKED);
                    break;
                case IDC_CHECK_MOUSE_CLICK:
                    if (code == BN_CLICKED)
                        self->enableMouseClick_ = (IsDlgButtonChecked(hwnd, IDC_CHECK_MOUSE_CLICK) == BST_CHECKED);
                    break;
                case IDC_CHECK_KEYBOARD:
                    if (code == BN_CLICKED)
                        self->enableKeyboard_ = (IsDlgButtonChecked(hwnd, IDC_CHECK_KEYBOARD) == BST_CHECKED);
                    break;
            }
            break;
        }

        case WM_DESKTOP_CLOSED:
            self->onDesktopWindowClosed();
            break;

        case WM_FILE_CLOSED:
            self->onFileWindowClosed();
            break;

        case WM_CLOSE:
            if (MessageBoxW(hwnd, L"Disconnect and exit?", L"Confirm",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                self->onDisconnect();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}