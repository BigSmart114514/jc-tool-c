#ifndef MULTI_TRANSPORT_H
#define MULTI_TRANSPORT_H

#include "transport.h"
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <iostream>

class MultiServerTransport : public IServerTransport {
public:
    MultiServerTransport() = default;
    ~MultiServerTransport() { stop(); }

    void addTransport(IServerTransport* transport, const std::string& name) {
        auto entry = std::make_unique<TransportEntry>();
        entry->transport = transport;
        entry->name = name;
        entry->owned = false;
        transports_.push_back(std::move(entry));
    }

    void addOwnedTransport(std::unique_ptr<IServerTransport> transport, const std::string& name) {
        auto entry = std::make_unique<TransportEntry>();
        entry->transport = transport.get();
        entry->name = name;
        entry->owned = true;
        entry->ownedPtr = std::move(transport);
        transports_.push_back(std::move(entry));
    }

    bool start() override {
        for (auto& entry : transports_) {
            setupCallbacks(entry.get());
        }
        running_ = true;
        return true;
    }

    void stop() override {
        running_ = false;
        for (auto& entry : transports_) {
            if (entry->owned && entry->ownedPtr) {
                entry->ownedPtr->stop();
            }
        }
        std::lock_guard<std::mutex> lock(mtx_);
        activeTransport_ = nullptr;
        activeTransportName_.clear();
    }

    bool send(const BinaryData& data) override {
        // 拷贝指针后立即释放锁，避免长时间持锁
        IServerTransport* transport = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (activeTransport_ && activeTransport_->hasClient()) {
                transport = activeTransport_;
            }
        }
        if (transport) {
            return transport->send(data);
        }
        return false;
    }

    bool hasClient() const override {
        std::lock_guard<std::mutex> lock(mtx_);
        return activeTransport_ != nullptr && activeTransport_->hasClient();
    }

    void setCallbacks(const TransportCallbacks& callbacks) override {
        std::lock_guard<std::mutex> lock(callbackMtx_);
        callbacks_ = callbacks;
    }

    std::string getActiveTransportName() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return activeTransportName_;
    }

private:
    struct TransportEntry {
        IServerTransport* transport = nullptr;
        std::string name;
        bool owned = false;
        std::unique_ptr<IServerTransport> ownedPtr;

        TransportEntry() = default;
        ~TransportEntry() = default;
        TransportEntry(const TransportEntry&) = delete;
        TransportEntry& operator=(const TransportEntry&) = delete;
        TransportEntry(TransportEntry&&) = default;
        TransportEntry& operator=(TransportEntry&&) = default;
    };

    void setupCallbacks(TransportEntry* entry) {
        TransportCallbacks cb;

        cb.onConnected = [this, entry]() {
            onTransportConnected(entry);
        };

        cb.onDisconnected = [this, entry]() {
            onTransportDisconnected(entry);
        };

        cb.onMessage = [this](const BinaryData& data) {
            TransportCallbacks cb;
            {
                std::lock_guard<std::mutex> lock(callbackMtx_);
                cb = callbacks_;
            }
            if (cb.onMessage) {
                cb.onMessage(data);
            }
        };

        cb.onError = [this](const std::string& err) {
            TransportCallbacks cb;
            {
                std::lock_guard<std::mutex> lock(callbackMtx_);
                cb = callbacks_;
            }
            if (cb.onError) {
                cb.onError(err);
            }
        };

        entry->transport->setCallbacks(cb);
    }

    void onTransportConnected(TransportEntry* entry) {
        // 先在锁内更新状态
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);

            if (activeTransport_ != nullptr && activeTransport_->hasClient()) {
                std::cout << "[MultiTransport] Reject " << entry->name
                          << " (active: " << activeTransportName_ << ")" << std::endl;
                return;
            }

            activeTransport_ = entry->transport;
            activeTransportName_ = entry->name;
            accepted = true;
        }

        // 在锁外调用回调，避免死锁
        if (accepted) {
            std::cout << "[MultiTransport] Client via " << entry->name << std::endl;

            TransportCallbacks cb;
            {
                std::lock_guard<std::mutex> lock(callbackMtx_);
                cb = callbacks_;
            }
            if (cb.onConnected) {
                cb.onConnected();
            }
        }
    }

    void onTransportDisconnected(TransportEntry* entry) {
        bool wasActive = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);

            if (activeTransport_ == entry->transport) {
                std::cout << "[MultiTransport] Disconnected from " << entry->name << std::endl;
                activeTransport_ = nullptr;
                activeTransportName_.clear();
                wasActive = true;
            }
        }

        // 在锁外调用回调
        if (wasActive) {
            TransportCallbacks cb;
            {
                std::lock_guard<std::mutex> lock(callbackMtx_);
                cb = callbacks_;
            }
            if (cb.onDisconnected) {
                cb.onDisconnected();
            }
        }
    }

    std::vector<std::unique_ptr<TransportEntry>> transports_;
    IServerTransport* activeTransport_ = nullptr;
    std::string activeTransportName_;
    TransportCallbacks callbacks_;
    mutable std::mutex mtx_;          // 保护 activeTransport_
    mutable std::mutex callbackMtx_;  // 保护 callbacks_
    std::atomic<bool> running_{false};
};

#endif // MULTI_TRANSPORT_H