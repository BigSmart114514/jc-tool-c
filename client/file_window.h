#ifndef FILE_WINDOW_H
#define FILE_WINDOW_H

#include "../common/protocol.h"
#include "../common/transport.h"
#include <commctrl.h>
#include <vector>
#include <mutex>
#include <functional>

#pragma comment(lib, "comctl32.lib")

class FileWindow {
public:
    FileWindow();
    ~FileWindow();

    // 只存储传输指针，不设置回调
    void init(ITransport* transport);
    
    bool create(HINSTANCE hInstance);
    void destroy();
    void show();

    // 由 ControlPanel 从传输回调中调用
    void handleMessage(const BinaryData& data);

    HWND getHwnd() const { return hwnd_; }
    void setOnClosed(std::function<void()> callback) { onClosed_ = callback; }

    void navigateTo(const std::wstring& path);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void createControls();
    void goUp();
    void refresh();
    void populateListView();
    void onListViewDblClick();
    void onListViewRightClick();
    void downloadFile(int index);

    HWND hwnd_ = nullptr;
    HWND hListView_ = nullptr;
    HWND hStatusBar_ = nullptr;
    HWND hAddressBar_ = nullptr;
    HIMAGELIST hImageList_ = nullptr;

    ITransport* transport_ = nullptr;

    std::wstring currentPath_;
    std::vector<FileManager::FileEntry> files_;
    std::mutex filesMtx_;

    std::wstring downloadPath_;
    HANDLE downloadFile_ = INVALID_HANDLE_VALUE;
    uint64_t downloadTotal_ = 0;
    uint64_t downloadReceived_ = 0;
    bool downloading_ = false;

    std::function<void()> onClosed_;
};

#endif