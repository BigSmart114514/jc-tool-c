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

#include "../common/protocol.h"
#include "../common/transport_tcp.h"        // 仅保留 TCP
#include "../common/easytier_manager.h"            // 新增

// 客户端窗口
#include "../client/control_panel.h"

// 服务端服务
#include "../server/desktop_service.h"
#include "../server/file_service.h"

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

// ======================= CLIENT =======================
int runClient() {
    // 1. 构造客户端 EasyTier 配置对话框
    QDialog dlg;
    dlg.setWindowTitle("Client - EasyTier Setup");
    dlg.setMinimumWidth(420);
    QFormLayout form(&dlg);

    QLineEdit* leInstName   = new QLineEdit("jc-client");
    QLineEdit* leNetName    = new QLineEdit("jc-tool-vpn");
    QLineEdit* leNetSecret  = new QLineEdit("your_secret_here");
    QLineEdit* leIpv4       = new QLineEdit("");          // 留空表示自动分配
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

    if (dlg.exec() != QDialog::Accepted)
        return 0;

    // 生成 TOML（ipv4 可空）
    std::string toml = EasytierManager::MakeConfig(
        leInstName->text().toStdString(),
        leNetName->text().toStdString(),
        leNetSecret->text().toStdString(),
        leIpv4->text().toStdString(),       // 空字符串将不写 ipv4 字段
        leListenPort->text().toInt(),
        lePeerUrl->text().toStdString()
    );

    EasytierManager easytierMgr(toml);
    if (!easytierMgr.start()) {
        QMessageBox::critical(nullptr, "EasyTier Error", "Could not start EasyTier network.");
        return 1;
    }

    // 显示本机虚拟 IP
    QMessageBox::information(nullptr, "Connected",
        "Your virtual IP: " + QString::fromStdString(easytierMgr.getVirtualIp()));

    // 2. 询问服务端信息（服务端虚拟 IP 仍然需要输入）
    bool ok;
    QString serverIp = QInputDialog::getText(nullptr, "Server Virtual IP",
        "Enter server virtual IP:", QLineEdit::Normal, "10.0.0.1", &ok);
    if (!ok || serverIp.isEmpty()) return 0;

    int desktopPort = QInputDialog::getInt(nullptr, "Desktop Port",
        "Desktop service port:", Config::DEFAULT_DESKTOP_PORT);
    int filePort = QInputDialog::getInt(nullptr, "File Port",
        "File service port:", Config::DEFAULT_FILE_PORT);
    if (desktopPort == 0 || filePort == 0) return 0;

    // 3. TCP 连接
    auto* desktopTransport = new TCPClientTransport();
    auto* fileTransport = new TCPClientTransport();

    if (!desktopTransport->connect(serverIp.toStdString(), desktopPort) ||
        !fileTransport->connect(serverIp.toStdString(), filePort)) {
        QMessageBox::critical(nullptr, "Connection Failed",
            "Cannot connect to server via EasyTier.");
        delete desktopTransport;
        delete fileTransport;
        return 1;
    }

    // 4. 控制面板
    ControlPanelConfig panelConfig;
    panelConfig.desktopTransport = desktopTransport;
    panelConfig.fileTransport = fileTransport;
    panelConfig.modeText = "EasyTier";
    panelConfig.connectInfo = serverIp.toStdString() + ":" + std::to_string(desktopPort);

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

    // 1. 服务端 EasyTier 配置对话框
    QDialog dlg;
    dlg.setWindowTitle("Server - EasyTier Setup");
    dlg.setMinimumWidth(420);
    QFormLayout form(&dlg);

    QLineEdit* leInstName   = new QLineEdit("jc-server");
    QLineEdit* leNetName    = new QLineEdit("jc-tool-vpn");
    QLineEdit* leNetSecret  = new QLineEdit("your_secret_here");
    QLineEdit* leIpv4       = new QLineEdit("10.0.0.1");
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

    int desktopPort = leDesktopPort->text().toInt();
    int filePort    = leFilePort->text().toInt();

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
        timeEndPeriod(1);
        return 1;
    }

    std::string myVirtualIp = easytierMgr.getVirtualIp();

    // 2. 初始化服务并绑定 TCP 传输
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

    // 3. 服务器状态窗口
    QDialog statusWin;
    statusWin.setWindowTitle("Server Running on EasyTier");
    statusWin.setMinimumSize(400, 250);
    QVBoxLayout vLayout(&statusWin);
    vLayout.addWidget(new QLabel(QString("Virtual IP: %1").arg(myVirtualIp.c_str())));
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

    // 4. 清理
    desktopService.stop();
    fileService.stop();
    desktopTCP->stop();
    fileTCP->stop();
    easytierMgr.stop();
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

    // 模式选择
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
    if (mode == 1)
        result = runClient();
    else if (mode == 2)
        result = runServer();

    WSACleanup();
    return result;
}