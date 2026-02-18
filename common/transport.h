#ifndef TRANSPORT_H
#define TRANSPORT_H

#include "protocol.h"
#include <functional>
#include <memory>

// ==================== 传输层回调 ====================
struct TransportCallbacks {
    std::function<void()> onConnected;
    std::function<void()> onDisconnected;
    std::function<void(const BinaryData&)> onMessage;
    std::function<void(const std::string&)> onError;
};

// ==================== 传输层接口 ====================
class ITransport {
public:
    virtual ~ITransport() = default;
    
    virtual bool send(const BinaryData& data) = 0;
    virtual bool isConnected() const = 0;
    virtual void disconnect() = 0;
    virtual void setCallbacks(const TransportCallbacks& callbacks) = 0;
    
    // 使用之前保存的参数重新连接
    virtual bool reconnect() = 0;
};

// ==================== 服务端传输接口 ====================
class IServerTransport {
public:
    virtual ~IServerTransport() = default;
    
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool send(const BinaryData& data) = 0;
    virtual bool hasClient() const = 0;
    virtual void setCallbacks(const TransportCallbacks& callbacks) = 0;
};

// ==================== 传输模式 ====================
enum class TransportMode {
    TCP,
    P2P,
    Relay
};

#endif // TRANSPORT_H