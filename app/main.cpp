#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>
#include <QDialog>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <iostream>
#include <memory>

#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/easytier_manager.h"

#include "../client/control_panel.h"
#include "../server/desktop_service.h"
#include "../server/file_service.h"

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "Wtsapi32.lib")

// ======================= CLIENT =======================
int runClient() {
    // 选择传输模式
    QMessageBox::StandardButton reply = QMessageBox::question(
        nullptr, "Transport Mode",
        "Use EasyTier virtual network?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    bool useEasyTier = (reply == QMessageBox::Yes);

    std::string serverIp;
    int desktopPort = Config::DEFAULT_DESKTOP_PORT;
    int filePort = Config::DEFAULT_FILE_PORT;

    if (useEasyTier) {
        // ---- EasyTier 模式配置 ----
        QDialog dlg;
        dlg.setWindowTitle("Client - EasyTier Setup");
        dlg.setMinimumWidth(420);
        QFormLayout form(&dlg);

        QLineEdit* leInstName   = new QLineEdit("jc-client");
        QLineEdit* leNetName    = new QLineEdit("jc-tool-vpn");
        QLineEdit* leNetSecret  = new QLineEdit("your_secret_here");
        QLineEdit* leIpv4       = new QLineEdit("");
        leIpv4->setPlaceholderText("Leave empty for auto-assign");
        QLineEdit* leListenPort = new QLineEdit("11012");
        QLineEdit* lePeerUrl    = new QLineEdit("tcp://225284.xyz:11010");

        form.addRow("Instance Name:", leInstName);
        form.addRow("Network Name:", leNetName);
        form.addRow("Network Secret:", leNetSecret);
        form.addRow("Virtual IPv4 (optional):", leIpv4);
        form.addRow("Listen Port:", leListenPort);
        form.addRow("Public Peer URL:", lePeerUrl);

        QPushButton* btnOk = new QPushButton("Next");
        form.addRow(btnOk);
        QObject::connect(btnOk, &QPushButton::clicked, &dlg, &QDialog::accept);

        if (dlg.exec() != QDialog::Accepted) return 0;

        std::string toml = EasytierManager::MakeConfig(
            leInstName->text().toStdString(),
            leNetName->text().toStdString(),
            leNetSecret->text().toStdString(),
            leIpv4->text().toStdString(),
            leListenPort->text().toInt(),
            lePeerUrl->text().toStdString()
        );

        EasytierManager easytierMgr(toml);
        if (!easytierMgr.start()) {
            QMessageBox::critical(nullptr, "EasyTier Error", "Could not start EasyTier network.");
            return 1;
        }

        QMessageBox::information(nullptr, "Connected",
            "Your virtual IP: " + QString::fromStdString(easytierMgr.getVirtualIp()));

        // 输入服务端虚拟IP
        bool ok;
        serverIp = QInputDialog::getText(nullptr, "Server Virtual IP",
            "Enter server virtual IP:", QLineEdit::Normal, "10.0.0.1", &ok).toStdString();
        if (!ok || serverIp.empty()) return 0;

        desktopPort = QInputDialog::getInt(nullptr, "Desktop Port",
            "Desktop service port:", Config::DEFAULT_DESKTOP_PORT);
        filePort = QInputDialog::getInt(nullptr, "File Port",
            "File service port:", Config::DEFAULT_FILE_PORT);
        if (desktopPort == 0 || filePort == 0) return 0;
    }
    else {
        // ---- 普通 TCP 直连模式 ----
        bool ok;
        serverIp = QInputDialog::getText(nullptr, "Server IP",
            "Enter server IP address:", QLineEdit::Normal, "127.0.0.1", &ok).toStdString();
        if (!ok || serverIp.empty()) return 0;

        desktopPort = QInputDialog::getInt(nullptr, "Desktop Port",
            "Desktop service port:", Config::DEFAULT_DESKTOP_PORT);
        filePort = QInputDialog::getInt(nullptr, "File Port",
            "File service port:", Config::DEFAULT_FILE_PORT);
        if (desktopPort == 0 || filePort == 0) return 0;
    }

    // 建立TCP连接（无论哪种模式，都是 TCP 直连）
    auto* desktopTransport = new TCPClientTransport();
    auto* fileTransport = new TCPClientTransport();

    if (!desktopTransport->connect(serverIp, desktopPort) ||
        !fileTransport->connect(serverIp, filePort)) {
        QMessageBox::critical(nullptr, "Connection Failed",
            "Cannot connect to server.");
        delete desktopTransport;
        delete fileTransport;
        return 1;
    }

    // 控制面板
    ControlPanelConfig panelConfig;
    panelConfig.desktopTransport = desktopTransport;
    panelConfig.fileTransport = fileTransport;
    panelConfig.modeText = useEasyTier ? "EasyTier" : "Direct TCP";
    panelConfig.connectInfo = serverIp + ":" + std::to_string(desktopPort);

    ControlPanel controlPanel;
    controlPanel.setConfig(panelConfig);
    controlPanel.setupTransportCallbacks();
    controlPanel.show();

    int result = qApp->exec();

    desktopTransport->disconnect();
    fileTransport->disconnect();
    delete desktopTransport;
    delete fileTransport;
    return result;
}

// ======================= SERVER =======================
int runServer() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // 选择传输模式
    QMessageBox::StandardButton reply = QMessageBox::question(
        nullptr, "Transport Mode",
        "Use EasyTier virtual network?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    bool useEasyTier = (reply == QMessageBox::Yes);

    std::unique_ptr<EasytierManager> easytierMgr;
    std::string myVirtualIp;
    int desktopPort = Config::DEFAULT_DESKTOP_PORT;
    int filePort = Config::DEFAULT_FILE_PORT;

    if (useEasyTier) {
        // ---- EasyTier 模式配置 ----
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

        form.addRow("Instance Name:", leInstName);
        form.addRow("Network Name:", leNetName);
        form.addRow("Network Secret:", leNetSecret);
        form.addRow("Virtual IPv4:", leIpv4);
        form.addRow("Listen Port:", leListenPort);
        form.addRow("Public Peer URL:", lePeerUrl);
        form.addRow("Desktop Service Port:", leDesktopPort);
        form.addRow("File Service Port:", leFilePort);

        QPushButton* btnStart = new QPushButton("Start Server");
        form.addRow(btnStart);
        QObject::connect(btnStart, &QPushButton::clicked, &dlg, &QDialog::accept);

        if (dlg.exec() != QDialog::Accepted) {
            timeEndPeriod(1);
            return 0;
        }

        desktopPort = leDesktopPort->text().toInt();
        filePort    = leFilePort->text().toInt();

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
    }
    else {
        // ---- 普通 TCP 直连模式（不需要 EasyTier） ----
        myVirtualIp = "Direct TCP (not using EasyTier)";
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

    // 服务器状态窗口
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
    vLayout.addWidget(new QLabel(QString("Resolution: %1x%2")
        .arg(desktopService.getWidth()).arg(desktopService.getHeight())));
    vLayout.addStretch();
    QPushButton* btnStop = new QPushButton("Stop Server");
    btnStop->setStyleSheet("background-color: #c9302c; color: white; padding: 8px;");
    vLayout.addWidget(btnStop);
    QObject::connect(btnStop, &QPushButton::clicked, &statusWin, &QDialog::accept);
    statusWin.show();

    qApp->exec();

    // 清理
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