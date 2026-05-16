#include "transport_tcp.h"
#include <iostream>

// ==================== TCP客户端 ====================
TCPClientTransport::TCPClientTransport() {}

TCPClientTransport::~TCPClientTransport() {
    disconnect();
}

bool TCPClientTransport::connect(const std::string& ip, int port) {
    // 保存连接参数用于重连
    savedIp_ = ip;
    savedPort_ = port;

    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == INVALID_SOCKET) {
        std::cerr << "[TCP Client] socket() failed: " << WSAGetLastError() << std::endl;
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[TCP Client] Invalid IP: " << ip << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }
    addr.sin_port = htons(port);

    std::cout << "[TCP Client] Connecting to " << ip << ":" << port << "..." << std::endl;

    if (::connect(socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        std::cerr << "[TCP Client] connect() failed: " << err;
        switch (err) {
            case WSAECONNREFUSED: std::cerr << " (Connection refused - server not running?)"; break;
            case WSAETIMEDOUT:    std::cerr << " (Timed out)"; break;
            case WSAENETUNREACH:  std::cerr << " (Network unreachable)"; break;
            case WSAEADDRNOTAVAIL: std::cerr << " (Address not available)"; break;
        }
        std::cerr << std::endl;
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        return false;
    }

    int flag = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    int bufSize = 2 * 1024 * 1024;
    setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (char*)&bufSize, sizeof(bufSize));
    setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, (char*)&bufSize, sizeof(bufSize));

    connected_ = true;

    std::cout << "[TCP Client] Connected!" << std::endl;

    recvThread_ = std::thread(&TCPClientTransport::recvLoop, this);

    if (callbacks_.onConnected) callbacks_.onConnected();
    return true;
}

bool TCPClientTransport::reconnect() {
    if (savedIp_.empty() || savedPort_ == 0) {
        std::cerr << "[TCP Client] No saved connection parameters for reconnect" << std::endl;
        return false;
    }
    
    std::cout << "[TCP Client] Attempting reconnect to " << savedIp_ << ":" << savedPort_ << std::endl;
    
    // 先断开旧连接
    disconnect();
    
    // 使用保存的参数重连
    return connect(savedIp_, savedPort_);
}

void TCPClientTransport::recvLoop() {
    std::vector<uint8_t> buffer;

    while (connected_) {
        uint32_t msgSize = 0;
        if (!NetUtil::RecvAll(socket_, &msgSize, sizeof(msgSize))) break;

        if (msgSize == 0 || msgSize > MAXMSG) {
            std::cerr << "[TCP Client] Invalid message size: " << msgSize
                      << ", closing connection to avoid stream desync" << std::endl;
            break;
        }

        buffer.resize(msgSize);
        if (!NetUtil::RecvAll(socket_, buffer.data(), msgSize)) break;

        if (callbacks_.onMessage) {
            callbacks_.onMessage(buffer);
        }
    }

    connected_ = false;
    if (callbacks_.onDisconnected) callbacks_.onDisconnected();
}

bool TCPClientTransport::send(const BinaryData& data) {
    if (!connected_) return false;

    std::lock_guard<std::mutex> lock(sendMtx_);
    uint32_t size = static_cast<uint32_t>(data.size());
    if (!NetUtil::SendAll(socket_, &size, sizeof(size))) return false;
    return NetUtil::SendAll(socket_, data.data(), static_cast<int>(data.size()));
}

bool TCPClientTransport::isConnected() const {
    return connected_;
}

void TCPClientTransport::disconnect() {
    connected_ = false;
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (recvThread_.joinable()) recvThread_.join();
}

void TCPClientTransport::setCallbacks(const TransportCallbacks& callbacks) {
    callbacks_ = callbacks;
}

// ==================== TCP服务端 ====================
TCPServerTransport::TCPServerTransport(int port) : port_(port) {}

TCPServerTransport::~TCPServerTransport() {
    stop();
}

bool TCPServerTransport::start() {
    listenSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket_ == INVALID_SOCKET) {
        std::cerr << "[TCP Server] socket() failed: " << WSAGetLastError() << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listenSocket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        std::cerr << "[TCP Server] bind() port " << port_ << " failed: " << err;
        if (err == WSAEADDRINUSE) {
            std::cerr << " (Port already in use! Kill old server process)";
        }
        std::cerr << std::endl;
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket_, 2) == SOCKET_ERROR) {
        std::cerr << "[TCP Server] listen() failed: " << WSAGetLastError() << std::endl;
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    running_ = true;
    listenThread_ = std::thread(&TCPServerTransport::listenLoop, this);

    std::cout << "[TCP Server] Listening on port " << port_ << std::endl;
    return true;
}

void TCPServerTransport::listenLoop() {
    while (running_) {
        sockaddr_in caddr;
        int clen = sizeof(caddr);
        SOCKET client = accept(listenSocket_, (sockaddr*)&caddr, &clen);

        if (client == INVALID_SOCKET) {
            if (running_) {
                std::cerr << "[TCP Server] accept() failed: " << WSAGetLastError() << std::endl;
            }
            break;
        }

        if (hasClient_) {
            std::cout << "[TCP Server] Reject extra client (already has one)" << std::endl;
            closesocket(client);
            continue;
        }

        int flag = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

        int bufSize = 2 * 1024 * 1024;
        setsockopt(client, SOL_SOCKET, SO_RCVBUF, (char*)&bufSize, sizeof(bufSize));
        setsockopt(client, SOL_SOCKET, SO_SNDBUF, (char*)&bufSize, sizeof(bufSize));

        std::cout << "[TCP Server] Client connected on port " << port_ << std::endl;

        {
            std::lock_guard<std::mutex> lock(sendMtx_);
            clientSocket_ = client;
            hasClient_ = true;
        }

        if (callbacks_.onConnected) callbacks_.onConnected();

        recvThread_ = std::thread(&TCPServerTransport::recvLoop, this);
        recvThread_.join();

        std::cout << "[TCP Server] Client disconnected from port " << port_ << std::endl;

        {
            std::lock_guard<std::mutex> lock(sendMtx_);
            hasClient_ = false;
            if (clientSocket_ != INVALID_SOCKET) {
                shutdown(clientSocket_, SD_BOTH);
                closesocket(clientSocket_);
                clientSocket_ = INVALID_SOCKET;
            }
        }

        if (callbacks_.onDisconnected) callbacks_.onDisconnected();

        std::cout << "[TCP Server] Ready for next client on port " << port_ << std::endl;
    }
}

void TCPServerTransport::recvLoop() {
    std::vector<uint8_t> buffer;

    while (running_ && hasClient_) {
        uint32_t msgSize;
        if (!NetUtil::RecvAll(clientSocket_, &msgSize, sizeof(msgSize))) {
            break;
        }

        if (msgSize == 0 || msgSize > MAXMSG) {
            std::cerr << "[TCP Server] Invalid message size: " << msgSize
                    << ", closing connection to avoid stream desync" << std::endl;
            break;
        }

        buffer.resize(msgSize);
        if (!NetUtil::RecvAll(clientSocket_, buffer.data(), msgSize)) {
            break;
        }

        if (callbacks_.onMessage) {
            callbacks_.onMessage(buffer);
        }
    }
}

bool TCPServerTransport::send(const BinaryData& data) {
    if (!hasClient_) return false;

    std::lock_guard<std::mutex> lock(sendMtx_);
    if (clientSocket_ == INVALID_SOCKET) return false;

    uint32_t size = static_cast<uint32_t>(data.size());
    if (!NetUtil::SendAll(clientSocket_, &size, sizeof(size))) return false;
    return NetUtil::SendAll(clientSocket_, data.data(), static_cast<int>(data.size()));
}

bool TCPServerTransport::hasClient() const {
    return hasClient_;
}

void TCPServerTransport::stop() {
    running_ = false;
    hasClient_ = false;

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }
    if (clientSocket_ != INVALID_SOCKET) {
        shutdown(clientSocket_, SD_BOTH);
        closesocket(clientSocket_);
        clientSocket_ = INVALID_SOCKET;
    }

    if (listenThread_.joinable()) listenThread_.join();
    if (recvThread_.joinable()) recvThread_.join();
}

void TCPServerTransport::setCallbacks(const TransportCallbacks& callbacks) {
    callbacks_ = callbacks;
}