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
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ==================== 配置 ====================
const int PORT = 12345;
const int CRF = 28;
const int FPS = 30;
const int KEYFRAME_INTERVAL = 120;
std::atomic<bool> serverRunning(true);

struct InputEvent {
    int type, x, y, key, flags;
};

#define MOUSE_MIDDLE_DOWN 5
#define MOUSE_MIDDLE_UP   6
#define MOUSE_WHEEL       7

enum PacketType : uint8_t {
    PACKET_VIDEO = 0,
    PACKET_KEYFRAME_REQUEST = 1,
    PACKET_DIMENSIONS = 2
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

// ==================== 轻量级屏幕捕获 ====================
class LiteScreenCapture {
private:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> stagingTexture;
    
    // GDI回退
    HDC hdcScreen = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    void* gdiBits = nullptr;
    
    uint8_t* frameBuffer = nullptr;  // 单一缓冲区
    int width = 0;
    int height = 0;
    bool useGDI = false;
    bool initialized = false;
    bool frameAcquired = false;

public:
    ~LiteScreenCapture() { Cleanup(); }
    
    void Cleanup() {
        if (frameAcquired && duplication.get()) {
            duplication->ReleaseFrame();
        }
        stagingTexture.reset();
        duplication.reset();
        context.reset();
        device.reset();
        
        if (hBitmap) DeleteObject(hBitmap);
        if (hdcMem) DeleteDC(hdcMem);
        if (hdcScreen) ReleaseDC(nullptr, hdcScreen);
        
        // 不要删除 gdiBits，它由CreateDIBSection管理
        // 也不要删除 frameBuffer（DXGI模式下它指向映射内存）
        if (useGDI) {
            // GDI模式下 frameBuffer 就是 gdiBits
        } else {
            delete[] frameBuffer;
        }
        frameBuffer = nullptr;
        
        hBitmap = nullptr;
        hdcMem = nullptr;
        hdcScreen = nullptr;
        initialized = false;
    }
    
    bool InitGDI() {
        hdcScreen = GetDC(nullptr);
        if (!hdcScreen) return false;
        
        hdcMem = CreateCompatibleDC(hdcScreen);
        if (!hdcMem) return false;
        
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
        
        useGDI = true;
        initialized = true;
        return true;
    }
    
    bool Init() {
        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &device, &featureLevel, &context
        );
        
        if (FAILED(hr)) return InitGDI();
        
        ComPtr<IDXGIDevice> dxgiDevice;
        hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (FAILED(hr)) return InitGDI();
        
        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) return InitGDI();
        
        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(0, &output);
        if (FAILED(hr)) return InitGDI();
        
        DXGI_OUTPUT_DESC outputDesc;
        output->GetDesc(&outputDesc);
        
        ComPtr<IDXGIOutput1> output1;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (FAILED(hr)) return InitGDI();
        
        hr = output1->DuplicateOutput(device.get(), &duplication);
        if (FAILED(hr)) return InitGDI();
        
        width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
        
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        
        hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
        if (FAILED(hr)) return InitGDI();
        
        // 只在DXGI模式下分配缓冲区
        frameBuffer = new uint8_t[static_cast<size_t>(width) * height * 4];
        
        useGDI = false;
        initialized = true;
        return true;
    }
    
    const uint8_t* Capture(bool& hasNewFrame) {
        hasNewFrame = false;
        if (!initialized) return nullptr;
        
        if (useGDI) {
            BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
            hasNewFrame = true;
            return frameBuffer;
        }
        
        if (frameAcquired) {
            duplication->ReleaseFrame();
            frameAcquired = false;
        }
        
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        ComPtr<IDXGIResource> resource;
        
        HRESULT hr = duplication->AcquireNextFrame(0, &frameInfo, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return frameBuffer;
        }
        if (FAILED(hr)) return frameBuffer;
        
        frameAcquired = true;
        hasNewFrame = true;
        
        ComPtr<ID3D11Texture2D> texture;
        hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
        if (FAILED(hr)) return frameBuffer;
        
        context->CopyResource(stagingTexture.get(), texture.get());
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            const int rowBytes = width * 4;
            if (mapped.RowPitch == static_cast<UINT>(rowBytes)) {
                memcpy(frameBuffer, mapped.pData, static_cast<size_t>(rowBytes) * height);
            } else {
                for (int y = 0; y < height; y++) {
                    memcpy(frameBuffer + y * rowBytes,
                           static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                           rowBytes);
                }
            }
            context->Unmap(stagingTexture.get(), 0);
        }
        
        return frameBuffer;
    }
    
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

// ==================== 低内存HEVC编码器 ====================
class LiteHEVCEncoder {
private:
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* swsCtx = nullptr;
    int width = 0;
    int height = 0;
    bool initialized = false;
    bool isHardware = false;
    std::mutex mtx;

public:
    ~LiteHEVCEncoder() { Cleanup(); }
    
    void Cleanup() {
        std::lock_guard<std::mutex> lock(mtx);
        if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }
        if (pkt) { av_packet_free(&pkt); pkt = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        if (ctx) { avcodec_free_context(&ctx); ctx = nullptr; }
        initialized = false;
    }
    
    bool Init(int w, int h, int fps, int crf) {
        std::lock_guard<std::mutex> lock(mtx);
        width = w;
        height = h;
        
        // 优先硬件编码器（内存占用低）
        const char* encoders[] = {"hevc_nvenc", "hevc_qsv", "hevc_amf", "libx265", nullptr};
        
        for (int i = 0; encoders[i]; i++) {
            codec = avcodec_find_encoder_by_name(encoders[i]);
            if (!codec) continue;
            
            ctx = avcodec_alloc_context3(codec);
            if (!ctx) continue;
            
            ctx->width = width;
            ctx->height = height;
            ctx->time_base = {1, fps};
            ctx->framerate = {fps, 1};
            ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            ctx->gop_size = KEYFRAME_INTERVAL;
            ctx->max_b_frames = 0;
            ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            
            if (strcmp(encoders[i], "hevc_nvenc") == 0) {
                av_opt_set(ctx->priv_data, "preset", "p1", 0);
                av_opt_set(ctx->priv_data, "tune", "ll", 0);
                av_opt_set(ctx->priv_data, "rc", "constqp", 0);
                av_opt_set_int(ctx->priv_data, "qp", crf, 0);
                isHardware = true;
            }
            else if (strcmp(encoders[i], "hevc_qsv") == 0) {
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
                ctx->global_quality = crf;
                isHardware = true;
            }
            else if (strcmp(encoders[i], "hevc_amf") == 0) {
                av_opt_set(ctx->priv_data, "quality", "speed", 0);
                av_opt_set(ctx->priv_data, "rc", "cqp", 0);
                av_opt_set_int(ctx->priv_data, "qp_i", crf, 0);
                av_opt_set_int(ctx->priv_data, "qp_p", crf, 0);
                isHardware = true;
            }
            else if (strcmp(encoders[i], "libx265") == 0) {
                // ========== 关键：最小化内存配置 ==========
                ctx->thread_count = 2;  // 减少线程
                
                char params[512];
                snprintf(params, sizeof(params),
                    "log-level=error:"
                    "pools=1:"              // 单线程池
                    "frame-threads=1:"      // 单帧线程
                    "lookahead-slices=1:"   // 最小lookahead
                    "rc-lookahead=0:"       // 禁用lookahead (大幅减少内存!)
                    "bframes=0:"            // 无B帧
                    "ref=1:"                // 单参考帧 (减少50%+内存!)
                    "no-wpp=1:"             // 禁用WPP
                    "no-pmode=1:"           // 禁用并行模式
                    "no-pme=1:"             // 禁用并行运动估计
                    "no-sao=1:"             // 禁用SAO
                    "no-weightp=1:"         // 禁用加权预测
                    "no-cutree=1:"          // 禁用cutree
                    "aq-mode=0:"            // 禁用自适应量化
                    "crf=%d",
                    crf + 3
                );
                
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->priv_data, "x265-params", params, 0);
                isHardware = false;
            }
            
            if (avcodec_open2(ctx, codec, nullptr) >= 0) {
                std::cout << "编码器: " << encoders[i];
                if (isHardware) std::cout << " [硬件]";
                std::cout << "\n";
                break;
            }
            
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        
        if (!ctx) {
            std::cerr << "无可用编码器\n";
            return false;
        }
        
        frame = av_frame_alloc();
        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) return false;
        
        pkt = av_packet_alloc();
        
        // 使用最快的缩放算法
        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_BGRA,
            width, height, AV_PIX_FMT_YUV420P,
            SWS_POINT, nullptr, nullptr, nullptr
        );
        
        initialized = true;
        return true;
    }
    
    bool Encode(const uint8_t* bgra, int64_t pts, std::vector<uint8_t>& out, bool keyframe = false) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!initialized) return false;
        
        out.clear();
        
        if (av_frame_make_writable(frame) < 0) return false;
        
        const uint8_t* src[1] = {bgra};
        int srcStride[1] = {width * 4};
        sws_scale(swsCtx, src, srcStride, 0, height, frame->data, frame->linesize);
        
        frame->pts = pts;
        frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        
        if (avcodec_send_frame(ctx, frame) < 0) return false;
        
        int ret;
        while ((ret = avcodec_receive_packet(ctx, pkt)) >= 0) {
            out.insert(out.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
        
        return !out.empty();
    }
    
    bool IsHardware() const { return isHardware; }
};

// ==================== 网络函数 ====================
inline bool SendAll(SOCKET s, const void* data, int len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int sent = send(s, p, len, 0);
        if (sent <= 0) return false;
        p += sent;
        len -= sent;
    }
    return true;
}

inline bool RecvAll(SOCKET s, void* buf, int len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        int r = recv(s, p, len, 0);
        if (r <= 0) return false;
        p += r;
        len -= r;
    }
    return true;
}

// ==================== 输入处理 ====================
void InputHandler(SOCKET sock, std::atomic<bool>& running) {
    INPUT input = {};
    while (running && serverRunning) {
        InputEvent ev;
        if (!RecvAll(sock, &ev, sizeof(ev))) break;
        
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
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = static_cast<WORD>(ev.key);
            input.ki.dwFlags = ev.flags ? KEYEVENTF_KEYUP : 0;
            SendInput(1, &input, sizeof(INPUT));
        }
    }
}

// ==================== 客户端处理 ====================
void ClientHandler(SOCKET sock) {
    std::cout << "客户端连接\n";
    std::atomic<bool> running(true);
    
    LiteScreenCapture capture;
    if (!capture.Init()) {
        closesocket(sock);
        return;
    }
    
    int w = capture.GetWidth(), h = capture.GetHeight();
    std::cout << "分辨率: " << w << "x" << h << "\n";
    
    int dims[2] = {w, h};
    if (!SendAll(sock, dims, sizeof(dims))) {
        closesocket(sock);
        return;
    }
    
    LiteHEVCEncoder encoder;
    if (!encoder.Init(w, h, FPS, CRF)) {
        closesocket(sock);
        return;
    }
    
    std::thread inputThread(InputHandler, sock, std::ref(running));
    
    // 预分配，但使用较小容量
    std::vector<uint8_t> encoded;
    encoded.reserve(64 * 1024);  // 64KB初始
    
    const DWORD frameMs = 1000 / FPS;
    int64_t pts = 0;
    
    while (serverRunning && running) {
        DWORD t0 = GetTickCount();
        
        bool hasNew = false;
        const uint8_t* data = capture.Capture(hasNew);
        if (!data) { Sleep(1); continue; }
        
        bool keyframe = (pts % KEYFRAME_INTERVAL == 0);
        
        if (encoder.Encode(data, pts, encoded, keyframe)) {
            uint8_t hdr[6];
            hdr[0] = PACKET_VIDEO;
            hdr[1] = keyframe ? 1 : 0;
            *reinterpret_cast<int32_t*>(&hdr[2]) = static_cast<int32_t>(encoded.size());
            
            if (!SendAll(sock, hdr, 6) || !SendAll(sock, encoded.data(), (int)encoded.size())) {
                break;
            }
        }
        
        pts++;
        
        DWORD elapsed = GetTickCount() - t0;
        if (elapsed < frameMs) Sleep(frameMs - elapsed);
    }
    
    running = false;
    if (inputThread.joinable()) inputThread.join();
    closesocket(sock);
    std::cout << "客户端断开\n";
}

// ==================== 主函数 ====================
int main() {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 5);
    
    std::cout << "================================\n";
    std::cout << "  HEVC远程桌面 (低内存版)\n";
    std::cout << "================================\n";
    std::cout << "端口: " << PORT << " CRF: " << CRF << " FPS: " << FPS << "\n";
    std::cout << "等待连接...\n";
    
    while (serverRunning) {
        sockaddr_in caddr;
        int clen = sizeof(caddr);
        SOCKET client = accept(srv, (sockaddr*)&caddr, &clen);
        if (client == INVALID_SOCKET) break;
        
        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        
        std::thread(ClientHandler, client).detach();
    }
    
    closesocket(srv);
    WSACleanup();
    return 0;
}