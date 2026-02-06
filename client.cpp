#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <string>
#include <dwmapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

const char* DEFAULT_SERVER_IP = "127.0.0.1";
int PORT = 12345;
std::atomic<bool> clientRunning(true);
std::atomic<bool> connectionActive(true);

int originalWidth = 0;
int originalHeight = 0;

std::atomic<bool> enableMouseMove(true);
std::atomic<bool> enableMouseClick(true);
std::atomic<bool> enableKeyboard(true);

struct InputEvent {
    int type;
    int x;
    int y;
    int key;
    int flags;
};

#define MOUSE_MIDDLE_DOWN 5
#define MOUSE_MIDDLE_UP   6
#define MOUSE_WHEEL       7

#define WM_UPDATE_DISPLAY (WM_USER + 100)
#define WM_CONNECTION_LOST (WM_USER + 101)

#define IDC_CHECK_MOUSE_MOVE  1001
#define IDC_CHECK_MOUSE_CLICK 1002
#define IDC_CHECK_KEYBOARD    1003
#define IDC_STATIC_INFO       1004

enum PacketType : uint8_t {
    PACKET_VIDEO = 0,
    PACKET_KEYFRAME_REQUEST = 1,
    PACKET_DIMENSIONS = 2
};

class HEVCDecoder {
private:
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* swsCtx = nullptr;
    int width = 0;
    int height = 0;
    bool initialized = false;
    std::mutex decodeMutex;
    AVCodecParserContext* parser = nullptr;

public:
    HEVCDecoder() = default;
    
    ~HEVCDecoder() {
        Cleanup();
    }
    
    void Cleanup() {
        std::lock_guard<std::mutex> lock(decodeMutex);
        if (parser) { av_parser_close(parser); parser = nullptr; }
        if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
        if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
        if (rgbFrame) { av_frame_free(&rgbFrame); rgbFrame = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (ctx) { avcodec_free_context(&ctx); ctx = nullptr; }
        initialized = false;
    }
    
    bool Init(int w, int h) {
        std::lock_guard<std::mutex> lock(decodeMutex);
        
        width = w;
        height = h;
        
        codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        if (!codec) {
            std::cerr << "错误: 未找到HEVC解码器\n";
            return false;
        }
        
        parser = av_parser_init(codec->id);
        if (!parser) {
            std::cerr << "错误: 无法创建解析器\n";
            return false;
        }
        
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            std::cerr << "错误: 无法分配解码器上下文\n";
            return false;
        }
        
        ctx->width = width;
        ctx->height = height;
        ctx->thread_count = 4;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            std::cerr << "错误: 无法打开解码器\n";
            return false;
        }
        
        frame = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        pkt = av_packet_alloc();
        
        if (!frame || !rgbFrame || !pkt) {
            std::cerr << "错误: 无法分配帧/数据包\n";
            return false;
        }
        
        rgbFrame->format = AV_PIX_FMT_BGRA;
        rgbFrame->width = width;
        rgbFrame->height = height;
        av_frame_get_buffer(rgbFrame, 32);
        
        initialized = true;
        std::cout << "HEVC解码器初始化成功: " << width << "x" << height << "\n";
        
        return true;
    }
    
    bool Decode(const uint8_t* data, int size, std::vector<uint8_t>& output) {
        std::lock_guard<std::mutex> lock(decodeMutex);
        
        if (!initialized) return false;
        
        output.clear();
        
        const uint8_t* dataPtr = data;
        int dataSize = size;
        
        while (dataSize > 0) {
            int parsedBytes = av_parser_parse2(
                parser, ctx,
                &pkt->data, &pkt->size,
                dataPtr, dataSize,
                AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0
            );
            
            if (parsedBytes < 0) return false;
            
            dataPtr += parsedBytes;
            dataSize -= parsedBytes;
            
            if (pkt->size > 0) {
                int ret = avcodec_send_packet(ctx, pkt);
                if (ret < 0) continue;
                
                while (ret >= 0) {
                    ret = avcodec_receive_frame(ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) return false;
                    
                    if (!swsCtx || frame->width != width || frame->height != height) {
                        if (swsCtx) sws_freeContext(swsCtx);
                        
                        width = frame->width;
                        height = frame->height;
                        
                        swsCtx = sws_getContext(
                            width, height, (AVPixelFormat)frame->format,
                            width, height, AV_PIX_FMT_BGRA,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                        );
                        
                        av_frame_unref(rgbFrame);
                        rgbFrame->format = AV_PIX_FMT_BGRA;
                        rgbFrame->width = width;
                        rgbFrame->height = height;
                        av_frame_get_buffer(rgbFrame, 32);
                    }
                    
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, height,
                              rgbFrame->data, rgbFrame->linesize);
                    
                    int outputSize = width * height * 4;
                    output.resize(outputSize);
                    
                    for (int y = 0; y < height; y++) {
                        memcpy(output.data() + y * width * 4,
                               rgbFrame->data[0] + y * rgbFrame->linesize[0],
                               width * 4);
                    }
                    
                    av_frame_unref(frame);
                }
            }
        }
        
        return !output.empty();
    }
    
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

class DisplayBuffer {
private:
    CRITICAL_SECTION cs;
    HDC memDC = nullptr;
    HBITMAP memBitmap = nullptr;
    HBITMAP oldBitmap = nullptr;
    void* bitmapBits = nullptr;
    int width = 0;
    int height = 0;

public:
    DisplayBuffer() { InitializeCriticalSection(&cs); }
    ~DisplayBuffer() { Cleanup(); DeleteCriticalSection(&cs); }
    
    void Cleanup() {
        EnterCriticalSection(&cs);
        if (memBitmap) {
            if (memDC && oldBitmap) SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            memBitmap = nullptr;
        }
        if (memDC) { DeleteDC(memDC); memDC = nullptr; }
        width = height = 0;
        bitmapBits = nullptr;
        LeaveCriticalSection(&cs);
    }
    
    bool UpdateFrame(const uint8_t* rgbData, int w, int h) {
        EnterCriticalSection(&cs);
        
        if (w != width || h != height) {
            if (memBitmap) {
                if (memDC && oldBitmap) SelectObject(memDC, oldBitmap);
                DeleteObject(memBitmap);
            }
            if (memDC) DeleteDC(memDC);
            
            HDC screenDC = GetDC(nullptr);
            memDC = CreateCompatibleDC(screenDC);
            
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            
            memBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
            if (!memBitmap) {
                ReleaseDC(nullptr, screenDC);
                LeaveCriticalSection(&cs);
                return false;
            }
            
            oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
            width = w;
            height = h;
            ReleaseDC(nullptr, screenDC);
        }
        
        if (bitmapBits && rgbData) {
            memcpy(bitmapBits, rgbData, w * h * 4);
        }
        
        LeaveCriticalSection(&cs);
        return true;
    }
    
    void Draw(HWND hwnd, HDC hdc) {
        EnterCriticalSection(&cs);
        
        if (!memDC || !memBitmap || width == 0 || height == 0) {
            LeaveCriticalSection(&cs);
            return;
        }
        
        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        int windowWidth = clientRect.right;
        int windowHeight = clientRect.bottom;
        
        // 创建临时DC用于双缓冲
        HDC bufferDC = CreateCompatibleDC(hdc);
        HBITMAP bufferBitmap = CreateCompatibleBitmap(hdc, windowWidth, windowHeight);
        HBITMAP oldBufferBitmap = (HBITMAP)SelectObject(bufferDC, bufferBitmap);
        
        // 填充黑色背景
        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(bufferDC, &clientRect, bgBrush);
        DeleteObject(bgBrush);
        
        // 计算显示区域
        RECT displayRect = CalculateDisplayRect(windowWidth, windowHeight);
        
        // 绘制图像
        SetStretchBltMode(bufferDC, HALFTONE);
        SetBrushOrgEx(bufferDC, 0, 0, nullptr);
        StretchBlt(bufferDC, 
                   displayRect.left, displayRect.top,
                   displayRect.right - displayRect.left,
                   displayRect.bottom - displayRect.top,
                   memDC, 0, 0, width, height, SRCCOPY);
        
        // 一次性复制到屏幕
        BitBlt(hdc, 0, 0, windowWidth, windowHeight, bufferDC, 0, 0, SRCCOPY);
        
        // 清理
        SelectObject(bufferDC, oldBufferBitmap);
        DeleteObject(bufferBitmap);
        DeleteDC(bufferDC);
        
        LeaveCriticalSection(&cs);
    }
    
    RECT CalculateDisplayRect(int windowWidth, int windowHeight) {
        RECT rect = {0, 0, windowWidth, windowHeight};
        if (width == 0 || height == 0) return rect;
        
        float aspectRatio = static_cast<float>(width) / height;
        float windowAspect = static_cast<float>(windowWidth) / windowHeight;
        
        if (aspectRatio > windowAspect) {
            int newHeight = static_cast<int>(windowWidth / aspectRatio);
            rect.top = (windowHeight - newHeight) / 2;
            rect.bottom = rect.top + newHeight;
        } else {
            int newWidth = static_cast<int>(windowHeight * aspectRatio);
            rect.left = (windowWidth - newWidth) / 2;
            rect.right = rect.left + newWidth;
        }
        return rect;
    }
    
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

DisplayBuffer g_displayBuffer;

struct WindowData {
    SOCKET serverSocket;
    std::string serverIP;
    HWND hwndMain;
    HWND hwndControlPanel;
    HEVCDecoder* decoder;
};

bool ReceiveFull(SOCKET socket, char* buffer, int size) {
    int totalReceived = 0;
    while (totalReceived < size && connectionActive) {
        int received = recv(socket, buffer + totalReceived, size - totalReceived, 0);
        if (received <= 0) return false;
        totalReceived += received;
    }
    return totalReceived == size;
}

bool SendData(SOCKET serverSocket, const char* data, int size) {
    if (serverSocket == INVALID_SOCKET) return false;
    int totalSent = 0;
    while (totalSent < size && connectionActive) {
        int sent = send(serverSocket, data + totalSent, size - totalSent, 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return totalSent == size;
}

void SendInputEvent(HWND hwnd, const InputEvent& event) {
    WindowData* windowData = reinterpret_cast<WindowData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!windowData || windowData->serverSocket == INVALID_SOCKET) return;
    SendData(windowData->serverSocket, reinterpret_cast<const char*>(&event), sizeof(InputEvent));
}

bool ConvertToImageCoords(HWND hwnd, int clientX, int clientY, int& imageX, int& imageY) {
    if (originalWidth == 0 || originalHeight == 0) return false;
    
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    
    RECT displayRect = g_displayBuffer.CalculateDisplayRect(clientRect.right, clientRect.bottom);
    
    if (clientX < displayRect.left || clientX >= displayRect.right ||
        clientY < displayRect.top || clientY >= displayRect.bottom) {
        return false;
    }
    
    float scaleX = static_cast<float>(originalWidth) / (displayRect.right - displayRect.left);
    float scaleY = static_cast<float>(originalHeight) / (displayRect.bottom - displayRect.top);
    
    imageX = static_cast<int>((clientX - displayRect.left) * scaleX);
    imageY = static_cast<int>((clientY - displayRect.top) * scaleY);
    
    imageX = std::max(0, std::min(originalWidth - 1, imageX));
    imageY = std::max(0, std::min(originalHeight - 1, imageY));
    
    return true;
}

LRESULT CALLBACK ControlPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CreateWindowA("STATIC", "服务器信息", WS_CHILD | WS_VISIBLE | SS_CENTER,
                10, 10, 180, 20, hwnd, (HMENU)IDC_STATIC_INFO, GetModuleHandle(nullptr), nullptr);
            
            CreateWindowA("BUTTON", "鼠标移动", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 50, 180, 20, hwnd, (HMENU)IDC_CHECK_MOUSE_MOVE, GetModuleHandle(nullptr), nullptr);
            CheckDlgButton(hwnd, IDC_CHECK_MOUSE_MOVE, BST_CHECKED);
            
            CreateWindowA("BUTTON", "鼠标点击", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 80, 180, 20, hwnd, (HMENU)IDC_CHECK_MOUSE_CLICK, GetModuleHandle(nullptr), nullptr);
            CheckDlgButton(hwnd, IDC_CHECK_MOUSE_CLICK, BST_CHECKED);
            
            CreateWindowA("BUTTON", "键盘", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 110, 180, 20, hwnd, (HMENU)IDC_CHECK_KEYBOARD, GetModuleHandle(nullptr), nullptr);
            CheckDlgButton(hwnd, IDC_CHECK_KEYBOARD, BST_CHECKED);
            break;
        }
        
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (id) {
                    case IDC_CHECK_MOUSE_MOVE:
                        enableMouseMove = (IsDlgButtonChecked(hwnd, IDC_CHECK_MOUSE_MOVE) == BST_CHECKED);
                        break;
                    case IDC_CHECK_MOUSE_CLICK:
                        enableMouseClick = (IsDlgButtonChecked(hwnd, IDC_CHECK_MOUSE_CLICK) == BST_CHECKED);
                        break;
                    case IDC_CHECK_KEYBOARD:
                        enableKeyboard = (IsDlgButtonChecked(hwnd, IDC_CHECK_KEYBOARD) == BST_CHECKED);
                        break;
                }
            }
            break;
        }
        
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

HWND CreateControlPanel(HWND hwndMain, const std::string& serverIP) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = ControlPanelProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "ControlPanelClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    
    RECT rc;
    GetWindowRect(hwndMain, &rc);
    
    HWND hwndControl = CreateWindowA("ControlPanelClass", "控制面板",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        rc.right, rc.top, 200, 200,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    
    if (hwndControl) {
        std::ostringstream oss;
        oss << "服务器: " << serverIP << ":" << PORT;
        SetDlgItemTextA(hwndControl, IDC_STATIC_INFO, oss.str().c_str());
    }
    
    return hwndControl;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WindowData* windowData = reinterpret_cast<WindowData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            windowData = reinterpret_cast<WindowData*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(windowData));
            if (windowData) {
                windowData->hwndControlPanel = CreateControlPanel(hwnd, windowData->serverIP);
            }
            break;
        }
        
        case WM_DESTROY:
            clientRunning = false;
            connectionActive = false;
            if (windowData && windowData->hwndControlPanel) {
                DestroyWindow(windowData->hwndControlPanel);
            }
            PostQuitMessage(0);
            return 0;
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            g_displayBuffer.Draw(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_ERASEBKGND:
            return 1;
        
        case WM_UPDATE_DISPLAY:
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            return 0;
        
        case WM_CONNECTION_LOST:
            MessageBoxA(hwnd, "与服务器的连接已断开", "连接错误", MB_ICONERROR);
            DestroyWindow(hwnd);
            return 0;
        
        case WM_MOVE:
            if (windowData && windowData->hwndControlPanel) {
                RECT rc;
                GetWindowRect(hwnd, &rc);
                SetWindowPos(windowData->hwndControlPanel, nullptr, rc.right, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            break;
        
        case WM_LBUTTONDOWN:
            if (enableMouseClick) {
                SetCapture(hwnd);
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, 1, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_LBUTTONUP:
            if (enableMouseClick) {
                ReleaseCapture();
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, 2, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_RBUTTONDOWN:
            if (enableMouseClick) {
                SetCapture(hwnd);
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, 3, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_RBUTTONUP:
            if (enableMouseClick) {
                ReleaseCapture();
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, 4, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_MBUTTONDOWN:
            if (enableMouseClick) {
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, MOUSE_MIDDLE_DOWN, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_MBUTTONUP:
            if (enableMouseClick) {
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, MOUSE_MIDDLE_UP, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_MOUSEWHEEL: {
            if (enableMouseClick) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                int x, y;
                if (ConvertToImageCoords(hwnd, pt.x, pt.y, x, y)) {
                    InputEvent event{0, x, y, MOUSE_WHEEL, delta};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        }
        
        case WM_MOUSEMOVE:
            if (enableMouseMove) {
                int x, y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y)) {
                    InputEvent event{0, x, y, 0, 0};
                    SendInputEvent(hwnd, event);
                }
            }
            break;
        
        case WM_KEYDOWN:
            if (enableKeyboard) {
                InputEvent event{1, 0, 0, static_cast<int>(wParam), 0};
                SendInputEvent(hwnd, event);
            }
            break;
        
        case WM_KEYUP:
            if (enableKeyboard) {
                InputEvent event{1, 0, 0, static_cast<int>(wParam), 1};
                SendInputEvent(hwnd, event);
            }
            break;
    }
    
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ReceiveThread(WindowData* windowData) {
    if (!windowData) return;
    
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> rgbData;
    
    const int TARGET_FPS = 60;
    const DWORD frameInterval = 1000 / TARGET_FPS;
    DWORD lastFrameTime = GetTickCount();
    
    while (clientRunning && connectionActive) {
        uint8_t packetType;
        if (!ReceiveFull(windowData->serverSocket, reinterpret_cast<char*>(&packetType), 1)) {
            break;
        }
        
        if (packetType == PACKET_VIDEO) {
            uint8_t isKeyframe;
            if (!ReceiveFull(windowData->serverSocket, reinterpret_cast<char*>(&isKeyframe), 1)) {
                break;
            }
            
            int32_t dataSize;
            if (!ReceiveFull(windowData->serverSocket, reinterpret_cast<char*>(&dataSize), sizeof(dataSize))) {
                break;
            }
            
            if (dataSize <= 0 || dataSize > 10 * 1024 * 1024) {
                continue;
            }
            
            buffer.resize(dataSize);
            if (!ReceiveFull(windowData->serverSocket, reinterpret_cast<char*>(buffer.data()), dataSize)) {
                break;
            }
            
            if (windowData->decoder->Decode(buffer.data(), dataSize, rgbData)) {
                int decodedWidth = windowData->decoder->GetWidth();
                int decodedHeight = windowData->decoder->GetHeight();
                
                originalWidth = decodedWidth;
                originalHeight = decodedHeight;
                
                g_displayBuffer.UpdateFrame(rgbData.data(), decodedWidth, decodedHeight);
                
                DWORD currentTime = GetTickCount();
                if (currentTime - lastFrameTime >= frameInterval) {
                    PostMessage(windowData->hwndMain, WM_UPDATE_DISPLAY, 0, 0);
                    lastFrameTime = currentTime;
                }
            }
        }
    }
    
    PostMessage(windowData->hwndMain, WM_CONNECTION_LOST, 0, 0);
}

int main() {
    std::string serverIP = DEFAULT_SERVER_IP;
    std::cout << "请输入服务器IP地址 (默认: " << serverIP << "): ";
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty()) serverIP = input;
    
    std::cout << "请输入端口 (默认: " << PORT << "): ";
    std::getline(std::cin, input);
    if (!input.empty()) PORT = atoi(input.c_str());
    
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup失败\n";
        return 1;
    }
    
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "创建socket失败\n";
        WSACleanup();
        return 1;
    }
    
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);
    serverAddr.sin_port = htons(PORT);
    
    std::cout << "正在连接到 " << serverIP << ":" << PORT << "...\n";
    
    if (connect(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "连接服务器失败\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    int flag = 1;
    setsockopt(serverSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
    
    int bufSize = 2 * 1024 * 1024;
    setsockopt(serverSocket, SOL_SOCKET, SO_RCVBUF, (char*)&bufSize, sizeof(bufSize));
    
    std::cout << "已连接到服务器\n";
    
    int dimensions[2];
    if (!ReceiveFull(serverSocket, reinterpret_cast<char*>(dimensions), sizeof(dimensions))) {
        std::cerr << "接收屏幕尺寸失败\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    int screenWidth = dimensions[0];
    int screenHeight = dimensions[1];
    originalWidth = screenWidth;
    originalHeight = screenHeight;
    
    std::cout << "远程屏幕尺寸: " << screenWidth << "x" << screenHeight << "\n";
    
    HEVCDecoder decoder;
    if (!decoder.Init(screenWidth, screenHeight)) {
        std::cerr << "HEVC解码器初始化失败\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "HEVCRemoteDesktop";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    RegisterClassA(&wc);
    
    WindowData windowData;
    windowData.serverSocket = serverSocket;
    windowData.serverIP = serverIP;
    windowData.decoder = &decoder;
    windowData.hwndMain = nullptr;
    windowData.hwndControlPanel = nullptr;
    
    int winWidth = GetSystemMetrics(SM_CXSCREEN) * 3 / 4;
    int winHeight = GetSystemMetrics(SM_CYSCREEN) * 3 / 4;
    
    HWND hwnd = CreateWindowExA(
        WS_EX_COMPOSITED,
        "HEVCRemoteDesktop", 
        "HEVC远程桌面客户端",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, winWidth, winHeight,
        nullptr, nullptr, GetModuleHandle(nullptr), &windowData);
    
    if (!hwnd) {
        std::cerr << "创建窗口失败\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    windowData.hwndMain = hwnd;
    
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, 
                          &disableTransitions, sizeof(disableTransitions));
    
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    
    std::thread receiver(ReceiveThread, &windowData);
    
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) && clientRunning) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    clientRunning = false;
    connectionActive = false;
    
    shutdown(serverSocket, SD_BOTH);
    closesocket(serverSocket);
    
    if (receiver.joinable()) {
        receiver.join();
    }
    
    g_displayBuffer.Cleanup();
    WSACleanup();
    
    return 0;
}