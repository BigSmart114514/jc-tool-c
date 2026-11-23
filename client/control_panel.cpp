#include "control_panel.h"
#include "sftp_window.h"
#include "ssh_terminal.h"
#include "desktop_window.h"
#include "../common/ssh_session.h"
#include <QGroupBox>
#include <QFrame>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <iostream>
#include <wchar.h>

static std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);
    return wstr;
}

ControlPanel::ControlPanel(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("JC Tool - Remote Control");
    setFixedSize(350, 500);
    createUI();
}

ControlPanel::~ControlPanel() {
    // ClientApplication disconnects the transport before this destructor runs,
    // so the receive callback can no longer reach the panel. Moving the windows
    // out under the lock still makes teardown safe if that ever changes.
    destroyAllWindows();
}

void ControlPanel::destroyAllWindows() {
    std::unique_ptr<DesktopWindow> desktop;
    {
        QMutexLocker lock(&windowMtx_);
        desktop = std::move(desktopWindow_);
    }
    auto sftp = std::move(sftpWindow_);
    auto terminal = std::move(sshTerminalWindow_);
    retiredWindows_.clear();

    // The locals are destroyed here, outside the lock. Destructors join worker
    // threads and release SSH resources.
}

void ControlPanel::closeDesktopWindow() {
    std::unique_ptr<DesktopWindow> window;
    {
        QMutexLocker lock(&windowMtx_);
        window = std::move(desktopWindow_);
    }
    if (window) {
        // closeEvent() sends ClientDisconnect and emits closed(). The member is
        // already null, so the slot only updates the UI; this local owns the
        // window until the event handler has returned.
        window->close();
    }
}

void ControlPanel::closeSftpWindow() {
    auto window = std::move(sftpWindow_);
    if (window) {
        window->close();
    }
}

void ControlPanel::closeSshTerminalWindow() {
    auto window = std::move(sshTerminalWindow_);
    if (window) {
        window->close();
    }
}

void ControlPanel::retireWindow(std::unique_ptr<QWidget> window) {
    if (!window) return;

    retiredWindows_.push_back(std::move(window));
    if (retiredWindows_.size() == 1) {
        // closed() is emitted synchronously from closeEvent(), so defer actual
        // deletion until the event handler has returned.
        QTimer::singleShot(0, this, [this]() {
            retiredWindows_.clear();
        });
    }
}

void ControlPanel::createUI() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(15, 10, 15, 10);
    mainLayout->setSpacing(10);

    QGroupBox* connectionGroup = new QGroupBox("Connection");
    QVBoxLayout* connLayout = new QVBoxLayout(connectionGroup);
    lblMode_ = new QLabel("Mode: ");
    connLayout->addWidget(lblMode_);
    lblInfo_ = new QLabel("");
    lblInfo_->setWordWrap(true);
    connLayout->addWidget(lblInfo_);
    mainLayout->addWidget(connectionGroup);

    QFrame* line1 = new QFrame(this);
    line1->setFrameShape(QFrame::HLine);
    line1->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line1);

    QGroupBox* windowGroup = new QGroupBox("Functions");
    QVBoxLayout* windowLayout = new QVBoxLayout(windowGroup);

    btnDesktop_ = new QPushButton("Remote Desktop");
    btnDesktop_->setMinimumHeight(32);
    connect(btnDesktop_, &QPushButton::clicked, this, &ControlPanel::toggleDesktop);
    windowLayout->addWidget(btnDesktop_);

    btnSshTerminal_ = new QPushButton("SSH Terminal");
    btnSshTerminal_->setMinimumHeight(32);
    connect(btnSshTerminal_, &QPushButton::clicked, this, &ControlPanel::toggleSshTerminal);
    windowLayout->addWidget(btnSshTerminal_);

    btnExternalTerminal_ = new QPushButton("External Terminal (Native)");
    btnExternalTerminal_->setMinimumHeight(32);
    connect(btnExternalTerminal_, &QPushButton::clicked, this, &ControlPanel::onExternalTerminal);
    windowLayout->addWidget(btnExternalTerminal_);

    btnSftp_ = new QPushButton("SFTP File Manager");
    btnSftp_->setMinimumHeight(32);
    connect(btnSftp_, &QPushButton::clicked, this, &ControlPanel::toggleSftp);
    windowLayout->addWidget(btnSftp_);

    btnScreenOff_ = new QPushButton("熄灭显示器");
    btnScreenOff_->setMinimumHeight(32);
    connect(btnScreenOff_, &QPushButton::clicked, this, &ControlPanel::onScreenOff);
    windowLayout->addWidget(btnScreenOff_);

    btnShutdown_ = new QPushButton("关闭远程服务器");
    btnShutdown_->setMinimumHeight(32);
    connect(btnShutdown_, &QPushButton::clicked, this, &ControlPanel::onShutdownServer);
    windowLayout->addWidget(btnShutdown_);

    mainLayout->addWidget(windowGroup);

    QFrame* line2 = new QFrame(this);
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line2);

    QGroupBox* inputGroup = new QGroupBox("Desktop Input Control");
    QVBoxLayout* inputLayout = new QVBoxLayout(inputGroup);

    chkMouseMove_ = new QCheckBox("Mouse Move");
    chkMouseMove_->setChecked(true);
    connect(chkMouseMove_, &QCheckBox::toggled, this, &ControlPanel::onMouseMoveToggled);
    inputLayout->addWidget(chkMouseMove_);

    chkMouseClick_ = new QCheckBox("Mouse Click");
    chkMouseClick_->setChecked(true);
    connect(chkMouseClick_, &QCheckBox::toggled, this, &ControlPanel::onMouseClickToggled);
    inputLayout->addWidget(chkMouseClick_);

    chkKeyboard_ = new QCheckBox("Keyboard");
    chkKeyboard_->setChecked(true);
    connect(chkKeyboard_, &QCheckBox::toggled, this, &ControlPanel::onKeyboardToggled);
    inputLayout->addWidget(chkKeyboard_);

    chkAudio_ = new QCheckBox("Enable Audio (system sound)");
    chkAudio_->setChecked(false);
    connect(chkAudio_, &QCheckBox::toggled, this, &ControlPanel::onAudioToggled);
    inputLayout->addWidget(chkAudio_);

    mainLayout->addWidget(inputGroup);
    mainLayout->addStretch();

    btnDisconnect_ = new QPushButton("Disconnect && Exit");
    btnDisconnect_->setMinimumHeight(32);
    connect(btnDisconnect_, &QPushButton::clicked, this, &ControlPanel::onDisconnect);
    mainLayout->addWidget(btnDisconnect_);

    statusBar_ = new QStatusBar(this);
    setStatusBar(statusBar_);
    updateStatus("Ready");
}

void ControlPanel::setConfig(const ControlPanelConfig& config) {
    config_ = config;
    lblMode_->setText(QString("Mode: %1").arg(QString::fromStdString(config.modeText)));
    lblInfo_->setText(QString::fromStdString(config.connectInfo));
}

void ControlPanel::setupDesktopTransportCallbacks() {
    if (!config_.desktopTransport) return;
    TransportCallbacks cb;
    cb.onConnected = []() { std::cout << "[Desktop] Connected" << std::endl; };
    cb.onDisconnected = []() { std::cout << "[Desktop] Disconnected" << std::endl; };
    cb.onMessage = [this](const BinaryData& data) {
        QMutexLocker lock(&windowMtx_);
        if (desktopWindow_) desktopWindow_->handleMessage(data);
    };
    config_.desktopTransport->setCallbacks(cb);
}

void ControlPanel::updateStatus(const QString& text) {
    statusBar_->showMessage("Status: " + text);
}

bool ControlPanel::ensureDesktopConnected() {
    if (!config_.desktopTransport) {
        QMessageBox::critical(this, "Error", "Desktop transport not configured");
        return false;
    }
    if (config_.desktopTransport->isConnected()) return true;

    updateStatus("Reconnecting desktop...");
    btnDesktop_->setEnabled(false);
    bool result = config_.desktopTransport->reconnect();
    btnDesktop_->setEnabled(true);

    if (!result) {
        QMessageBox::critical(this, "Connection Error",
            "Failed to reconnect to remote desktop.");
        updateStatus("Desktop connection failed");
        return false;
    }
    updateStatus("Desktop reconnected");
    return true;
}

void ControlPanel::toggleDesktop() {
    if (desktopWindow_) {
        closeDesktopWindow();
        return;
    }

    if (!ensureDesktopConnected()) return;

    auto dw = std::make_unique<DesktopWindow>();
    dw->init(config_.desktopTransport, &inputState_);
    connect(dw.get(), &DesktopWindow::closed, this, &ControlPanel::onDesktopWindowClosed);
    dw->setWindowTitle("Remote Desktop [" + QString::fromStdString(config_.modeText) + "]");

    DesktopWindow* window = dw.get();
    {
        QMutexLocker lock(&windowMtx_);
        desktopWindow_ = std::move(dw);
    }

    // Publish the window before sending ClientReady: the transport callback
    // drops incoming messages while desktopWindow_ is null, and the server
    // sends the initial ScreenInfo only in response to ClientReady.
    window->show();
    window->requestStream();

    btnDesktop_->setText("Close Desktop");
    updateStatus("Desktop opened");

    if (chkAudio_->isChecked()) {
        sendAudioEnable(true);
    }
}

void ControlPanel::toggleSftp() {
    if (sftpWindow_) {
        closeSftpWindow();
        return;
    }

    auto sftpSession = std::make_unique<SshSession>();
    if (!sftpSession->connect(config_.sshHost, config_.sshPort,
                              config_.sshUser, config_.sshPassword)) {
        QMessageBox::critical(this, "SFTP Error",
            QString("SSH connection failed: %1")
                .arg(QString::fromStdString(sftpSession->getError())));
        return;
    }

    // The DirectDesktop session keeps its own channel busy for the lifetime
    // of the connection, so SFTP gets a dedicated SSH connection.
    auto sw = std::make_unique<SftpWindow>(std::move(sftpSession));
    connect(sw.get(), &SftpWindow::closed, this, &ControlPanel::onSftpWindowClosed);

    SftpWindow* window = sw.get();
    sftpWindow_ = std::move(sw);
    window->show();
    window->navigateTo("/");
    btnSftp_->setText("Close SFTP");
    updateStatus("SFTP opened");
}

void ControlPanel::toggleSshTerminal() {
    if (sshTerminalWindow_) {
        closeSshTerminalWindow();
        return;
    }

    auto termSession = std::make_unique<SshSession>();
    if (!termSession->connect(config_.sshHost, config_.sshPort,
                               config_.sshUser, config_.sshPassword)) {
        QMessageBox::critical(this, "Terminal Error",
            QString("SSH connection failed: %1")
                .arg(QString::fromStdString(termSession->getError())));
        return;
    }

    auto tw = std::make_unique<SshTerminalWindow>(std::move(termSession));
    connect(tw.get(), &SshTerminalWindow::closed, this, &ControlPanel::onSshTerminalClosed);

    SshTerminalWindow* window = tw.get();
    sshTerminalWindow_ = std::move(tw);
    window->show();

    btnSshTerminal_->setText("Close Terminal");
    updateStatus("SSH Terminal opened");
}

void ControlPanel::onExternalTerminal() {
    std::string sshCmd = "ssh -o StrictHostKeyChecking=accept-new "
        + config_.sshUser + "@" + config_.sshHost
        + " -p " + std::to_string(config_.sshPort);

    std::wstring wcmd = widen(sshCmd);

    auto hasWindowsTerminal = []() -> bool {
        wchar_t path[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return false;
        wcscat_s(path, L"\\Microsoft\\WindowsApps\\wt.exe");
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    };

    STARTUPINFOW si = { sizeof(si), 0 };
    PROCESS_INFORMATION pi = {};

    if (hasWindowsTerminal()) {
        std::wstring wtCmd = L"wt.exe " + wcmd;
        std::vector<wchar_t> buf(wtCmd.begin(), wtCmd.end());
        buf.push_back(0);
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
            std::cerr << "[ExternalTerminal] wt.exe failed, fallback to conhost" << std::endl;
        } else {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            updateStatus("External Terminal launched (Windows Terminal)");
            return;
        }
    }

    std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) {
        QMessageBox::critical(this, "External Terminal Error",
            "Failed to launch ssh.exe. Make sure OpenSSH Client is installed.");
        updateStatus("External Terminal failed");
        return;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    updateStatus("External Terminal launched (conhost)");
}

void ControlPanel::onDesktopWindowClosed() {
    std::unique_ptr<DesktopWindow> window;
    {
        QMutexLocker lock(&windowMtx_);
        window = std::move(desktopWindow_);
    }

    sendAudioEnable(false);
    btnDesktop_->setText("Remote Desktop");
    updateStatus("Desktop closed");

    // If this was a title-bar close, the member still owned the window and it
    // is still inside closeEvent(). Retire it so it is deleted after returning.
    // For a panel-initiated close the member is already null and the caller
    // owns the window.
    retireWindow(std::move(window));
}

void ControlPanel::onSftpWindowClosed() {
    auto window = std::move(sftpWindow_);
    btnSftp_->setText("SFTP File Manager");
    updateStatus("SFTP closed");
    retireWindow(std::move(window));
}

void ControlPanel::onSshTerminalClosed() {
    auto window = std::move(sshTerminalWindow_);
    btnSshTerminal_->setText("SSH Terminal");
    updateStatus("SSH Terminal closed");
    retireWindow(std::move(window));
}

void ControlPanel::onDisconnect() {
    auto reply = QMessageBox::question(this, "Confirm",
        "Disconnect and exit?", QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    btnDisconnect_->setEnabled(false);
    updateStatus("Disconnecting...");
    QTimer::singleShot(0, this, [this]() {
        // Stop the receive thread first: its onMessage callback captures this.
        if (config_.desktopTransport) config_.desktopTransport->disconnect();
        if (config_.sshSession) config_.sshSession->disconnect();

        destroyAllWindows();

        close();
        qApp->quit();
    });
}

void ControlPanel::onMouseMoveToggled(bool checked) { inputState_.mouseMove = checked; }
void ControlPanel::onMouseClickToggled(bool checked) { inputState_.mouseClick = checked; }
void ControlPanel::onKeyboardToggled(bool checked) { inputState_.keyboard = checked; }

void ControlPanel::onAudioToggled(bool checked) {
    sendAudioEnable(checked);
}

void ControlPanel::sendAudioEnable(bool enabled) {
    if (config_.desktopTransport && config_.desktopTransport->isConnected()) {
        auto msg = MessageBuilder::AudioEnableMsg(enabled);
        config_.desktopTransport->send(msg);
    }
    updateStatus(enabled ? "Audio enabled" : "Audio disabled");
}

void ControlPanel::onScreenOff() {
    if (!config_.desktopTransport || !config_.desktopTransport->isConnected()) {
        QMessageBox::warning(this, "熄屏", "未连接到远程桌面");
        return;
    }
    auto msg = MessageBuilder::ScreenOff();
    config_.desktopTransport->send(msg);
    updateStatus("已发送熄屏指令");
}

void ControlPanel::onShutdownServer() {
    if (!config_.desktopTransport || !config_.desktopTransport->isConnected()) {
        QMessageBox::warning(this, "关闭服务器", "未连接到远程桌面");
        return;
    }
    updateStatus("正在关闭远程服务器...");
    btnShutdown_->setEnabled(false);
    auto msg = MessageBuilder::ShutdownCmd();
    config_.desktopTransport->send(msg);
    QTimer::singleShot(0, this, [this]() {
        // Stop the receive thread first: its onMessage callback captures this.
        if (config_.desktopTransport) config_.desktopTransport->disconnect();
        if (config_.sshSession) config_.sshSession->disconnect();

        destroyAllWindows();

        close();
        qApp->quit();
    });
}
