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
#include <QMutex>
#include <atomic>
#include <memory>
#include <vector>
#include "../common/protocol.h"
#include "../common/transport.h"

class SshSession;
class SftpWindow;
class SshTerminalWindow;
class DesktopWindow;

struct InputControlState {
    std::atomic<bool> mouseMove{true};
    std::atomic<bool> mouseClick{true};
    std::atomic<bool> keyboard{true};
};

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
    void onAudioToggled(bool checked);
    void onScreenOff();
    void onShutdownServer();

private:
    void createUI();
    void updateStatus(const QString& text);
    bool ensureDesktopConnected();
    void sendAudioEnable(bool enabled);

    void destroyAllWindows();
    void closeDesktopWindow();
    void closeSftpWindow();
    void closeSshTerminalWindow();
    void retireWindow(std::unique_ptr<QWidget> window);

    QLabel* lblMode_;
    QLabel* lblInfo_;
    QStatusBar* statusBar_;
    QPushButton* btnDesktop_;
    QPushButton* btnSftp_;
    QPushButton* btnScreenOff_;
    QPushButton* btnShutdown_;
    QPushButton* btnSshTerminal_;
    QPushButton* btnExternalTerminal_;
    QPushButton* btnDisconnect_;
    QCheckBox* chkMouseMove_;
    QCheckBox* chkMouseClick_;
    QCheckBox* chkKeyboard_;
    QCheckBox* chkAudio_;

    ControlPanelConfig config_;

    // Declared before the windows so they outlive the objects they reference
    // during teardown, even if a window destructor runs as a member.
    QMutex windowMtx_;
    InputControlState inputState_;

    std::unique_ptr<DesktopWindow> desktopWindow_;
    std::unique_ptr<SftpWindow> sftpWindow_;
    std::unique_ptr<SshTerminalWindow> sshTerminalWindow_;

    // Windows that emitted closed() from inside closeEvent(). They cannot be
    // destroyed until the event handler returns, so they are retired here and
    // deleted on the next event-loop turn.
    std::vector<std::unique_ptr<QWidget>> retiredWindows_;
};

#endif
