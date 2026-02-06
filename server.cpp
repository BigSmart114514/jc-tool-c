#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

const int PORT = 12345;
const int CRF = 23;
const int FPS = 30;
const int KEYFRAME_INTERVAL = 60;
std::atomic<bool> serverRunning(true);

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

enum PacketType : uint8_t {
    PACKET_VIDEO = 0,
    PACKET_KEYFRAME_REQUEST = 1,
    PACKET_DIMENSIONS = 2
};

// HEVC编码器封装类
class HEVCEncoder {
private:
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    SwsContext* swsCtx = nullptr;
    int width = 0;
    int height = 0;
    bool initialized = false;
    std::mutex encodeMutex;

public:
    HEVCEncoder() = default;
    
    ~HEVCEncoder() {
        Cleanup();
    }
    
    void Cleanup() {
        std::lock_guard<std::mutex> lock(encodeMutex);
        if (swsCtx) {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }
        if (pkt) {
            av_packet_free(&pkt);
            pkt = nullptr;
        }
        if (frame) {
            av_frame_free(&frame);
            frame = nullptr;
        }
        if (ctx) {
            avcodec_free_context(&ctx);
            ctx = nullptr;
        }
        initialized = false;
    }
    
    bool Init(int w, int h, int fps, int crf) {
        std::lock_guard<std::mutex> lock(encodeMutex);
        
        width = w;
        height = h;
        
        const char* encoderNames[] = {
            "libx265",
            "hevc_nvenc",
            "hevc_qsv",
            "hevc_amf",
            nullptr
        };
        
        for (int i = 0; encoderNames[i] != nullptr; i++) {
            codec = avcodec_find_encoder_by_name(encoderNames[i]);
            if (codec) {
                std::cout << "尝试使用编码器: " << encoderNames[i] << "\n";
                break;
            }
        }
        
        if (!codec) {
            codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        }
        
        if (!codec) {
            std::cerr << "错误: 未找到HEVC编码器\n";
            return false;
        }
        
        std::cout << "使用编码器: " << codec->name << "\n";
        
        ctx = avcodec_alloc_context3(codec);
        if (!ctx) {
            std::cerr << "错误: 无法分配编码器上下文\n";
            return false;
        }
        
        ctx->width = width;
        ctx->height = height;
        ctx->time_base = AVRational{1, fps};
        ctx->framerate = AVRational{fps, 1};
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->gop_size = KEYFRAME_INTERVAL;
        ctx->max_b_frames = 0;
        ctx->thread_count = 4;
        
        char crfStr[16];
        snprintf(crfStr, sizeof(crfStr), "%d", crf);
        
        if (strcmp(codec->name, "libx265") == 0) {
            av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
            av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
            av_opt_set(ctx->priv_data, "crf", crfStr, 0);
            av_opt_set(ctx->priv_data, "x265-params", 
                "log-level=error:repeat-headers=1:aud=1", 0);
        } else if (strstr(codec->name, "nvenc")) {
            av_opt_set(ctx->priv_data, "preset", "p1", 0);
            av_opt_set(ctx->priv_data, "tune", "ll", 0);
            av_opt_set(ctx->priv_data, "rc", "constqp", 0);
            ctx->global_quality = crf;
        } else if (strstr(codec->name, "qsv")) {
            av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
            ctx->global_quality = crf;
        } else if (strstr(codec->name, "amf")) {
            av_opt_set(ctx->priv_data, "quality", "speed", 0);
            ctx->global_quality = crf;
        }
        
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        
        int ret = avcodec_open2(ctx, codec, nullptr);
        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            std::cerr << "错误: 无法打开编码器: " << errBuf << "\n";
            avcodec_free_context(&ctx);
            return false;
        }
        
        frame = av_frame_alloc();
        if (!frame) {
            std::cerr << "错误: 无法分配帧\n";
            return false;
        }
        
        frame->format = ctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        
        ret = av_frame_get_buffer(frame, 32);
        if (ret < 0) {
            std::cerr << "错误: 无法分配帧缓冲区\n";
            return false;
        }
        
        pkt = av_packet_alloc();
        if (!pkt) {
            std::cerr << "错误: 无法分配数据包\n";
            return false;
        }
        
        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_BGRA,
            width, height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!swsCtx) {
            std::cerr << "错误: 无法创建颜色空间转换上下文\n";
            return false;
        }
        
        initialized = true;
        std::cout << "HEVC编码器初始化成功: " << width << "x" << height 
                  << " @ " << fps << "fps, CRF=" << crf << "\n";
        return true;
    }
    
    bool Encode(const uint8_t* bgraData, int64_t pts, std::vector<uint8_t>& output, bool forceKeyframe = false) {
        std::lock_guard<std::mutex> lock(encodeMutex);
        
        if (!initialized) return false;
        
        output.clear();
        
        int ret = av_frame_make_writable(frame);
        if (ret < 0) {
            return false;
        }
        
        const uint8_t* srcSlice[1] = {bgraData};
        int srcStride[1] = {width * 4};
        
        sws_scale(swsCtx, srcSlice, srcStride, 0, height,
                  frame->data, frame->linesize);
        
        frame->pts = pts;
        
        if (forceKeyframe) {
            frame->pict_type = AV_PICTURE_TYPE_I;
#if LIBAVUTIL_VERSION_MAJOR >= 58
            frame->flags |= AV_FRAME_FLAG_KEY;
#endif
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
#if LIBAVUTIL_VERSION_MAJOR >= 58
            frame->flags &= ~AV_FRAME_FLAG_KEY;
#endif
        }
        
        ret = avcodec_send_frame(ctx, frame);
        if (ret < 0) {
            return false;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_packet(ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                return false;
            }
            
            size_t oldSize = output.size();
            output.resize(oldSize + pkt->size);
            memcpy(output.data() + oldSize, pkt->data, pkt->size);
            
            av_packet_unref(pkt);
        }
        
        return !output.empty();
    }
    
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
    bool IsInitialized() const { return initialized; }
};

// 屏幕捕获类
class ScreenCapture {
private:
    HDC hdcScreen = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
    bool initialized = false;

public:
    ~ScreenCapture() {
        Cleanup();
    }
    
    void Cleanup() {
        if (hBitmap) {
            DeleteObject(hBitmap);
            hBitmap = nullptr;
        }
        if (hdcMem) {
            DeleteDC(hdcMem);
            hdcMem = nullptr;
        }
        if (hdcScreen) {
            ReleaseDC(nullptr, hdcScreen);
            hdcScreen = nullptr;
        }
        initialized = false;
    }
    
    bool Init() {
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
        
        hBitmap = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hBitmap) return false;
        
        SelectObject(hdcMem, hBitmap);
        initialized = true;
        
        return true;
    }
    
    const uint8_t* Capture() {
        if (!initialized) return nullptr;
        BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
        return static_cast<uint8_t*>(bits);
    }
    
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};

bool SendData(SOCKET clientSocket, const char* data, int size) {
    int totalSent = 0;
    while (totalSent < size) {
        int sent = send(clientSocket, data + totalSent, size - totalSent, 0);
        if (sent <= 0) return false;
        totalSent += sent;
    }
    return true;
}

bool ReceiveData(SOCKET socket, char* buffer, int size) {
    int totalReceived = 0;
    while (totalReceived < size) {
        int received = recv(socket, buffer + totalReceived, size - totalReceived, 0);
        if (received <= 0) return false;
        totalReceived += received;
    }
    return true;
}

void InputHandler(SOCKET clientSocket, std::atomic<bool>& running) {
    while (running && serverRunning) {
        InputEvent event;
        if (ReceiveData(clientSocket, reinterpret_cast<char*>(&event), sizeof(InputEvent))) {
            if (event.type == 0) {
                SetCursorPos(event.x, event.y);
                
                switch (event.key) {
                    case 1: mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); break;
                    case 2: mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); break;
                    case 3: mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0); break;
                    case 4: mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0); break;
                    case MOUSE_MIDDLE_DOWN: mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, 0); break;
                    case MOUSE_MIDDLE_UP: mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0); break;
                    case MOUSE_WHEEL: mouse_event(MOUSEEVENTF_WHEEL, 0, 0, event.flags, 0); break;
                }
            } else if (event.type == 1) {
                INPUT input = {};
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = static_cast<WORD>(event.key);
                input.ki.dwFlags = event.flags ? KEYEVENTF_KEYUP : 0;
                SendInput(1, &input, sizeof(INPUT));
            }
        } else {
            break;
        }
    }
}

void ClientHandler(SOCKET clientSocket) {
    std::cout << "客户端已连接\n";
    
    std::atomic<bool> clientRunning(true);
    
    ScreenCapture capture;
    if (!capture.Init()) {
        std::cerr << "屏幕捕获初始化失败\n";
        closesocket(clientSocket);
        return;
    }
    
    int screenWidth = capture.GetWidth();
    int screenHeight = capture.GetHeight();
    
    std::cout << "屏幕尺寸: " << screenWidth << "x" << screenHeight << "\n";
    
    int dimensions[2] = {screenWidth, screenHeight};
    if (!SendData(clientSocket, reinterpret_cast<char*>(dimensions), sizeof(dimensions))) {
        closesocket(clientSocket);
        return;
    }
    
    HEVCEncoder encoder;
    if (!encoder.Init(screenWidth, screenHeight, FPS, CRF)) {
        std::cerr << "HEVC编码器初始化失败\n";
        closesocket(clientSocket);
        return;
    }
    
    std::thread inputThread(InputHandler, clientSocket, std::ref(clientRunning));
    
    const DWORD frameTime = 1000 / FPS;
    int64_t pts = 0;
    std::vector<uint8_t> encodedData;
    
    while (serverRunning && clientRunning) {
        DWORD startTime = GetTickCount();
        
        const uint8_t* screenData = capture.Capture();
        if (!screenData) {
            break;
        }
        
        bool forceKeyframe = (pts % KEYFRAME_INTERVAL == 0);
        
        if (encoder.Encode(screenData, pts, encodedData, forceKeyframe)) {
            uint8_t packetType = PACKET_VIDEO;
            if (!SendData(clientSocket, reinterpret_cast<char*>(&packetType), 1)) {
                break;
            }
            
            uint8_t isKeyframe = forceKeyframe ? 1 : 0;
            if (!SendData(clientSocket, reinterpret_cast<char*>(&isKeyframe), 1)) {
                break;
            }
            
            int32_t dataSize = static_cast<int32_t>(encodedData.size());
            if (!SendData(clientSocket, reinterpret_cast<char*>(&dataSize), sizeof(dataSize))) {
                break;
            }
            
            if (!SendData(clientSocket, reinterpret_cast<char*>(encodedData.data()), dataSize)) {
                break;
            }
        }
        
        pts++;
        
        DWORD elapsed = GetTickCount() - startTime;
        if (elapsed < frameTime) {
            Sleep(frameTime - elapsed);
        }
    }
    
    clientRunning = false;
    
    if (inputThread.joinable()) {
        inputThread.join();
    }
    
    closesocket(clientSocket);
    std::cout << "客户端断开连接\n";
}

int main() {
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
    
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);
    
    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "绑定端口失败\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "监听失败\n";
        closesocket(serverSocket);
        WSACleanup();
        return 1;
    }
    
    std::cout << "========================================\n";
    std::cout << "    HEVC (H.265) 远程桌面服务端\n";
    std::cout << "========================================\n";
    std::cout << "端口: " << PORT << "\n";
    std::cout << "编码参数: CRF=" << CRF << ", FPS=" << FPS << "\n";
    std::cout << "等待连接...\n";
    
    while (serverRunning) {
        sockaddr_in clientAddr;
        int clientSize = sizeof(clientAddr);
        SOCKET clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientSize);
        
        if (clientSocket == INVALID_SOCKET) {
            if (serverRunning) {
                std::cerr << "接受连接失败\n";
            }
            break;
        }
        
        int flag = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
        
        int bufSize = 1024 * 1024;
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, (char*)&bufSize, sizeof(bufSize));
        
        std::thread(ClientHandler, clientSocket).detach();
    }
    
    closesocket(serverSocket);
    WSACleanup();
    
    return 0;
}