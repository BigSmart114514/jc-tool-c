
#ifndef TRANSPORT_TCP_H
#define TRANSPORT_TCP_H

#include "transport.h"
#include <thread>
#include <atomic>

// ==================== TCP客户端传输 ====================
class TCPClientTransport : public ITransport {
public:
    TCPClientTransport();
    ~TCPClientTransport();

    bool connect(const std::string& ip, int port);
    
    bool send(const BinaryData& data) override;
    bool isConnected() const override;
    void disconnect() override;
    void setCallbacks(const TransportCallbacks& callbacks) override;

private:
    void recvLoop();

    SOCKET socket_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::thread recvThread_;
    TransportCallbacks callbacks_;
    std::mutex sendMtx_;
};

// ==================== TCP服务端传输 ====================
class TCPServerTransport : public IServerTransport {
public:
    TCPServerTransport(int port);
    ~TCPServerTransport();

    bool start() override;
    void stop() override;
    bool send(const BinaryData& data) override;
    bool hasClient() const override;
    void setCallbacks(const TransportCallbacks& callbacks) override;

private:
    void listenLoop();
    void recvLoop();

    int port_;
    SOCKET listenSocket_ = INVALID_SOCKET;
    SOCKET clientSocket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::atomic<bool> hasClient_{false};
    std::thread listenThread_;
    std::thread recvThread_;
    TransportCallbacks callbacks_;
    std::mutex sendMtx_;
};

#endif // TRANSPORT_TCP_H
