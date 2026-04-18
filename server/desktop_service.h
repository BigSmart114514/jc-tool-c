
#ifndef DESKTOP_SERVICE_H
#define DESKTOP_SERVICE_H

#include "../common/protocol.h"
#include "../common/transport.h"
#include "screen_capture.h"
#include "hevc_encoder.h"
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>

class DesktopService {
public:
    DesktopService();
    ~DesktopService();

    bool init();
    void setTransport(IServerTransport* transport);
    
    void start();
    void stop();
    
    int getWidth() const { return capture_.getWidth(); }
    int getHeight() const { return capture_.getHeight(); }

private:
    void onClientConnected();
    void onClientDisconnected();
    void onMessage(const BinaryData& data);
    
    void captureLoop();
    void processInput();

    ScreenCapture capture_;
    HEVCEncoder encoder_;
    IServerTransport* transport_ = nullptr;

    std::queue<Desktop::InputEvent> inputQueue_;
    std::mutex inputMtx_;

    std::thread captureThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> clientReady_{false};
    std::atomic<bool> keyframeRequested_{false};
    std::condition_variable clientCV_;
    std::mutex clientMtx_;
    // --- 新增：动态流控配置目标值 ---
    int targetWidth_ = 0;
    int targetHeight_ = 0;
    int targetFps_ = 0;
    int targetKfIntervalSec_ = 0;
    std::atomic<bool> configChanged_{false}; // 标记是否需要重新初始化编码器
};

#endif // DESKTOP_SERVICE_H
