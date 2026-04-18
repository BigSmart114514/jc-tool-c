#ifndef DESKTOP_WINDOW_H
#define DESKTOP_WINDOW_H

#include <QWidget>
#include <QLabel>
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
#include "hevc_decoder.h"

class DesktopWindow : public QWidget {
    Q_OBJECT

public:
    explicit DesktopWindow(QWidget* parent = nullptr);
    ~DesktopWindow();

    void init(ITransport* transport);
    void requestStream();
    void handleMessage(const BinaryData& data);

    // 解决 control_panel.cpp 报错：增加这个成员函数
    void setInputToggles(std::atomic<bool>* mm, std::atomic<bool>* mc, std::atomic<bool>* kb) {
        pEnableMouseMove_ = mm;
        pEnableMouseClick_ = mc;
        pEnableKeyboard_ = kb;
    }

    std::atomic<bool>* pEnableMouseMove_ = nullptr;
    std::atomic<bool>* pEnableMouseClick_ = nullptr;
    std::atomic<bool>* pEnableKeyboard_ = nullptr;

signals:
    void frameReady();
    void closed();
    void openFileManager();

private slots:
    void updateDisplay();
    void onResizeCooldown();

private:
    std::thread decodeThread_;
    std::atomic<bool> decoding_{false};
    std::mutex queueMtx_;
    std::condition_variable queueCV_;
    std::queue<BinaryData> videoQueue_;

    // --- 关键修正：类名必须匹配 hevc_decoder.h 中的 HEVCDecoder ---
    HEVCDecoder decoder_; 
    // -------------------------------------------------------
    
    bool decoderReady_ = false;
    QImage latestFrame_;
    std::mutex frameMutex_;
    bool hasNewFrame_ = false;

    int screenWidth_ = 0;
    int screenHeight_ = 0;
    ITransport* transport_ = nullptr;
    QLabel* displayLabel_ = nullptr;

    // --- 统计相关 ---
    uint64_t totalFramesReceived_ = 0; // 总接收帧数
    uint64_t droppedFrames_ = 0;       // 丢弃帧数（由于队列积压）
    uint64_t decodedFrames_ = 0;       // 成功解码帧数
    int64_t lastLogTime_ = 0;          // 上次打印时间

    // --- 新增：动态流控常量 ---
    const int MAX_FPS = 30;
    const double FPS_UP_RATIO = 1.5;
    const double FPS_DOWN_RATIO = 0.7;
    const int STATS_INTERVAL_MS = 5000;  // 统计间隔：5秒
    const int BLIND_PERIOD_MS = 2000;    // 盲区间隔：2秒
    const int RESIZE_COOLDOWN_MS = 1000; // 窗口改变冷却：1秒

    // --- 新增：动态流控变量 ---
    int currentFps_ = 30;
    int currentKfIntervalSec_ = 5; // 默认5秒关键帧
    int pendingResizeWidth_ = 0;
    QTimer resizeTimer_;

    // 统计相关
    uint64_t intervalFramesDecoded_ = 0;
    uint64_t intervalFramesDropped_ = 0;
    uint64_t intervalDecodeTimeMs_ = 0;
    std::chrono::steady_clock::time_point lastStatsTime_;
    std::chrono::steady_clock::time_point lastFpsChangeTime_;

    void checkAndAdjustStreamQuality(); // 新增：评估性能并调整逻辑

    void logStatistics();               // 打印函数
    void decodeLoop();
    void handleScreenInfo(const BinaryData& data);
    void sendInput(const Desktop::InputEvent& ev);
    bool convertToImageCoords(int wx, int wy, int& ix, int& iy);

protected:
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