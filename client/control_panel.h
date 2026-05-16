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
#include <memory>
#include "../common/protocol.h"
#include "../common/transport.h"
#include "desktop_window.h"

class SshSession;
class SftpWindow;
class SshTerminalWindow;

struct ControlPanelConfig {
    SshSession* sshSession = nullptr;
    ITransport* desktopTransport = nullptr;
    std::string modeText;
    std::string connectInfo;
    std::string sshHost;
    int sshPort = 2222;
    std::string sshUser;
    std::string sshPassword;
};

class ControlPanel : public QMainWindow {
    Q_OBJECT

public:
    explicit ControlPanel(QWidget* parent = nullptr);
    ~ControlPanel();

    void setConfig(const ControlPanelConfig& config);
    void setupDesktopTransportCallbacks();

private slots:
    void toggleDesktop();
    void toggleSftp();
    void toggleSshTerminal();
    void onExternalTerminal();
    void onDisconnect();
    void onDesktopWindowClosed();
    void onSftpWindowClosed();
    void onSshTerminalClosed();
    void onMouseMoveToggled(bool checked);
    void onMouseClickToggled(bool checked);
    void onKeyboardToggled(bool checked);

private:
    void createUI();
    void updateStatus(const QString& text);
    bool ensureDesktopConnected();

    QLabel* lblMode_;
    QLabel* lblInfo_;
    QStatusBar* statusBar_;
    QPushButton* btnDesktop_;
    QPushButton* btnSftp_;
    QPushButton* btnSshTerminal_;
    QPushButton* btnExternalTerminal_;
    QPushButton* btnDisconnect_;
    QCheckBox* chkMouseMove_;
    QCheckBox* chkMouseClick_;
    QCheckBox* chkKeyboard_;

    ControlPanelConfig config_;
    DesktopWindow* desktopWindow_ = nullptr;
    SftpWindow* sftpWindow_ = nullptr;
    SshTerminalWindow* sshTerminalWindow_ = nullptr;
    std::mutex windowMtx_;

    bool desktopOpen_ = false;
    bool sftpOpen_ = false;
    bool sshTerminalOpen_ = false;

    std::atomic<bool> enableMouseMove_{true};
    std::atomic<bool> enableMouseClick_{true};
    std::atomic<bool> enableKeyboard_{true};
};

#endif // CONTROL_PANEL_H