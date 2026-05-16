#include "desktop_service.h"
#include <iostream>

DesktopService::DesktopService() {}
DesktopService::~DesktopService() { stop(); }

// 在 init 中赋予初始服务端默认设置：
bool DesktopService::init() {
    if (!capture_.init()) { return false; }
    
    // 初始使用服务端配置
    targetWidth_ = capture_.getWidth();
    targetHeight_ = capture_.getHeight();
    targetFps_ = Config::FPS;
    targetKfIntervalSec_ = 5; // 默认5秒

    // 【修改】传入源宽高和目标宽高
    if (!encoder_.init(capture_.getWidth(), capture_.getHeight(), targetWidth_, targetHeight_, targetFps_, Config::CRF)) {
        std::cerr << "[Desktop] Encoder init failed" << std::endl;
        return false;
    }
    return true;
}

void DesktopService::setTransport(IServerTransport* transport) {
    transport_ = transport;

    TransportCallbacks callbacks;
    callbacks.onConnected = [this]() { onClientConnected(); };
    callbacks.onDisconnected = [this]() { onClientDisconnected(); };
    callbacks.onMessage = [this](const BinaryData& data) { onMessage(data); };
    transport_->setCallbacks(callbacks);
}

void DesktopService::onClientConnected() {
    std::cout << "[Desktop] Transport connected (waiting for ClientReady)" << std::endl;
    // 不再在这里发 ScreenInfo！等客户端准备好再发
}

void DesktopService::onClientDisconnected() {
    std::cout << "[Desktop] Client disconnected" << std::endl;
    clientReady_ = false;
    clientCV_.notify_all();

    std::lock_guard<std::mutex> lock(inputMtx_);
    while (!inputQueue_.empty()) inputQueue_.pop();
}

void DesktopService::onMessage(const BinaryData& data) {
    if (data.empty()) return;

    auto type = static_cast<Desktop::MsgType>(data[0]);

    switch (type) {
        case Desktop::MsgType::ClientReady: {
            std::cout << "[Desktop] ClientReady received, sending ScreenInfo and starting stream" << std::endl;
            if (transport_ && transport_->hasClient()) {
                auto msg = MessageBuilder::ScreenInfo(capture_.getWidth(), capture_.getHeight());
                transport_->send(msg);
            }
            clientReady_ = true;
            clientCV_.notify_one();
            break;
        }

        case Desktop::MsgType::InputEvent:
            if (data.size() >= 1 + sizeof(Desktop::InputEvent)) {
                Desktop::InputEvent ev;
                memcpy(&ev, data.data() + 1, sizeof(ev));
                std::lock_guard<std::mutex> lock(inputMtx_);
                inputQueue_.push(ev);
            }
            break;

        case Desktop::MsgType::KeyframeRequest:
            keyframeRequested_ = true;
            break;
        
        case Desktop::MsgType::StreamConfig: {
            if (data.size() >= 1 + sizeof(Desktop::StreamConfig)) {
                Desktop::StreamConfig cfg;
                memcpy(&cfg, data.data() + 1, sizeof(cfg));

                int origW = capture_.getWidth();
                int origH = capture_.getHeight();

                int calcWidth = std::max(cfg.width , MINWIDTH);
                int newH = (origW > 0) ? (origH * calcWidth / origW) : origH;
                
                // 【修改】强制 16 像素对齐，防止 HEVC 编码器内部 padding 导致步长错乱花屏
                calcWidth = (calcWidth + 15) & ~15;
                newH = (newH + 15) & ~15;

                targetWidth_ = calcWidth;
                targetHeight_ = newH;
                targetFps_ = cfg.fps > 0 ? cfg.fps : 1;
                targetKfIntervalSec_ = cfg.keyframeIntervalSec > 0 ? cfg.keyframeIntervalSec : 5;
                
                configChanged_ = true;
                
                std::cout << "[Desktop] Client specified Stream Config: " 
                          << targetWidth_ << "x" << targetHeight_ 
                          << " @ " << targetFps_ << "fps. Will apply on next keyframe." << std::endl;
            }
            break;
        }
        default:
            break;
    }
}

void DesktopService::start() {
    running_ = true;
    captureThread_ = std::thread(&DesktopService::captureLoop, this);
}

void DesktopService::stop() {
    running_ = false;
    clientReady_ = false;
    clientCV_.notify_all();
    if (captureThread_.joinable()) captureThread_.join();
}

void DesktopService::captureLoop() {
    std::vector<uint8_t> encoded;
    encoded.reserve(256 * 1024);
    const DWORD frameMs = 1000 / Config::FPS;
    int64_t pts = 0;

    std::cout << "[Desktop] Capture loop started" << std::endl;

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(clientMtx_);
            clientCV_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                return (clientReady_.load() && transport_ && transport_->hasClient()) || !running_;
            });
            if (!running_) break;
            if (!clientReady_ || !transport_ || !transport_->hasClient()) continue;
        }

        DWORD t0 = GetTickCount();
        // 【动态修改1】实时帧率导致的单帧延迟时间
        DWORD frameMs = 1000 / targetFps_;

        processInput();

        bool hasNew = false;
        const uint8_t* frame = capture_.capture(hasNew);

        if (!frame) {
            std::cerr << "[Desktop Server] Capture returned null" << std::endl;
            Sleep(10);
            continue;
        }

        if (!hasNew) {
            Sleep(1);
            continue;
        }

        // 【动态修改2】判断本次是否应该发送关键帧
        bool kfRequested = keyframeRequested_.exchange(false);
        // 根据客户端指定的帧率和秒数计算关键帧间隔
        bool isTimeForKeyframe = (pts % (targetFps_ * targetKfIntervalSec_) == 0) || kfRequested;
        
        // 【动态修改3】如果有新的配置请求，并且当下马上要发关键帧，此时再重置编码器！
        if (configChanged_ && isTimeForKeyframe) {
            std::cout << "[Desktop] Applying new config and forcing keyframe..." << std::endl;
            encoder_.cleanup();
            // 【修改】重置编码器时，传入源截图宽高和新计算出的目标宽高
            encoder_.init(capture_.getWidth(), capture_.getHeight(), targetWidth_, targetHeight_, targetFps_, Config::CRF);
            configChanged_ = false;

            // 极为关键的一步：告诉客户端分辨率变了，让它的解码器也立即重新初始化！
            if (transport_ && transport_->hasClient()) {
                auto msg = MessageBuilder::ScreenInfo(targetWidth_, targetHeight_);
                transport_->send(msg);
            }
            pts = 0; // 重置时间戳
        }

        if (encoder_.encode(frame, pts, encoded, isTimeForKeyframe)) {
            if (!encoded.empty() && transport_ && transport_->hasClient()) {
                auto msg = MessageBuilder::VideoFrame(encoded.data(), encoded.size(), isTimeForKeyframe);
                if (!transport_->send(msg)) {
                    clientReady_ = false;
                }
            }
        }

        pts++;
        DWORD elapsed = GetTickCount() - t0;
        if (elapsed < frameMs) Sleep(frameMs - elapsed);
    }
}

void DesktopService::processInput() {
    Desktop::InputEvent ev;
    INPUT input = {};

    std::lock_guard<std::mutex> lock(inputMtx_);
    while (!inputQueue_.empty()) {
        ev = inputQueue_.front();
        inputQueue_.pop();

        if (ev.type == 0) {
            SetCursorPos(ev.x, ev.y);
            switch (ev.key) {
                case 1: mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); break;
                case 2: mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); break;
                case 3: mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0); break;
                case 4: mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0); break;
                case 5: mouse_event(MOUSEEVENTF_MIDDLEDOWN, 0, 0, 0, 0); break;
                case 6: mouse_event(MOUSEEVENTF_MIDDLEUP, 0, 0, 0, 0); break;
                case 7: mouse_event(MOUSEEVENTF_WHEEL, 0, 0, ev.flags, 0); break;
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