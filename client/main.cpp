#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>
#include <iostream>
#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/transport_p2p.h"
#include "control_panel.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    
    std::cout << "========================================\n";
    std::cout << "  Remote Control Client (Qt6)\n";
    std::cout << "========================================\n\n";

    if (!NetUtil::InitWinsock()) {
        QMessageBox::critical(nullptr, "Error", "WSAStartup failed!");
        return 1;
    }

    p2p::P2PClient::setLogLevel(2);

    // 选择传输模式
    QStringList modes = {"TCP", "P2P", "Relay"};
    bool ok;
    QString mode = QInputDialog::getItem(nullptr, "Transport Mode",
                                        "Select transport mode:",
                                        modes, 0, false, &ok);
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

        sigUrl = "ws://localhost:8080";
        sigUrl = QInputDialog::getText(nullptr, "Signaling Server",
                                      "Signaling URL:",
                                      QLineEdit::Normal, QString::fromStdString(sigUrl)).toStdString();

        desktopPeerId = QInputDialog::getText(nullptr, "Desktop Peer",
                                             "Desktop Peer ID:").toStdString();
        filePeerId = QInputDialog::getText(nullptr, "File Peer",
                                          "File Peer ID:").toStdString();

        if (desktopPeerId.empty() || filePeerId.empty()) {
            QMessageBox::critical(nullptr, "Error", "Peer IDs required");
            return 1;
        }

        if (useRelay) {
            relayPassword = QInputDialog::getText(nullptr, "Relay Password",
                                                 "Password:", QLineEdit::Password).toStdString();
        }

        connectInfo = desktopPeerId + " / " + filePeerId;
    } else {
        transportMode = TransportMode::TCP;
        modeText = "TCP";

        ip = "127.0.0.1";
        ip = QInputDialog::getText(nullptr, "Server IP",
                                  "Server IP:",
                                  QLineEdit::Normal, QString::fromStdString(ip)).toStdString();

        desktopPort = QInputDialog::getInt(nullptr, "Desktop Port",
                                          "Desktop port:",
                                          Config::DEFAULT_DESKTOP_PORT);
        filePort = QInputDialog::getInt(nullptr, "File Port",
                                       "File port:",
                                       Config::DEFAULT_FILE_PORT);

        connectInfo = ip + ":" + std::to_string(desktopPort) + "/" + std::to_string(filePort);
    }

    // 创建传输层
    ITransport* desktopTransport = nullptr;
    ITransport* fileTransport = nullptr;

    std::cout << "\nConnecting..." << std::endl;

    if (transportMode == TransportMode::TCP) {
        auto* dt = new TCPClientTransport();
        auto* ft = new TCPClientTransport();

        if (!dt->connect(ip, desktopPort)) {
            QMessageBox::critical(nullptr, "Connection Failed",
                QString("Desktop connection failed!\nMake sure the server is running and port %1 is correct.")
                .arg(desktopPort));
            delete dt;
            delete ft;
            WSACleanup();
            return 1;
        }

        if (!ft->connect(ip, filePort)) {
            QMessageBox::critical(nullptr, "Connection Failed",
                QString("File connection failed!\nMake sure port %1 is correct.")
                .arg(filePort));
            dt->disconnect();
            delete dt;
            delete ft;
            WSACleanup();
            return 1;
        }

        desktopTransport = dt;
        fileTransport = ft;
    } else {
        auto* dt = new P2PClientTransport();
        auto* ft = new P2PClientTransport();

        if (!dt->connect(sigUrl, desktopPeerId, ServiceType::Desktop, useRelay, relayPassword)) {
            QMessageBox::critical(nullptr, "Connection Failed", "Desktop P2P connection failed");
            delete dt;
            delete ft;
            WSACleanup();
            return 1;
        }

        if (!ft->connect(sigUrl, filePeerId, ServiceType::FileManager, useRelay, relayPassword)) {
            QMessageBox::critical(nullptr, "Connection Failed", "File P2P connection failed");
            dt->disconnect();
            delete dt;
            delete ft;
            WSACleanup();
            return 1;
        }

        desktopTransport = dt;
        fileTransport = ft;
    }

    std::cout << "Connected successfully!" << std::endl;

    // 创建控制面板
    ControlPanelConfig panelConfig;
    panelConfig.desktopTransport = desktopTransport;
    panelConfig.fileTransport = fileTransport;
    panelConfig.modeText = modeText;
    panelConfig.connectInfo = connectInfo;

    ControlPanel controlPanel;
    controlPanel.setConfig(panelConfig);
    controlPanel.setupTransportCallbacks();
    controlPanel.show();

    int result = app.exec();

    desktopTransport->disconnect();
    fileTransport->disconnect();
    delete desktopTransport;
    delete fileTransport;
    WSACleanup();

    std::cout << "Client stopped" << std::endl;
    return result;
}