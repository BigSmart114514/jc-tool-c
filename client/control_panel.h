#ifndef CONTROL_PANEL_H
#define CONTROL_PANEL_H

#include "../common/protocol.h"
#include "../common/transport.h"
#include "desktop_window.h"
#include "file_window.h"
#include <atomic>
#include <mutex>
#include <string>

#define IDC_STATIC_MODE         4001
#define IDC_STATIC_STATUS       4002
#define IDC_BTN_DESKTOP         4010
#define IDC_BTN_FILEMANAGER     4011
#define IDC_BTN_DISCONNECT      4012
#define IDC_CHECK_MOUSE_MOVE    4020
#define IDC_CHECK_MOUSE_CLICK   4021
#define IDC_CHECK_KEYBOARD      4022

struct ControlPanelConfig {
    ITransport* desktopTransport = nullptr;
    ITransport* fileTransport = nullptr;
    std::string modeText;
    std::string connectInfo;
};

class ControlPanel {
public:
    ControlPanel();
    ~ControlPanel();

    void setConfig(const ControlPanelConfig& config);
    bool create(HINSTANCE hInstance);
    void destroy();

    HWND getHwnd() const { return hwnd_; }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void createControls();
    void setupTransportCallbacks();
    void updateStatus(const wchar_t* text);
    void toggleDesktop();
    void toggleFileManager();
    void onDisconnect();
    void onDesktopWindowClosed();
    void onFileWindowClosed();
    
    // 检查连接状态，必要时尝试重连
    bool ensureDesktopConnected();
    bool ensureFileConnected();

    HWND hwnd_ = nullptr;
    HWND hBtnDesktop_ = nullptr;
    HWND hBtnFileManager_ = nullptr;
    HWND hBtnDisconnect_ = nullptr;
    HWND hStaticStatus_ = nullptr;

    ControlPanelConfig config_;

    DesktopWindow* desktopWindow_ = nullptr;
    FileWindow* fileWindow_ = nullptr;
    std::mutex windowMtx_;

    bool desktopOpen_ = false;
    bool fileManagerOpen_ = false;

    std::atomic<bool> enableMouseMove_{true};
    std::atomic<bool> enableMouseClick_{true};
    std::atomic<bool> enableKeyboard_{true};

    HINSTANCE hInstance_ = nullptr;
    HFONT hFont_ = nullptr;
    HFONT hFontBold_ = nullptr;
};

#endif