#include "desktop_service.h"
#include <iostream>
#include <algorithm>

DesktopService::DesktopService() {}
DesktopService::~DesktopService() { stop(); }

bool DesktopService::init() {
    if (!capture_.init()) { return false; }
    
    targetWidth_ = capture_.getWidth();
    targetHeight_ = capture_.getHeight();
    targetFps_ = Config::FPS;
    targetKfIntervalSec_ = 5;

    int bitrate = std::max(10000000, targetWidth_ * targetHeight_ * 4);
    if (!encoder_.init(capture_.getDevice(), capture_.getWidth(), capture_.getHeight(),
                        targetWidth_, targetHeight_, targetFps_, bitrate)) {
        std::cerr << "[Desktop] Encoder init failed" << std::endl;
        return false;
    }

    return true;
}

bool DesktopService::initAudio() {
    if (audioCapture_.initialized()) return true;

    if (!audioCapture_.init()) {
        std::cerr << "[Desktop] Audio capture init failed" << std::endl;
        return false;
    }
    if (!audioEncoder_.init(audioCapture_.sampleRate(), audioCapture_.channels(), 64000)) {
        std::cerr << "[Desktop] Audio encoder init failed" << std::endl;
        audioCapture_.cleanup();
        return false;
    }

    audioCapture_.setDataCallback([this](const uint8_t* pcm, int samples) {
        if (!clientReady_ || !transport_ || !transport_->hasClient()) return;
        std::vector<uint8_t> aacFrame;
        if (audioEncoder_.encode(pcm, samples, aacFrame) && !aacFrame.empty()) {
            auto msg = MessageBuilder::AudioData(aacFrame.data(), aacFrame.size());
            transport_->send(msg);
        }
    });

    std::cout << "[Desktop] Audio initialized (" << audioCapture_.sampleRate()
              << "Hz " << audioCapture_.channels() << "ch)" << std::endl;
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
                auto msg = MessageBuilder::ScreenInfo(encoder_.encodedWidth(), encoder_.encodedHeight());
                transport_->send(msg);
            }
            keyframeRequested_ = true;
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
        
        case Desktop::MsgType::ClientDisconnect:
            std::cout << "[Desktop] Client requested disconnect, stopping stream" << std::endl;
            clientReady_ = false;
            configChanged_ = false;
            reinitEncoder_ = false;
            disableAudio();
            {
                std::lock_guard<std::mutex> lock(inputMtx_);
                while (!inputQueue_.empty()) inputQueue_.pop();
            }
            break;

        case Desktop::MsgType::AudioEnable: {
            bool enable = (data.size() > 1 && data[1] != 0);
            if (enable != audioEnabled_) {
                if (enable) enableAudio();
                else disableAudio();
            }
            break;
        }

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
                configChangeCV_.notify_all();
                
                std::cout << "[Desktop] Client specified Stream Config: " 
                          << targetWidth_ << "x" << targetHeight_ 
                          << " @ " << targetFps_ << "fps." << std::endl;
            }
            break;
        }
        case Desktop::MsgType::ScreenOff: {
            std::lock_guard<std::mutex> lock(displayMtx_);
            if (!running_) break;
            if (capture_.usesRequestedOutput()) {
                screenOffOutput_ = capture_.getCaptureOutput();
                DisablePhysicalMonitors(screenOffOutput_);
            } else {
                std::cerr << "[Desktop] Screen-off ignored: virtual output is not being captured" << std::endl;
            }
            break;
        }

        case Desktop::MsgType::Shutdown:
            std::cout << "[Desktop] Shutdown requested via TCP" << std::endl;
            running_ = false;
            break;

        default:
            break;
    }
}

void DesktopService::enableAudio() {
    if (audioEnabled_) return;
    if (!audioCapture_.initialized() && !initAudio()) return;
    audioCapture_.start();
    audioEnabled_ = true;
    std::cout << "[Desktop] Audio enabled" << std::endl;

    if (transport_ && transport_->hasClient()) {
        auto& asc = audioEncoder_.audioSpecificConfig();
        auto msg = MessageBuilder::AudioConfig(
            audioCapture_.sampleRate(), audioCapture_.channels(),
            asc.data(), (int)asc.size());
        transport_->send(msg);
    }
}

void DesktopService::disableAudio() {
    if (!audioEnabled_) return;
    audioCapture_.stop();
    audioEnabled_ = false;
    std::cout << "[Desktop] Audio disabled" << std::endl;
}

void DesktopService::start() {
    running_ = true;
    captureThread_ = std::thread(&DesktopService::captureLoop, this);
    configChangeLoopThread_ = std::thread(&DesktopService::configChangeLoop, this);
}

void DesktopService::stop() {
    running_ = false;
    clientReady_ = false;
    disableAudio();
    clientCV_.notify_all();
    configChangeCV_.notify_all();
    if (captureThread_.joinable()) captureThread_.join();
    if (configChangeLoopThread_.joinable()) configChangeLoopThread_.join();
    std::lock_guard<std::mutex> lock(displayMtx_);
    if (!screenOffOutput_.empty()) {
        RestorePhysicalMonitors(screenOffOutput_);
        screenOffOutput_.clear();
    }
}

void DesktopService::captureLoop() {
    std::vector<MediaEncoder::Frame> encoded;
    int64_t pts = 0;

    std::cout << "[Desktop] Capture loop started" << std::endl;

    while (running_) {
        {
            std::unique_lock<std::mutex> lock(clientMtx_);
            clientCV_.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                return (clientReady_.load() && transport_ && transport_->hasClient()) || !running_;
            });
            if (!running_) break;
            if (!clientReady_ || !transport_ || !transport_->hasClient()) continue;
        }

        DWORD t0 = GetTickCount();
        // 【动态修改1】实时帧率导致的单帧延迟时间
        DWORD frameMs = 1000 / targetFps_;

        processInput();

        // 【动态修改2】判断本次是否应该发送关键帧
        bool kfRequested = keyframeRequested_.exchange(false);
        // 根据客户端指定的帧率和秒数计算关键帧间隔
        bool isTimeForKeyframe = (pts % (targetFps_ * targetKfIntervalSec_) == 0) || kfRequested;
        
        // 【动态修改3】如果有新的配置请求，并且当下马上要发关键帧，此时再重置编码器！
        if (reinitEncoder_ && isTimeForKeyframe) {
            std::cout << "[Desktop] Applying new config and forcing keyframe..." << std::endl;
            encoder_.cleanup();
            int configBitrate = std::max(10000000, targetWidth_ * targetHeight_ * 4);
            if (!encoder_.init(capture_.getDevice(), capture_.getWidth(), capture_.getHeight(),
                               targetWidth_, targetHeight_, targetFps_, configBitrate)) {
                std::cerr << "[Desktop] Encoder init failed during config change" << std::endl;
                reinitEncoder_ = false;
                return;
            }
            reinitEncoder_ = false;

            // 极为关键的一步：告诉客户端分辨率变了，让它的解码器也立即重新初始化！
            if (transport_ && transport_->hasClient()) {
                auto msg = MessageBuilder::ScreenInfo(
                    encoder_.encodedWidth(), encoder_.encodedHeight());
                transport_->send(msg);
            }
            pts = 0; // 重置时间戳
        }

        bool encodeOk = false;

        // 抓取源分辨率可能因为虚拟显示器切换而变化。
        // VideoProcessor 的输入尺寸是构造时固定的，尺寸一变
        // VideoProcessorBlt 就会读错纹理 —— 表现为花屏/撕裂。
        // 这里检测到变化后重建编码器。
        if (capture_.getWidth() > 0 && capture_.getHeight() > 0 &&
            (capture_.getWidth() != encoder_.sourceWidth() ||
             capture_.getHeight() != encoder_.sourceHeight())) {
            std::cout << "[Desktop] Capture size changed "
                      << encoder_.sourceWidth() << "x" << encoder_.sourceHeight()
                      << " -> " << capture_.getWidth() << "x" << capture_.getHeight()
                      << ", reinitializing encoder" << std::endl;
            encoder_.cleanup();
            int newBitrate = std::max(10000000, targetWidth_ * targetHeight_ * 4);
            if (encoder_.init(capture_.getDevice(), capture_.getWidth(), capture_.getHeight(),
                              targetWidth_, targetHeight_, targetFps_, newBitrate)) {
                if (transport_ && transport_->hasClient()) {
                    auto msg = MessageBuilder::ScreenInfo(
                        encoder_.encodedWidth(), encoder_.encodedHeight());
                    transport_->send(msg);
                }
                pts = 0;
                isTimeForKeyframe = true;
            } else {
                std::cerr << "[Desktop] Encoder reinit after capture change failed" << std::endl;
                // 不能直接 continue：会跳过下面的 Sleep 造成忙等
                Sleep(200);
                continue;
            }
        }

        ID3D11Texture2D* tex = nullptr;
        if (capture_.getDevice() && encoder_.hasGPUPath()) {
            std::lock_guard<std::mutex> lock(displayMtx_);
            if (capture_.captureTexture(&tex)) {
                encodeOk = encoder_.encodeFromTexture(tex, pts, encoded, isTimeForKeyframe);
            }
        } else {
            bool hasNew = false;
            const uint8_t* bgra = nullptr;
            {
                std::lock_guard<std::mutex> lock(displayMtx_);
                bgra = capture_.capture(hasNew);
            }
            if (bgra && (hasNew || isTimeForKeyframe) &&
                capture_.getWidth() == encoder_.sourceWidth() && capture_.getHeight() == encoder_.sourceHeight()) {
                encodeOk = encoder_.encode(bgra, pts, encoded, isTimeForKeyframe);
            }
        }

        if (isTimeForKeyframe && (!encodeOk || encoded.empty())) keyframeRequested_ = true;
        if (transport_ && transport_->hasClient()) {
            for (const auto& frame : encoded) {
                auto msg = MessageBuilder::VideoFrame(frame.data.data(), frame.data.size(), frame.keyframe);
                if (!transport_->send(msg)) {
                    clientReady_ = false;
                    break;
                }
                if (pts % 30 == 0)
                    std::cout << "[Desktop] Sent frame pts=" << pts
                              << " size=" << frame.data.size()
                              << " kf=" << frame.keyframe << std::endl;
            }
        }
        encoded.clear();

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
            int physX = ev.x, physY = ev.y;
            int capW = capture_.getWidth(), capH = capture_.getHeight();
            int encW = encoder_.encodedWidth();
            int encH = encoder_.encodedHeight();
            if (encW > 0 && encH > 0 && capW > 0 && capH > 0) {
                physX = ev.x * capW / encW + capture_.getOriginX();
                physY = ev.y * capH / encH + capture_.getOriginY();
            }

            ZeroMemory(&input, sizeof(input));
            input.type = INPUT_MOUSE;
            int vsLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vsTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vsW    = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vsH    = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (vsW > 1 && vsH > 1) {
                input.mi.dx = (DWORD)(((physX - vsLeft) * 65535) / (vsW - 1));
                input.mi.dy = (DWORD)(((physY - vsTop) * 65535) / (vsH - 1));
                input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            } else {
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
            }

            switch (ev.key) {
                case 0: break; // move only
                case 1: input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN; break;
                case 2: input.mi.dwFlags |= MOUSEEVENTF_LEFTUP; break;
                case 3: input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN; break;
                case 4: input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP; break;
                case 5: input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN; break;
                case 6: input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP; break;
                case 7: input.mi.dwFlags |= MOUSEEVENTF_WHEEL; input.mi.mouseData = ev.flags; break;
            }

            UINT sent = SendInput(1, &input, sizeof(INPUT));
            if (sent == 0) {
                std::cerr << "[Desktop] SendInput failed: " << GetLastError() << std::endl;
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

void DesktopService::configChangeLoop() {
    std::unique_lock<std::mutex> lock(ConfigChangeLoopMtx_);
    while (running_)
    {
        // 等待configChange
        while (running_ && !configChanged_) {
            configChangeCV_.wait(lock, [&]() {
                return !running_ || configChanged_;
            });
        }
        if (!running_) break;
        configChanged_ = false;
        // 两秒超时
        configChangeCV_.wait_for(lock, std::chrono::seconds(ConfigWaitSeconds), [&]() {
            return !running_ || configChanged_;
        });
        if (!running_) break;
        if (configChanged_) continue;
        else 
        {
            std::cout << "[Desktop] " << "About to reinit Encoder"<< std::endl;
            reinitEncoder_ = true;
        }
    }
}
