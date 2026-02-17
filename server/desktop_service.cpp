#include "desktop_service.h"
#include <iostream>

DesktopService::DesktopService() {}
DesktopService::~DesktopService() { stop(); }

bool DesktopService::init() {
    if (!capture_.init()) {
        std::cerr << "[Desktop] Capture init failed" << std::endl;
        return false;
    }
    if (!encoder_.init(capture_.getWidth(), capture_.getHeight(), Config::FPS, Config::CRF)) {
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

            // 每次收到 ClientReady 都发送 ScreenInfo（支持重连和重新打开窗口）
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

        processInput();

        bool hasNew = false;
        const uint8_t* frame = capture_.capture(hasNew);
        if (!frame) { Sleep(10); continue; }

        bool keyframe = (pts % Config::KEYFRAME_INTERVAL == 0) || keyframeRequested_.exchange(false);

        if (encoder_.encode(frame, pts, encoded, keyframe)) {
            if (!encoded.empty() && transport_ && transport_->hasClient()) {
                auto msg = MessageBuilder::VideoFrame(encoded.data(), encoded.size(), keyframe);
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