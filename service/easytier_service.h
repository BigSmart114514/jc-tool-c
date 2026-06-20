#ifndef EASYTIER_SERVICE_H
#define EASYTIER_SERVICE_H

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include "../common/easytier_control.h"

class EasytierManager;

class EasyTierService {
public:
    EasyTierService();
    ~EasyTierService();

    bool init();

    bool startEasyTier();
    void stopEasyTier();
    bool restartEasyTier();
    bool isRunning() const;
    std::string getVirtualIp() const;

    EasyTierConfig getConfig() const;
    bool configureAndRestart(const EasyTierConfig& newConfig, bool restartNow);

    void requestStop() { stopRequested_ = true; }
    bool isStopRequested() const { return stopRequested_; }

private:
    bool saveConfigToDisk(const EasyTierConfig& cfg);

    mutable std::mutex mutex_;
    std::unique_ptr<EasytierManager> mgr_;
    EasyTierConfig config_;
    std::atomic<bool> stopRequested_{ false };
};

#endif
