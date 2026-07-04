#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>

#include "connection_dialog.h"
#include "service_manager_dialog.h"
#include "../common/easytier_control.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QMessageBox>
#include <QHeaderView>
#include <QColor>
#include <QPointer>
#include <QApplication>
#include <thread>
#include <cstring>
#include <cstdlib>

#define DISCOVERY_MAGIC "JC_DISC"
#define DISCOVERY_PORT 22220

ConnectionDialog::ConnectionDialog(QWidget* parent)
    : QDialog(parent), scanTimer_(new QTimer(this)) {
    setWindowTitle("Connect to Remote Host");
    setMinimumWidth(520);
    createUI();

    scanTimer_->setInterval(10000);
    connect(scanTimer_, &QTimer::timeout, this, &ConnectionDialog::onScanTimer);
}

void ConnectionDialog::createUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // SSH connection group
    QGroupBox* sshGroup = new QGroupBox("SSH Connection");
    QFormLayout* sshForm = new QFormLayout(sshGroup);

    leHost_ = new QLineEdit("127.0.0.1");
    lePort_ = new QLineEdit("2222");
    leUsername_ = new QLineEdit("root");
    lePassword_ = new QLineEdit();
    lePassword_->setEchoMode(QLineEdit::Password);

    sshForm->addRow("Host:", leHost_);
    sshForm->addRow("Port:", lePort_);
    sshForm->addRow("Username:", leUsername_);
    sshForm->addRow("Password:", lePassword_);

    QLabel* pwdNote = new QLabel("桌面端口通过 SSH 自动获取");
    pwdNote->setStyleSheet("color: #888; font-size: 11px;");
    sshForm->addRow("", pwdNote);
    mainLayout->addWidget(sshGroup);

    // Online Devices group
    QGroupBox* devGroup = new QGroupBox("Online Devices");
    QVBoxLayout* devLayout = new QVBoxLayout(devGroup);

    tblDevices_ = new QTableWidget(0, 5);
    tblDevices_->setHorizontalHeaderLabels({"Hostname", "Virtual IP", "Latency", "Protocol", "NAT Type"});
    tblDevices_->horizontalHeader()->setStretchLastSection(true);
    tblDevices_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    tblDevices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tblDevices_->setSelectionMode(QAbstractItemView::SingleSelection);
    tblDevices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tblDevices_->setAlternatingRowColors(true);
    tblDevices_->verticalHeader()->setVisible(false);
    tblDevices_->setMinimumHeight(120);
    connect(tblDevices_, &QTableWidget::cellActivated, this, &ConnectionDialog::onDeviceActivated);
    devLayout->addWidget(tblDevices_);

    QHBoxLayout* scanBar = new QHBoxLayout();
    btnScanDevices_ = new QPushButton("Scan for Devices");
    connect(btnScanDevices_, &QPushButton::clicked, this, &ConnectionDialog::onScanDevices);
    scanBar->addWidget(btnScanDevices_);

    lblScanStatus_ = new QLabel("Double-click a device to auto-fill");
    lblScanStatus_->setStyleSheet("color: #888; font-size: 11px;");
    scanBar->addWidget(lblScanStatus_, 1);
    devLayout->addLayout(scanBar);

    mainLayout->addWidget(devGroup);

    // EasyTier group
    QGroupBox* etGroup = new QGroupBox("EasyTier VPN (via System Service)");
    QVBoxLayout* etLayout = new QVBoxLayout(etGroup);

    chkEasyTier_ = new QCheckBox("Route traffic through EasyTier VPN");
    connect(chkEasyTier_, &QCheckBox::toggled, this, &ConnectionDialog::onEasyTierToggled);
    etLayout->addWidget(chkEasyTier_);

    QFormLayout* etForm = new QFormLayout();
    leServerVip_ = new QLineEdit();
    leServerVip_->setPlaceholderText("Server's VPN virtual IP (e.g. 10.144.0.1)");
    etForm->addRow("Server Virtual IP:", leServerVip_);

    btnManageService_ = new QPushButton("Manage EasyTier Service...");
    connect(btnManageService_, &QPushButton::clicked, this, &ConnectionDialog::onManageService);
    etForm->addRow("", btnManageService_);

    etLayout->addLayout(etForm);

    onEasyTierToggled(false);
    mainLayout->addWidget(etGroup);

    // Connect button
    QPushButton* btnConnect = new QPushButton("Connect");
    btnConnect->setMinimumHeight(36);
    btnConnect->setStyleSheet("background-color: #337ab7; color: white; font-weight: bold;");
    connect(btnConnect, &QPushButton::clicked, this, &ConnectionDialog::onConnect);
    mainLayout->addWidget(btnConnect);

    QPushButton* btnCancel = new QPushButton("Cancel");
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    mainLayout->addWidget(btnCancel);
}

void ConnectionDialog::onEasyTierToggled(bool checked) {
    leServerVip_->setEnabled(checked);
    btnManageService_->setEnabled(checked);
}

void ConnectionDialog::onManageService() {
    ServiceManagerDialog dlg(this);
    dlg.exec();
}

void ConnectionDialog::onScanDevices() {
    if (isScanning_) return;
    isScanning_ = true;
    btnScanDevices_->setEnabled(false);
    btnScanDevices_->setText("Scanning...");
    lblScanStatus_->setText("Scanning...");

    bool useEasyTier = chkEasyTier_->isChecked();
    QPointer<ConnectionDialog> guard = this;

    std::thread([guard, useEasyTier]() {
        if (!guard) return;
        std::vector<DiscoveredDevice> devices;
        std::string errMsg;

        if (useEasyTier) {
            guard->doEasyTierScan(devices, errMsg);
        } else {
            guard->doTcpScan(devices);
        }

        QMetaObject::invokeMethod(guard.data(), [guard, devices, errMsg]() {
            if (!guard) return;
            guard->populateDeviceTable(devices);
            if (!errMsg.empty()) {
                guard->lblScanStatus_->setText(QString::fromStdString(errMsg));
            } else {
                guard->lblScanStatus_->setText(
                    QString("Found %1 device(s) | Double-click to connect").arg(devices.size()));
            }
            guard->btnScanDevices_->setText("Scan for Devices");
            guard->btnScanDevices_->setEnabled(true);
            guard->isScanning_ = false;

            if (!devices.empty() && !guard->scanTimer_->isActive())
                guard->scanTimer_->start();
        });
    }).detach();
}

void ConnectionDialog::onScanTimer() {
    if (!isScanning_)
        onScanDevices();
}

void ConnectionDialog::doTcpScan(std::vector<DiscoveredDevice>& devices) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) { WSACleanup(); return; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof(opt));

    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    // Send broadcast discovery request
    sockaddr_in broadcastAddr = {};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(DISCOVERY_PORT);
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(sock, DISCOVERY_MAGIC, 8, 0, (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    // Collect responses
    char buf[1500];
    sockaddr_in from;
    int fromLen = sizeof(from);

    while (true) {
        int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;
        if (n > 8 && memcmp(buf, DISCOVERY_MAGIC, 8) != 0) continue;

        std::string json(buf + 8, n - 8);
        std::string hostname = ExtractJsonString(json, "hostname");
        if (hostname.empty()) continue;

        // Deduplicate by IP
        std::string ip = inet_ntoa(from.sin_addr);
        bool dup = false;
        for (const auto& d : devices) {
            if (d.ipv4Addr == ip) { dup = true; break; }
        }
        if (dup) continue;

        DiscoveredDevice dev;
        dev.hostname = hostname;
        dev.ipv4Addr = ip;
        dev.version = ExtractJsonString(json, "version");
        devices.push_back(dev);
    }

    closesocket(sock);
    WSACleanup();
}

void ConnectionDialog::doEasyTierScan(std::vector<DiscoveredDevice>& devices, std::string& errMsg) {
    EasyTierControlClient cli;
    if (!cli.connect(3000)) {
        errMsg = "EasyTier service not reachable";
        return;
    }

    std::string resp;
    if (!cli.getNetworkInfos(resp)) {
        errMsg = "Failed to query network info";
        return;
    }

    // Parse response: {"code":0,"devices":[...]}
    auto devPos = resp.find("\"devices\":");
    if (devPos == std::string::npos) {
        errMsg = "Invalid response from service";
        return;
    }
    devPos += strlen("\"devices\":");
    while (devPos < resp.size() && (resp[devPos] == ' ' || resp[devPos] == '\t')) ++devPos;
    if (devPos >= resp.size() || resp[devPos] != '[') {
        errMsg = "Invalid devices array";
        return;
    }

    // Parse array content
    size_t pos = devPos + 1;

    while (pos < resp.size()) {
        // Skip whitespace and commas
        while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t' ||
               resp[pos] == '\n' || resp[pos] == '\r' || resp[pos] == ','))
            ++pos;
        if (pos >= resp.size() || resp[pos] == ']') break;
        if (resp[pos] != '{') break;

        // Extract one peer object
        int bracketDepth = 0;
        bool inStr = false;
        size_t objStart = pos;
        for (; pos < resp.size(); ++pos) {
            if (inStr) {
                if (resp[pos] == '"' && (pos == 0 || resp[pos - 1] != '\\')) inStr = false;
            } else {
                if (resp[pos] == '"') inStr = true;
                else if (resp[pos] == '{') ++bracketDepth;
                else if (resp[pos] == '}') { --bracketDepth; if (bracketDepth == 0) { ++pos; break; } }
            }
        }

        std::string peerObj = resp.substr(objStart, pos - objStart);

        DiscoveredDevice dev;
        dev.peerId = ExtractJsonString(peerObj, "peer_id");
        dev.ipv4Addr = ExtractJsonString(peerObj, "ipv4_addr");
        dev.hostname = ExtractJsonString(peerObj, "hostname");
        dev.version = ExtractJsonString(peerObj, "version");
        dev.tunnelProto = ExtractJsonString(peerObj, "tunnel_proto");
        dev.natType = ExtractJsonString(peerObj, "nat_type");
        dev.cost = ExtractJsonInt(peerObj, "cost");

        // lat_ms and loss_rate are JSON numbers, need custom extraction
        auto extractNum = [&](const std::string& obj, const std::string& key) -> double {
            std::string search = "\"" + key + "\":";
            auto p = obj.find(search);
            if (p == std::string::npos) return 0;
            p += search.size();
            while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
            char* end = nullptr;
            return std::strtod(obj.c_str() + p, &end);
        };

        dev.latencyMs = extractNum(peerObj, "lat_ms");
        dev.lossRate = extractNum(peerObj, "loss_rate");

        bool dup = false;
        for (const auto& d : devices) {
            if (d.peerId == dev.peerId && !dev.peerId.empty()) { dup = true; break; }
            if (d.ipv4Addr == dev.ipv4Addr && !dev.ipv4Addr.empty()) { dup = true; break; }
        }
        if (dup) continue;

        devices.push_back(dev);
    }
}

void ConnectionDialog::populateDeviceTable(const std::vector<DiscoveredDevice>& devices) {
    tblDevices_->setRowCount((int)devices.size());
    for (int i = 0; i < (int)devices.size(); ++i) {
        const auto& dev = devices[i];

        QString hostname = QString::fromStdString(dev.hostname);
        if (hostname.isEmpty()) hostname = QString::fromStdString(dev.ipv4Addr);
        tblDevices_->setItem(i, 0, new QTableWidgetItem(hostname));

        tblDevices_->setItem(i, 1, new QTableWidgetItem(QString::fromStdString(dev.ipv4Addr)));

        QString latStr = dev.latencyMs > 0
            ? QString::number(dev.latencyMs, 'f', 1) + " ms"
            : "-";
        QTableWidgetItem* latItem = new QTableWidgetItem(latStr);
        if (dev.latencyMs > 0) {
            if (dev.latencyMs < 50)
                latItem->setForeground(QColor("#5cb85c"));
            else if (dev.latencyMs < 200)
                latItem->setForeground(QColor("#f0ad4e"));
            else
                latItem->setForeground(QColor("#d9534f"));
        }
        tblDevices_->setItem(i, 2, latItem);

        tblDevices_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(dev.tunnelProto)));
        tblDevices_->setItem(i, 4, new QTableWidgetItem(QString::fromStdString(dev.natType)));
    }
    tblDevices_->resizeRowsToContents();
}

void ConnectionDialog::onDeviceActivated(int row, int) {
    if (row < 0 || row >= tblDevices_->rowCount()) return;

    QTableWidgetItem* ipItem = tblDevices_->item(row, 1);
    if (!ipItem) return;

    QString ip = ipItem->text();
    if (ip.isEmpty()) return;

    leHost_->setText(ip);
    lePort_->setText("2222");
    leServerVip_->setText(ip);
    chkEasyTier_->setChecked(true);
}

void ConnectionDialog::onConnect() {
    if (leHost_->text().isEmpty()) {
        QMessageBox::warning(this, "Validation", "Host cannot be empty");
        return;
    }
    if (leUsername_->text().isEmpty()) {
        QMessageBox::warning(this, "Validation", "Username cannot be empty");
        return;
    }
    if (lePassword_->text().isEmpty()) {
        QMessageBox::warning(this, "Validation", "Password cannot be empty");
        return;
    }

    config_.host = leHost_->text().toStdString();
    config_.sshPort = lePort_->text().toInt();
    config_.username = leUsername_->text().toStdString();
    config_.password = lePassword_->text().toStdString();

    config_.useEasyTier = chkEasyTier_->isChecked();
    config_.easytierServerVip = leServerVip_->text().toStdString();

    accept();
}