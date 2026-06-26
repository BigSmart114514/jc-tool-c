#ifndef EASYTIER_SERVICE_H
#define EASYTIER_SERVICE_H

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>
#include "../common/easytier_control.h"

class EasytierManager;

class EasyTierService {
public:
    EasyTierService();
    ~EasyTierService();

    EasyTierConfig getConfig() const;
    bool init();
    bool startEasyTier();
    void stopEasyTier();
    bool restartEasyTier();
    std::string getVirtualIp() const { return virtualIp_; }
    bool isActive() const { return active_.load(); }
    bool configureAndRestart(const EasyTierConfig& newConfig, bool restartNow);
private:
    void monitorLoop();
    bool saveConfigToDisk(const EasyTierConfig& cfg);
    bool loadIpFromInstances();
    bool attemptReconnect();
    bool fetchNetworkInfos(std::map<std::string, std::string>& outInfo);
    std::string ExtractInstanceName(const std::string& toml);
    std::string ExtractVpnPortalCfg(const std::string& json);

    static std::string MakeConfig(const std::string& instanceName,
                            const std::string& networkName,
                            const std::string& networkSecret,
                            const std::string& ipv4,   // 可为空
                            int listenPort,
                            const std::string& peerUrl);

    std::string tomlConfig_;
    std::string instanceName_;
    std::string virtualIp_;
    
    mutable std::mutex mutex_;
    EasyTierConfig config_;
    std::atomic<bool> active_{ false };
    std::atomic<bool> running_{ false };
    std::thread monitorThread_;
};

#endif
