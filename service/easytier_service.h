#ifndef EASYTIER_SERVICE_H
#define EASYTIER_SERVICE_H

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <map>
#include "../common/easytier_control.h"
#include "ssh_server.h"

class EasytierManager;

// configureAndRestart 的结果：保存成功和"重启成功"是两件事。
// 以前用一个 bool 表达，导致实例没在跑时（restart 必然失败）连
// "配置已保存"都被判成失败。
struct ConfigureResult {
    bool saved = false;          // 配置是否已写盘
    bool restarted = false;      // 是否真的重启了 EasyTier 实例
    bool wasRunning = false;     // 调用时实例是否在运行
    std::string message;         // 失败原因 / 提示
};

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
    ConfigureResult configureAndRestart(const EasyTierConfig& newConfig, bool restartNow);

    // SSH server management
    bool startSshServer(int port, const std::string& password);
    void stopSshServer();
    bool isSshRunning() const { return sshRunning_.load(); }
    int getSshPort() const { return sshPort_; }
    std::string getDevicesJson();
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

    // SSH server
    SshServer sshServer_;
    std::atomic<bool> sshRunning_{ false };
    int sshPort_ = 2222;
};

#endif
