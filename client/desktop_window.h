#ifndef DESKTOP_WINDOW_H
#define DESKTOP_WINDOW_H

#include <QWidget>
#include <QLabel>
#include <QPixmap>
#include <QImage>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <atomic>
#include "../common/protocol.h"
#include "../common/transport.h"
#include "hevc_decoder.h"

class DesktopWindow : public QWidget {
    Q_OBJECT

public:
    explicit DesktopWindow(QWidget* parent = nullptr);
    ~DesktopWindow();

    void init(ITransport* transport);
    void handleMessage(const BinaryData& data);
    void requestStream();

    void setInputToggles(std::atomic<bool>* mouseMove,
                        std::atomic<bool>* mouseClick,
                        std::atomic<bool>* keyboard) {
        pEnableMouseMove_ = mouseMove;
        pEnableMouseClick_ = mouseClick;
        pEnableKeyboard_ = keyboard;
    }

signals:
    void closed();
    void openFileManager();
    void frameReady();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void updateDisplay();

private:
    void handleVideoFrame(const uint8_t* data, size_t size, bool isKeyframe);
    void handleScreenInfo(const BinaryData& data);
    void sendInput(const Desktop::InputEvent& ev);
    bool convertToImageCoords(int wx, int wy, int& ix, int& iy);

    QLabel* displayLabel_;
    QPixmap currentFrame_;
    
    ITransport* transport_ = nullptr;
    HEVCDecoder decoder_;

    std::atomic<int> screenWidth_{0};
    std::atomic<int> screenHeight_{0};
    bool decoderReady_ = false;

    std::atomic<bool>* pEnableMouseMove_ = nullptr;
    std::atomic<bool>* pEnableMouseClick_ = nullptr;
    std::atomic<bool>* pEnableKeyboard_ = nullptr;

    QTimer* updateTimer_;
};

#endif // DESKTOP_WINDOW_H