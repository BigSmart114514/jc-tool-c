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
#include "../common/easytier_manager.h"
#include "../common/ssh_session.h"

#include "../client/control_panel.h"
#include "../client/connection_dialog.h"
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

    std::unique_ptr<EasytierManager> easytierMgr;
    std::string modeText = "SSH+SFTP";
    std::string connectInfo;

    // EasyTier setup
    if (cfg.useEasyTier) {
        modeText = "EasyTier + SSH+SFTP";
        std::string toml = EasytierManager::MakeConfig(
            cfg.easytierInstanceName,
            cfg.easytierNetworkName,
            cfg.easytierNetworkSecret,
            cfg.easytierIpv4,
            cfg.easytierListenPort,
            cfg.easytierPeerUrl
        );

        easytierMgr = std::make_unique<EasytierManager>(toml);
        if (!easytierMgr->start()) {
            QMessageBox::critical(nullptr, "EasyTier Error", "Could not start EasyTier network.");
            return 1;
        }

        // Get virtual IP - but keep original SSH host config
        QMessageBox::information(nullptr, "EasyTier Connected",
            "Your virtual IP: " + QString::fromStdString(easytierMgr->getVirtualIp()));

        connectInfo = "SSH: " + cfg.host + ":" + std::to_string(cfg.sshPort)
                     + " | VPN: " + easytierMgr->getVirtualIp();
    } else {
        connectInfo = cfg.host + ":" + std::to_string(cfg.sshPort);
    }

    // Connect SSH
    auto* sshSession = new SshSession();
    if (!sshSession->connect(targetHost, cfg.sshPort, cfg.username, cfg.password)) {
        QMessageBox::critical(nullptr, "SSH Connection Failed",
            QString::fromStdString(sshSession->getError()));
        delete sshSession;
        if (easytierMgr) easytierMgr->stop();
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
    if (easytierMgr) easytierMgr->stop();

    return result;
}

// ======================= SERVER =======================
int runServer() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    QMessageBox::StandardButton reply = QMessageBox::question(
        nullptr, "Transport Mode",
        "Use EasyTier virtual network?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    bool useEasyTier = (reply == QMessageBox::Yes);

    std::unique_ptr<EasytierManager> easytierMgr;
    std::string myVirtualIp;
    int desktopPort = Config::DEFAULT_DESKTOP_PORT;
    int filePort = Config::DEFAULT_FILE_PORT;
    int sshPort = 2223;
    std::string sshPassword;

    if (useEasyTier) {
        QDialog dlg;
        dlg.setWindowTitle("Server - EasyTier Setup");
        dlg.setMinimumWidth(420);
        QFormLayout form(&dlg);

        QLineEdit* leInstName   = new QLineEdit("jc-server");
        QLineEdit* leNetName    = new QLineEdit("jc-tool-vpn");
        QLineEdit* leNetSecret  = new QLineEdit("your_secret_here");
        QLineEdit* leIpv4       = new QLineEdit("");
        QLineEdit* leListenPort = new QLineEdit("11011");
        QLineEdit* lePeerUrl    = new QLineEdit("tcp://225284.xyz:11010");

        QLineEdit* leDesktopPort = new QLineEdit(QString::number(Config::DEFAULT_DESKTOP_PORT));
        QLineEdit* leFilePort    = new QLineEdit(QString::number(Config::DEFAULT_FILE_PORT));
        QLineEdit* leSshPort     = new QLineEdit("2223");
        QLineEdit* leSshPassword = new QLineEdit();
        leSshPassword->setEchoMode(QLineEdit::Password);

        form.addRow("Instance Name:", leInstName);
        form.addRow("Network Name:", leNetName);
        form.addRow("Network Secret:", leNetSecret);
        form.addRow("Virtual IPv4:", leIpv4);
        form.addRow("Listen Port:", leListenPort);
        form.addRow("Public Peer URL:", lePeerUrl);
        form.addRow("Desktop Service Port:", leDesktopPort);
        form.addRow("File Service Port:", leFilePort);
        form.addRow("SSH Server Port:", leSshPort);
        form.addRow("SSH Password:", leSshPassword);

        QPushButton* btnStart = new QPushButton("Start Server");
        form.addRow(btnStart);
        QObject::connect(btnStart, &QPushButton::clicked, &dlg, &QDialog::accept);

        if (dlg.exec() != QDialog::Accepted) {
            timeEndPeriod(1);
            return 0;
        }

        desktopPort = leDesktopPort->text().toInt();
        filePort    = leFilePort->text().toInt();
        sshPort     = leSshPort->text().toInt();
        sshPassword = leSshPassword->text().toStdString();

        std::string toml = EasytierManager::MakeConfig(
            leInstName->text().toStdString(),
            leNetName->text().toStdString(),
            leNetSecret->text().toStdString(),
            leIpv4->text().toStdString(),
            leListenPort->text().toInt(),
            lePeerUrl->text().toStdString()
        );

        easytierMgr = std::make_unique<EasytierManager>(toml);
        if (!easytierMgr->start()) {
            QMessageBox::critical(nullptr, "EasyTier Error", "Could not start EasyTier network.");
            timeEndPeriod(1);
            return 1;
        }
        myVirtualIp = easytierMgr->getVirtualIp();
    } else {
        myVirtualIp = "Direct TCP (not using EasyTier)";
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
        vLayout.addWidget(new QLabel(QString("Virtual IP: %1").arg(myVirtualIp.c_str())));
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
    QObject::connect(btnStop, &QPushButton::clicked, [&statusWin]() {
        statusWin.accept();
        QApplication::quit();
    });
    statusWin.show();

    qApp->exec();

    sshServer.stop();
    desktopService.stop();
    fileService.stop();
    desktopTCP->stop();
    fileTCP->stop();
    if (easytierMgr) easytierMgr->stop();
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
