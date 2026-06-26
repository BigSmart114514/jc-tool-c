#ifndef EASYTIER_MANAGER_H
#define EASYTIER_MANAGER_H

#include <string>
#include <atomic>
#include <thread>
#include <map>

class EasytierManager {
public:
    // 直接传入 TOML 格式的完整配置字符串
    explicit EasytierManager(const std::string& tomlConfig);
    ~EasytierManager();

    bool start();
    void stop();
    std::string getVirtualIp() const { return virtualIp_; }
    bool isActive() const { return active_.load(); }

    // 辅助函数：根据参数生成 TOML 配置字符串
    // ipv4 留空表示自动分配
    static std::string MakeConfig(const std::string& instanceName,
                                  const std::string& networkName,
                                  const std::string& networkSecret,
                                  const std::string& ipv4,   // 可为空
                                  int listenPort,
                                  const std::string& peerUrl);

private:
    void monitorLoop();
    bool loadIpFromInstances();   // 解析 JSON 获取本机虚拟 IP
    bool fetchNetworkInfos(std::map<std::string, std::string>& outInfo);
    bool attemptReconnect();

    std::string tomlConfig_;
    std::string instanceName_;
    std::string virtualIp_;
    std::atomic<bool> active_{ false };
    std::atomic<bool> running_{ false };
    std::thread monitorThread_;
};

#endif