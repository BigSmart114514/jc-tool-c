#include "desktop_window.h"
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QPainter>
#include <QApplication>
#include <QScreen>
#include <iostream>

DesktopWindow::DesktopWindow(QWidget* parent)
    : QWidget(parent) {
    
    // 窗口设置
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(Qt::Window);
    
    QScreen* screen = QApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    resize(screenGeometry.width() * 3 / 4, screenGeometry.height() * 3 / 4);

    // 创建显示标签
    displayLabel_ = new QLabel(this);
    displayLabel_->setAlignment(Qt::AlignCenter);
    displayLabel_->setScaledContents(false);
    displayLabel_->setStyleSheet("QLabel { background-color: black; }");

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(displayLabel_);

    // 更新定时器
    updateTimer_ = new QTimer(this);
    connect(this, &DesktopWindow::frameReady, this, &DesktopWindow::updateDisplay);
    
    setMouseTracking(true);
    displayLabel_->setMouseTracking(true);
}

DesktopWindow::~DesktopWindow() {
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

void DesktopWindow::handleMessage(const BinaryData& data) {
    if (data.empty()) return;

    auto type = static_cast<Desktop::MsgType>(data[0]);

    switch (type) {
        case Desktop::MsgType::ScreenInfo:
            handleScreenInfo(data);
            break;

        case Desktop::MsgType::VideoFrame:
            if (data.size() > 2 && decoderReady_) {
                bool isKeyframe = data[1] != 0;
                handleVideoFrame(data.data() + 2, data.size() - 2, isKeyframe);
            }
            break;

        default:
            break;
    }
}

void DesktopWindow::handleScreenInfo(const BinaryData& data) {
    if (data.size() < 1 + sizeof(Desktop::ScreenInfo)) return;

    auto* info = reinterpret_cast<const Desktop::ScreenInfo*>(data.data() + 1);
    
    std::cout << "[Desktop] Screen: " << info->width << "x" << info->height << std::endl;

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

void DesktopWindow::handleVideoFrame(const uint8_t* data, size_t size, bool isKeyframe) {
    std::vector<uint8_t> rgbData;
    // 解码器输出的其实是 BGRA 格式，每像素 4 个字节
    if (decoder_.decode(data, static_cast<int>(size), rgbData)) {
        int w = decoder_.getWidth();
        int h = decoder_.getHeight();
        
        // 更新屏幕尺寸，用于鼠标坐标映射
        screenWidth_ = w;
        screenHeight_ = h;

        // 验证数据大小 (宽度 * 高度 * 4字节)
        if (rgbData.size() != static_cast<size_t>(w * h * 4)) {
            std::cerr << "[Desktop] Invalid BGRA data size: " << rgbData.size() 
                      << " expected: " << (w * h * 4) << std::endl;
            return;
        }

        // QImage::Format_RGB32 在小端机器上的内存排列正好是 B G R A
        // 宽度 * 4 是每一行的字节数 (bytesPerLine)
        QImage image(rgbData.data(), w, h, w * 4, QImage::Format_RGB32);
        
        // 必须进行深拷贝(copy)，因为 rgbData 是局部变量，函数结束时会被释放
        currentFrame_ = QPixmap::fromImage(image.copy());

        // 通知 UI 线程刷新画面
        emit frameReady();
    }
}

void DesktopWindow::updateDisplay() {
    if (!currentFrame_.isNull()) {
        // 计算缩放以保持宽高比
        QSize labelSize = displayLabel_->size();
        QPixmap scaled = currentFrame_.scaled(labelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        displayLabel_->setPixmap(scaled);
    }
}

void DesktopWindow::sendInput(const Desktop::InputEvent& ev) {
    if (transport_ && transport_->isConnected()) {
        auto msg = MessageBuilder::InputEvent(ev);
        transport_->send(msg);
    }
}

bool DesktopWindow::convertToImageCoords(int wx, int wy, int& ix, int& iy) {
    int ow = screenWidth_, oh = screenHeight_;
    if (ow == 0 || oh == 0) return false;

    QSize labelSize = displayLabel_->size();
    QSize imageSize = currentFrame_.size();
    
    if (imageSize.isEmpty()) return false;

    // 计算显示区域
    QSize scaledSize = imageSize.scaled(labelSize, Qt::KeepAspectRatio);
    int offsetX = (labelSize.width() - scaledSize.width()) / 2;
    int offsetY = (labelSize.height() - scaledSize.height()) / 2;

    // 转换为显示标签坐标
    QPoint labelPos = displayLabel_->mapFromParent(QPoint(wx, wy));
    int lx = labelPos.x();
    int ly = labelPos.y();

    // 检查是否在图像区域内
    if (lx < offsetX || lx >= offsetX + scaledSize.width() ||
        ly < offsetY || ly >= offsetY + scaledSize.height()) {
        return false;
    }

    // 转换为图像坐标
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
    if (event->button() == Qt::LeftButton) {
        key = 1; // Left down
        grabMouse();
    } else if (event->button() == Qt::RightButton) {
        key = 3; // Right down
    } else if (event->button() == Qt::MiddleButton) {
        key = 5; // Middle down
    }

    if (key > 0) {
        sendInput({0, x, y, key, 0});
    }
}

void DesktopWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (!pEnableMouseClick_ || !pEnableMouseClick_->load()) return;

    int x, y;
    if (!convertToImageCoords(event->pos().x(), event->pos().y(), x, y)) return;

    int key = 0;
    if (event->button() == Qt::LeftButton) {
        key = 2; // Left up
        releaseMouse();
    } else if (event->button() == Qt::RightButton) {
        key = 4; // Right up
    } else if (event->button() == Qt::MiddleButton) {
        key = 6; // Middle up
    }

    if (key > 0) {
        sendInput({0, x, y, key, 0});
    }
}

void DesktopWindow::mouseMoveEvent(QMouseEvent* event) {
    if (!pEnableMouseMove_ || !pEnableMouseMove_->load()) return;

    int x, y;
    if (convertToImageCoords(event->pos().x(), event->pos().y(), x, y)) {
        sendInput({0, x, y, 0, 0}); // Move
    }
}

void DesktopWindow::wheelEvent(QWheelEvent* event) {
    if (!pEnableMouseClick_ || !pEnableMouseClick_->load()) return;

    int x, y;
    if (convertToImageCoords(event->position().x(), event->position().y(), x, y)) {
        int delta = event->angleDelta().y();
        sendInput({0, x, y, 7, delta}); // Wheel
    }
}

void DesktopWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateDisplay();
}