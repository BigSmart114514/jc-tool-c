#include "transport_p2p.h"
#include <iostream>

// ==================== P2P客户端 ====================
P2PClientTransport::P2PClientTransport() {}

P2PClientTransport::~P2PClientTransport() {
    disconnect();
}

bool P2PClientTransport::connect(const std::string& signalingUrl,
                                  const std::string& peerId,
                                  ServiceType service,
                                  bool useRelay,
                                  const std::string& relayPassword) {
    // 保存连接参数用于重连
    savedSignalingUrl_ = signalingUrl;
    savedPeerId_ = peerId;
    savedService_ = service;
    savedUseRelay_ = useRelay;
    savedRelayPassword_ = relayPassword;

    peerId_ = peerId;
    service_ = service;
    useRelay_ = useRelay;

    std::string localId = (service == ServiceType::Desktop) ? "desktop_" : "file_";
    localId += std::to_string(GetTickCount());

    p2p::ClientConfig config;
    config.signalingUrl = signalingUrl;
    config.peerId = localId;

    client_ = std::make_unique<p2p::P2PClient>(config);

    client_->setOnConnected([this]() {
        std::cout << "[P2P] Signaling connected: " << client_->getLocalId() << std::endl;
    });

    client_->setOnDisconnected([this](const p2p::Error& err) {
        connected_ = false;
        ready_ = false;
        if (callbacks_.onDisconnected) callbacks_.onDisconnected();
    });

    client_->setOnError([this](const p2p::Error& err) {
        if (callbacks_.onError) callbacks_.onError(err.message);
    });

    client_->setOnPeerConnected([this](const std::string& peer) {
        if (!useRelay_ && peer == peerId_) {
            connected_ = true;
            if (callbacks_.onConnected) callbacks_.onConnected();
        }
    });

    client_->setOnPeerDisconnected([this](const std::string& peer) {
        if (!useRelay_ && peer == peerId_) {
            connected_ = false;
            ready_ = false;
            if (callbacks_.onDisconnected) callbacks_.onDisconnected();
        }
    });

    client_->setOnRelayConnected([this](const std::string& peer) {
        if (useRelay_ && peer == peerId_) {
            connected_ = true;
            if (callbacks_.onConnected) callbacks_.onConnected();
        }
    });

    client_->setOnRelayDisconnected([this](const std::string& peer) {
        if (useRelay_ && peer == peerId_) {
            connected_ = false;
            ready_ = false;
            if (callbacks_.onDisconnected) callbacks_.onDisconnected();
        }
    });

    client_->setOnBinaryMessage([this](const std::string& from, const p2p::BinaryData& data) {
        if (from == peerId_ && callbacks_.onMessage) {
            callbacks_.onMessage(data);
        }
    });

    if (!client_->connect()) {
        return false;
    }

    if (useRelay_) {
        if (!client_->authenticateRelay(relayPassword)) return false;
        if (!client_->connectToPeerViaRelay(peerId_)) return false;
    } else {
        client_->connectToPeer(peerId_);
    }

    auto start = GetTickCount();
    while (!connected_ && (GetTickCount() - start) < 30000) {
        Sleep(100);
    }

    return connected_;
}

bool P2PClientTransport::reconnect() {
    if (savedSignalingUrl_.empty() || savedPeerId_.empty()) {
        std::cerr << "[P2P Client] No saved connection parameters for reconnect" << std::endl;
        return false;
    }
    
    std::cout << "[P2P Client] Attempting reconnect to " << savedPeerId_ << std::endl;
    
    // 先断开旧连接
    disconnect();
    
    // 使用保存的参数重连
    return connect(savedSignalingUrl_, savedPeerId_, savedService_, savedUseRelay_, savedRelayPassword_);
}

bool P2PClientTransport::send(const BinaryData& data) {
    if (!connected_ || !client_) return false;
    
    if (useRelay_) {
        return client_->sendBinaryViaRelay(peerId_, data);
    } else {
        return client_->sendBinary(peerId_, data);
    }
}

bool P2PClientTransport::isConnected() const {
    return connected_;
}

void P2PClientTransport::disconnect() {
    connected_ = false;
    ready_ = false;
    if (client_) {
        if (useRelay_) {
            client_->disconnectFromPeerViaRelay(peerId_);
        } else {
            client_->disconnectFromPeer(peerId_);
        }
        client_->disconnect();
        client_.reset();
    }
}

void P2PClientTransport::setCallbacks(const TransportCallbacks& callbacks) {
    callbacks_ = callbacks;
}

// ==================== P2P服务端 ====================
P2PServerTransport::P2PServerTransport(ServiceType service) : service_(service) {}

P2PServerTransport::~P2PServerTransport() {
    stop();
}

bool P2PServerTransport::start() {
    if (signalingUrl_.empty()) {
        std::cerr << "[P2P Server] No signaling URL configured" << std::endl;
        return false;
    }
    return start(signalingUrl_, peerId_);
}

bool P2PServerTransport::start(const std::string& signalingUrl, const std::string& peerId) {
    signalingUrl_ = signalingUrl;
    
    std::string localId = peerId;
    if (localId.empty()) {
        localId = (service_ == ServiceType::Desktop) ? "desktop_server_" : "file_server_";
        localId += std::to_string(GetTickCount());
    }
    peerId_ = localId;

    p2p::ClientConfig config;
    config.signalingUrl = signalingUrl;
    config.peerId = localId;

    client_ = std::make_unique<p2p::P2PClient>(config);

    client_->setOnConnected([this]() {
        std::cout << "[P2P Server] ID: " << client_->getLocalId() << std::endl;
    });

    client_->setOnDisconnected([this](const p2p::Error& err) {
        std::cerr << "[P2P Server] Signaling disconnected: " << err.message << std::endl;
    });

    client_->setOnError([this](const p2p::Error& err) {
        if (callbacks_.onError) callbacks_.onError(err.message);
    });

    client_->setOnPeerConnected([this](const std::string& peer) {
        if (hasClient_) {
            client_->disconnectFromPeer(peer);
            return;
        }
        clientPeerId_ = peer;
        clientIsRelay_ = false;
        hasClient_ = true;
        if (callbacks_.onConnected) callbacks_.onConnected();
    });

    client_->setOnPeerDisconnected([this](const std::string& peer) {
        if (peer == clientPeerId_ && !clientIsRelay_) {
            hasClient_ = false;
            ready_ = false;
            clientPeerId_.clear();
            if (callbacks_.onDisconnected) callbacks_.onDisconnected();
        }
    });

    client_->setOnRelayConnected([this](const std::string& peer) {
        if (hasClient_) {
            client_->disconnectFromPeerViaRelay(peer);
            return;
        }
        clientPeerId_ = peer;
        clientIsRelay_ = true;
        hasClient_ = true;
        if (callbacks_.onConnected) callbacks_.onConnected();
    });

    client_->setOnRelayDisconnected([this](const std::string& peer) {
        if (peer == clientPeerId_ && clientIsRelay_) {
            hasClient_ = false;
            ready_ = false;
            clientPeerId_.clear();
            if (callbacks_.onDisconnected) callbacks_.onDisconnected();
        }
    });

    client_->setOnBinaryMessage([this](const std::string& from, const p2p::BinaryData& data) {
        if (from == clientPeerId_ && callbacks_.onMessage) {
            callbacks_.onMessage(data);
        }
    });

    return client_->connect();
}

bool P2PServerTransport::send(const BinaryData& data) {
    if (!hasClient_ || !client_) return false;
    
    if (clientIsRelay_) {
        return client_->sendBinaryViaRelay(clientPeerId_, data);
    } else {
        return client_->sendBinary(clientPeerId_, data);
    }
}

bool P2PServerTransport::hasClient() const {
    return hasClient_;
}

void P2PServerTransport::stop() {
    hasClient_ = false;
    ready_ = false;
    if (client_) {
        client_->disconnect();
        client_.reset();
    }
}

void P2PServerTransport::setCallbacks(const TransportCallbacks& callbacks) {
    callbacks_ = callbacks;
}

std::string P2PServerTransport::getLocalId() const {
    return client_ ? client_->getLocalId() : "";
}