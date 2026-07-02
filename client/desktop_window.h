#ifndef DESKTOP_WINDOW_H
#define DESKTOP_WINDOW_H

#include <QWidget>
#include <QImage>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <QTimer>

#include "../common/transport.h"
#include "../common/protocol.h"
#include "media_decoder.h"

struct InputControlState;

class DesktopWindow : public QWidget {
    Q_OBJECT

public:
    explicit DesktopWindow(QWidget* parent = nullptr);
    ~DesktopWindow();

    void init(ITransport* transport, InputControlState* inputState);
    void requestStream();
    void handleMessage(const BinaryData& data);

    QSize displayedImageSize();

signals:
    void frameReady();
    void closed();

private slots:
    void updateDisplay();
    void onResizeCooldown();

private:
    std::thread decodeThread_;
    std::atomic<bool> decoding_{false};
    std::mutex queueMtx_;
    std::condition_variable queueCV_;
    std::queue<BinaryData> videoQueue_;

    MediaDecoder decoder_;
    bool decoderReady_ = false;
    QImage latestFrame_;
    std::mutex frameMutex_;
    bool hasNewFrame_ = false;

    int screenWidth_ = 0;
    int screenHeight_ = 0;
    ITransport* transport_ = nullptr;
    InputControlState* inputState_ = nullptr;

    const int MAX_FPS = 30;
    const double FPS_UP_RATIO = 1.5;
    const double FPS_DOWN_RATIO = 0.7;
    const int STATS_INTERVAL_MS = 5000;
    const int BLIND_PERIOD_MS = 2000;
    const int RESIZE_COOLDOWN_MS = 1000;

    int currentFps_ = 30;
    int currentKfIntervalSec_ = 5;
    int pendingResizeWidth_ = 0;
    QTimer resizeTimer_;

    uint64_t intervalFramesDecoded_ = 0;
    uint64_t intervalFramesDropped_ = 0;
    uint64_t intervalDecodeTimeMs_ = 0;
    std::chrono::steady_clock::time_point lastStatsTime_;
    std::chrono::steady_clock::time_point lastFpsChangeTime_;
    std::chrono::steady_clock::time_point frameReadyTime_;

    std::mutex decoderMtx_;

    void joinDecodeThread();
    bool initialStreamConfigured_ = false;

    void checkAndAdjustStreamQuality();
    void logStatistics();
    void decodeLoop();
    void handleScreenInfo(const BinaryData& data);
    void sendInput(const Desktop::InputEvent& ev);
    bool convertToImageCoords(int wx, int wy, int& ix, int& iy);

protected:
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void paintEvent(QPaintEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
};

#endif
