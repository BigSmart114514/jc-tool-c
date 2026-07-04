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

#include "discovery_server.h"
#include "../common/easytier_control.h"
#include "logging.h"
#include <cstring>

DiscoveryServer::DiscoveryServer() = default;

DiscoveryServer::~DiscoveryServer() {
    stop();
}

bool DiscoveryServer::start(int port) {
    if (running_.load()) return true;
    running_ = true;
    thread_ = std::thread(&DiscoveryServer::listenLoop, this);
    WriteLog("DiscoveryServer started on UDP port " + std::to_string(port));
    return true;
}

void DiscoveryServer::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable())
            thread_.join();
        WriteLog("DiscoveryServer stopped");
    }
}

std::string DiscoveryServer::getResponseJson() {
    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(compName, &size);
    std::string hostname;
    int len = WideCharToMultiByte(CP_UTF8, 0, compName, -1, NULL, 0, NULL, NULL);
    if (len > 0) {
        hostname.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, compName, -1, &hostname[0], len, NULL, NULL);
    }

    return std::string("{\"hostname\":\"") + JsonEscape(hostname)
         + "\",\"ssh_port\":" + std::to_string(sshPort_)
         + ",\"version\":\"1.0.0\"}";
}

void DiscoveryServer::listenLoop() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        WSACleanup();
        WriteLog("DiscoveryServer: bind failed on port " + std::to_string(DISCOVERY_PORT));
        return;
    }

    // Set receive timeout for clean shutdown
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    constexpr int MAGIC_LEN = 8;
    char buf[512];

    while (running_.load()) {
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) {
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
            break;
        }

        if (n >= MAGIC_LEN && memcmp(buf, DISCOVERY_MAGIC, MAGIC_LEN) == 0) {
            std::string resp = DISCOVERY_MAGIC + getResponseJson();
            sendto(sock, resp.data(), (int)resp.size(), 0,
                   (sockaddr*)&from, fromLen);
        }
    }

    closesocket(sock);
    WSACleanup();
}