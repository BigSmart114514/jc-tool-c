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
#include <timeapi.h>

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

// ==================== 配置 ====================
const int PORT = 12345;
const int CRF = 28;
const int FPS = 30;
const int KEYFRAME_INTERVAL = 120; // GOP size
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
    
    HDC hdcScreen = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    void* gdiBits = nullptr;
    
    uint8_t* frameBuffer = nullptr;
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
            frameAcquired = false;
        }
        stagingTexture.reset();
        duplication.reset();
        context.reset();
        device.reset();
        
        if (hBitmap) DeleteObject(hBitmap);
        if (hdcMem) DeleteDC(hdcMem);
        if (hdcScreen) ReleaseDC(nullptr, hdcScreen);
        
        if (!useGDI && frameBuffer) {
            delete[] frameBuffer;
        }
        frameBuffer = nullptr;
        initialized = false;
    }
    
    bool InitGDI() {
        hdcScreen = GetDC(nullptr);
        hdcMem = CreateCompatibleDC(hdcScreen);
        width = GetDeviceCaps(hdcScreen, DESKTOPHORZRES);
        height = GetDeviceCaps(hdcScreen, DESKTOPVERTRES);
        
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // Top-down
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
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
        if (FAILED(hr)) return InitGDI();
        
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) return InitGDI();
        
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return InitGDI();
        
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, &output))) return InitGDI();
        
        DXGI_OUTPUT_DESC outputDesc;
        output->GetDesc(&outputDesc);
        
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) return InitGDI();
        
        if (FAILED(output1->DuplicateOutput(device.get(), &duplication))) return InitGDI();
        
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
        
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture))) return InitGDI();
        
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
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return frameBuffer;
        if (FAILED(hr)) {
            // 丢失设备等情况，尝试重置或直接返回旧帧
            return frameBuffer;
        }
        
        frameAcquired = true;
        if (frameInfo.LastPresentTime.QuadPart == 0) {
            // 只有鼠标移动，图像未变
             // hasNewFrame = false; // 可选：如果希望严格只有画面变化才编码，设为false
             hasNewFrame = true; // 保持流畅度，继续返回当前帧
        } else {
            hasNewFrame = true;
        }
        
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture))) return frameBuffer;
        
        context->CopyResource(stagingTexture.get(), texture.get());
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            if (mapped.RowPitch == width * 4) {
                memcpy(frameBuffer, mapped.pData, static_cast<size_t>(width) * height * 4);
            } else {
                for (int y = 0; y < height; y++) {
                    memcpy(frameBuffer + y * width * 4,
                           static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                           width * 4);
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
            
            // ========== 关键修复：动态选择像素格式 ==========
            // QSV 和 AMF 通常需要 NV12，而 x265 偏好 YUV420P
            if (strstr(encoders[i], "qsv") || strstr(encoders[i], "amf")) {
                ctx->pix_fmt = AV_PIX_FMT_NV12; 
            } else {
                ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            }
            
            ctx->gop_size = KEYFRAME_INTERVAL;
            ctx->max_b_frames = 0;
            ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
            
            bool hw_config_success = true;

            if (strcmp(encoders[i], "hevc_nvenc") == 0) {
                av_opt_set(ctx->priv_data, "preset", "p1", 0); // fastest
                av_opt_set(ctx->priv_data, "tune", "ll", 0);
                av_opt_set(ctx->priv_data, "rc", "constqp", 0);
                av_opt_set_int(ctx->priv_data, "qp", crf, 0);
                av_opt_set(ctx->priv_data, "delay", "0", 0);
                isHardware = true;
            }
            else if (strcmp(encoders[i], "hevc_qsv") == 0) {
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
                av_opt_set(ctx->priv_data, "async_depth", "1", 0); // 降低延迟
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
                ctx->thread_count = 2;
                char params[512];
                snprintf(params, sizeof(params),
                    "log-level=error:pools=1:frame-threads=1:lookahead-slices=1:rc-lookahead=0:"
                    "bframes=0:ref=1:no-wpp=1:no-pmode=1:no-pme=1:no-sao=1:no-weightp=1:no-cutree=1:"
                    "aq-mode=0:crf=%d", crf + 3);
                av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->priv_data, "x265-params", params, 0);
                isHardware = false;
            }
            
            if (avcodec_open2(ctx, codec, nullptr) >= 0) {
                std::cout << "已选择编码器: " << encoders[i] 
                          << " (Format: " << av_get_pix_fmt_name(ctx->pix_fmt) << ")" << std::endl;
                break;
            } else {
                // 如果打开失败，打印一下原因（可选）
                // char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
                // std::cerr << "打开 " << encoders[i] << " 失败" << std::endl;
            }
            
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        
        if (!ctx) {
            std::cerr << "错误：未找到可用的HEVC编码器\n";
            return false;
        }
        
        frame = av_frame_alloc();
        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) return false;
        
        pkt = av_packet_alloc();
        
        // 根据编码器要求的格式初始化 SWS
        // 源格式是 BGRA (Windows DXGI/GDI 标准)
        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_BGRA,
            width, height, ctx->pix_fmt,
            SWS_POINT, nullptr, nullptr, nullptr // SWS_POINT 最快，但质量一般。SWS_BILINEAR 较平衡
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
        
        // 执行格式转换
        sws_scale(swsCtx, src, srcStride, 0, height, frame->data, frame->linesize);
        
        frame->pts = pts;
        
        // ========== 修正点开始 ==========
        // 设置图片类型为 I 帧即可，不需要再设置 frame->key_frame
        frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        
        // 如果你使用的是极新的 FFmpeg，想显式设置标志位（可选，通常不需要）：
        if (keyframe) {
            frame->flags |= AV_FRAME_FLAG_KEY; 
        }
        // ========== 修正点结束 ==========

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

// ==================== 网络函数 ====================
inline bool SendAll(SOCKET s, const void* data, int len) {
    const char* p = static_cast<const char*>(data);
    while (len > 0) {
        int sent = send(s, p, len, 0);
        if (sent <= 0) return false; // Error or disconnect
        p += sent;
        len -= sent;
    }
    return true;
}

inline bool RecvAll(SOCKET s, void* buf, int len) {
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        int r = recv(s, p, len, 0);
        if (r <= 0) return false; // Error or disconnect
        p += r;
        len -= r;
    }
    return true;
}

// ==================== 输入处理 ====================
void InputHandler(SOCKET sock, std::atomic<bool>& running) {
    INPUT input = {};
    InputEvent ev;
    
    while (running && serverRunning) {
        // RecvAll 是阻塞的。如果客户端断开，它返回 false。
        if (!RecvAll(sock, &ev, sizeof(ev))) {
            running = false; // 通知主线程停止
            break;
        }
        
        if (ev.type == 0) { // 鼠标
            // 简单处理：将客户端坐标映射到屏幕
            // 注意：实际生产中应处理多显示器坐标映射
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
        } else { // 键盘
            ZeroMemory(&input, sizeof(INPUT));
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = static_cast<WORD>(ev.key);
            input.ki.dwFlags = ev.flags ? KEYEVENTF_KEYUP : 0;
            SendInput(1, &input, sizeof(INPUT));
        }
    }
}

// ==================== 客户端处理 ====================
void ClientHandler(SOCKET sock) {
    std::cout << "客户端已连接" << std::endl;
    std::atomic<bool> running(true);
    
    LiteScreenCapture capture;
    if (!capture.Init()) {
        std::cerr << "屏幕捕获初始化失败" << std::endl;
        closesocket(sock);
        return;
    }
    
    int w = capture.GetWidth(), h = capture.GetHeight();
    std::cout << "分辨率: " << w << "x" << h << std::endl;
    
    // 发送分辨率
    int dims[2] = {w, h};
    if (!SendAll(sock, dims, sizeof(dims))) {
        closesocket(sock);
        return;
    }
    
    LiteHEVCEncoder encoder;
    if (!encoder.Init(w, h, FPS, CRF)) {
        std::cerr << "编码器初始化失败" << std::endl;
        closesocket(sock);
        return;
    }
    
    // 启动输入线程
    std::thread inputThread(InputHandler, sock, std::ref(running));
    
    std::vector<uint8_t> encoded;
    encoded.reserve(256 * 1024);
    
    const DWORD frameMs = 1000 / FPS;
    int64_t pts = 0;
    
    while (serverRunning && running) {
        DWORD t0 = GetTickCount();
        
        bool hasNew = false;
        const uint8_t* data = capture.Capture(hasNew);
        
        if (!data) { 
            Sleep(10); 
            continue; 
        }
        
        // 关键帧策略
        bool keyframe = (pts % KEYFRAME_INTERVAL == 0);
        
        if (encoder.Encode(data, pts, encoded, keyframe)) {
            if (!encoded.empty()) {
                uint8_t hdr[6];
                hdr[0] = PACKET_VIDEO;
                hdr[1] = keyframe ? 1 : 0;
                *reinterpret_cast<int32_t*>(&hdr[2]) = static_cast<int32_t>(encoded.size());
                
                // 连续发送头和体
                if (!SendAll(sock, hdr, 6) || !SendAll(sock, encoded.data(), (int)encoded.size())) {
                    std::cerr << "发送数据失败，断开连接" << std::endl;
                    running = false;
                    break;
                }
            }
        } else {
             std::cerr << "编码错误" << std::endl;
        }
        
        pts++;
        
        DWORD elapsed = GetTickCount() - t0;
        if (elapsed < frameMs) Sleep(frameMs - elapsed);
    }
    
    running = false; // 确保Input线程也能退出
    
    // 关闭 Socket 会强制 InputHandler 中的 recv 返回错误从而退出循环
    shutdown(sock, SD_BOTH);
    closesocket(sock);
    
    if (inputThread.joinable()) inputThread.join();
    std::cout << "客户端断开" << std::endl;
}

// ==================== 主函数 ====================
int main() {
    // 提高定时器精度
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed" << std::endl;
        return -1;
    }
    
    listen(srv, 1); // 限制1个连接
    
    std::cout << "================================\n";
    std::cout << "  HEVC 远程桌面 (修复增强版)\n";
    std::cout << "================================\n";
    std::cout << "端口: " << PORT << " | 等待连接...\n";
    
    while (serverRunning) {
        sockaddr_in caddr;
        int clen = sizeof(caddr);
        SOCKET client = accept(srv, (sockaddr*)&caddr, &clen);
        if (client == INVALID_SOCKET) {
             std::cout << "Accept failed or interrupted.\n";
             break;
        }
        
        // 禁用 Nagle 算法，降低延迟
        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        
        // 处理单个客户端（阻塞式，因为只需要一对一）
        ClientHandler(client);
    }
    
    closesocket(srv);
    WSACleanup();
    timeEndPeriod(1);
    return 0;
}