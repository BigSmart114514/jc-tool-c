#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/transport_p2p.h"
#include "../common/multi_transport.h"
#include "desktop_service.h"
#include "file_service.h"
#include <iostream>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

std::atomic<bool> g_running(true);

int main() {
    SetConsoleOutputCP(65001);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    std::cout << "========================================\n";
    std::cout << "  Remote Control Server\n";
    std::cout << "  (TCP + P2P + Relay)\n";
    std::cout << "========================================\n\n";

    if (!NetUtil::InitWinsock()) {
        std::cerr << "WSAStartup failed!" << std::endl;
        return 1;
    }

    p2p::P2PClient::setLogLevel(2);

    DesktopService desktopService;
    FileService fileService;

    if (!desktopService.init()) {
        std::cerr << "Desktop service init failed" << std::endl;
        return 1;
    }

    std::cout << "Resolution: " << desktopService.getWidth()
              << "x" << desktopService.getHeight() << std::endl;

    MultiServerTransport desktopMultiTransport;
    MultiServerTransport fileMultiTransport;

    TCPServerTransport* desktopTCP = nullptr;
    TCPServerTransport* fileTCP = nullptr;
    P2PServerTransport* desktopP2P = nullptr;
    P2PServerTransport* fileP2P = nullptr;

    bool tcpEnabled = false;
    bool p2pEnabled = false;

    // ==================== TCP ====================
    std::cout << "\n[TCP Configuration]\n";

    int desktopPort = Config::DEFAULT_DESKTOP_PORT;
    int filePort = Config::DEFAULT_FILE_PORT;
    std::string input;

    std::cout << "Desktop TCP port (default " << desktopPort << ", 0 to skip): ";
    std::getline(std::cin, input);
    if (!input.empty()) desktopPort = std::atoi(input.c_str());

    if (desktopPort > 0) {
        std::cout << "File TCP port (default " << filePort << "): ";
        std::getline(std::cin, input);
        if (!input.empty()) filePort = std::atoi(input.c_str());

        auto desktopTCPPtr = std::make_unique<TCPServerTransport>(desktopPort);
        auto fileTCPPtr = std::make_unique<TCPServerTransport>(filePort);

        desktopTCP = desktopTCPPtr.get();
        fileTCP = fileTCPPtr.get();

        // 先启动，确认成功后再加入 MultiTransport
        bool dtOk = desktopTCP->start();
        bool ftOk = fileTCP->start();

        if (dtOk && ftOk) {
            desktopMultiTransport.addOwnedTransport(std::move(desktopTCPPtr),
                                                     "TCP:" + std::to_string(desktopPort));
            fileMultiTransport.addOwnedTransport(std::move(fileTCPPtr),
                                                  "TCP:" + std::to_string(filePort));
            tcpEnabled = true;
            std::cout << "TCP: OK (Desktop=" << desktopPort << ", File=" << filePort << ")\n";
        } else {
            std::cerr << "TCP: FAILED to start!\n";
            if (dtOk) desktopTCP->stop();
            if (ftOk) fileTCP->stop();
            // unique_ptr会自动释放
            desktopTCP = nullptr;
            fileTCP = nullptr;
        }
    }

    // ==================== P2P ====================
    std::cout << "\n[P2P Configuration]\n";

    std::string sigUrl = "ws://localhost:8080";
    std::cout << "Signaling URL (default " << sigUrl << ", 'skip' to skip): ";
    std::getline(std::cin, input);

    if (input != "skip") {
        if (!input.empty()) sigUrl = input;

        std::string desktopPeerId, filePeerId;
        std::cout << "Desktop Peer ID (empty for auto): ";
        std::getline(std::cin, desktopPeerId);
        std::cout << "File Peer ID (empty for auto): ";
        std::getline(std::cin, filePeerId);

        auto desktopP2PPtr = std::make_unique<P2PServerTransport>(ServiceType::Desktop);
        auto fileP2PPtr = std::make_unique<P2PServerTransport>(ServiceType::FileManager);

        desktopP2PPtr->setConfig(sigUrl, desktopPeerId);
        fileP2PPtr->setConfig(sigUrl, filePeerId);

        desktopP2P = desktopP2PPtr.get();
        fileP2P = fileP2PPtr.get();

        bool dpOk = desktopP2P->start();
        bool fpOk = fileP2P->start();

        if (dpOk && fpOk) {
            desktopMultiTransport.addOwnedTransport(std::move(desktopP2PPtr), "P2P");
            fileMultiTransport.addOwnedTransport(std::move(fileP2PPtr), "P2P");
            p2pEnabled = true;
            std::cout << "P2P: OK\n";
        } else {
            std::cerr << "P2P: FAILED to start!\n";
            if (dpOk) desktopP2P->stop();
            if (fpOk) fileP2P->stop();
            desktopP2P = nullptr;
            fileP2P = nullptr;
        }
    }

    if (!tcpEnabled && !p2pEnabled) {
        std::cerr << "\nNo transport started! Exiting." << std::endl;
        timeEndPeriod(1);
        WSACleanup();
        std::cout << "Press Enter...";
        std::cin.get();
        return 1;
    }

    // 设置回调
    desktopMultiTransport.start();
    fileMultiTransport.start();

    desktopService.setTransport(&desktopMultiTransport);
    fileService.setTransport(&fileMultiTransport);

    desktopService.start();
    fileService.start();

    // 状态输出
    std::cout << "\n========================================\n";
    std::cout << "  Server Ready\n";
    std::cout << "========================================\n";

    if (tcpEnabled) {
        std::cout << "  [TCP]\n";
        std::cout << "    Desktop: port " << desktopPort << "\n";
        std::cout << "    File:    port " << filePort << "\n";
    }

    if (p2pEnabled && desktopP2P && fileP2P) {
        std::cout << "  [P2P/Relay]\n";
        std::cout << "    Desktop: " << desktopP2P->getLocalId() << "\n";
        std::cout << "    File:    " << fileP2P->getLocalId() << "\n";
    }

    std::cout << "========================================\n";
    std::cout << "Press Ctrl+C to stop\n\n";

    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
            g_running = false;
            return TRUE;
        }
        return FALSE;
    }, TRUE);

    while (g_running) {
        Sleep(1000);
    }

    std::cout << "\nStopping...\n";

    desktopService.stop();
    fileService.stop();
    desktopMultiTransport.stop();
    fileMultiTransport.stop();

    timeEndPeriod(1);
    WSACleanup();

    std::cout << "Server stopped" << std::endl;
    return 0;
}