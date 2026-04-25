#include "desktop_window.h"
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QPainter>
#include <QApplication>
#include <QScreen>
#include <iostream>

DesktopWindow::DesktopWindow(QWidget* parent)
    : QWidget(parent) {
    
    // --- 【保留】窗口基础设置 ---
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(Qt::Window);
    setMouseTracking(true); // 必须保留，用于捕获鼠标移动发送给远端
    
    // --- 【保留】初始大小设置 ---
    QScreen* screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    resize(screenGeometry.width() * 3 / 4, screenGeometry.height() * 3 / 4);

    // --- 【核心修改】彻底删掉 displayLabel_ 和 Layout ---
    /* 不要创建 displayLabel_
       不要创建 QVBoxLayout
       因为我们要直接在 DesktopWindow 窗口本身上画图（通过 paintEvent）
    */

    // --- 【修改】绑定渲染信号 ---
    // 依然监听信号，但 updateDisplay 内部只需调用 update()
    connect(this, &DesktopWindow::frameReady, this, &DesktopWindow::updateDisplay);

    // --- 【保留】流控与解码初始化 ---
    decoding_ = true;
    decodeThread_ = std::thread(&DesktopWindow::decodeLoop, this);

    // 初始化缩放定时器
    resizeTimer_.setSingleShot(true);
    connect(&resizeTimer_, &QTimer::timeout, this, &DesktopWindow::onResizeCooldown);

    // 初始化统计时间
    lastStatsTime_ = std::chrono::steady_clock::now();
    lastFpsChangeTime_ = std::chrono::steady_clock::now();
}

DesktopWindow::~DesktopWindow() {
    // 停止解码线程
    decoding_ = false;
    queueCV_.notify_all();
    if (decodeThread_.joinable()) {
        decodeThread_.join();
    }
    
    decoderReady_ = false;
    decoder_.cleanup();
}

void DesktopWindow::init(ITransport* transport) {
    transport_ = transport;
}

void DesktopWindow::requestStream() {
    if (transport_ && transport_->isConnected()) {
        std::cout << "[Desktop] Requesting stream..." << std::endl;
        auto ready = MessageBuilder::ClientReady();
        transport_->send(ready);
    }
}

// 网络回调线程：仅负责分发消息，不执行耗时操作
void DesktopWindow::handleMessage(const BinaryData& data) {
    if (data.empty()) return;

    auto type = static_cast<Desktop::MsgType>(data[0]);

    switch (type) {
        case Desktop::MsgType::ScreenInfo:
            handleScreenInfo(data);
            break;

        case Desktop::MsgType::VideoFrame:
            if (data.size() > 2) {
                std::lock_guard<std::mutex> lock(queueMtx_);
                //totalFramesReceived_++;

                if (videoQueue_.size() > 3) {
                    int dropCount = videoQueue_.size();
                    //droppedFrames_ += dropCount;
                    intervalFramesDropped_ += dropCount; // 记录区间内丢帧

                    std::queue<BinaryData> empty;
                    std::swap(videoQueue_, empty);

                    // --- 新增：发生严重丢帧，立刻要求服务端发送关键帧 ---
                    if (transport_ && transport_->isConnected()) {
                        auto msg = MessageBuilder::KeyframeRequest();
                        transport_->send(msg);
                    }
                }
                
                videoQueue_.push(data);
                queueCV_.notify_one();

                // 每隔 5 秒（或 150 帧）自动打印一次统计
                // if (totalFramesReceived_ % 150 == 0) {
                //     logStatistics();
                // }
            }
            break;
        default:
            break;
    }
}

void DesktopWindow::handleScreenInfo(const BinaryData& data) {
    if (data.size() < 1 + sizeof(Desktop::ScreenInfo)) return;

    auto* info = reinterpret_cast<const Desktop::ScreenInfo*>(data.data() + 1);
    std::cout << "[Desktop] Remote Screen: " << info->width << "x" << info->height << std::endl;

    screenWidth_ = info->width;
    screenHeight_ = info->height;

    // 清空旧分辨率帧
    {
        std::lock_guard<std::mutex> lock(queueMtx_);
        std::queue<BinaryData> empty;
        std::swap(videoQueue_, empty);
    }

    // 【修改】加锁保护解码器的重新初始化，避免与 decodeLoop 产生数据竞争
    {
        std::lock_guard<std::mutex> decLock(decoderMtx_);
        decoder_.cleanup();
        if (decoder_.init(info->width, info->height)) {
            decoderReady_ = true;
            std::cout << "[Desktop] Decoder initialized" << std::endl;
        } else {
            std::cerr << "[Desktop] Decoder init failed" << std::endl;
            decoderReady_ = false;
        }
    }
}

// 独立的解码线程：消费者模式
void DesktopWindow::decodeLoop() {
    std::vector<uint8_t> rgbData;
    
    while (decoding_) {
        BinaryData data;
        {
            std::unique_lock<std::mutex> lock(queueMtx_);
            queueCV_.wait(lock, [this] { return !videoQueue_.empty() || !decoding_; });
            if (!decoding_) break;
            
            data = std::move(videoQueue_.front());
            videoQueue_.pop();
        }

        const uint8_t* rawH265 = data.data() + 2;
        size_t rawSize = data.size() - 2;

        auto decodeStart = std::chrono::steady_clock::now();

        bool success = false;
        int w = 0, h = 0, stride = 0;

        // 【修改】加锁保护解码操作，防止网络线程在此期间销毁上下文
        {
            std::lock_guard<std::mutex> decLock(decoderMtx_);
            if (!decoderReady_) continue;
            
            success = decoder_.decode(rawH265, static_cast<int>(rawSize), rgbData);
            if (success) {
                w = decoder_.getWidth();
                h = decoder_.getHeight();
                stride = decoder_.getStride();
            }
        }

        auto decodeEnd = std::chrono::steady_clock::now();
        
        if (success) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
            intervalDecodeTimeMs_ += static_cast<uint64_t>(duration);
            intervalFramesDecoded_++;

            checkAndAdjustStreamQuality();

            // 使用获取到的参数构造 QImage
            QImage img(rgbData.data(), w, h, stride, QImage::Format_RGB32);
            {
                std::lock_guard<std::mutex> lock(frameMutex_);
                latestFrame_ = img.copy();   
                hasNewFrame_ = true;
                screenWidth_ = w;
                screenHeight_ = h;
            }

            emit frameReady(); 
        } else {
            if (transport_ && transport_->isConnected()) {
                auto msg = MessageBuilder::KeyframeRequest();
                transport_->send(msg);
            }
        }
    }
}

// UI 渲染线程
void DesktopWindow::updateDisplay() {
    // 仅仅检查是否有新帧，然后触发重绘
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!hasNewFrame_) return; 
        // 我们不需要在这里 copy，paintEvent 会处理最新的 latestFrame_
    }
    
    // 触发系统的 paintEvent。这是 Qt 最标准的做法。
    // 它会将多次 update() 调用合并为一次重绘，极大地节省 CPU。
    update(); 
}

void DesktopWindow::sendInput(const Desktop::InputEvent& ev) {
    if (transport_ && transport_->isConnected()) {
        auto msg = MessageBuilder::InputEvent(ev);
        transport_->send(msg);
    }
}

bool DesktopWindow::convertToImageCoords(int wx, int wy, int& ix, int& iy) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (latestFrame_.isNull()) return false;

    // 计算 paintEvent 中实际绘制的画面区域
    QSize scaledSize = latestFrame_.size().scaled(this->size(), Qt::KeepAspectRatio);
    int offsetX = (width() - scaledSize.width()) / 2;
    int offsetY = (height() - scaledSize.height()) / 2;

    // 检查鼠标是否在画面区域内
    if (wx < offsetX || wx > offsetX + scaledSize.width() ||
        wy < offsetY || wy > offsetY + scaledSize.height()) {
        return false;
    }

    // 将窗口坐标转换为视频原始坐标
    double ratioX = (double)latestFrame_.width() / scaledSize.width();
    double ratioY = (double)latestFrame_.height() / scaledSize.height();

    ix = static_cast<int>((wx - offsetX) * ratioX);
    iy = static_cast<int>((wy - offsetY) * ratioY);

    return true;
}

void DesktopWindow::closeEvent(QCloseEvent* event) {
    emit closed();
    event->accept();
}

void DesktopWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F12) {
        emit openFileManager();
        return;
    }
    if (pEnableKeyboard_ && pEnableKeyboard_->load()) {
        sendInput({1, 0, 0, static_cast<int32_t>(event->nativeVirtualKey()), 0});
    }
}

void DesktopWindow::keyReleaseEvent(QKeyEvent* event) {
    if (pEnableKeyboard_ && pEnableKeyboard_->load()) {
        sendInput({1, 0, 0, static_cast<int32_t>(event->nativeVirtualKey()), 1});
    }
}

void DesktopWindow::mousePressEvent(QMouseEvent* event) {
    if (!pEnableMouseClick_ || !pEnableMouseClick_->load()) return;
    int x, y;
    if (!convertToImageCoords(event->pos().x(), event->pos().y(), x, y)) return;

    int key = 0;
    if (event->button() == Qt::LeftButton) { key = 1; grabMouse(); }
    else if (event->button() == Qt::RightButton) { key = 3; }
    else if (event->button() == Qt::MiddleButton) { key = 5; }

    if (key > 0) sendInput({0, x, y, key, 0});
}

void DesktopWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (!pEnableMouseClick_ || !pEnableMouseClick_->load()) return;
    int x, y;
    if (!convertToImageCoords(event->pos().x(), event->pos().y(), x, y)) return;

    int key = 0;
    if (event->button() == Qt::LeftButton) { key = 2; releaseMouse(); }
    else if (event->button() == Qt::RightButton) { key = 4; }
    else if (event->button() == Qt::MiddleButton) { key = 6; }

    if (key > 0) sendInput({0, x, y, key, 0});
}

void DesktopWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!pEnableMouseMove_ || !pEnableMouseMove_->load()) return;
    int x, y;
    if (convertToImageCoords(event->pos().x(), event->pos().y(), x, y)) {
        sendInput({0, x, y, 0, 0});
    }
}

void DesktopWindow::wheelEvent(QWheelEvent* event) {
    if (!pEnableMouseClick_ || !pEnableMouseClick_->load()) return;
    int x, y;
    if (convertToImageCoords(event->position().x(), event->position().y(), x, y)) {
        int delta = event->angleDelta().y();
        sendInput({0, x, y, 7, delta});
    }
}

void DesktopWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    // 获取当前窗口内实际显示图像的宽度（等比缩放后）
    int dispWidth = displayedImageSize().width();
    if (dispWidth > 0) {
        pendingResizeWidth_ = dispWidth;
        // 启动防抖定时器，冷却后发送新的分辨率需求
        resizeTimer_.start(RESIZE_COOLDOWN_MS);
    }
    // 如果 dispWidth == 0，说明还没有收到第一帧，不做任何分辨率调整

    updateDisplay();  // 触发重绘，保持画面刷新
}
/*
void DesktopWindow::logStatistics() {
    double dropRate = 0.0;
    if (totalFramesReceived_ > 0) {
        dropRate = (double)droppedFrames_ / totalFramesReceived_ * 100.0;
    }

    std::cout << "------------------------------------------" << std::endl;
    std::cout << "[Network Stats]" << std::endl;
    std::cout << "  Total Received: " << totalFramesReceived_ << std::endl;
    std::cout << "  Dropped (Buffer Full): " << droppedFrames_ << " (" << dropRate << "%)" << std::endl;
    std::cout << "  Successfully Decoded: " << decodedFrames_ << std::endl;
    std::cout << "  Queue Current Size: " << videoQueue_.size() << std::endl;
    std::cout << "------------------------------------------" << std::endl;
}
*/

// --- 新增：处理流控统计的函数 ---
void DesktopWindow::checkAndAdjustStreamQuality() {
    auto now = std::chrono::steady_clock::now();
    auto actualIntervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStatsTime_).count();

    if (actualIntervalMs >= STATS_INTERVAL_MS) {
        auto sinceLastChangeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsChangeTime_).count();
        bool needUpdate = false;

        if (sinceLastChangeMs > BLIND_PERIOD_MS) {
            // 修正 1：降帧逻辑
            if (intervalFramesDropped_ > 0) {
                // 不要用“区间实际帧数”去算，而是基于“当前设定值”打折
                // 否则一次抖动就会让 FPS 归零
                int newFps = static_cast<int>(currentFps_ * FPS_DOWN_RATIO);
                
                // 修正 2：保底帧率建议设为 5，设为 1 画面就彻底卡死了
                newFps = std::max(5, newFps); 
                
                if (newFps < currentFps_) {
                    currentFps_ = newFps;
                    needUpdate = true;
                    std::cout << "[Quality] Dropped frames detected. Downgrading FPS to " << currentFps_ << std::endl;
                }
            } 
            // 修正 3：升帧逻辑
            else if (intervalDecodeTimeMs_ < actualIntervalMs / 2) {
                // 在乘率基础上 + 2，确保从 1fps 或低 fps 能跳出来
                int newFps = std::min(MAX_FPS, static_cast<int>(currentFps_ * FPS_UP_RATIO + 2));
                
                if (newFps > currentFps_) {
                    currentFps_ = newFps;
                    needUpdate = true;
                    std::cout << "[Quality] Decoding is fast. Upgrading FPS to " << currentFps_ << std::endl;
                }
            }
        }

        if (needUpdate && transport_ && transport_->isConnected()) {
            int targetWidth = (pendingResizeWidth_ > 0) ? pendingResizeWidth_ : screenWidth_;
            // 确保你的协议函数名匹配（StreamConfigMsg 或 StreamConfig）
            auto msg = MessageBuilder::StreamConfigMsg(targetWidth, currentFps_, currentKfIntervalSec_);
            transport_->send(msg);
            lastFpsChangeTime_ = now;
        }

        // 重置区间统计（保持不变）
        intervalFramesDecoded_ = 0;
        intervalFramesDropped_ = 0;
        intervalDecodeTimeMs_ = 0;
        lastStatsTime_ = now;
    }
}

// --- 新增：冷却完成时发送配置 ---
void DesktopWindow::onResizeCooldown() {
    if (transport_ && transport_->isConnected() && pendingResizeWidth_ > 0) {
        std::cout << "[Desktop] Window resized, updating target width to: " << pendingResizeWidth_ << std::endl;
        auto msg = MessageBuilder::StreamConfigMsg(pendingResizeWidth_, currentFps_, currentKfIntervalSec_);
        transport_->send(msg);
    }
}

void DesktopWindow::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QImage imageToDraw;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (latestFrame_.isNull()) return;
        imageToDraw = latestFrame_; // 这里获取最新的帧
        hasNewFrame_ = false;
    }

    QPainter painter(this);
    
    // 1. 背景涂黑（处理比例不一致时的黑边）
    painter.fillRect(rect(), Qt::black);

    // 2. 核心：让图片自适应“当前窗口”的大小
    // 注意：这里使用的是 this->size() 而不是固定的大小
    // Qt::FastTransformation 保证了低延迟
    QImage scaledImg = imageToDraw.scaled(this->size(), 
                                          Qt::KeepAspectRatio, 
                                          Qt::FastTransformation);

    // 3. 计算居中位置
    int x = (width() - scaledImg.width()) / 2;
    int y = (height() - scaledImg.height()) / 2;

    // 4. 绘制到屏幕
    painter.drawImage(x, y, scaledImg);
}

QSize DesktopWindow::displayedImageSize() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (latestFrame_.isNull()) return QSize(0, 0);
    return latestFrame_.size().scaled(this->size(), Qt::KeepAspectRatio);
}