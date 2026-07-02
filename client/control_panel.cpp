#include "control_panel.h"
#include "sftp_window.h"
#include "ssh_terminal.h"
#include "desktop_window.h"
#include "../common/ssh_session.h"
#include <QGroupBox>
#include <QFrame>
#include <QApplication>
#include <QMessageBox>
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
    QMutexLocker lock(&windowMtx_);
    auto safeDelete = [](auto& ptr) {
        if (ptr) {
            auto raw = ptr.data(); ptr.clear();
            if (raw) { raw->close(); delete raw; }
        }
    };
    safeDelete(desktopWindow_);
    safeDelete(sftpWindow_);
    safeDelete(sshTerminalWindow_);
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
        auto* toDelete = desktopWindow_.data();
        { QMutexLocker lock(&windowMtx_); desktopWindow_.clear(); }
        toDelete->close();
        delete toDelete;
        btnDesktop_->setText("Remote Desktop");
        updateStatus("Desktop closed");
        return;
    }

    if (!ensureDesktopConnected()) return;

    auto* dw = new DesktopWindow();
    dw->init(config_.desktopTransport, &inputState_);
    connect(dw, &DesktopWindow::closed, this, &ControlPanel::onDesktopWindowClosed);
    dw->setWindowTitle("Remote Desktop [" + QString::fromStdString(config_.modeText) + "]");
    dw->show();

    { QMutexLocker lock(&windowMtx_); desktopWindow_ = dw; }
    btnDesktop_->setText("Close Desktop");
    updateStatus("Desktop opened");
    dw->requestStream();

    if (chkAudio_->isChecked()) {
        sendAudioEnable(true);
    }
}

void ControlPanel::toggleSftp() {
    if (sftpWindow_) {
        auto* toDelete = sftpWindow_.data();
        { QMutexLocker lock(&windowMtx_); sftpWindow_.clear(); }
        toDelete->close();
        delete toDelete;
        btnSftp_->setText("SFTP File Manager");
        updateStatus("SFTP closed");
        return;
    }

    if (!config_.sshSession || !config_.sshSession->isConnected()) {
        QMessageBox::critical(this, "Error", "SSH not connected");
        return;
    }

    auto* sw = new SftpWindow(config_.sshSession);
    connect(sw, &SftpWindow::closed, this, &ControlPanel::onSftpWindowClosed);
    sw->show();
    sw->navigateTo("/");

    { QMutexLocker lock(&windowMtx_); sftpWindow_ = sw; }
    btnSftp_->setText("Close SFTP");
    updateStatus("SFTP opened");
}

void ControlPanel::toggleSshTerminal() {
    if (sshTerminalWindow_) {
        auto* toDelete = sshTerminalWindow_.data();
        { QMutexLocker lock(&windowMtx_); sshTerminalWindow_.clear(); }
        toDelete->close();
        delete toDelete;
        btnSshTerminal_->setText("SSH Terminal");
        updateStatus("SSH Terminal closed");
        return;
    }

    auto* termSession = new SshSession();
    if (!termSession->connect(config_.sshHost, config_.sshPort,
                               config_.sshUser, config_.sshPassword)) {
        QMessageBox::critical(this, "Terminal Error",
            QString("SSH connection failed: %1")
                .arg(QString::fromStdString(termSession->getError())));
        delete termSession;
        return;
    }

    auto* tw = new SshTerminalWindow(std::unique_ptr<SshSession>(termSession));
    connect(tw, &SshTerminalWindow::closed, this, &ControlPanel::onSshTerminalClosed);
    tw->show();

    { QMutexLocker lock(&windowMtx_); sshTerminalWindow_ = tw; }
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
    btnDesktop_->setText("Remote Desktop");
    updateStatus("Desktop closed");
    sendAudioEnable(false);
    QMutexLocker lock(&windowMtx_);
    desktopWindow_.clear();
}

void ControlPanel::onSftpWindowClosed() {
    btnSftp_->setText("SFTP File Manager");
    updateStatus("SFTP closed");
}

void ControlPanel::onSshTerminalClosed() {
    btnSshTerminal_->setText("SSH Terminal");
    updateStatus("SSH Terminal closed");
}

void ControlPanel::onDisconnect() {
    auto reply = QMessageBox::question(this, "Confirm",
        "Disconnect and exit?", QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    auto safeDelete = [](auto& ptr) {
        if (ptr) {
            auto raw = ptr.data(); ptr.clear();
            if (raw) { raw->close(); delete raw; }
        }
    };
    {
        QMutexLocker lock(&windowMtx_);
        safeDelete(desktopWindow_);
        safeDelete(sftpWindow_);
        safeDelete(sshTerminalWindow_);
    }

    if (config_.desktopTransport) config_.desktopTransport->disconnect();
    if (config_.sshSession) config_.sshSession->disconnect();

    close();
    qApp->quit();
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
