#include "service_manager_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QFrame>
#include <QApplication>

#include <windows.h>
#include <winsvc.h>

ServiceManagerDialog::ServiceManagerDialog(QWidget* parent)
    : QDialog(parent), timer_(new QTimer(this))
{
    setWindowTitle("EasyTier Service Manager");
    setMinimumWidth(500);
    createUI();

    connect(timer_, &QTimer::timeout, this, &ServiceManagerDialog::refreshStatus);
    timer_->start(3000);

    refreshStatus();
}

ServiceManagerDialog::~ServiceManagerDialog() {
    client_.disconnect();
}

void ServiceManagerDialog::createUI() {
    auto* main = new QVBoxLayout(this);

    // Status section
    auto* statusGroup = new QGroupBox("Service Status");
    auto* statusLayout = new QHBoxLayout(statusGroup);
    lblStatusIcon_ = new QLabel("●");
    lblStatusIcon_->setStyleSheet("font-size: 24px; color: #888;");
    lblStatusIcon_->setFixedWidth(30);
    statusLayout->addWidget(lblStatusIcon_);

    auto* statusInfo = new QVBoxLayout();
    lblStatusText_ = new QLabel("Unknown");
    lblStatusText_->setStyleSheet("font-weight: bold; font-size: 14px;");
    lblIp_ = new QLabel("IP: --");
    lblIp_->setStyleSheet("color: #555;");
    statusInfo->addWidget(lblStatusText_);
    statusInfo->addWidget(lblIp_);
    statusLayout->addLayout(statusInfo);
    statusLayout->addStretch();

    main->addWidget(statusGroup);

    // Control buttons
    auto* ctrlLayout = new QHBoxLayout();
    btnStart_   = new QPushButton("Start");
    btnStop_    = new QPushButton("Stop");
    btnRestart_ = new QPushButton("Restart");
    ctrlLayout->addWidget(btnStart_);
    ctrlLayout->addWidget(btnStop_);
    ctrlLayout->addWidget(btnRestart_);
    main->addLayout(ctrlLayout);

    connect(btnStart_,   &QPushButton::clicked, this, &ServiceManagerDialog::onStart);
    connect(btnStop_,    &QPushButton::clicked, this, &ServiceManagerDialog::onStop);
    connect(btnRestart_, &QPushButton::clicked, this, &ServiceManagerDialog::onRestart);

    // Config section
    auto* configGroup = new QGroupBox("Configuration");
    auto* configForm = new QFormLayout(configGroup);

    leInstName_   = new QLineEdit("jc-client");
    leNetName_    = new QLineEdit("jc-tool-vpn");
    leNetSecret_  = new QLineEdit();
    leNetSecret_->setEchoMode(QLineEdit::Password);
    leIpv4_       = new QLineEdit();
    leIpv4_->setPlaceholderText("Leave empty for DHCP");
    leListenPort_ = new QLineEdit("11012");
    lePeerUrl_    = new QLineEdit("tcp://225284.xyz:11010");
    chkAutoStart_ = new QCheckBox("Auto-start EasyTier on service boot");

    configForm->addRow("Instance Name:", leInstName_);
    configForm->addRow("Network Name:", leNetName_);
    configForm->addRow("Network Secret:", leNetSecret_);
    configForm->addRow("Virtual IPv4:", leIpv4_);
    configForm->addRow("Listen Port:", leListenPort_);
    configForm->addRow("Peer URL:", lePeerUrl_);
    configForm->addRow("", chkAutoStart_);

    btnApply_ = new QPushButton("Apply & Restart");
    btnApply_->setStyleSheet("background-color: #5cb85c; color: white; padding: 8px;");
    configForm->addRow("", btnApply_);

    connect(btnApply_, &QPushButton::clicked, this, &ServiceManagerDialog::onApplyConfig);

    main->addWidget(configGroup);

    // Service lifecycle (SCM)
    auto* installGroup = new QGroupBox("Windows Service Management");
    auto* installLayout = new QHBoxLayout(installGroup);
    btnInstall_   = new QPushButton("Install Service");
    btnUninstall_ = new QPushButton("Uninstall Service");
    installLayout->addWidget(btnInstall_);
    installLayout->addWidget(btnUninstall_);
    main->addWidget(installGroup);

    connect(btnInstall_,   &QPushButton::clicked, this, &ServiceManagerDialog::onInstallService);
    connect(btnUninstall_, &QPushButton::clicked, this, &ServiceManagerDialog::onUninstallService);

    // Close button
    auto* btnClose = new QPushButton("Close");
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    main->addWidget(btnClose);
}

void ServiceManagerDialog::loadConfigFromService() {
    if (client_.isConnected()) {
        EasyTierConfig cfg;
        if (client_.getConfig(cfg)) {
            leInstName_->setText(QString::fromStdString(cfg.instanceName));
            leNetName_->setText(QString::fromStdString(cfg.networkName));
            leNetSecret_->setText(QString::fromStdString(cfg.networkSecret));
            leIpv4_->setText(QString::fromStdString(cfg.ipv4));
            leListenPort_->setText(QString::number(cfg.listenPort));
            lePeerUrl_->setText(QString::fromStdString(cfg.peerUrl));
            chkAutoStart_->setChecked(cfg.autoStart);
            return;
        }
    }
}

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

void ServiceManagerDialog::refreshStatus() {
    // 如果尚未连接，尝试连一次（1000ms 超时）
    if (!client_.isConnected() && !client_.connect(1000)) {
        bool installed = IsServiceInstalledScm();
        lblStatusIcon_->setStyleSheet("font-size: 24px; color: #888;");
        lblStatusText_->setText(installed
            ? "Service Not Running" : "Service Not Installed");
        lblIp_->setText(installed
            ? "Click Start or run: sc.exe start EasyTier"
            : "Click Install Service first");
        btnStart_->setEnabled(true);
        btnStop_->setEnabled(false);
        btnRestart_->setEnabled(false);
        return;
    }

    // 已连接 → 发送 STATUS 命令
    std::string state, ip;
    uint32_t uptime;
    if (client_.getStatus(state, ip, uptime)) {
        if (state == "running") {
            lblStatusIcon_->setStyleSheet("font-size: 24px; color: #5cb85c;");
            lblStatusText_->setText("EasyTier Running");
            btnStart_->setEnabled(false);
            btnStop_->setEnabled(true);
            btnRestart_->setEnabled(true);
        } else {
            lblStatusIcon_->setStyleSheet("font-size: 24px; color: #d9534f;");
            lblStatusText_->setText("EasyTier Stopped");
            btnStart_->setEnabled(true);
            btnStop_->setEnabled(false);
            btnRestart_->setEnabled(false);
        }
        lblIp_->setText("IP: " + QString::fromStdString(ip.empty() ? "--" : ip));
        return; // 保持连接，等待下次定时器复用
    }

    // 连接已断开（服务端关闭了管道），下个 tick 重新连
    client_.disconnect();
}

void ServiceManagerDialog::setButtonsEnabled(bool enabled) {
    btnApply_->setEnabled(enabled);
}

void ServiceManagerDialog::onInstallService() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);

    // Find the service exe relative to RemoteControl path
    std::wstring dir(path);
    auto pos = dir.find_last_of(L"\\");
    if (pos != std::wstring::npos) dir.resize(pos + 1);
    dir += L"EasyTierService.exe";

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        QMessageBox::critical(this, "Error",
            "OpenSCManager failed. Run as Administrator.");
        return;
    }

    SC_HANDLE svc = CreateServiceW(scm, L"EasyTier", L"EasyTier VPN Service",
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        dir.c_str(), NULL, NULL, NULL, NULL, NULL);

    DWORD err = GetLastError();
    if (svc) {
        CloseServiceHandle(svc);
        QMessageBox::information(this, "Success", "EasyTier service installed.\nStart it from the control panel or sc.exe start EasyTier");
    } else if (err == ERROR_SERVICE_EXISTS) {
        QMessageBox::information(this, "Info", "Service already installed.");
    } else {
        QMessageBox::critical(this, "Error",
            "CreateService failed: " + QString::number(err));
    }
    CloseServiceHandle(scm);
}

void ServiceManagerDialog::onUninstallService() {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        QMessageBox::critical(this, "Error",
            "OpenSCManager failed. Run as Administrator.");
        return;
    }

    SC_HANDLE svc = OpenServiceW(scm, L"EasyTier", SERVICE_ALL_ACCESS);
    if (!svc) {
        QMessageBox::warning(this, "Error", "Service not found.");
        CloseServiceHandle(scm);
        return;
    }

    SERVICE_STATUS ss;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);
    if (!DeleteService(svc)) {
        QMessageBox::critical(this, "Error",
            "DeleteService failed: " + QString::number(GetLastError()));
    } else {
        QMessageBox::information(this, "Success", "Service uninstalled.");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

void ServiceManagerDialog::onStart() {
    if (!client_.isConnected()) {
        // 服务进程未运行 → 尝试通过 SCM 启动
        bool scmOk = StartServiceViaScm();
        if (scmOk) {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            for (int i = 0; i < 10; ++i) {
                if (client_.connect(500)) break;
                Sleep(500);
            }
            QApplication::restoreOverrideCursor();
        }
    }

    if (client_.isConnected() && client_.start()) {
        refreshStatus();
    } else if (IsServiceInstalledScm()) {
        QMessageBox::warning(this, "Error",
            "EasyTier service is installed but not responding.\n"
            "Try: sc.exe start EasyTier\n"
            "Then retry.");
    } else {
        QMessageBox::warning(this, "Error",
            "EasyTier service is not installed.\n"
            "Click 'Install Service' first.");
    }
}

void ServiceManagerDialog::onStop() {
    if (client_.stop()) {
        lblIp_->setText("IP: --");
        refreshStatus();
    }
}

void ServiceManagerDialog::onRestart() {
    if (client_.restart())
        refreshStatus();
}

void ServiceManagerDialog::onApplyConfig() {
    EasyTierConfig cfg;
    cfg.instanceName  = leInstName_->text().toStdString();
    cfg.networkName   = leNetName_->text().toStdString();
    cfg.networkSecret = leNetSecret_->text().toStdString();
    cfg.ipv4          = leIpv4_->text().toStdString();
    cfg.listenPort    = leListenPort_->text().toInt();
    cfg.peerUrl       = lePeerUrl_->text().toStdString();
    cfg.autoStart     = chkAutoStart_->isChecked();

    setButtonsEnabled(false);

    if (!client_.isConnected() && !client_.connect(3000)) {
        QMessageBox::warning(this, "Not Connected",
            "Cannot connect to EasyTier service.\nIf the service is installed but stopped, start it first.");
        setButtonsEnabled(true);
        return;
    }

    if (!client_.configure(cfg, true)) {
        QMessageBox::critical(this, "Error", "Failed to apply configuration.");
        setButtonsEnabled(true);
        return;
    }

    QMessageBox::information(this, "Success",
        "Configuration applied and EasyTier restarted.");
    refreshStatus();
    setButtonsEnabled(true);
}
