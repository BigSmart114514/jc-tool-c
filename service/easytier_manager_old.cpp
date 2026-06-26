#include "easytier_manager.h"
extern "C" {
#include <easytier.h>
}
#include <sstream>
#include <chrono>
#include <iostream>
#include <cstring>
#include <cstring>
#include <WinSock2.h>

namespace {

void LogInfo(const std::string& msg) {
    //std::cout << "[EasyTier] " << msg << std::endl;
}

void LogError(const std::string& msg) {
    std::cerr << "[EasyTier] ERROR: " << msg << std::endl;
}

std::string ExtractInstanceName(const std::string& toml) {
    auto pos = toml.find("instance_name");
    if (pos == std::string::npos) return "";
    auto start = toml.find('"', pos);
    auto end = toml.find('"', start + 1);
    if (start == std::string::npos || end == std::string::npos) return "";
    return toml.substr(start + 1, end - start - 1);
}

// 从 JSON 字符串中提取 "virtual_ipv4":"..." 的值，如果为 null 返回空
std::string ExtractVirtualIp(const std::string& json) {
    const char* key = "\"virtual_ipv4\":";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += strlen(key);
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos < json.size() && json[pos] == '"') {
        auto start = pos + 1;
        auto end = json.find('"', start);
        if (end != std::string::npos) return json.substr(start, end - start);
    } else if (json.compare(pos, 4, "null") == 0) {
        return ""; // 未分配
    }
    return "";
}

// 提取 vpn_portal_cfg 的状态
std::string ExtractVpnPortalCfg(const std::string& json) {
    const char* key = "\"vpn_portal_cfg\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += strlen(key);
    auto end = json.find('"', pos);
    if (end != std::string::npos) return json.substr(pos, end - pos);
    return "";
}

} // namespace

// 生成 TOML 配置字符串
std::string EasytierManager::MakeConfig(const std::string& instanceName,
                                        const std::string& networkName,
                                        const std::string& networkSecret,
                                        const std::string& ipv4,
                                        int listenPort,
                                        const std::string& peerUrl)
{
    std::ostringstream ss;
    // 注释头
    ss << "# ============================\n";
    ss << "# EasyTier 配置文件（自动生成）\n";
    ss << "# ============================\n\n";

    // [instance] 部分
    ss << "instance_name = \"" << instanceName << "\"\n";
    ss << "hostname = \"" << instanceName << "\"\n";  // 用实例名代替主机名
    // 在 [instance] 部分
    if (!ipv4.empty()) {
        ss << "ipv4 = \"" << ipv4 << "\"\n";

    } else {
        ss << "dhcp = true\n";
    }
    ss << "\n";
    ss << "listeners = [\n";
    ss << "  \"tcp://0.0.0.0:" << listenPort << "\",\n";
    ss << "  \"udp://0.0.0.0:" << listenPort << "\",\n";
    //ss << "  \"wg://0.0.0.0:" << (listenPort + 1) << "\"\n";
    // 可选监听 ws/wss/wg，这里为了简单只开 tcp 和 udp
    ss << "]\n\n";

    ss << "exit_nodes = [\n";
    ss << "]\n\n";

    ss << "rpc_portal = \"127.0.0.1:15888\"\n\n";

    // [network_identity] 节
    ss << "[network_identity]\n";
    ss << "network_name = \"" << networkName << "\"\n";
    ss << "network_secret = \"" << networkSecret << "\"\n\n";

    // [[peer]] 数组
    if (!peerUrl.empty()) {
        // 注意：peerUrl 应是完整 URI，如 "tcp://225284.xyz:11010"
        ss << "[[peer]]\n";
        ss << "uri = \"" << peerUrl << "\"\n";
        // 如果还需其他协议，可再加一个 udp 的 peer，这里简化只加一个
    }

    // [flags] 节（使用官方默认值）
    ss << "\n[flags]\n";
    ss << "default_protocol = \"tcp\"\n";
    ss << "enable_encryption = true\n";
    ss << "enable_ipv6 = false\n";
    ss << "mtu = 1500\n";
    ss << "latency_first = false\n";
    ss << "enable_exit_node = false\n";
    ss << "no_tun = false\n";
    ss << "use_smoltcp = false\n";
    ss << "foreign_network_whitelist = \"*\"\n";

    return ss.str();
}

EasytierManager::EasytierManager(const std::string& tomlConfig)
    : tomlConfig_(tomlConfig)
{
    instanceName_ = ExtractInstanceName(tomlConfig_);
}

EasytierManager::~EasytierManager() {
    stop();
}

bool EasytierManager::start() {
    if (tomlConfig_.empty()) {
        LogError("TOML config is empty");
        return false;
    }

    // 1. 验证配置
    if (parse_config(tomlConfig_.c_str()) != 0) {
        const char* err = nullptr;
        get_error_msg(&err);
        LogError("parse_config failed: " + (err ? std::string(err) : "unknown"));
        if (err) free_string(err);
        return false;
    }
    LogInfo("Config parsed successfully");

    // 2. 启动网络实例
    if (run_network_instance(tomlConfig_.c_str()) != 0) {
        const char* err = nullptr;
        get_error_msg(&err);
        LogError("run_network_instance failed: " + (err ? std::string(err) : "unknown"));
        if (err) free_string(err);
        return false;
    }
    LogInfo("Network instance started");

    // 3. 轮询等待虚拟 IP（最多 60 秒，并打印详细状态）
    bool gotIp = false;
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 每隔一次轮询详细打印一次（避免刷屏）
        if (i % 3 == 0) {
            LogInfo("=== Polling cycle " + std::to_string(i) + "s ===");
        }

        // 调用 loadIpFromInstances 时会输出诊断信息
        if (loadIpFromInstances() && !virtualIp_.empty()) {
            gotIp = true;
            break;
        }

        // 如果超过 10 秒仍无 IP，额外打印一次 vpn_portal 状态
        if (i == 10) {
            LogInfo("Still no IP after 10s, checking vpn_portal_cfg...");
            constexpr int MAX = 64;
            KeyValuePair pairs[MAX];
            int count = collect_network_infos(pairs, MAX);
            for (int j = 0; j < count; ++j) {
                if (std::strcmp(pairs[j].key, "default") == 0) {
                    std::string vpnStatus = ExtractVpnPortalCfg(pairs[j].value);
                    LogInfo("VPN Portal Status: " + (vpnStatus.empty() ? "empty" : vpnStatus));
                    break;
                }
            }
        }
    }

    if (!gotIp) {
        LogError("Failed to obtain virtual IP after 60s");
        return false;
    }

    LogInfo("Virtual IP obtained: " + virtualIp_);
    active_ = true;
    running_ = true;
    monitorThread_ = std::thread(&EasytierManager::monitorLoop, this);
    return true;
}

void EasytierManager::stop() {
    running_ = false;
    active_ = false;
    if (monitorThread_.joinable())
        monitorThread_.join();
}


// 将 addr 整数（如 176061953）转为点分十进制 IP
static std::string addrToIp(uint32_t addr) {
    struct in_addr in;
    in.s_addr = htonl(addr);   // 网络字节序
    return inet_ntoa(in);
}

bool EasytierManager::loadIpFromInstances() {
    constexpr int MAX = 64;
    KeyValuePair pairs[MAX];
    int count = collect_network_infos(pairs, MAX);
    if (count <= 0) return false;

    // 遍历所有键，找到值里包含 "virtual_ipv4" 的那一个
    for (int i = 0; i < count; ++i) {
        std::string value(pairs[i].value);
        if (value.find("\"virtual_ipv4\":") != std::string::npos) {
            // 提取 virtual_ipv4 部分的 JSON
            auto pos = value.find("\"virtual_ipv4\":{");
            if (pos == std::string::npos) continue;
            // 找到 "addr":
            auto addrPos = value.find("\"addr\":", pos);
            if (addrPos == std::string::npos) continue;
            addrPos += strlen("\"addr\":");
            auto endPos = value.find_first_of(",}", addrPos);
            std::string addrStr = value.substr(addrPos, endPos - addrPos);
            uint32_t addr = std::stoul(addrStr);
            virtualIp_ = addrToIp(addr);
            
            //LogInfo("Found virtual IP: " + virtualIp_ + " in key '" + pairs[i].key + "'");
            return true;
        }
    }
    return false;
}

bool EasytierManager::fetchNetworkInfos(std::map<std::string, std::string>& outInfo) {
    constexpr int MAX = 64;
    KeyValuePair pairs[MAX];
    int count = collect_network_infos(pairs, MAX);
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i)
        outInfo[pairs[i].key] = pairs[i].value;
    return true;
}

bool EasytierManager::attemptReconnect() {
    active_ = false;
    if (!instanceName_.empty()) {
        const char* names[] = { instanceName_.c_str() };
        retain_network_instance(names, 1);
    }
    if (parse_config(tomlConfig_.c_str()) != 0 ||
        run_network_instance(tomlConfig_.c_str()) != 0) {
        LogError("Reconnect failed");
        return false;
    }
    for (int i = 0; i < 15; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (loadIpFromInstances() && !virtualIp_.empty()) {
            active_ = true;
            LogInfo("Reconnected, new IP: " + virtualIp_);
            return true;
        }
    }
    LogError("Reconnect: could not get IP after restart");
    return false;
}

void EasytierManager::monitorLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!active_) continue;

        std::string oldIp = virtualIp_;
        if (loadIpFromInstances()) {
            if (!virtualIp_.empty() && virtualIp_ != oldIp) {
                LogInfo("IP changed from " + oldIp + " to " + virtualIp_);
            }
        } else {
            LogError("Monitor: lost virtual IP, attempting reconnect...");
            attemptReconnect();
        }
    }
}