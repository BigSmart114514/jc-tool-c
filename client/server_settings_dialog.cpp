#include "server_settings_dialog.h"
#include "service_manager_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFrame>
#include <QApplication>
#include <QPointer>
#include <thread>
#include <windows.h>
#include <winsvc.h>

static bool IsServiceInstalledScm() {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"EasyTier", SERVICE_QUERY_STATUS);
    bool found = (svc != NULL);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return found;
}

static bool StartServiceViaScm() {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"EasyTier", SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return false; }

    SERVICE_STATUS_PROCESS ssp;
    DWORD needed;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (BYTE*)&ssp, sizeof(ssp), &needed)) {
        if (ssp.dwCurrentState == SERVICE_RUNNING) {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return true;
        }
    }

    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return err == ERROR_SERVICE_ALREADY_RUNNING;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

ServerSettingsDialog::ServerSettingsDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Server Settings");
    setMinimumWidth(450);
    createUI();
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setTimerType(Qt::CoarseTimer);
    connect(refreshTimer_, &QTimer::timeout, this, &ServerSettingsDialog::refreshEasyTierStatus);
    refreshTimer_->start(3000);
    QTimer::singleShot(100, this, &ServerSettingsDialog::refreshEasyTierStatus);
}

void ServerSettingsDialog::createUI() {
    auto* main = new QVBoxLayout(this);
    main->setSpacing(10);

    // EasyTier Service section �?always visible, required for SSH
    auto* svcGroup = new QGroupBox("EasyTier Service (required for SSH)");
    auto* svcLayout = new QVBoxLayout(svcGroup);

    auto* statusLine = new QHBoxLayout();
    lblStatusIcon_ = new QLabel("\xe2\x97\x8f");
    lblStatusIcon_->setStyleSheet("font-size: 20px; color: #888;");
    lblStatusIcon_->setFixedWidth(24);
    statusLine->addWidget(lblStatusIcon_);

    auto* statusInfo = new QVBoxLayout();
    lblStatusText_ = new QLabel("Checking...");
    lblStatusText_->setStyleSheet("font-weight: bold; font-size: 13px;");
    lblIp_ = new QLabel("");
    lblIp_->setStyleSheet("color: #555;");
    statusInfo->addWidget(lblStatusText_);
    statusInfo->addWidget(lblIp_);
    statusLine->addLayout(statusInfo);
    statusLine->addStretch();
    svcLayout->addLayout(statusLine);

    auto* svcBtnLayout = new QHBoxLayout();
    btnStartService_ = new QPushButton("Start Service");
    btnStartService_->setMinimumHeight(30);
    connect(btnStartService_, &QPushButton::clicked, this, &ServerSettingsDialog::onStartService);
    svcBtnLayout->addWidget(btnStartService_);

    btnManageService_ = new QPushButton("Manage...");
    btnManageService_->setMinimumHeight(30);
    connect(btnManageService_, &QPushButton::clicked, this, &ServerSettingsDialog::onManageService);
    svcBtnLayout->addWidget(btnManageService_);
    svcBtnLayout->addStretch();
    svcLayout->addLayout(svcBtnLayout);

    main->addWidget(svcGroup);

    // EasyTier VPN toggle
    chkEasyTier_ = new QCheckBox("Enable EasyTier VPN (for P2P connection)");
    connect(chkEasyTier_, &QCheckBox::toggled, this, &ServerSettingsDialog::onEasyTierToggled);
    main->addWidget(chkEasyTier_);

    // SSH Server group
    auto* sshGroup = new QGroupBox("SSH Server");
    auto* sshForm = new QFormLayout(sshGroup);

    leSshPort_ = new QLineEdit("2222");
    leSshPassword_ = new QLineEdit();
    leSshPassword_->setEchoMode(QLineEdit::Password);
    leSshPassword_->setPlaceholderText("Set SSH password");

    sshForm->addRow("Port:", leSshPort_);
    sshForm->addRow("Password:", leSshPassword_);
    main->addWidget(sshGroup);

    // Desktop Streaming group
    auto* desktopGroup = new QGroupBox("Desktop Streaming");
    auto* desktopForm = new QFormLayout(desktopGroup);

    leDesktopPort_ = new QLineEdit("12345");
    desktopForm->addRow("Desktop Port:", leDesktopPort_);
    main->addWidget(desktopGroup);

    main->addStretch();

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    main->addWidget(line);

    // Buttons
    auto* btnLayout = new QHBoxLayout();
    auto* btnStart = new QPushButton("Start Server");
    btnStart->setMinimumHeight(36);
    btnStart->setStyleSheet("background-color: #5cb85c; color: white; font-weight: bold;");
    connect(btnStart, &QPushButton::clicked, this, [this]() {
        if (leSshPassword_->text().isEmpty()) {
            QMessageBox::warning(this, "Validation", "SSH password cannot be empty");
            return;
        }
        accept();
    });

    auto* btnCancel = new QPushButton("Cancel");
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);

    btnLayout->addStretch();
    btnLayout->addWidget(btnStart);
    btnLayout->addWidget(btnCancel);
    main->addLayout(btnLayout);

    onEasyTierToggled(false);
}

void ServerSettingsDialog::runAsync(std::function<void()> fn) {
    std::thread([fn = std::move(fn)]() {
        fn();
    }).detach();
}

void ServerSettingsDialog::onEasyTierToggled(bool /*checked*/) {
}

void ServerSettingsDialog::onManageService() {
    ServiceManagerDialog dlg(this);
    dlg.exec();
    refreshEasyTierStatus();
}

void ServerSettingsDialog::onStartService() {
    btnStartService_->setEnabled(false);
    btnStartService_->setText("Starting...");
    QPointer<ServerSettingsDialog> guard = this;
    std::thread([guard]() {
        if (!guard) return;
        bool ok = StartServiceViaScm();
        if (ok) {
            EasyTierControlClient cli;
            for (int i = 0; i < 20; ++i) {
                if (cli.connect(200)) { ok = true; break; }
                Sleep(300);
            }
        }
        QMetaObject::invokeMethod(guard.data(), [guard, ok]() {
            if (!guard) return;
            guard->btnStartService_->setEnabled(true);
            guard->btnStartService_->setText("Start Service");
            guard->refreshEasyTierStatus();
            if (!ok) {
                QMessageBox::StandardButton btn = QMessageBox::question(
                    (QWidget*)guard,
                    "Start Failed",
                    "Could not start the EasyTier Windows Service.\n\n"
                    "Open Service Manager to diagnose?",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (btn == QMessageBox::Yes) {
                    guard->onManageService();
                }
            }
        });
    }).detach();
}

void ServerSettingsDialog::refreshEasyTierStatus() {
    if (refreshBusy_.exchange(true)) return;

    QPointer<ServerSettingsDialog> guard = this;
    std::thread([guard]() {
        if (!guard) { return; }

        EasyTierControlClient cli;
        bool pipeOk = cli.connect(200);
        bool installed = IsServiceInstalledScm();

        QString iconColor, statusText, ipText;
        bool enableStart = installed;
        bool enableManage = true;

        if (!pipeOk) {
            iconColor = installed ? "#d9534f" : "#f0ad4e";
            statusText = installed ? "Service Not Running" : "Service Not Installed";
            ipText = installed
                ? "Click 'Start Service' or run: sc.exe start EasyTier"
                : "Click 'Manage' to install first";
        } else {
            std::string state, ip;
            uint32_t uptime;
            if (cli.getStatus(state, ip, uptime)) {
                if (state == "running") {
                    iconColor = "#5cb85c";
                    statusText = "EasyTier Running";
                    ipText = "IP: " + QString::fromStdString(ip.empty() ? "--" : ip);
                    enableStart = false;
                } else {
                    iconColor = "#337ab7";
                    statusText = "Service Running";
                    ipText = "EasyTier Stopped — click Manage to start";
                    enableStart = true;
                }
            } else {
                iconColor = "#888";
                statusText = "Status Unknown";
                ipText = "";
            }
        }

        QMetaObject::invokeMethod(guard.data(), [guard, iconColor, statusText, ipText, enableStart, enableManage]() {
            if (!guard) return;
            guard->lblStatusIcon_->setStyleSheet(
                QString("font-size: 20px; color: %1;").arg(iconColor));
            guard->lblStatusText_->setText(statusText);
            guard->lblIp_->setText(ipText);
            guard->btnStartService_->setEnabled(enableStart);
            guard->btnManageService_->setEnabled(enableManage);
            guard->refreshBusy_ = false;
        });
    }).detach();
}

ServerSettings ServerSettingsDialog::getSettings() const {
    ServerSettings s;
    s.useEasyTier = chkEasyTier_->isChecked();
    s.desktopPort = leDesktopPort_->text().toInt();
    s.sshPort = leSshPort_->text().toInt();
    s.sshPassword = leSshPassword_->text().toStdString();
    return s;
}
