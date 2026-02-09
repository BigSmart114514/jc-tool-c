#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <cstdio>
#include <timeapi.h>
#include <p2p/p2p_client.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "p2p-client.lib")

// ==================== 配置 ====================
const int TCP_PORT = 12345;
const char* DEFAULT_SIGNALING_URL = "ws://localhost:8080";
const int CRF = 28;
const int FPS = 30;
const int KEYFRAME_INTERVAL = 120;

std::atomic<bool> serverRunning(true);

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

// ==================== 传输模式 ====================
enum class TransportMode { None, TCP, P2P };

// ==================== 线程安全输入队列 ====================
class InputQueue {
    std::queue<InputEvent> queue_;
    std::mutex mtx_;
public:
    void push(const InputEvent& ev) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push(ev);
    }
    bool pop(InputEvent& ev) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return false;
        ev = queue_.front();
        queue_.pop();
        return true;
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        while (!queue_.empty()) queue_.pop();
    }
};

// ==================== COM智能指针 ====================
template<typename T>
class ComPtr {
    T* ptr = nullptr;
public:
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** operator&() { return &ptr; }
    T* operator->() { return ptr; }
    T* get() { return ptr; }
    void reset() { if (ptr) { ptr->Release(); ptr = nullptr; } }
    explicit operator bool() const { return ptr != nullptr; }
};

// ==================== 屏幕捕获 ====================
class LiteScreenCapture {
private:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> stagingTexture;
    HDC hdcScreen = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    void* gdiBits = nullptr;
    uint8_t* frameBuffer = nullptr;
    int width = 0, height = 0;
    bool useGDI = false, initialized = false, frameAcquired = false;

public:
    ~LiteScreenCapture() { Cleanup(); }

    void Cleanup() {
        if (frameAcquired && duplication.get()) { duplication->ReleaseFrame(); frameAcquired = false; }
        stagingTexture.reset(); duplication.reset(); context.reset(); device.reset();
        if (hBitmap) DeleteObject(hBitmap);
        if (hdcMem) DeleteDC(hdcMem);
        if (hdcScreen) ReleaseDC(nullptr, hdcScreen);
        if (!useGDI && frameBuffer) delete[] frameBuffer;
        frameBuffer = nullptr; initialized = false;
    }

    bool InitGDI() {
        hdcScreen = GetDC(nullptr);
        hdcMem = CreateCompatibleDC(hdcScreen);
        width = GetDeviceCaps(hdcScreen, DESKTOPHORZRES);
        height = GetDeviceCaps(hdcScreen, DESKTOPVERTRES);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &gdiBits, nullptr, 0);
        if (!hBitmap) return false;
        SelectObject(hdcMem, hBitmap);
        frameBuffer = static_cast<uint8_t*>(gdiBits);
        useGDI = true; initialized = true;
        return true;
    }

    bool Init() {
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                      nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context)))
            return InitGDI();
        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return InitGDI();
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(&adapter))) return InitGDI();
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, &output))) return InitGDI();
        DXGI_OUTPUT_DESC desc; output->GetDesc(&desc);
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) return InitGDI();
        if (FAILED(output1->DuplicateOutput(device.get(), &duplication))) return InitGDI();
        width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &stagingTexture))) return InitGDI();
        frameBuffer = new uint8_t[size_t(width) * height * 4];
        useGDI = false; initialized = true;
        return true;
    }

    const uint8_t* Capture(bool& hasNew) {
        hasNew = false;
        if (!initialized) return nullptr;
        if (useGDI) {
            BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
            hasNew = true; return frameBuffer;
        }
        if (frameAcquired) { duplication->ReleaseFrame(); frameAcquired = false; }
        DXGI_OUTDUPL_FRAME_INFO fi; ComPtr<IDXGIResource> res;
        HRESULT hr = duplication->AcquireNextFrame(0, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return frameBuffer;
        if (FAILED(hr)) return frameBuffer;
        frameAcquired = true; hasNew = true;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) return frameBuffer;
        context->CopyResource(stagingTexture.get(), tex.get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            if (mapped.RowPitch == width * 4)
                memcpy(frameBuffer, mapped.pData, size_t(width) * height * 4);
            else
                for (int y = 0; y < height; y++)
                    memcpy(frameBuffer + y * width * 4,
                           (uint8_t*)mapped.pData + y * mapped.RowPitch, width * 4);
            context->Unmap(stagingTexture.get(), 0);
        }
        return frameBuffer;
    }

    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

// ==================== HEVC编码器 ====================
class LiteHEVCEncoder {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* swsCtx = nullptr;
    int width = 0, height = 0;
    bool initialized = false;
    std::mutex mtx;
public:
    ~LiteHEVCEncoder() { Cleanup(); }

    void Cleanup() {
        std::lock_guard<std::mutex> lock(mtx);
        if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
        if (pkt) { av_packet_free(&pkt); }
        if (frame) { av_frame_free(&frame); }
        if (ctx) { avcodec_free_context(&ctx); }
        initialized = false;
    }

    bool Init(int w, int h, int fps, int crf) {
        std::lock_guard<std::mutex> lock(mtx);
        width = w; height = h;
        const char* encoders[] = {"hevc_nvenc", "hevc_qsv", "hevc_amf", "libx265", nullptr};
        for (int i = 0; encoders[i]; i++) {
            codec = avcodec_find_encoder_by_name(encoders[i]);
            if (!codec) continue;
            ctx = avcodec_alloc_context3(codec);
            if (!ctx) continue;
            ctx->width = width; ctx->height = height;
            ctx->time_base = {1, fps}; ctx->framerate = {fps, 1};
            ctx->pix_fmt = (strstr(encoders[i], "qsv") || strstr(encoders[i], "amf"))
                           ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
            ctx->gop_size = KEYFRAME_INTERVAL; ctx->max_b_frames = 0;
            ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

            if (strcmp(encoders[i], "hevc_nvenc") == 0) {
                av_opt_set(ctx->priv_data, "preset", "p1", 0);
                av_opt_set(ctx->priv_data, "tune", "ll", 0);
                av_opt_set(ctx->priv_data, "rc", "constqp", 0);
                av_opt_set_int(ctx->priv_data, "qp", crf, 0);
                av_opt_set(ctx->priv_data, "delay", "0", 0);
            } else if (strcmp(encoders[i], "hevc_qsv") == 0) {
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
                av_opt_set(ctx->priv_data, "async_depth", "1", 0);
                ctx->global_quality = crf;
            } else if (strcmp(encoders[i], "hevc_amf") == 0) {
                av_opt_set(ctx->priv_data, "quality", "speed", 0);
                av_opt_set(ctx->priv_data, "rc", "cqp", 0);
                av_opt_set_int(ctx->priv_data, "qp_i", crf, 0);
                av_opt_set_int(ctx->priv_data, "qp_p", crf, 0);
            } else {
                ctx->thread_count = 2;
                char params[512];
                snprintf(params, sizeof(params),
                    "log-level=error:pools=1:frame-threads=1:lookahead-slices=1:rc-lookahead=0:"
                    "bframes=0:ref=1:no-wpp=1:no-pmode=1:no-pme=1:no-sao=1:no-weightp=1:"
                    "no-cutree=1:aq-mode=0:crf=%d", crf + 3);
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->priv_data, "x265-params", params, 0);
            }
            if (avcodec_open2(ctx, codec, nullptr) >= 0) {
                std::cout << "编码器: " << encoders[i] << std::endl;
                break;
            }
            avcodec_free_context(&ctx); ctx = nullptr;
        }
        if (!ctx) { std::cerr << "未找到可用HEVC编码器\n"; return false; }
        frame = av_frame_alloc();
        frame->format = ctx->pix_fmt; frame->width = width; frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) return false;
        pkt = av_packet_alloc();
        swsCtx = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                width, height, ctx->pix_fmt, SWS_POINT, nullptr, nullptr, nullptr);
        initialized = true;
        return true;
    }

    bool Encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& out, bool keyframe = false) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!initialized) return false;
        out.clear();
        if (av_frame_make_writable(frame) < 0) return false;
        const uint8_t* src[1] = {bgra}; int srcStride[1] = {width * 4};
        sws_scale(swsCtx, src, srcStride, 0, height, frame->data, frame->linesize);
        frame->pts = pts;
        frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (keyframe) frame->flags |= AV_FRAME_FLAG_KEY;
        int ret = avcodec_send_frame(ctx, frame);
        if (ret < 0) return false;
        while (ret >= 0) {
            ret = avcodec_receive_packet(ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) return false;
            out.insert(out.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
        return true;
    }
};

// ==================== 网络工具 ====================
inline bool SendAll(SOCKET s, const void* data, int len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int sent = send(s, p, len, 0);
        if (sent <= 0) return false;
        p += sent; len -= sent;
    }
    return true;
}

inline bool RecvAll(SOCKET s, void* buf, int len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        int r = recv(s, p, len, 0);
        if (r <= 0) return false;
        p += r; len -= r;
    }
    return true;
}

// ==================== 服务端核心 ====================
class RemoteDesktopServer {
private:
    // 共享资源
    LiteScreenCapture capture_;
    LiteHEVCEncoder encoder_;
    InputQueue inputQueue_;

    // 传输状态
    TransportMode activeMode_ = TransportMode::None;
    std::mutex transportMtx_;
    std::condition_variable clientCV_;

    // TCP 状态
    SOCKET tcpServerSocket_ = INVALID_SOCKET;
    SOCKET tcpClientSocket_ = INVALID_SOCKET;
    std::thread tcpListenThread_;
    std::thread tcpInputThread_;
    std::atomic<bool> tcpClientActive_{false};

    // P2P 状态
    std::unique_ptr<p2p::P2PClient> p2pClient_;
    std::string p2pPeerId_;
    std::atomic<bool> p2pClientReady_{false};
    std::atomic<bool> keyframeRequested_{false};

    int screenW_ = 0, screenH_ = 0;

public:
    ~RemoteDesktopServer() { stop(); }

    bool init() {
        if (!capture_.Init()) {
            std::cerr << "屏幕捕获初始化失败" << std::endl;
            return false;
        }
        screenW_ = capture_.GetWidth();
        screenH_ = capture_.GetHeight();
        std::cout << "分辨率: " << screenW_ << "x" << screenH_ << std::endl;

        if (!encoder_.Init(screenW_, screenH_, FPS, CRF)) {
            std::cerr << "编码器初始化失败" << std::endl;
            return false;
        }
        return true;
    }

    // ========== TCP ==========
    bool startTCP(int port) {
        tcpServerSocket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcpServerSocket_ == INVALID_SOCKET) return false;

        int opt = 1;
        setsockopt(tcpServerSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(tcpServerSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "TCP bind 失败" << std::endl;
            return false;
        }
        listen(tcpServerSocket_, 2);
        std::cout << "TCP 监听端口: " << port << std::endl;

        tcpListenThread_ = std::thread([this]() { tcpListenLoop(); });
        return true;
    }

    void tcpListenLoop() {
        while (serverRunning) {
            sockaddr_in caddr;
            int clen = sizeof(caddr);
            SOCKET client = accept(tcpServerSocket_, (sockaddr*)&caddr, &clen);
            if (client == INVALID_SOCKET) break;

            // 检查是否已有连接
            {
                std::lock_guard<std::mutex> lock(transportMtx_);
                if (activeMode_ != TransportMode::None) {
                    std::cout << "已有客户端连接，拒绝 TCP 新连接" << std::endl;
                    closesocket(client);
                    continue;
                }
            }

            int flag = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

            // 发送分辨率
            int dims[2] = {screenW_, screenH_};
            if (!SendAll(client, dims, sizeof(dims))) {
                closesocket(client);
                continue;
            }

            std::cout << "TCP 客户端已连接" << std::endl;

            // 激活 TCP 传输
            {
                std::lock_guard<std::mutex> lock(transportMtx_);
                tcpClientSocket_ = client;
                tcpClientActive_ = true;
                activeMode_ = TransportMode::TCP;
            }
            clientCV_.notify_one();

            // 启动 TCP 输入接收线程
            tcpInputThread_ = std::thread([this]() { tcpInputLoop(); });

            // 等待 TCP 客户端断开
            if (tcpInputThread_.joinable()) tcpInputThread_.join();

            // 清理
            {
                std::lock_guard<std::mutex> lock(transportMtx_);
                if (activeMode_ == TransportMode::TCP) {
                    activeMode_ = TransportMode::None;
                }
                tcpClientActive_ = false;
                shutdown(tcpClientSocket_, SD_BOTH);
                closesocket(tcpClientSocket_);
                tcpClientSocket_ = INVALID_SOCKET;
            }
            inputQueue_.clear();
            std::cout << "TCP 客户端断开" << std::endl;
        }
    }

    void tcpInputLoop() {
        InputEvent ev;
        while (serverRunning && tcpClientActive_) {
            if (!RecvAll(tcpClientSocket_, &ev, sizeof(ev))) {
                tcpClientActive_ = false;
                break;
            }
            inputQueue_.push(ev);
        }
    }

    // ========== P2P ==========
    bool startP2P(const std::string& signalingUrl, const std::string& peerId = "") {
        p2p::ClientConfig config;
        config.signalingUrl = signalingUrl;
        config.peerId = peerId;

        p2pClient_ = std::make_unique<p2p::P2PClient>(config);

        p2pClient_->setOnConnected([this]() {
            std::cout << "P2P 已连接信令服务器, ID: " << p2pClient_->getLocalId() << std::endl;
        });

        p2pClient_->setOnDisconnected([](const p2p::Error& err) {
            std::cerr << "P2P 信令断开: " << err.message << std::endl;
        });

        p2pClient_->setOnPeerConnected([this](const std::string& peerId) {
            std::lock_guard<std::mutex> lock(transportMtx_);
            if (activeMode_ != TransportMode::None) {
                std::cout << "已有客户端连接，拒绝 P2P: " << peerId << std::endl;
                p2pClient_->disconnectFromPeer(peerId);
                return;
            }
            p2pPeerId_ = peerId;
            std::cout << "P2P 客户端连接: " << peerId << std::endl;

            // 发送屏幕信息
            std::string json = "{\"type\":\"screen_info\",\"width\":" +
                              std::to_string(screenW_) + ",\"height\":" +
                              std::to_string(screenH_) + "}";
            p2pClient_->sendText(peerId, json);
        });

        p2pClient_->setOnPeerDisconnected([this](const std::string& peerId) {
            std::lock_guard<std::mutex> lock(transportMtx_);
            if (peerId == p2pPeerId_ && activeMode_ == TransportMode::P2P) {
                std::cout << "P2P 客户端断开: " << peerId << std::endl;
                activeMode_ = TransportMode::None;
                p2pClientReady_ = false;
                p2pPeerId_.clear();
                inputQueue_.clear();
            }
        });

        p2pClient_->setOnTextMessage([this](const std::string& from, const std::string& msg) {
            if (msg.find("\"type\":\"ready\"") != std::string::npos) {
                std::lock_guard<std::mutex> lock(transportMtx_);
                if (from == p2pPeerId_) {
                    activeMode_ = TransportMode::P2P;
                    p2pClientReady_ = true;
                    std::cout << "P2P 客户端准备就绪" << std::endl;
                    clientCV_.notify_one();
                }
            } else if (msg.find("\"type\":\"keyframe_request\"") != std::string::npos) {
                keyframeRequested_ = true;
            }
        });

        p2pClient_->setOnBinaryMessage([this](const std::string& from, const p2p::BinaryData& data) {
            if (data.size() >= 1 + sizeof(InputEvent) &&
                data[0] == static_cast<uint8_t>(BinaryMsgType::InputEvent)) {
                InputEvent ev;
                memcpy(&ev, data.data() + 1, sizeof(InputEvent));
                inputQueue_.push(ev);
            }
        });

        p2pClient_->setOnError([](const p2p::Error& err) {
            std::cerr << "P2P 错误: " << err.message << std::endl;
        });

        if (!p2pClient_->connect()) {
            std::cerr << "P2P 连接信令服务器失败" << std::endl;
            return false;
        }

        return true;
    }

    // ========== 统一发送 ==========
    bool sendVideo(const std::vector<uint8_t>& encoded, bool isKeyframe) {
        std::lock_guard<std::mutex> lock(transportMtx_);

        if (activeMode_ == TransportMode::TCP) {
            uint8_t hdr[6];
            hdr[0] = PACKET_VIDEO;
            hdr[1] = isKeyframe ? 1 : 0;
            *reinterpret_cast<int32_t*>(&hdr[2]) = static_cast<int32_t>(encoded.size());

            if (!SendAll(tcpClientSocket_, hdr, 6) ||
                !SendAll(tcpClientSocket_, encoded.data(), (int)encoded.size())) {
                tcpClientActive_ = false;
                return false;
            }
            return true;
        }
        else if (activeMode_ == TransportMode::P2P) {
            p2p::BinaryData packet;
            packet.reserve(2 + encoded.size());
            packet.push_back(static_cast<uint8_t>(BinaryMsgType::VideoFrame));
            packet.push_back(isKeyframe ? 1 : 0);
            packet.insert(packet.end(), encoded.begin(), encoded.end());

            return p2pClient_->sendBinary(p2pPeerId_, packet);
        }

        return false;
    }

    // ========== 输入注入 ==========
    void processInput() {
        InputEvent ev;
        INPUT input = {};
        while (inputQueue_.pop(ev)) {
            if (ev.type == 0) {
                SetCursorPos(ev.x, ev.y);
                switch (ev.key) {
                    case 1: mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); break;
                    case 2: mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); break;
                    case 3: mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0); break;
                    case 4: mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0); break;
                    case MOUSE_MIDDLE_DOWN: mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, 0); break;
                    case MOUSE_MIDDLE_UP: mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0); break;
                    case MOUSE_WHEEL: mouse_event(MOUSEEVENTF_WHEEL, 0, 0, ev.flags, 0); break;
                }
            } else {
                ZeroMemory(&input, sizeof(INPUT));
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = static_cast<WORD>(ev.key);
                input.ki.dwFlags = ev.flags ? KEYEVENTF_KEYUP : 0;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
    }

    // ========== 主循环 ==========
    void run() {
        std::vector<uint8_t> encoded;
        encoded.reserve(256 * 1024);
        const DWORD frameMs = 1000 / FPS;
        int64_t pts = 0;

        while (serverRunning) {
            // 等待客户端连接
            {
                std::unique_lock<std::mutex> lock(transportMtx_);
                clientCV_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                    return activeMode_ != TransportMode::None || !serverRunning;
                });
                if (!serverRunning) break;
                if (activeMode_ == TransportMode::None) continue;
            }

            DWORD t0 = GetTickCount();

            processInput();

            bool hasNew = false;
            const uint8_t* data = capture_.Capture(hasNew);
            if (!data) { Sleep(10); continue; }

            bool keyframe = (pts % KEYFRAME_INTERVAL == 0) || keyframeRequested_.exchange(false);

            if (encoder_.Encode(data, pts, encoded, keyframe)) {
                if (!encoded.empty()) {
                    if (!sendVideo(encoded, keyframe)) {
                        // 发送失败，连接已断开
                    }
                }
            }

            pts++;
            DWORD elapsed = GetTickCount() - t0;
            if (elapsed < frameMs) Sleep(frameMs - elapsed);
        }
    }

    void stop() {
        serverRunning = false;

        // 关闭 TCP
        if (tcpServerSocket_ != INVALID_SOCKET) {
            closesocket(tcpServerSocket_);
            tcpServerSocket_ = INVALID_SOCKET;
        }
        if (tcpClientSocket_ != INVALID_SOCKET) {
            shutdown(tcpClientSocket_, SD_BOTH);
            closesocket(tcpClientSocket_);
            tcpClientSocket_ = INVALID_SOCKET;
        }
        if (tcpListenThread_.joinable()) tcpListenThread_.join();
        if (tcpInputThread_.joinable()) tcpInputThread_.join();

        // 关闭 P2P
        if (p2pClient_) {
            p2pClient_->disconnect();
            p2pClient_.reset();
        }

        clientCV_.notify_all();
    }
};

// ==================== 主函数 ====================
int main() {
    SetConsoleOutputCP(65001);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    std::cout << "========================================\n";
    std::cout << "   双模式 HEVC 远程桌面服务端\n";
    std::cout << "   (同时支持 TCP 直连 + P2P)\n";
    std::cout << "========================================\n\n";

    p2p::P2PClient::setLogLevel(2);

    RemoteDesktopServer server;

    if (!server.init()) {
        timeEndPeriod(1);
        return 1;
    }


    WSADATA wsaData;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        std::cerr << "WSAStartup 失败: " << wsaResult << std::endl;
        return 1;
    }
    // -------------------------

    // 启动 TCP 监听
    std::cout << "\n[TCP 模式]\n";
    int tcpPort = TCP_PORT;
    std::cout << "TCP 端口 (默认 " << tcpPort << "): ";
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty()) tcpPort = std::atoi(input.c_str());

    bool tcpOk = server.startTCP(tcpPort);

    // 启动 P2P
    std::cout << "\n[P2P 模式]\n";
    std::string sigUrl = DEFAULT_SIGNALING_URL;
    std::cout << "信令服务器 URL (默认 " << sigUrl << ", 输入 skip 跳过): ";
    std::getline(std::cin, input);

    bool p2pOk = false;
    if (input != "skip") {
        if (!input.empty()) sigUrl = input;

        std::string peerId;
        std::cout << "服务端 Peer ID (留空自动生成): ";
        std::getline(std::cin, peerId);

        p2pOk = server.startP2P(sigUrl, peerId);
    }

    if (!tcpOk && !p2pOk) {
        std::cerr << "\nTCP 和 P2P 均启动失败" << std::endl;
        timeEndPeriod(1);
        return 1;
    }

    std::cout << "\n========================================\n";
    if (tcpOk) std::cout << "  TCP 就绪: 端口 " << tcpPort << "\n";
    if (p2pOk) std::cout << "  P2P 就绪: 等待 Peer 连接\n";
    std::cout << "========================================\n";
    std::cout << "按 Ctrl+C 停止\n\n";

    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
            serverRunning = false;
            return TRUE;
        }
        return FALSE;
    }, TRUE);

    server.run();
    server.stop();

    timeEndPeriod(1);

    // 清理 Winsock ---
    WSACleanup();
    // -------------------------
    std::cout << "服务端已停止" << std::endl;
    return 0;
}