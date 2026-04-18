#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <iostream>

#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/transport_p2p.h"
#include "../common/multi_transport.h"

// Client Headers
#include "../client/control_panel.h"

// Server Headers
#include "../server/desktop_service.h"
#include "../server/file_service.h"
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

// ======================= CLIENT 逻辑 =======================
int runClient() {
    p2p::P2PClient::setLogLevel(2);

    QStringList modes = {"TCP", "P2P", "Relay"};
    bool ok;
    QString mode = QInputDialog::getItem(nullptr, "Transport Mode", "Select transport mode:", modes, 0, false, &ok);
    if (!ok) return 0;

    std::string modeText, connectInfo;
    std::string ip, sigUrl, desktopPeerId, filePeerId, relayPassword;
    int desktopPort = 0, filePort = 0;
    bool useRelay = false;
    TransportMode transportMode;

    if (mode == "Relay" || mode == "P2P") {
        useRelay = (mode == "Relay");
        transportMode = useRelay ? TransportMode::Relay : TransportMode::P2P;
        modeText = mode.toStdString();

        sigUrl = QInputDialog::getText(nullptr, "Signaling Server", "Signaling URL:", QLineEdit::Normal, "ws://localhost:8080").toStdString();
        desktopPeerId = QInputDialog::getText(nullptr, "Desktop Peer", "Desktop Peer ID:").toStdString();
        filePeerId = QInputDialog::getText(nullptr, "File Peer", "File Peer ID:").toStdString();

        if (desktopPeerId.empty() || filePeerId.empty()) {
            QMessageBox::critical(nullptr, "Error", "Peer IDs required");
            return 1;
        }

        if (useRelay) {
            relayPassword = QInputDialog::getText(nullptr, "Relay Password", "Password:", QLineEdit::Password).toStdString();
        }
        connectInfo = desktopPeerId + " / " + filePeerId;
    } else {
        transportMode = TransportMode::TCP;
        modeText = "TCP";

        ip = QInputDialog::getText(nullptr, "Server IP", "Server IP:", QLineEdit::Normal, "127.0.0.1").toStdString();
        desktopPort = QInputDialog::getInt(nullptr, "Desktop Port", "Desktop port:", Config::DEFAULT_DESKTOP_PORT);
        filePort = QInputDialog::getInt(nullptr, "File Port", "File port:", Config::DEFAULT_FILE_PORT);

        connectInfo = ip + ":" + std::to_string(desktopPort) + "/" + std::to_string(filePort);
    }

    ITransport* desktopTransport = nullptr;
    ITransport* fileTransport = nullptr;

    if (transportMode == TransportMode::TCP) {
        auto* dt = new TCPClientTransport();
        auto* ft = new TCPClientTransport();

        if (!dt->connect(ip, desktopPort) || !ft->connect(ip, filePort)) {
            QMessageBox::critical(nullptr, "Connection Failed", "Failed to connect via TCP!");
            delete dt; delete ft;
            return 1;
        }
        desktopTransport = dt;
        fileTransport = ft;
    } else {
        auto* dt = new P2PClientTransport();
        auto* ft = new P2PClientTransport();

        if (!dt->connect(sigUrl, desktopPeerId, ServiceType::Desktop, useRelay, relayPassword) ||
            !ft->connect(sigUrl, filePeerId, ServiceType::FileManager, useRelay, relayPassword)) {
            QMessageBox::critical(nullptr, "Connection Failed", "Failed to connect via P2P/Relay!");
            dt->disconnect(); delete dt; delete ft;
            return 1;
        }
        desktopTransport = dt;
        fileTransport = ft;
    }

    ControlPanelConfig panelConfig;
    panelConfig.desktopTransport = desktopTransport;
    panelConfig.fileTransport = fileTransport;
    panelConfig.modeText = modeText;
    panelConfig.connectInfo = connectInfo;

    ControlPanel controlPanel;
    controlPanel.setConfig(panelConfig);
    controlPanel.setupTransportCallbacks();
    controlPanel.show();

    int result = qApp->exec(); // Client Event Loop

    desktopTransport->disconnect();
    fileTransport->disconnect();
    delete desktopTransport;
    delete fileTransport;
    return result;
}

// ======================= SERVER 逻辑 =======================
int runServer() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    p2p::P2PClient::setLogLevel(2);

    // 1. Qt Server Configuration Dialog
    QDialog configDlg;
    configDlg.setWindowTitle("Server Setup");
    configDlg.setMinimumWidth(400);
    QFormLayout form(&configDlg);

    QCheckBox* cbTcp = new QCheckBox("Enable TCP");
    cbTcp->setChecked(true);
    QLineEdit* leDesktopPort = new QLineEdit(QString::number(Config::DEFAULT_DESKTOP_PORT));
    QLineEdit* leFilePort = new QLineEdit(QString::number(Config::DEFAULT_FILE_PORT));
    
    QCheckBox* cbP2p = new QCheckBox("Enable P2P");
    cbP2p->setChecked(true);
    QLineEdit* leSigUrl = new QLineEdit("ws://localhost:8080");
    QLineEdit* leDesktopPeerId = new QLineEdit("");
    leDesktopPeerId->setPlaceholderText("Leave empty for Auto ID");
    QLineEdit* leFilePeerId = new QLineEdit("");
    leFilePeerId->setPlaceholderText("Leave empty for Auto ID");

    form.addRow(cbTcp);
    form.addRow("Desktop Port:", leDesktopPort);
    form.addRow("File Port:", leFilePort);
    form.addRow(new QLabel("--------------------------------------"));
    form.addRow(cbP2p);
    form.addRow("Signaling URL:", leSigUrl);
    form.addRow("Desktop Peer ID:", leDesktopPeerId);
    form.addRow("File Peer ID:", leFilePeerId);

    QPushButton* btnStart = new QPushButton("Start Server");
    form.addRow(btnStart);
    QObject::connect(btnStart, &QPushButton::clicked, &configDlg, &QDialog::accept);

    if (configDlg.exec() != QDialog::Accepted) {
        timeEndPeriod(1);
        return 0;
    }

    bool tcpEnabled = cbTcp->isChecked();
    bool p2pEnabled = cbP2p->isChecked();

    if (!tcpEnabled && !p2pEnabled) {
        QMessageBox::warning(nullptr, "Error", "You must enable at least one transport mode.");
        timeEndPeriod(1);
        return 1;
    }

    // 2. Initialize Services
    DesktopService desktopService;
    FileService fileService;
    if (!desktopService.init()) {
        QMessageBox::critical(nullptr, "Error", "Desktop service init failed!");
        return 1;
    }

    MultiServerTransport desktopMultiTransport;
    MultiServerTransport fileMultiTransport;

    // TCP Setup
    if (tcpEnabled) {
        int dPort = leDesktopPort->text().toInt();
        int fPort = leFilePort->text().toInt();
        auto desktopTCP = std::make_unique<TCPServerTransport>(dPort);
        auto fileTCP = std::make_unique<TCPServerTransport>(fPort);

        if (desktopTCP->start() && fileTCP->start()) {
            desktopMultiTransport.addOwnedTransport(std::move(desktopTCP), "TCP:" + std::to_string(dPort));
            fileMultiTransport.addOwnedTransport(std::move(fileTCP), "TCP:" + std::to_string(fPort));
        } else {
            QMessageBox::critical(nullptr, "TCP Error", "Failed to start TCP transports. Port may be in use.");
            timeEndPeriod(1);
            return 1;
        }
    }

    // P2P Setup
    std::string finalDesktopId = "Auto-generated";
    std::string finalFileId = "Auto-generated";
    if (p2pEnabled) {
        auto desktopP2P = std::make_unique<P2PServerTransport>(ServiceType::Desktop);
        auto fileP2P = std::make_unique<P2PServerTransport>(ServiceType::FileManager);
        desktopP2P->setConfig(leSigUrl->text().toStdString(), leDesktopPeerId->text().toStdString());
        fileP2P->setConfig(leSigUrl->text().toStdString(), leFilePeerId->text().toStdString());

        if (desktopP2P->start() && fileP2P->start()) {
            finalDesktopId = desktopP2P->getLocalId();
            finalFileId = fileP2P->getLocalId();
            desktopMultiTransport.addOwnedTransport(std::move(desktopP2P), "P2P");
            fileMultiTransport.addOwnedTransport(std::move(fileP2P), "P2P");
        } else {
            QMessageBox::critical(nullptr, "P2P Error", "Failed to start P2P transports.");
            timeEndPeriod(1);
            return 1;
        }
    }

    // 3. Start Multi Transports and Services
    desktopMultiTransport.start();
    fileMultiTransport.start();
    desktopService.setTransport(&desktopMultiTransport);
    fileService.setTransport(&fileMultiTransport);
    desktopService.start();
    fileService.start();

    // 4. Server Control Window (Replaces console while loop)
    QDialog serverStatusWin;
    serverStatusWin.setWindowTitle("Server Running");
    serverStatusWin.setMinimumSize(350, 200);
    QVBoxLayout vLayout(&serverStatusWin);
    
    vLayout.addWidget(new QLabel(QString("<b>Resolution:</b> %1x%2").arg(desktopService.getWidth()).arg(desktopService.getHeight())));
    if (tcpEnabled) {
        vLayout.addWidget(new QLabel(QString("<b>TCP Desktop Port:</b> %1").arg(leDesktopPort->text())));
        vLayout.addWidget(new QLabel(QString("<b>TCP File Port:</b> %1").arg(leFilePort->text())));
    }
    if (p2pEnabled) {
        vLayout.addWidget(new QLabel(QString("<b>P2P Desktop ID:</b> %1").arg(QString::fromStdString(finalDesktopId))));
        vLayout.addWidget(new QLabel(QString("<b>P2P File ID:</b> %1").arg(QString::fromStdString(finalFileId))));
    }

    vLayout.addStretch();
    QPushButton* btnStop = new QPushButton("Stop Server");
    btnStop->setStyleSheet("background-color: #c9302c; color: white; padding: 8px;");
    vLayout.addWidget(btnStop);
    QObject::connect(btnStop, &QPushButton::clicked, &serverStatusWin, &QDialog::accept);

    serverStatusWin.show();

    // 等待用户点击 Stop Server 关闭窗口
    qApp->exec(); 

    // 5. Cleanup
    desktopService.stop();
    fileService.stop();
    desktopMultiTransport.stop();
    fileMultiTransport.stop();
    timeEndPeriod(1);

    return 0;
}

// ======================= 入口 =======================
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    // 初始化全局 Winsock (对 Client 和 Server 都是必须的)
    if (!NetUtil::InitWinsock()) {
        QMessageBox::critical(nullptr, "Error", "WSAStartup failed!");
        return 1;
    }

    // 启动选择窗口
    QDialog startupDlg;
    startupDlg.setWindowTitle("JC Tool Mode Selector");
    startupDlg.setFixedSize(300, 150);
    QVBoxLayout layout(&startupDlg);
    
    QLabel* label = new QLabel("Select application mode to start:");
    label->setAlignment(Qt::AlignCenter);
    layout.addWidget(label);

    QPushButton* btnClient = new QPushButton("Start as Client");
    QPushButton* btnServer = new QPushButton("Start as Server");
    btnClient->setMinimumHeight(35);
    btnServer->setMinimumHeight(35);

    layout.addWidget(btnClient);
    layout.addWidget(btnServer);

    int mode = 0; // 1 = Client, 2 = Server
    QObject::connect(btnClient, &QPushButton::clicked, [&](){ mode = 1; startupDlg.accept(); });
    QObject::connect(btnServer, &QPushButton::clicked, [&](){ mode = 2; startupDlg.accept(); });

    if (startupDlg.exec() != QDialog::Accepted) {
        WSACleanup();
        return 0;
    }

    int result = 0;
    if (mode == 1) {
        result = runClient();
    } else if (mode == 2) {
        result = runServer();
    }

    WSACleanup();
    return result;
}