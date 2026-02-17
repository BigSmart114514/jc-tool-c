#ifndef TRANSPORT_P2P_H
#define TRANSPORT_P2P_H

#include "transport.h"
#include <p2p/p2p_client.hpp>
#include <memory>

// ==================== P2P客户端传输 ====================
class P2PClientTransport : public ITransport {
public:
    P2PClientTransport();
    ~P2PClientTransport();

    bool connect(const std::string& signalingUrl, 
                 const std::string& peerId,
                 ServiceType service,
                 bool useRelay = false,
                 const std::string& relayPassword = "");
    
    bool send(const BinaryData& data) override;
    bool isConnected() const override;
    void disconnect() override;
    void setCallbacks(const TransportCallbacks& callbacks) override;

private:
    std::unique_ptr<p2p::P2PClient> client_;
    std::string peerId_;
    ServiceType service_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ready_{false};
    bool useRelay_ = false;
    TransportCallbacks callbacks_;
};

// ==================== P2P服务端传输 ====================
class P2PServerTransport : public IServerTransport {
public:
    P2PServerTransport(ServiceType service);
    ~P2PServerTransport();

    // 带参数的启动
    bool start(const std::string& signalingUrl, const std::string& peerId = "");
    
    // IServerTransport 接口
    bool start() override;  // 使用预设参数
    void stop() override;
    bool send(const BinaryData& data) override;
    bool hasClient() const override;
    void setCallbacks(const TransportCallbacks& callbacks) override;
    
    // 预设参数
    void setConfig(const std::string& signalingUrl, const std::string& peerId = "") {
        signalingUrl_ = signalingUrl;
        peerId_ = peerId;
    }
    
    std::string getLocalId() const;

private:
    std::unique_ptr<p2p::P2PClient> client_;
    std::string clientPeerId_;
    std::string signalingUrl_;
    std::string peerId_;
    ServiceType service_;
    std::atomic<bool> hasClient_{false};
    std::atomic<bool> ready_{false};
    bool clientIsRelay_ = false;
    TransportCallbacks callbacks_;
};

#endif // TRANSPORT_P2P_H