#ifndef CONTROL_PANEL_H
#define CONTROL_PANEL_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QStatusBar>
#include <QMessageBox>
#include <atomic>
#include <mutex>
#include "../common/protocol.h"
#include "../common/transport.h"
#include "desktop_window.h"
#include "file_window.h"

struct ControlPanelConfig {
    ITransport* desktopTransport = nullptr;
    ITransport* fileTransport = nullptr;
    std::string modeText;
    std::string connectInfo;
};

class ControlPanel : public QMainWindow {
    Q_OBJECT

public:
    explicit ControlPanel(QWidget* parent = nullptr);
    ~ControlPanel();

    void setConfig(const ControlPanelConfig& config);
    void setupTransportCallbacks();

private slots:
    void toggleDesktop();
    void toggleFileManager();
    void onDisconnect();
    void onDesktopWindowClosed();
    void onFileWindowClosed();
    void onMouseMoveToggled(bool checked);
    void onMouseClickToggled(bool checked);
    void onKeyboardToggled(bool checked);

private:
    void createUI();
    void updateStatus(const QString& text);
    bool ensureDesktopConnected();
    bool ensureFileConnected();

    // UI组件
    QLabel* lblMode_;
    QLabel* lblInfo_;
    QStatusBar* statusBar_;
    QPushButton* btnDesktop_;
    QPushButton* btnFileManager_;
    QPushButton* btnDisconnect_;
    QCheckBox* chkMouseMove_;
    QCheckBox* chkMouseClick_;
    QCheckBox* chkKeyboard_;

    // 配置和状态
    ControlPanelConfig config_;
    DesktopWindow* desktopWindow_ = nullptr;
    FileWindow* fileWindow_ = nullptr;
    std::mutex windowMtx_;

    bool desktopOpen_ = false;
    bool fileManagerOpen_ = false;

    std::atomic<bool> enableMouseMove_{true};
    std::atomic<bool> enableMouseClick_{true};
    std::atomic<bool> enableKeyboard_{true};
};

#endif // CONTROL_PANEL_H