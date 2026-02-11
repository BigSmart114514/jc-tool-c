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
#include <p2p/p2p_client.hpp>

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
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "p2p-client.lib")

// ==================== 配置 ====================
std::atomic<bool> clientRunning(true);
std::atomic<int> originalWidth(0);
std::atomic<int> originalHeight(0);

std::atomic<bool> enableMouseMove(true);
std::atomic<bool> enableMouseClick(true);
std::atomic<bool> enableKeyboard(true);

// ==================== 协议 ====================
struct InputEvent {
    int type, x, y, key, flags;
};

#define MOUSE_MIDDLE_DOWN 5
#define MOUSE_MIDDLE_UP   6
#define MOUSE_WHEEL       7

enum class BinaryMsgType : uint8_t {
    VideoFrame = 0,
    InputEvent = 1
};

enum PacketType : uint8_t {
    PACKET_VIDEO = 0
};

// 新增 Relay 模式
enum class TransportMode { TCP, P2P, Relay };

#define WM_UPDATE_DISPLAY   (WM_USER + 100)
#define WM_CONNECTION_LOST  (WM_USER + 101)

#define IDC_CHECK_MOUSE_MOVE  1001
#define IDC_CHECK_MOUSE_CLICK 1002
#define IDC_CHECK_KEYBOARD    1003
#define IDC_STATIC_INFO       1004

// ==================== HEVC解码器 ====================
class HEVCDecoder {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* rgbFrame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* swsCtx = nullptr;
    AVCodecParserContext* parser = nullptr;
    int width = 0, height = 0;
    bool initialized = false;
    std::mutex decodeMutex;

public:
    ~HEVCDecoder() { Cleanup(); }

    void Cleanup() {
        std::lock_guard<std::mutex> lock(decodeMutex);
        if (parser) { av_parser_close(parser); parser = nullptr; }
        if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
        if (pkt) { av_packet_free(&pkt); }
        if (rgbFrame) { av_frame_free(&rgbFrame); }
        if (frame) { av_frame_free(&frame); }
        if (ctx) { avcodec_free_context(&ctx); }
        initialized = false;
    }

    bool Init(int w, int h) {
        std::lock_guard<std::mutex> lock(decodeMutex);
        width = w; height = h;
        codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        if (!codec) { std::cerr << "未找到HEVC解码器\n"; return false; }
        parser = av_parser_init(codec->id);
        if (!parser) return false;
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        ctx->width = width; ctx->height = height;
        ctx->thread_count = 4;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        if (avcodec_open2(ctx, codec, nullptr) < 0) return false;
        frame = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !rgbFrame || !pkt) return false;
        rgbFrame->format = AV_PIX_FMT_BGRA;
        rgbFrame->width = width; rgbFrame->height = height;
        av_frame_get_buffer(rgbFrame, 32);
        initialized = true;
        std::cout << "HEVC解码器: " << width << "x" << height << "\n";
        return true;
    }

    bool Decode(const uint8_t* data, int size, std::vector<uint8_t>& output) {
        std::lock_guard<std::mutex> lock(decodeMutex);
        if (!initialized) return false;
        output.clear();
        const uint8_t* p = data; int remaining = size;
        while (remaining > 0) {
            int parsed = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size,
                                           p, remaining, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (parsed < 0) return false;
            p += parsed; remaining -= parsed;
            if (pkt->size > 0) {
                int ret = avcodec_send_packet(ctx, pkt);
                if (ret < 0) continue;
                while (ret >= 0) {
                    ret = avcodec_receive_frame(ctx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) return false;
                    if (!swsCtx || frame->width != width || frame->height != height) {
                        if (swsCtx) sws_freeContext(swsCtx);
                        width = frame->width; height = frame->height;
                        swsCtx = sws_getContext(width, height, (AVPixelFormat)frame->format,
                                                width, height, AV_PIX_FMT_BGRA,
                                                SWS_BILINEAR, nullptr, nullptr, nullptr);
                        av_frame_unref(rgbFrame);
                        rgbFrame->format = AV_PIX_FMT_BGRA;
                        rgbFrame->width = width; rgbFrame->height = height;
                        av_frame_get_buffer(rgbFrame, 32);
                    }
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, height,
                              rgbFrame->data, rgbFrame->linesize);
                    output.resize(width * height * 4);
                    for (int y = 0; y < height; y++)
                        memcpy(output.data() + y * width * 4,
                               rgbFrame->data[0] + y * rgbFrame->linesize[0], width * 4);
                    av_frame_unref(frame);
                }
            }
        }
        return !output.empty();
    }

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

// ==================== 显示缓冲 ====================
class DisplayBuffer {
    CRITICAL_SECTION cs;
    HDC memDC = nullptr;
    HBITMAP memBitmap = nullptr;
    HBITMAP oldBitmap = nullptr;
    void* bitmapBits = nullptr;
    int width = 0, height = 0;

public:
    DisplayBuffer() { InitializeCriticalSection(&cs); }
    ~DisplayBuffer() { Cleanup(); DeleteCriticalSection(&cs); }

    void Cleanup() {
        EnterCriticalSection(&cs);
        if (memBitmap) { if (memDC && oldBitmap) SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); memBitmap = nullptr; }
        if (memDC) { DeleteDC(memDC); memDC = nullptr; }
        width = height = 0; bitmapBits = nullptr;
        LeaveCriticalSection(&cs);
    }

    bool UpdateFrame(const uint8_t* data, int w, int h) {
        EnterCriticalSection(&cs);
        if (w != width || h != height) {
            if (memBitmap) { if (memDC && oldBitmap) SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); }
            if (memDC) DeleteDC(memDC);
            HDC sdc = GetDC(nullptr);
            memDC = CreateCompatibleDC(sdc);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = w; bmi.bmiHeader.biHeight = -h;
            bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            memBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
            if (!memBitmap) { ReleaseDC(nullptr, sdc); LeaveCriticalSection(&cs); return false; }
            oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
            width = w; height = h;
            ReleaseDC(nullptr, sdc);
        }
        if (bitmapBits && data) memcpy(bitmapBits, data, w * h * 4);
        LeaveCriticalSection(&cs);
        return true;
    }

    RECT CalculateDisplayRect(int ww, int wh) {
        RECT r = {0, 0, ww, wh};
        if (width == 0 || height == 0) return r;
        float ar = (float)width / height, war = (float)ww / wh;
        if (ar > war) {
            int nh = (int)(ww / ar); r.top = (wh - nh) / 2; r.bottom = r.top + nh;
        } else {
            int nw = (int)(wh * ar); r.left = (ww - nw) / 2; r.right = r.left + nw;
        }
        return r;
    }

    void Draw(HWND hwnd, HDC hdc) {
        EnterCriticalSection(&cs);
        if (!memDC || !memBitmap || width == 0 || height == 0) { LeaveCriticalSection(&cs); return; }
        RECT cr; GetClientRect(hwnd, &cr);
        int ww = cr.right, wh = cr.bottom;
        HDC bdc = CreateCompatibleDC(hdc);
        HBITMAP bb = CreateCompatibleBitmap(hdc, ww, wh);
        HBITMAP ob = (HBITMAP)SelectObject(bdc, bb);
        HBRUSH bg = CreateSolidBrush(RGB(0,0,0));
        FillRect(bdc, &cr, bg); DeleteObject(bg);
        RECT dr = CalculateDisplayRect(ww, wh);
        SetStretchBltMode(bdc, HALFTONE);
        StretchBlt(bdc, dr.left, dr.top, dr.right-dr.left, dr.bottom-dr.top,
                   memDC, 0, 0, width, height, SRCCOPY);
        BitBlt(hdc, 0, 0, ww, wh, bdc, 0, 0, SRCCOPY);
        SelectObject(bdc, ob); DeleteObject(bb); DeleteDC(bdc);
        LeaveCriticalSection(&cs);
    }

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

// ==================== 全局实例 ====================
DisplayBuffer g_display;
HEVCDecoder g_decoder;
HWND g_hwndMain = nullptr;
DWORD g_lastFrameTime = 0;

// ==================== 传输层抽象 ====================
class IClientTransport {
public:
    virtual ~IClientTransport() = default;
    virtual bool sendInput(const InputEvent& ev) = 0;
    virtual bool isConnected() const = 0;
    virtual void disconnect() = 0;
};

IClientTransport* g_transport = nullptr;

// ==================== 视频帧处理 (共享) ====================
void HandleVideoFrame(const uint8_t* data, size_t size, bool isKeyframe) {
    std::vector<uint8_t> rgbData;
    if (g_decoder.Decode(data, (int)size, rgbData)) {
        int w = g_decoder.GetWidth();
        int h = g_decoder.GetHeight();
        originalWidth = w;
        originalHeight = h;
        g_display.UpdateFrame(rgbData.data(), w, h);

        DWORD now = GetTickCount();
        if (now - g_lastFrameTime >= 16) {
            if (g_hwndMain) PostMessage(g_hwndMain, WM_UPDATE_DISPLAY, 0, 0);
            g_lastFrameTime = now;
        }
    }
}

// ==================== TCP 传输 ====================
class TCPTransport : public IClientTransport {
    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::thread recvThread_;

public:
    ~TCPTransport() { disconnect(); }

    bool connect(const std::string& ip, int port) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);

        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == INVALID_SOCKET) return false;

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        addr.sin_port = htons(port);

        std::cout << "连接 " << ip << ":" << port << "..." << std::endl;

        if (::connect(sock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "TCP 连接失败" << std::endl;
            closesocket(sock_);
            return false;
        }

        int flag = 1;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        int bufSize = 2 * 1024 * 1024;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (char*)&bufSize, sizeof(bufSize));

        connected_ = true;

        // 接收屏幕尺寸
        int dims[2];
        if (!recvAll(dims, sizeof(dims))) {
            closesocket(sock_);
            return false;
        }

        originalWidth = dims[0];
        originalHeight = dims[1];
        std::cout << "远程屏幕: " << dims[0] << "x" << dims[1] << std::endl;

        if (!g_decoder.Init(dims[0], dims[1])) {
            closesocket(sock_);
            return false;
        }


        // 启动接收线程
        recvThread_ = std::thread([this]() { recvLoop(); });

        return true;
    }

    void recvLoop() {
        std::vector<uint8_t> buffer;
        while (clientRunning && connected_) {
            uint8_t packetType;
            if (!recvAll(&packetType, 1)) break;

            if (packetType == PACKET_VIDEO) {
                uint8_t isKeyframe;
                if (!recvAll(&isKeyframe, 1)) break;

                int32_t dataSize;
                if (!recvAll(&dataSize, sizeof(dataSize))) break;
                if (dataSize <= 0 || dataSize > 10*1024*1024) continue;

                buffer.resize(dataSize);
                if (!recvAll(buffer.data(), dataSize)) break;

                HandleVideoFrame(buffer.data(), dataSize, isKeyframe != 0);
            }
        }

        connected_ = false;
        if (g_hwndMain) PostMessage(g_hwndMain, WM_CONNECTION_LOST, 0, 0);
    }

    bool sendInput(const InputEvent& ev) override {
        if (!connected_) return false;
        return sendAll(&ev, sizeof(ev));
    }

    bool isConnected() const override { return connected_; }

    void disconnect() override {
        connected_ = false;
        if (sock_ != INVALID_SOCKET) {
            shutdown(sock_, SD_BOTH);
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (recvThread_.joinable()) recvThread_.join();
    }

private:
    bool recvAll(void* buf, int len) {
        char* p = (char*)buf;
        while (len > 0 && connected_) {
            int r = recv(sock_, p, len, 0);
            if (r <= 0) return false;
            p += r; len -= r;
        }
        return len == 0;
    }

    bool sendAll(const void* data, int len) {
        const char* p = (const char*)data;
        while (len > 0) {
            int s = send(sock_, p, len, 0);
            if (s <= 0) return false;
            p += s; len -= s;
        }
        return true;
    }
};

// ==================== P2P/Relay 传输 ====================
class P2PTransport : public IClientTransport {
    std::unique_ptr<p2p::P2PClient> client_;
    std::string serverPeerId_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    bool useRelay_ = false;  // 是否使用中继模式

public:
    ~P2PTransport() { disconnect(); }

    // 统一的连接方法，支持 P2P 直连和中继模式
    bool connect(const std::string& signalingUrl, const std::string& serverPeerId,
                 bool useRelay = false, const std::string& relayPassword = "") {
        serverPeerId_ = serverPeerId;
        useRelay_ = useRelay;

        client_ = std::make_unique<p2p::P2PClient>(signalingUrl);

        // ========== 基础回调 ==========
        client_->setOnConnected([this]() {
            std::cout << "信令已连接, 本地 ID: " << client_->getLocalId() << std::endl;
        });

        client_->setOnDisconnected([](const p2p::Error& err) {
            std::cerr << "信令断开: " << err.message << std::endl;
        });

        client_->setOnError([](const p2p::Error& err) {
            std::cerr << "P2P 错误: " << err.message << std::endl;
        });

        // ========== P2P 直连回调 ==========
        client_->setOnPeerConnected([this](const std::string& peerId) {
            if (!useRelay_ && peerId == serverPeerId_) {
                std::cout << "P2P 已连接到服务端: " << peerId << std::endl;
                connected_ = true;
            }
        });

        client_->setOnPeerDisconnected([this](const std::string& peerId) {
            if (!useRelay_ && peerId == serverPeerId_) {
                std::cout << "P2P 服务端断开: " << peerId << std::endl;
                connected_ = false;
                ready_ = false;
                if (g_hwndMain) PostMessage(g_hwndMain, WM_CONNECTION_LOST, 0, 0);
            }
        });

        // ========== 中继模式回调 ==========
        client_->setOnRelayConnected([this](const std::string& peerId) {
            if (useRelay_ && peerId == serverPeerId_) {
                std::cout << "中继已连接到服务端: " << peerId << std::endl;
                connected_ = true;
            }
        });

        client_->setOnRelayDisconnected([this](const std::string& peerId) {
            if (useRelay_ && peerId == serverPeerId_) {
                std::cout << "中继服务端断开: " << peerId << std::endl;
                connected_ = false;
                ready_ = false;
                if (g_hwndMain) PostMessage(g_hwndMain, WM_CONNECTION_LOST, 0, 0);
            }
        });

        client_->setOnRelayAuthResult([](bool success, const std::string& message) {
            if (success) {
                std::cout << "中继认证成功: " << message << std::endl;
            } else {
                std::cerr << "中继认证失败: " << message << std::endl;
            }
        });

        // ========== 消息回调 (P2P 和中继共用) ==========
        client_->setOnTextMessage([this](const std::string& from, const std::string& msg) {
            if (msg.find("\"type\":\"screen_info\"") != std::string::npos) {
                // 解析屏幕信息 JSON
                size_t wp = msg.find("\"width\":") + 8;
                size_t hp = msg.find("\"height\":") + 9;
                int w = std::atoi(msg.c_str() + wp);
                int h = std::atoi(msg.c_str() + hp);

                std::cout << "远程屏幕: " << w << "x" << h << std::endl;
                originalWidth = w;
                originalHeight = h;

                if (g_decoder.Init(w, h)) {
                    // 根据模式选择发送方式
                    if (useRelay_) {
                        client_->sendTextViaRelay(from, "{\"type\":\"ready\"}");
                    } else {
                        client_->sendText(from, "{\"type\":\"ready\"}");
                    }
                    ready_ = true;
                }
            }
        });

        client_->setOnBinaryMessage([this](const std::string& from, const p2p::BinaryData& data) {
            if (data.size() < 2) return;
            if (data[0] == static_cast<uint8_t>(BinaryMsgType::VideoFrame)) {
                bool isKeyframe = data[1] != 0;
                HandleVideoFrame(data.data() + 2, data.size() - 2, isKeyframe);
            }
        });

        // ========== 连接信令服务器 ==========
        std::cout << "连接信令服务器: " << signalingUrl << std::endl;
        if (!client_->connect()) {
            std::cerr << "连接信令服务器失败" << std::endl;
            return false;
        }

        // ========== 根据模式进行连接 ==========
        if (useRelay_) {
            // 中继模式：先认证，再连接
            std::cout << "进行中继认证..." << std::endl;
            if (!client_->authenticateRelay(relayPassword)) {
                std::cerr << "中继认证失败" << std::endl;
                return false;
            }

            std::cout << "通过中继连接服务端: " << serverPeerId << std::endl;
            if (!client_->connectToPeerViaRelay(serverPeerId)) {
                std::cerr << "中继连接请求失败" << std::endl;
                return false;
            }
        } else {
            // P2P 模式：直接连接
            std::cout << "P2P 连接服务端: " << serverPeerId << std::endl;
            client_->connectToPeer(serverPeerId);
        }

        // ========== 等待连接完成和屏幕信息 ==========
        auto start = GetTickCount();
        while (!ready_ && (GetTickCount() - start) < 30000) {
            Sleep(100);
        }

        if (!ready_) {
            std::cerr << (useRelay_ ? "中继" : "P2P") << " 连接超时" << std::endl;
            return false;
        }

        return true;
    }

    bool sendInput(const InputEvent& ev) override {
        if (!connected_ || !ready_ || !client_) return false;

        // 构建输入事件数据包
        p2p::BinaryData packet;
        packet.reserve(1 + sizeof(InputEvent));
        packet.push_back(static_cast<uint8_t>(BinaryMsgType::InputEvent));
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&ev);
        packet.insert(packet.end(), ptr, ptr + sizeof(InputEvent));

        // 根据模式选择发送方式
        if (useRelay_) {
            return client_->sendBinaryViaRelay(serverPeerId_, packet);
        } else {
            return client_->sendBinary(serverPeerId_, packet);
        }
    }

    bool isConnected() const override { return connected_ && ready_; }

    void disconnect() override {
        connected_ = false;
        ready_ = false;
        if (client_) {
            if (useRelay_) {
                client_->disconnectFromPeerViaRelay(serverPeerId_);
            } else {
                client_->disconnectFromPeer(serverPeerId_);
            }
            client_->disconnect();
            client_.reset();
        }
    }

    bool isRelayMode() const { return useRelay_; }
};

// ==================== 发送输入 ====================
void SendInputToServer(const InputEvent& ev) {
    if (g_transport && g_transport->isConnected()) {
        g_transport->sendInput(ev);
    }
}

bool ConvertToImageCoords(HWND hwnd, int cx, int cy, int& ix, int& iy) {
    int ow = originalWidth, oh = originalHeight;
    if (ow == 0 || oh == 0) return false;
    RECT cr; GetClientRect(hwnd, &cr);
    RECT dr = g_display.CalculateDisplayRect(cr.right, cr.bottom);
    if (cx < dr.left || cx >= dr.right || cy < dr.top || cy >= dr.bottom) return false;
    float sx = (float)ow / (dr.right - dr.left);
    float sy = (float)oh / (dr.bottom - dr.top);
    ix = std::clamp((int)((cx - dr.left) * sx), 0, ow - 1);
    iy = std::clamp((int)((cy - dr.top) * sy), 0, oh - 1);
    return true;
}

// ==================== 控制面板 ====================
LRESULT CALLBACK ControlPanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            CreateWindowA("STATIC", "连接信息", WS_CHILD|WS_VISIBLE|SS_CENTER,
                10, 10, 180, 40, hwnd, (HMENU)IDC_STATIC_INFO, GetModuleHandle(0), 0);
            CreateWindowA("BUTTON", "鼠标移动", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                10, 60, 180, 20, hwnd, (HMENU)IDC_CHECK_MOUSE_MOVE, GetModuleHandle(0), 0);
            CheckDlgButton(hwnd, IDC_CHECK_MOUSE_MOVE, BST_CHECKED);
            CreateWindowA("BUTTON", "鼠标点击", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                10, 90, 180, 20, hwnd, (HMENU)IDC_CHECK_MOUSE_CLICK, GetModuleHandle(0), 0);
            CheckDlgButton(hwnd, IDC_CHECK_MOUSE_CLICK, BST_CHECKED);
            CreateWindowA("BUTTON", "键盘", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                10, 120, 180, 20, hwnd, (HMENU)IDC_CHECK_KEYBOARD, GetModuleHandle(0), 0);
            CheckDlgButton(hwnd, IDC_CHECK_KEYBOARD, BST_CHECKED);
            break;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case IDC_CHECK_MOUSE_MOVE:
                        enableMouseMove = IsDlgButtonChecked(hwnd, IDC_CHECK_MOUSE_MOVE) == BST_CHECKED; break;
                    case IDC_CHECK_MOUSE_CLICK:
                        enableMouseClick = IsDlgButtonChecked(hwnd, IDC_CHECK_MOUSE_CLICK) == BST_CHECKED; break;
                    case IDC_CHECK_KEYBOARD:
                        enableKeyboard = IsDlgButtonChecked(hwnd, IDC_CHECK_KEYBOARD) == BST_CHECKED; break;
                }
            }
            break;
        case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ==================== 窗口数据 ====================
struct WindowData {
    HWND hwndControl;
    TransportMode mode;
    std::string connectInfo;
};

HWND CreateControlPanel(HWND parent, const std::string& info) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = ControlPanelProc;
    wc.hInstance = GetModuleHandle(0);
    wc.lpszClassName = "DualModeControlPanel";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassA(&wc);
    RECT rc; GetWindowRect(parent, &rc);
    HWND h = CreateWindowA("DualModeControlPanel", "控制面板",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE, rc.right, rc.top, 200, 200,
        nullptr, nullptr, GetModuleHandle(0), nullptr);
    if (h) SetDlgItemTextA(h, IDC_STATIC_INFO, info.c_str());
    return h;
}

// ==================== 主窗口过程 ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WindowData* wd = (WindowData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            wd = (WindowData*)cs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)wd);
            if (wd) wd->hwndControl = CreateControlPanel(hwnd, wd->connectInfo);
            break;
        }
        case WM_DESTROY:
            clientRunning = false;
            if (wd && wd->hwndControl) DestroyWindow(wd->hwndControl);
            PostQuitMessage(0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            g_display.Draw(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_UPDATE_DISPLAY:
            RedrawWindow(hwnd, 0, 0, RDW_INVALIDATE|RDW_UPDATENOW);
            return 0;
        case WM_CONNECTION_LOST:
            MessageBoxA(hwnd, "连接已断开", "连接错误", MB_ICONERROR);
            DestroyWindow(hwnd);
            return 0;
        case WM_MOVE:
            if (wd && wd->hwndControl) {
                RECT rc; GetWindowRect(hwnd, &rc);
                SetWindowPos(wd->hwndControl, 0, rc.right, rc.top, 0, 0, SWP_NOSIZE|SWP_NOZORDER);
            }
            break;

        // ===== 鼠标 =====
        case WM_LBUTTONDOWN:
            if (enableMouseClick) {
                SetCapture(hwnd); int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, 1, 0});
            } break;
        case WM_LBUTTONUP:
            if (enableMouseClick) {
                ReleaseCapture(); int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, 2, 0});
            } break;
        case WM_RBUTTONDOWN:
            if (enableMouseClick) {
                SetCapture(hwnd); int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, 3, 0});
            } break;
        case WM_RBUTTONUP:
            if (enableMouseClick) {
                ReleaseCapture(); int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, 4, 0});
            } break;
        case WM_MBUTTONDOWN:
            if (enableMouseClick) {
                int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, MOUSE_MIDDLE_DOWN, 0});
            } break;
        case WM_MBUTTONUP:
            if (enableMouseClick) {
                int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, MOUSE_MIDDLE_UP, 0});
            } break;
        case WM_MOUSEWHEEL:
            if (enableMouseClick) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt); int x,y;
                if (ConvertToImageCoords(hwnd, pt.x, pt.y, x, y))
                    SendInputToServer({0, x, y, MOUSE_WHEEL, delta});
            } break;
        case WM_MOUSEMOVE:
            if (enableMouseMove) {
                int x,y;
                if (ConvertToImageCoords(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), x, y))
                    SendInputToServer({0, x, y, 0, 0});
            } break;

        // ===== 键盘 =====
        case WM_KEYDOWN:
            if (enableKeyboard) SendInputToServer({1, 0, 0, (int)wParam, 0});
            break;
        case WM_KEYUP:
            if (enableKeyboard) SendInputToServer({1, 0, 0, (int)wParam, 1});
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ==================== 主函数 ====================
int main() {
    //SetConsoleOutputCP(65001);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    std::cout << "========================================\n";
    std::cout << "   三模式 HEVC 远程桌面客户端\n";
    std::cout << "========================================\n\n";
    std::cout << "选择连接方式:\n";
    std::cout << "  1. TCP 直连 (IP:端口)\n";
    std::cout << "  2. P2P 穿透 (信令服务器 + Peer ID)\n";
    std::cout << "  3. 中继模式 (信令服务器 + Peer ID + 密码)\n";
    std::cout << "请选择 (1/2/3): ";

    std::string choice;
    std::getline(std::cin, choice);

    TransportMode mode;
    std::string connectInfo;

    if (choice == "3") {
        // ===== 中继模式 =====
        mode = TransportMode::Relay;
        p2p::P2PClient::setLogLevel(2);

        std::string sigUrl = "ws://localhost:8080";
        std::cout << "信令服务器 URL (默认 " << sigUrl << "): ";
        std::string input; std::getline(std::cin, input);
        if (!input.empty()) sigUrl = input;

        std::string peerId;
        std::cout << "服务端 Peer ID: ";
        std::getline(std::cin, peerId);
        if (peerId.empty()) {
            std::cerr << "必须输入服务端 Peer ID" << std::endl;
            return 1;
        }

        std::string password;
        std::cout << "中继密码: ";
        std::getline(std::cin, password);
        if (password.empty()) {
            std::cerr << "必须输入中继密码" << std::endl;
            return 1;
        }

        auto* transport = new P2PTransport();
        g_transport = transport;

        std::cout << "\n正在通过中继连接..." << std::endl;
        if (!transport->connect(sigUrl, peerId, true, password)) {
            delete transport;
            g_transport = nullptr;
            WSACleanup();
            std::cout << "按回车键退出..." << std::endl;
            std::cin.get();
            return 1;
        }

        connectInfo = "Relay: " + peerId;

    } else if (choice == "2") {
        // ===== P2P 模式 =====
        mode = TransportMode::P2P;
        p2p::P2PClient::setLogLevel(2);

        std::string sigUrl = "ws://localhost:8080";
        std::cout << "信令服务器 URL (默认 " << sigUrl << "): ";
        std::string input; std::getline(std::cin, input);
        if (!input.empty()) sigUrl = input;

        std::string peerId;
        std::cout << "服务端 Peer ID: ";
        std::getline(std::cin, peerId);
        if (peerId.empty()) {
            std::cerr << "必须输入服务端 Peer ID" << std::endl;
            return 1;
        }

        auto* transport = new P2PTransport();
        g_transport = transport;

        std::cout << "\n正在 P2P 连接..." << std::endl;
        if (!transport->connect(sigUrl, peerId, false, "")) {
            delete transport;
            g_transport = nullptr;
            WSACleanup();
            std::cout << "按回车键退出..." << std::endl;
            std::cin.get();
            return 1;
        }

        connectInfo = "P2P: " + peerId;

    } else {
        // ===== TCP 模式 =====
        mode = TransportMode::TCP;

        std::string ip = "127.0.0.1";
        std::cout << "服务器 IP (默认 " << ip << "): ";
        std::string input; std::getline(std::cin, input);
        if (!input.empty()) ip = input;

        int port = 12345;
        std::cout << "端口 (默认 " << port << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) port = std::atoi(input.c_str());

        auto* tcp = new TCPTransport();
        g_transport = tcp;

        if (!tcp->connect(ip, port)) {
            delete tcp;
            g_transport = nullptr;
            WSACleanup();
            std::cout << "按回车键退出..." << std::endl;
            std::cin.get();
            return 1;
        }

        connectInfo = "TCP: " + ip + ":" + std::to_string(port);
    }

    std::cout << "连接成功！创建窗口..." << std::endl;

    // 创建窗口
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(0);
    wc.lpszClassName = "TriModeRemoteDesktop";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    RegisterClassA(&wc);

    WindowData wd;
    wd.hwndControl = nullptr;
    wd.mode = mode;
    wd.connectInfo = connectInfo;

    int ww = GetSystemMetrics(SM_CXSCREEN) * 3 / 4;
    int wh = GetSystemMetrics(SM_CYSCREEN) * 3 / 4;

    const char* title = nullptr;
    switch (mode) {
        case TransportMode::TCP:   title = "远程桌面 [TCP]";   break;
        case TransportMode::P2P:   title = "远程桌面 [P2P]";   break;
        case TransportMode::Relay: title = "远程桌面 [Relay]"; break;
    }

    HWND hwnd = CreateWindowExA(WS_EX_COMPOSITED, "TriModeRemoteDesktop", title,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
        nullptr, nullptr, GetModuleHandle(0), &wd);

    if (!hwnd) {
        std::cerr << "创建窗口失败" << std::endl;
        g_transport->disconnect();
        delete g_transport;
        WSACleanup();
        return 1;
    }

    g_hwndMain = hwnd;

    BOOL dt = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &dt, sizeof(dt));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // 消息循环
    MSG msg;
    while (GetMessage(&msg, 0, 0, 0) && clientRunning) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 清理
    g_hwndMain = nullptr;
    if (g_transport) {
        g_transport->disconnect();
        delete g_transport;
        g_transport = nullptr;
    }
    g_display.Cleanup();
    g_decoder.Cleanup();
    WSACleanup();

    std::cout << "客户端已退出" << std::endl;
    return 0;
}