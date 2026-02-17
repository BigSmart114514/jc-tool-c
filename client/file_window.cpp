#define UNICODE
#define _UNICODE

#include "file_window.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <iostream>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

#pragma comment(linker,"\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_FILE_LIST_READY   (WM_USER + 200)
#define WM_DOWNLOAD_PROGRESS (WM_USER + 201)
#define WM_DOWNLOAD_COMPLETE (WM_USER + 202)

#define IDC_LISTVIEW    2001
#define IDC_STATUSBAR   2002
#define IDC_ADDRESSBAR  2003
#define IDC_GO_BUTTON   2004
#define IDC_UP_BUTTON   2005
#define IDC_REFRESH_BTN 2006

#define IDM_DOWNLOAD   3001
#define IDM_REFRESH    3002
#define IDM_OPEN       3003
#define IDM_PROPERTIES 3004

// Unicode 版本的 ListView 辅助函数
static inline int LV_InsertItemW(HWND hwnd, const LVITEMW* item) {
    return (int)SendMessageW(hwnd, LVM_INSERTITEMW, 0, (LPARAM)item);
}

static inline void LV_SetItemTextW(HWND hwnd, int iItem, int iSubItem, LPWSTR pszText) {
    LVITEMW lvi = {};
    lvi.iSubItem = iSubItem;
    lvi.pszText = pszText;
    SendMessageW(hwnd, LVM_SETITEMTEXTW, (WPARAM)iItem, (LPARAM)&lvi);
}

static inline int LV_InsertColumnW(HWND hwnd, int iCol, const LVCOLUMNW* col) {
    return (int)SendMessageW(hwnd, LVM_INSERTCOLUMNW, (WPARAM)iCol, (LPARAM)col);
}

static FileWindow* g_fileWindow = nullptr;

FileWindow::FileWindow() {
    g_fileWindow = this;
}

FileWindow::~FileWindow() {
    destroy();
    g_fileWindow = nullptr;
}

void FileWindow::init(ITransport* transport) {
    transport_ = transport;
    // 不设置传输层回调！回调由 ControlPanel 管理
}

void FileWindow::handleMessage(const BinaryData& data) {
    if (data.empty()) return;

    auto type = static_cast<FileManager::MsgType>(data[0]);

    switch (type) {
        case FileManager::MsgType::Response: {
            if (data.size() < 1 + sizeof(FileManager::ListHeader)) break;

            auto* header = reinterpret_cast<const FileManager::ListHeader*>(data.data() + 1);

            std::lock_guard<std::mutex> lock(filesMtx_);
            files_.clear();

            if (header->status == static_cast<uint8_t>(FileManager::Status::OK) && header->count > 0) {
                auto* entries = reinterpret_cast<const FileManager::FileEntry*>(
                    data.data() + 1 + sizeof(FileManager::ListHeader));

                size_t expectedSize = 1 + sizeof(FileManager::ListHeader) + header->count * sizeof(FileManager::FileEntry);
                if (data.size() >= expectedSize) {
                    for (uint32_t i = 0; i < header->count; i++) {
                        files_.push_back(entries[i]);
                    }
                }
            }

            if (hwnd_) PostMessageW(hwnd_, WM_FILE_LIST_READY, 0, 0);
            break;
        }

        case FileManager::MsgType::DownloadData: {
            if (data.size() < 1 + sizeof(FileManager::TransferHeader)) break;

            auto* header = reinterpret_cast<const FileManager::TransferHeader*>(data.data() + 1);

            if (header->status == static_cast<uint8_t>(FileManager::Status::NotFound) ||
                header->status == static_cast<uint8_t>(FileManager::Status::Error)) {
                if (downloadFile_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(downloadFile_);
                    downloadFile_ = INVALID_HANDLE_VALUE;
                    DeleteFileW(downloadPath_.c_str());
                }
                downloading_ = false;
                if (hwnd_) PostMessageW(hwnd_, WM_DOWNLOAD_COMPLETE, 0, 1);
                break;
            }

            if (!downloading_) break;

            if (header->chunkSize > 0 && downloadFile_ != INVALID_HANDLE_VALUE) {
                const uint8_t* chunkData = data.data() + 1 + sizeof(FileManager::TransferHeader);
                DWORD written;
                WriteFile(downloadFile_, chunkData, header->chunkSize, &written, nullptr);
                downloadReceived_ += written;
            }

            if (hwnd_ && header->totalSize > 0) {
                int progress = static_cast<int>((downloadReceived_ * 100) / header->totalSize);
                PostMessageW(hwnd_, WM_DOWNLOAD_PROGRESS, progress, 0);
            }

            if (header->status == static_cast<uint8_t>(FileManager::Status::Complete)) {
                if (downloadFile_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(downloadFile_);
                    downloadFile_ = INVALID_HANDLE_VALUE;
                }
                downloading_ = false;
                if (hwnd_) PostMessageW(hwnd_, WM_DOWNLOAD_COMPLETE, 0, 0);
            }
            break;
        }

        default:
            break;
    }
}

void FileWindow::navigateTo(const std::wstring& path) {
    if (!transport_ || !transport_->isConnected()) return;
    
    currentPath_ = path;
    SetWindowTextW(hAddressBar_, path.c_str());
    SendMessageW(hStatusBar_, SB_SETTEXTW, 0, (LPARAM)L"Loading...");
    
    auto request = MessageBuilder::FileListRequest(path);
    transport_->send(request);
}

void FileWindow::goUp() {
    if (currentPath_.empty()) return;
    
    size_t pos = currentPath_.rfind(L'\\');
    if (pos == std::wstring::npos) {
        navigateTo(L"");
    } else if (pos == 2 && currentPath_.length() == 3) {
        navigateTo(L"");
    } else {
        navigateTo(currentPath_.substr(0, pos));
    }
}

void FileWindow::refresh() {
    navigateTo(currentPath_);
}

void FileWindow::populateListView() {
    SendMessageW(hListView_, LVM_DELETEALLITEMS, 0, 0);
    
    std::lock_guard<std::mutex> lock(filesMtx_);
    
    for (size_t i = 0; i < files_.size(); i++) {
        const auto& entry = files_[i];
        
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(entry.name);
        item.lParam = static_cast<LPARAM>(i);
        
        switch (static_cast<FileManager::FileType>(entry.type)) {
            case FileManager::FileType::Drive:     item.iImage = 2; break;
            case FileManager::FileType::Directory: item.iImage = 0; break;
            default:                               item.iImage = 1; break;
        }
        
        int idx = LV_InsertItemW(hListView_, &item);
        
        // 大小
        if (static_cast<FileManager::FileType>(entry.type) == FileManager::FileType::File) {
            wchar_t sizeStr[64];
            if (entry.size < 1024) {
                swprintf(sizeStr, 64, L"%llu B", entry.size);
            } else if (entry.size < 1024 * 1024) {
                swprintf(sizeStr, 64, L"%.2f KB", entry.size / 1024.0);
            } else if (entry.size < 1024ULL * 1024 * 1024) {
                swprintf(sizeStr, 64, L"%.2f MB", entry.size / (1024.0 * 1024.0));
            } else {
                swprintf(sizeStr, 64, L"%.2f GB", entry.size / (1024.0 * 1024.0 * 1024.0));
            }
            LV_SetItemTextW(hListView_, idx, 1, sizeStr);
        }
        
        // 类型
        wchar_t typeStr[32] = L"";
        switch (static_cast<FileManager::FileType>(entry.type)) {
            case FileManager::FileType::Drive:     wcscpy(typeStr, L"Drive"); break;
            case FileManager::FileType::Directory: wcscpy(typeStr, L"Folder"); break;
            default:                               wcscpy(typeStr, L"File"); break;
        }
        LV_SetItemTextW(hListView_, idx, 2, typeStr);
        
        // 时间
        if (entry.modifyTime != 0) {
            FILETIME ft;
            ft.dwLowDateTime = static_cast<DWORD>(entry.modifyTime & 0xFFFFFFFF);
            ft.dwHighDateTime = static_cast<DWORD>(entry.modifyTime >> 32);
            
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&ft, &ft);
            FileTimeToSystemTime(&ft, &st);
            
            wchar_t timeStr[64];
            swprintf(timeStr, 64, L"%04d-%02d-%02d %02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            LV_SetItemTextW(hListView_, idx, 3, timeStr);
        }
    }
    
    wchar_t status[256];
    swprintf(status, 256, L"%zu items", files_.size());
    SendMessageW(hStatusBar_, SB_SETTEXTW, 0, (LPARAM)status);
}

void FileWindow::onListViewDblClick() {
    int sel = ListView_GetNextItem(hListView_, -1, LVNI_SELECTED);
    if (sel < 0) return;
    
    FileManager::FileEntry entry;
    {
        std::lock_guard<std::mutex> lock(filesMtx_);
        if (sel >= static_cast<int>(files_.size())) return;
        entry = files_[sel];
    }
    
    auto type = static_cast<FileManager::FileType>(entry.type);
    
    if (type == FileManager::FileType::Drive) {
        std::wstring newPath = entry.name;
        newPath += L"\\";
        navigateTo(newPath);
    } else if (type == FileManager::FileType::Directory) {
        if (wcscmp(entry.name, L"..") == 0) {
            goUp();
        } else {
            std::wstring newPath = currentPath_;
            if (!newPath.empty() && newPath.back() != L'\\') {
                newPath += L"\\";
            }
            newPath += entry.name;
            navigateTo(newPath);
        }
    }
}

void FileWindow::onListViewRightClick() {
    POINT pt;
    GetCursorPos(&pt);
    
    int sel = ListView_GetNextItem(hListView_, -1, LVNI_SELECTED);
    if (sel < 0) return;
    
    FileManager::FileEntry entry;
    {
        std::lock_guard<std::mutex> lock(filesMtx_);
        if (sel >= static_cast<int>(files_.size())) return;
        entry = files_[sel];
    }
    
    HMENU hMenu = CreatePopupMenu();
    auto type = static_cast<FileManager::FileType>(entry.type);
    
    if (type == FileManager::FileType::File) {
        AppendMenuW(hMenu, MF_STRING, IDM_DOWNLOAD, L"Download");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    } else {
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN, L"Open");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    
    AppendMenuW(hMenu, MF_STRING, IDM_REFRESH, L"Refresh");
    AppendMenuW(hMenu, MF_STRING, IDM_PROPERTIES, L"Properties");
    
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);
    
    switch (cmd) {
        case IDM_DOWNLOAD:
            downloadFile(sel);
            break;
        case IDM_OPEN:
            onListViewDblClick();
            break;
        case IDM_REFRESH:
            refresh();
            break;
        case IDM_PROPERTIES: {
            std::wstring info = L"Name: ";
            info += entry.name;
            info += L"\nType: ";
            switch (type) {
                case FileManager::FileType::File: info += L"File"; break;
                case FileManager::FileType::Directory: info += L"Folder"; break;
                case FileManager::FileType::Drive: info += L"Drive"; break;
            }
            if (type == FileManager::FileType::File) {
                wchar_t sizeStr[64];
                swprintf(sizeStr, 64, L"\nSize: %llu bytes", entry.size);
                info += sizeStr;
            }
            MessageBoxW(hwnd_, info.c_str(), L"Properties", MB_OK | MB_ICONINFORMATION);
            break;
        }
    }
}

void FileWindow::downloadFile(int index) {
    FileManager::FileEntry entry;
    {
        std::lock_guard<std::mutex> lock(filesMtx_);
        if (index < 0 || index >= static_cast<int>(files_.size())) return;
        entry = files_[index];
    }
    
    if (static_cast<FileManager::FileType>(entry.type) != FileManager::FileType::File) return;
    if (downloading_) {
        MessageBoxW(hwnd_, L"Download in progress", L"Info", MB_OK);
        return;
    }
    
    wchar_t savePath[MAX_PATH] = {};
    wcscpy(savePath, entry.name);
    
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = savePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save File";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    
    if (!GetSaveFileNameW(&ofn)) return;
    
    downloadFile_ = CreateFileW(savePath, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (downloadFile_ == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd_, L"Cannot create file", L"Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    downloadPath_ = savePath;
    downloadTotal_ = entry.size;
    downloadReceived_ = 0;
    downloading_ = true;
    
    SendMessageW(hStatusBar_, SB_SETTEXTW, 0, (LPARAM)L"Downloading...");
    
    std::wstring remotePath = currentPath_;
    if (!remotePath.empty() && remotePath.back() != L'\\') {
        remotePath += L"\\";
    }
    remotePath += entry.name;
    
    auto request = MessageBuilder::DownloadRequest(remotePath);
    transport_->send(request);
}

void FileWindow::createControls() {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwnd_, GWLP_HINSTANCE);
    
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);
    
    int y = 5;
    
    CreateWindowW(L"BUTTON", L"Up", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  5, y, 40, 24, hwnd_, (HMENU)IDC_UP_BUTTON, hInst, nullptr);
    
    hAddressBar_ = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  50, y, 500, 24, hwnd_, (HMENU)IDC_ADDRESSBAR, hInst, nullptr);
    
    CreateWindowW(L"BUTTON", L"Go", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  555, y, 45, 24, hwnd_, (HMENU)IDC_GO_BUTTON, hInst, nullptr);
    
    CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  605, y, 60, 24, hwnd_, (HMENU)IDC_REFRESH_BTN, hInst, nullptr);
    
    y += 32;
    
    // 图标
    hImageList_ = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 3, 0);
    
    SHFILEINFOW sfi = {};
    SHGetFileInfoW(L"folder", FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(sfi),
                   SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    if (sfi.hIcon) {
        ImageList_AddIcon(hImageList_, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
    }
    
    memset(&sfi, 0, sizeof(sfi));
    SHGetFileInfoW(L"file.txt", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    if (sfi.hIcon) {
        ImageList_AddIcon(hImageList_, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
    }
    
    memset(&sfi, 0, sizeof(sfi));
    SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
    if (sfi.hIcon) {
        ImageList_AddIcon(hImageList_, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
    }
    
    // ListView
    hListView_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_BORDER |
                                  LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                  5, y, 660, 350, hwnd_, (HMENU)IDC_LISTVIEW, hInst, nullptr);
    
    ListView_SetExtendedListViewStyle(hListView_,
                                       LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    ListView_SetImageList(hListView_, hImageList_, LVSIL_SMALL);
    
    // 添加列
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    
    wchar_t colName[] = L"Name";
    col.pszText = colName;
    col.cx = 280;
    LV_InsertColumnW(hListView_, 0, &col);
    
    wchar_t colSize[] = L"Size";
    col.pszText = colSize;
    col.cx = 100;
    LV_InsertColumnW(hListView_, 1, &col);
    
    wchar_t colType[] = L"Type";
    col.pszText = colType;
    col.cx = 80;
    LV_InsertColumnW(hListView_, 2, &col);
    
    wchar_t colModified[] = L"Modified";
    col.pszText = colModified;
    col.cx = 130;
    LV_InsertColumnW(hListView_, 3, &col);
    
    // 状态栏
    hStatusBar_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 0, 0, hwnd_, (HMENU)IDC_STATUSBAR, hInst, nullptr);
    SendMessageW(hStatusBar_, SB_SETTEXTW, 0, (LPARAM)L"Ready");
}

bool FileWindow::create(HINSTANCE hInstance) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RemoteFileManager";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    
    hwnd_ = CreateWindowExW(0, L"RemoteFileManager", L"Remote File Manager",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 700, 500,
                            nullptr, nullptr, hInstance, nullptr);
    
    if (!hwnd_) return false;
    
    createControls();
    return true;
}

void FileWindow::show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        
        if (transport_ && transport_->isConnected()) {
            navigateTo(currentPath_);
        }
    }
}

void FileWindow::destroy() {
    if (downloadFile_ != INVALID_HANDLE_VALUE) {
        CloseHandle(downloadFile_);
        downloadFile_ = INVALID_HANDLE_VALUE;
    }
    downloading_ = false;
    onClosed_ = nullptr;

    if (hwnd_) {
        if (g_fileWindow == this) g_fileWindow = nullptr;
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (hImageList_) {
        ImageList_Destroy(hImageList_);
        hImageList_ = nullptr;
    }
    if (g_fileWindow == this) g_fileWindow = nullptr;
}

LRESULT CALLBACK FileWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_fileWindow) return DefWindowProcW(hwnd, msg, wParam, lParam);
    
    auto* self = g_fileWindow;
    
    switch (msg) {
        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            
            MoveWindow(self->hAddressBar_, 50, 5, width - 175, 24, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_GO_BUTTON), width - 120, 5, 45, 24, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_REFRESH_BTN), width - 70, 5, 60, 24, TRUE);
            
            RECT rcStatus;
            GetWindowRect(self->hStatusBar_, &rcStatus);
            int statusHeight = rcStatus.bottom - rcStatus.top;
            MoveWindow(self->hListView_, 5, 37, width - 15, height - 45 - statusHeight, TRUE);
            
            SendMessageW(self->hStatusBar_, WM_SIZE, 0, 0);
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case IDC_UP_BUTTON:
                    self->goUp();
                    break;
                case IDC_GO_BUTTON: {
                    wchar_t path[MAX_PATH];
                    GetDlgItemTextW(hwnd, IDC_ADDRESSBAR, path, MAX_PATH);
                    self->navigateTo(path);
                    break;
                }
                case IDC_REFRESH_BTN:
                    self->refresh();
                    break;
            }
            break;
        }
        
        case WM_NOTIFY: {
            auto* pnmh = (LPNMHDR)lParam;
            if (pnmh->hwndFrom == self->hListView_) {
                switch (pnmh->code) {
                    case NM_DBLCLK:
                        self->onListViewDblClick();
                        break;
                    case NM_RCLICK:
                        self->onListViewRightClick();
                        break;
                }
            }
            break;
        }
        
        case WM_FILE_LIST_READY:
            self->populateListView();
            break;
            
        case WM_DOWNLOAD_PROGRESS: {
            wchar_t status[128];
            swprintf(status, 128, L"Downloading... %d%%", static_cast<int>(wParam));
            SendMessageW(self->hStatusBar_, SB_SETTEXTW, 0, (LPARAM)status);
            break;
        }
        
        case WM_DOWNLOAD_COMPLETE:
            if (lParam == 0) {
                SendMessageW(self->hStatusBar_, SB_SETTEXTW, 0, (LPARAM)L"Download complete");
                MessageBoxW(hwnd, L"Download complete!", L"Success", MB_OK | MB_ICONINFORMATION);
            } else {
                SendMessageW(self->hStatusBar_, SB_SETTEXTW, 0, (LPARAM)L"Download failed");
                MessageBoxW(hwnd, L"Download failed!", L"Error", MB_OK | MB_ICONERROR);
            }
            break;
        
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            if (self->onClosed_) {
                self->onClosed_();
            }
            return 0;
    }
    
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}