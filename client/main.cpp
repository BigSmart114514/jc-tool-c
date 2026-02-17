#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/transport_p2p.h"
#include "control_panel.h"
#include <iostream>

int main() {
    SetConsoleOutputCP(65001);

    std::cout << "========================================\n";
    std::cout << "  Remote Control Client\n";
    std::cout << "========================================\n\n";

    if (!NetUtil::InitWinsock()) {
        std::cerr << "WSAStartup failed!" << std::endl;
        return 1;
    }

    p2p::P2PClient::setLogLevel(2);

    std::cout << "Transport mode:\n";
    std::cout << "  1. TCP\n";
    std::cout << "  2. P2P\n";
    std::cout << "  3. Relay\n";
    std::cout << "Select (1/2/3): ";

    std::string choice;
    std::getline(std::cin, choice);

    std::string modeText, connectInfo;
    std::string ip, sigUrl, desktopPeerId, filePeerId, relayPassword;
    int desktopPort = 0, filePort = 0;
    bool useRelay = false;
    TransportMode mode;

    if (choice == "3" || choice == "2") {
        useRelay = (choice == "3");
        mode = useRelay ? TransportMode::Relay : TransportMode::P2P;
        modeText = useRelay ? "Relay" : "P2P";

        sigUrl = "ws://localhost:8080";
        std::string input;
        std::cout << "Signaling URL (default " << sigUrl << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) sigUrl = input;

        std::cout << "Desktop Peer ID: ";
        std::getline(std::cin, desktopPeerId);
        std::cout << "File Peer ID: ";
        std::getline(std::cin, filePeerId);
        if (desktopPeerId.empty() || filePeerId.empty()) {
            std::cerr << "Peer IDs required" << std::endl;
            return 1;
        }
        if (useRelay) {
            std::cout << "Relay Password: ";
            std::getline(std::cin, relayPassword);
        }
        connectInfo = desktopPeerId + " / " + filePeerId;
    } else {
        mode = TransportMode::TCP;
        modeText = "TCP";

        ip = "127.0.0.1";
        std::string input;
        std::cout << "Server IP (default " << ip << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) ip = input;

        desktopPort = Config::DEFAULT_DESKTOP_PORT;
        filePort = Config::DEFAULT_FILE_PORT;
        std::cout << "Desktop port (default " << desktopPort << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) desktopPort = std::atoi(input.c_str());
        std::cout << "File port (default " << filePort << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) filePort = std::atoi(input.c_str());

        connectInfo = ip + ":" + std::to_string(desktopPort) + "/" + std::to_string(filePort);
    }

    // 创建传输层
    ITransport* desktopTransport = nullptr;
    ITransport* fileTransport = nullptr;

    std::cout << "\nConnecting..." << std::endl;

    if (mode == TransportMode::TCP) {
        auto* dt = new TCPClientTransport();
        auto* ft = new TCPClientTransport();

        if (!dt->connect(ip, desktopPort)) {
            std::cerr << "\nDesktop connection failed!" << std::endl;
            std::cerr << "Make sure the server is running and port " << desktopPort << " is correct.\n";
            delete dt;
            delete ft;
            WSACleanup();
            std::cout << "Press Enter to exit...";
            std::cin.get();
            return 1;
        }

        if (!ft->connect(ip, filePort)) {
            std::cerr << "\nFile connection failed!" << std::endl;
            std::cerr << "Make sure port " << filePort << " is correct.\n";
            dt->disconnect();
            delete dt;
            delete ft;
            WSACleanup();
            std::cout << "Press Enter to exit...";
            std::cin.get();
            return 1;
        }

        desktopTransport = dt;
        fileTransport = ft;
    } else {
        auto* dt = new P2PClientTransport();
        auto* ft = new P2PClientTransport();

        if (!dt->connect(sigUrl, desktopPeerId, ServiceType::Desktop, useRelay, relayPassword)) {
            std::cerr << "Desktop P2P connection failed" << std::endl;
            delete dt;
            delete ft;
            WSACleanup();
            std::cout << "Press Enter to exit...";
            std::cin.get();
            return 1;
        }

        if (!ft->connect(sigUrl, filePeerId, ServiceType::FileManager, useRelay, relayPassword)) {
            std::cerr << "File P2P connection failed" << std::endl;
            dt->disconnect();
            delete dt;
            delete ft;
            WSACleanup();
            std::cout << "Press Enter to exit...";
            std::cin.get();
            return 1;
        }

        desktopTransport = dt;
        fileTransport = ft;
    }

    std::cout << "Connected successfully!" << std::endl;

    // 创建控制面板
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    ControlPanelConfig panelConfig;
    panelConfig.desktopTransport = desktopTransport;
    panelConfig.fileTransport = fileTransport;
    panelConfig.modeText = modeText;
    panelConfig.connectInfo = connectInfo;

    ControlPanel controlPanel;
    controlPanel.setConfig(panelConfig);

    if (!controlPanel.create(hInstance)) {
        std::cerr << "Failed to create control panel" << std::endl;
        desktopTransport->disconnect();
        fileTransport->disconnect();
        delete desktopTransport;
        delete fileTransport;
        WSACleanup();
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    controlPanel.destroy();
    desktopTransport->disconnect();
    fileTransport->disconnect();
    delete desktopTransport;
    delete fileTransport;
    WSACleanup();

    std::cout << "Client stopped" << std::endl;
    return 0;
}