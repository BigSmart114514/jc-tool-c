#include "desktop_window.h"
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QPainter>
#include <QApplication>
#include <QScreen>
#include <iostream>

DesktopWindow::DesktopWindow(QWidget* parent)
    : QWidget(parent) {
    
    // 窗口基础设置
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(Qt::Window);
    
    QScreen* screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    resize(screenGeometry.width() * 3 / 4, screenGeometry.height() * 3 / 4);

    // 创建显示容器
    displayLabel_ = new QLabel(this);
    displayLabel_->setAlignment(Qt::AlignCenter);
    displayLabel_->setScaledContents(false);
    displayLabel_->setStyleSheet("QLabel { background-color: black; }");

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(displayLabel_);

    // 绑定渲染信号
    connect(this, &DesktopWindow::frameReady, this, &DesktopWindow::updateDisplay);
    
    setMouseTracking(true);
    displayLabel_->setMouseTracking(true);

    // 启动独立解码线程
    decoding_ = true;
    decodeThread_ = std::thread(&DesktopWindow::decodeLoop, this);
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
                totalFramesReceived_++;

                // 如果积压超过 3 帧，认为处理速度跟不上，执行丢弃
                if (videoQueue_.size() > 3) {
                    droppedFrames_ += videoQueue_.size(); // 统计丢弃掉的帧数
                    std::queue<BinaryData> empty;
                    std::swap(videoQueue_, empty);
                }
                
                videoQueue_.push(data);
                queueCV_.notify_one();

                // 每隔 5 秒（或 150 帧）自动打印一次统计
                if (totalFramesReceived_ % 150 == 0) {
                    logStatistics();
                }
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

    // 更新原始分辨率信息
    screenWidth_ = info->width;
    screenHeight_ = info->height;

    decoder_.cleanup();
    if (decoder_.init(info->width, info->height)) {
        decoderReady_ = true;
        std::cout << "[Desktop] Decoder initialized" << std::endl;
    } else {
        std::cerr << "[Desktop] Decoder init failed" << std::endl;
        decoderReady_ = false;
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

        if (!decoderReady_) continue;

        // 跳过 MsgType(1B) 和 isKeyframe(1B)
        const uint8_t* rawH265 = data.data() + 2;
        size_t rawSize = data.size() - 2;

        if (decoder_.decode(rawH265, static_cast<int>(rawSize), rgbData)) {
            int w = decoder_.getWidth();
            int h = decoder_.getHeight();

            // 构造线程安全的 QImage
            // Format_RGB32 匹配 BGRA 排列
            QImage img(rgbData.data(), w, h, w * 4, QImage::Format_RGB32);
            
            {
                std::lock_guard<std::mutex> lock(frameMutex_);
                decodedFrames_++; // 记录解码成功
                latestFrame_ = img.copy(); // 深拷贝，解耦内存
                hasNewFrame_ = true;
                screenWidth_ = w;
                screenHeight_ = h;
            }
            emit frameReady(); // 触发 UI 渲染
        }
    }
}

// UI 渲染线程
void DesktopWindow::updateDisplay() {
    QImage imageToDraw;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (!hasNewFrame_) return; // 如果信号积压，直接跳过旧帧
        imageToDraw = latestFrame_;
        hasNewFrame_ = false;
    }

    if (!imageToDraw.isNull()) {
        QSize labelSize = displayLabel_->size();
        // 使用 FastTransformation 降低 CPU 缩放开销，彻底解决延迟
        QPixmap pix = QPixmap::fromImage(imageToDraw);
        displayLabel_->setPixmap(pix.scaled(labelSize, Qt::KeepAspectRatio, Qt::FastTransformation));
    }
}

void DesktopWindow::sendInput(const Desktop::InputEvent& ev) {
    if (transport_ && transport_->isConnected()) {
        auto msg = MessageBuilder::InputEvent(ev);
        transport_->send(msg);
    }
}

bool DesktopWindow::convertToImageCoords(int wx, int wy, int& ix, int& iy) {
    int ow, oh;
    QSize imageSize;

    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (latestFrame_.isNull()) return false;
        ow = screenWidth_;
        oh = screenHeight_;
        imageSize = latestFrame_.size();
    }

    if (ow == 0 || oh == 0) return false;

    QSize labelSize = displayLabel_->size();
    QSize scaledSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    
    int offsetX = (labelSize.width() - scaledSize.width()) / 2;
    int offsetY = (labelSize.height() - scaledSize.height()) / 2;

    QPoint labelPos = displayLabel_->mapFromParent(QPoint(wx, wy));
    int lx = labelPos.x();
    int ly = labelPos.y();

    if (lx < offsetX || lx >= offsetX + scaledSize.width() ||
        ly < offsetY || ly >= offsetY + scaledSize.height()) {
        return false;
    }

    float sx = (float)ow / scaledSize.width();
    float sy = (float)oh / scaledSize.height();
    ix = std::clamp((int)((lx - offsetX) * sx), 0, ow - 1);
    iy = std::clamp((int)((ly - offsetY) * sy), 0, oh - 1);
    
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
    updateDisplay();
}

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