#include <QApplication>
#include <QMessageBox>
#include <QDialog>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QFormLayout>
#include <iostream>
#include <memory>

#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/easytier_control.h"
#include "../common/ssh_session.h"

#include "../client/control_panel.h"
#include "../client/connection_dialog.h"
#include "../client/service_manager_dialog.h"
#include "../server/desktop_service.h"
#include "../server/file_service.h"
#include "../server/ssh_server.h"

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Wtsapi32.lib")

// ======================= CLIENT =======================
int runClient() {
    ConnectionDialog dlg;
    if (dlg.exec() != QDialog::Accepted) return 0;

    ConnectionConfig cfg = dlg.getConfig();
    std::string targetHost = cfg.host;
    int desktopPort = cfg.desktopPort;

    // 如果启用 EasyTier，使用服务提供的 VPN 虚拟 IP 作为连接目标
    if (cfg.useEasyTier && !cfg.easytierServerVip.empty()) {
        targetHost = cfg.easytierServerVip;
    }

    // Connect SSH
    auto* sshSession = new SshSession();
    if (!sshSession->connect(targetHost, cfg.sshPort, cfg.username, cfg.password)) {
        QMessageBox::critical(nullptr, "SSH Connection Failed",
            QString::fromStdString(sshSession->getError()));
        delete sshSession;
        return 1;
    }

    // Initialize SFTP
    if (!sshSession->sftpInit()) {
        QMessageBox::warning(nullptr, "SFTP Init Warning",
            "SFTP initialization failed. File manager may not work.");
    }

    // Connect desktop transport
    auto* desktopTransport = new TCPClientTransport();
    ITransport* desktopTransportPtr = nullptr;
    if (!desktopTransport->connect(targetHost, desktopPort)) {
        QMessageBox::warning(nullptr, "Desktop Warning",
            "Could not connect to desktop service. Desktop will be unavailable.");
        delete desktopTransport;
        desktopTransport = nullptr;
    } else {
        desktopTransportPtr = desktopTransport;
    }

    std::string connectInfo = targetHost + ":" + std::to_string(cfg.sshPort);
    std::string modeText = "SSH+SFTP";
    if (cfg.useEasyTier) {
        modeText = "EasyTier + SSH+SFTP";
        connectInfo = "SSH: " + targetHost + ":" + std::to_string(cfg.sshPort)
                     + " | via EasyTier Service";
    }

    ControlPanelConfig panelConfig;
    panelConfig.sshSession = sshSession;
    panelConfig.desktopTransport = desktopTransportPtr;
    panelConfig.modeText = modeText;
    panelConfig.connectInfo = connectInfo;
    panelConfig.sshHost = targetHost;
    panelConfig.sshPort = cfg.sshPort;
    panelConfig.sshUser = cfg.username;
    panelConfig.sshPassword = cfg.password;

    ControlPanel controlPanel;
    controlPanel.setConfig(panelConfig);
    controlPanel.setupDesktopTransportCallbacks();
    controlPanel.show();

    int result = qApp->exec();

    // Cleanup
    if (desktopTransportPtr) {
        desktopTransportPtr->disconnect();
        delete desktopTransportPtr;
    }
    if (sshSession) {
        sshSession->disconnect();
        delete sshSession;
    }

    return result;
}

// ======================= SERVER =======================
int runServer() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    int desktopPort = Config::DEFAULT_DESKTOP_PORT;
    int filePort = Config::DEFAULT_FILE_PORT;
    int sshPort = 2223;
    std::string sshPassword;
    std::string myVirtualIp;

    QMessageBox::StandardButton reply = QMessageBox::question(
        nullptr, "Transport Mode",
        "Use EasyTier virtual network?\n\nMake sure the EasyTier service is installed and running.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    bool useEasyTier = (reply == QMessageBox::Yes);

    if (useEasyTier) {
        EasyTierControlClient client;
        if (!client.connect(3000)) {
            QMessageBox::StandardButton openMgr = QMessageBox::question(
                nullptr, "EasyTier Service Not Found",
                "Cannot connect to EasyTier service.\n\n"
                "Open Service Manager to install/start it?",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (openMgr == QMessageBox::Yes) {
                client.disconnect();
                ServiceManagerDialog mgrDlg;
                mgrDlg.exec();
                if (!client.connect(3000)) {
                    QMessageBox::warning(nullptr, "Still Not Connected",
                        "EasyTier service is still not available. Starting without VPN.");
                    useEasyTier = false;
                }
            } else {
                useEasyTier = false;
            }
        }

        if (useEasyTier) {
            std::string state, ip;
            uint32_t uptime;
            if (client.getStatus(state, ip, uptime) && state == "running") {
                myVirtualIp = ip;
            } else {
                QMessageBox::StandardButton startBtn = QMessageBox::question(
                    nullptr, "EasyTier Not Running",
                    "EasyTier service is not running. Start it now?",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (startBtn == QMessageBox::Yes) {
                    client.start();
                    Sleep(2000); // brief wait for IP assignment
                    client.getStatus(state, ip, uptime);
                    myVirtualIp = ip;
                }
                if (myVirtualIp.empty()) {
                    QMessageBox::warning(nullptr, "No VPN IP",
                        "Could not get a virtual IP. Starting without VPN.");
                    useEasyTier = false;
                }
            }
            client.disconnect();
        }
    }

    if (!useEasyTier) {
        myVirtualIp = "Direct TCP";
        bool ok;
        QString pwd = QInputDialog::getText(nullptr, "SSH Server Password",
            "Set password for SSH access:", QLineEdit::Password, "", &ok);
        if (!ok || pwd.isEmpty()) {
            timeEndPeriod(1);
            return 0;
        }
        sshPassword = pwd.toStdString();
    }

    // 初始化桌面和文件服务
    DesktopService desktopService;
    FileService fileService;
    if (!desktopService.init()) {
        QMessageBox::critical(nullptr, "Error", "Desktop service init failed!");
        timeEndPeriod(1);
        return 1;
    }

    auto desktopTCP = std::make_unique<TCPServerTransport>(desktopPort);
    auto fileTCP    = std::make_unique<TCPServerTransport>(filePort);

    if (!desktopTCP->start() || !fileTCP->start()) {
        QMessageBox::critical(nullptr, "TCP Error", "Failed to start TCP transport.");
        timeEndPeriod(1);
        return 1;
    }

    desktopService.setTransport(desktopTCP.get());
    fileService.setTransport(fileTCP.get());
    desktopService.start();
    fileService.start();

    // Start embedded SSH server
    SshServer sshServer;
    if (!sshServer.start(sshPort, sshPassword)) {
        QMessageBox::critical(nullptr, "SSH Error", "Failed to start SSH server on port "
            + QString::number(sshPort));
    }

    QDialog statusWin;
    statusWin.setWindowTitle("Server Running");
    statusWin.setMinimumSize(400, 250);
    QVBoxLayout vLayout(&statusWin);

    if (useEasyTier) {
        vLayout.addWidget(new QLabel(QString("Virtual IP: %1 (EasyTier Service)").arg(myVirtualIp.c_str())));
    } else {
        vLayout.addWidget(new QLabel("Mode: Direct TCP (no VPN)"));
    }

    vLayout.addWidget(new QLabel(QString("Desktop Port: %1").arg(desktopPort)));
    vLayout.addWidget(new QLabel(QString("File Port: %1").arg(filePort)));
    vLayout.addWidget(new QLabel(QString("SSH Port: %1").arg(sshPort)));
    vLayout.addWidget(new QLabel("SSH Auth: Password"));
    vLayout.addWidget(new QLabel(QString("Resolution: %1x%2")
        .arg(desktopService.getWidth()).arg(desktopService.getHeight())));
    vLayout.addStretch();
    QPushButton* btnStop = new QPushButton("Stop Server");
    btnStop->setStyleSheet("background-color: #c9302c; color: white; padding: 8px;");
    vLayout.addWidget(btnStop);
    QObject::connect(btnStop, &QPushButton::clicked, [&]() {
        std::cout << "[Server] Stop requested..." << std::endl;
        statusWin.hide();
        sshServer.stop();
        desktopService.stop();
        fileService.stop();
        desktopTCP->stop();
        fileTCP->stop();
        qApp->quit();
    });
    statusWin.show();

    qApp->exec();

    timeEndPeriod(1);
    return 0;
}

// ======================= 入口 =======================
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    if (!NetUtil::InitWinsock()) {
        QMessageBox::critical(nullptr, "Error", "WSAStartup failed!");
        return 1;
    }

    QDialog startupDlg;
    startupDlg.setWindowTitle("JC Tool Mode Selector");
    startupDlg.setFixedSize(300, 150);
    QVBoxLayout layout(&startupDlg);

    QLabel* label = new QLabel("Select application mode:");
    label->setAlignment(Qt::AlignCenter);
    layout.addWidget(label);

    QPushButton* btnClient = new QPushButton("Start as Client");
    QPushButton* btnServer = new QPushButton("Start as Server");
    btnClient->setMinimumHeight(35);
    btnServer->setMinimumHeight(35);
    layout.addWidget(btnClient);
    layout.addWidget(btnServer);

    int mode = 0;
    QObject::connect(btnClient, &QPushButton::clicked, [&](){ mode = 1; startupDlg.accept(); });
    QObject::connect(btnServer, &QPushButton::clicked, [&](){ mode = 2; startupDlg.accept(); });

    if (startupDlg.exec() != QDialog::Accepted) {
        WSACleanup();
        return 0;
    }

    int result = 0;
    if (mode == 1) result = runClient();
    else if (mode == 2) result = runServer();

    WSACleanup();
    return result;
}
