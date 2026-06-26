#include "easytier_service.h"
#include <fstream>
#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <map>
#include <windows.h>
extern "C" {
#include <easytier.h>
}


static void WriteLog(const std::string& msg) {
    HANDLE hFile = CreateFileW(L"C:\\ProgramData\\EasyTier\\service.log",
        FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    sprintf_s(buf, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = buf + msg + "\r\n";
    DWORD written;
    WriteFile(hFile, line.data(), (DWORD)line.size(), &written, NULL);
    CloseHandle(hFile);
}

static std::string addrToIp(uint32_t addr) {
    struct in_addr in;
    in.s_addr = htonl(addr);   // 网络字节序
    return inet_ntoa(in);
}

bool EasyTierService::loadIpFromInstances() {
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

bool EasyTierService::fetchNetworkInfos(std::map<std::string, std::string>& outInfo) {
    constexpr int MAX = 64;
    KeyValuePair pairs[MAX];
    int count = collect_network_infos(pairs, MAX);
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i)
        outInfo[pairs[i].key] = pairs[i].value;
    return true;
}

// 生成 TOML 配置字符串
std::string EasyTierService::MakeConfig(const std::string& instanceName,
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

EasyTierService::EasyTierService() = default;

EasyTierService::~EasyTierService() {
    stopEasyTier();
}

bool EasyTierService::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!LoadConfigFromFile(config_)) {
        WriteLog("No config file, using defaults");
    } else {
        config_.networkSecret = DecryptSecret(config_.networkSecret);
    }
    WriteLog("Config: instance=" + config_.instanceName
             + " network=" + config_.networkName
             + " autoStart=" + (config_.autoStart ? "yes" : "no"));
    return true;
}

bool EasyTierService::startEasyTier() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isActive()) return true;
        tomlConfig_ = EasyTierService::MakeConfig(
            config_.instanceName, config_.networkName, config_.networkSecret,
            config_.ipv4, config_.listenPort, config_.peerUrl);
    }
    stopEasyTier();            // ← 此时没有锁，不会 crash
    std::string tomlConfig;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WriteLog("Trying to start EasyTier");
        if (isActive()) return true;  // 双重检查，防止并发
        instanceName_ = ExtractInstanceName(tomlConfig_);

        if (tomlConfig_.empty()) {
            WriteLog("TOML config is empty");
            return false;
        }

        // 1. 验证配置
        if (parse_config(tomlConfig_.c_str()) != 0) {
            const char* err = nullptr;
            get_error_msg(&err);
            WriteLog("parse_config failed: " + (err ? std::string(err) : "unknown"));
            if (err) free_string(err);
            return false;
        }
        WriteLog("Config parsed successfully");
        tomlConfig = tomlConfig_; 
    }

    // 2. 启动网络实例
    if (run_network_instance(tomlConfig.c_str()) != 0) {
        const char* err = nullptr;
        get_error_msg(&err);
        WriteLog("run_network_instance failed: " + (err ? std::string(err) : "unknown"));
        if (err) free_string(err);
        return false;
    }
    WriteLog("Network instance started");

    // 3. 轮询等待虚拟 IP（最多 60 秒，并打印详细状态）
    bool gotIp = false;
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // 每隔一次轮询详细打印一次（避免刷屏）
        if (i % 3 == 0) {
            WriteLog("=== Polling cycle " + std::to_string(i) + "s ===");
        }

        // 调用 loadIpFromInstances 时会输出诊断信息
        if (loadIpFromInstances() && !virtualIp_.empty()) {
            gotIp = true;
            break;
        }

        // 如果超过 10 秒仍无 IP，额外打印一次 vpn_portal 状态
        if (i == 10) {
            WriteLog("Still no IP after 10s, checking vpn_portal_cfg...");
            constexpr int MAX = 64;
            KeyValuePair pairs[MAX];
            int count = collect_network_infos(pairs, MAX);
            for (int j = 0; j < count; ++j) {
                if (std::strcmp(pairs[j].key, "default") == 0) {
                    std::string vpnStatus = ExtractVpnPortalCfg(pairs[j].value);
                    WriteLog("VPN Portal Status: " + (vpnStatus.empty() ? "empty" : vpnStatus));
                    break;
                }
            }
        }
    }

    if (!gotIp) {
        WriteLog("Failed to obtain virtual IP after 60s");
        WriteLog("startEasyTier FAILED");
        return false;
    }

    WriteLog("Virtual IP obtained: " + virtualIp_);
    active_ = true;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitorThread_ = std::thread(&EasyTierService::monitorLoop, this);
    }

    WriteLog("EasyTier started, IP=" + getVirtualIp());
    
    return true;
}

std::string EasyTierService::ExtractInstanceName(const std::string& toml) {
    auto pos = toml.find("instance_name");
    if (pos == std::string::npos) return "";
    auto start = toml.find('"', pos);
    auto end = toml.find('"', start + 1);
    if (start == std::string::npos || end == std::string::npos) return "";
    return toml.substr(start + 1, end - start - 1);
}

std::string EasyTierService::ExtractVpnPortalCfg(const std::string& json) {
    const char* key = "\"vpn_portal_cfg\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += strlen(key);
    auto end = json.find('"', pos);
    if (end != std::string::npos) return json.substr(pos, end - pos);
    return "";
}

void EasyTierService::stopEasyTier() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        running_ = false;
        active_ = false;
        virtualIp_ = "";
        if (monitorThread_.joinable())
            monitorThread_.join();
        retain_network_instance(nullptr, 0);   // <-- 释放 FFI 后台实例
        WriteLog("EasyTier stopped");
    }
}

bool EasyTierService::restartEasyTier() {
    stopEasyTier();
    return startEasyTier();
}

EasyTierConfig EasyTierService::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool EasyTierService::saveConfigToDisk(const EasyTierConfig& cfg) {
    EasyTierConfig toSave = cfg;
    toSave.networkSecret = EncryptSecret(cfg.networkSecret);
    bool ok = SaveConfigToFile(toSave);
    WriteLog(ok ? "Config saved" : "Config SAVE FAILED");
    return ok;
}

bool EasyTierService::configureAndRestart(const EasyTierConfig& newConfig, bool restartNow) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = newConfig;
    }
    if (!saveConfigToDisk(newConfig)) return false;
    if (restartNow) return restartEasyTier();
    return true;
}

void EasyTierService::monitorLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!active_) continue;

        std::string oldIp = virtualIp_;
        if (loadIpFromInstances()) {
            if (!virtualIp_.empty() && virtualIp_ != oldIp) {
                WriteLog("IP changed from " + oldIp + " to " + virtualIp_);
            }
        } else {
            WriteLog("Monitor: lost virtual IP, attempting reconnect...");
            attemptReconnect();
        }
    }
}

bool EasyTierService::attemptReconnect() {
    active_ = false;
    if (!instanceName_.empty()) {
        const char* names[] = { instanceName_.c_str() };
        retain_network_instance(names, 1);
    }
    if (parse_config(tomlConfig_.c_str()) != 0 ||
        run_network_instance(tomlConfig_.c_str()) != 0) {
        WriteLog("Reconnect failed");
        return false;
    }
    for (int i = 0; i < 15; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (loadIpFromInstances() && !virtualIp_.empty()) {
            active_ = true;
            WriteLog("Reconnected, new IP: " + virtualIp_);
            return true;
        }
    }
    WriteLog("Reconnect: could not get IP after restart");
    return false;
}