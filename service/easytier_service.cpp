#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include "easytier_service.h"
#include "logging.h"
#include "../server/virtual_display.h"
#include <fstream>
#include <iostream>
#include <string>
#include <cstring>
#include <sstream>
#include <map>
#include <thread>
#include <utility>
#include <windows.h>
extern "C" {
#include <easytier.h>
}


static std::string addrToIp(uint32_t addr) {
    struct in_addr in;
    in.s_addr = htonl(addr);   // 网络字节序
    return inet_ntoa(in);
}

bool EasyTierService::loadIpFromInstances() {
    constexpr int MAX = 64;
    KeyValuePair pairs[MAX] = {};
    int count = collect_network_infos(pairs, MAX);
    if (count <= 0) return false;

    bool found = false;
    for (int i = 0; i < count; ++i) {
        std::string value(pairs[i].value);

        // virtual_ipv4 在不同 FFI 版本下的序列化格式不同：
        //   旧版（对象）："virtual_ipv4":{"addr":169090561,...}
        //   新版（字符串）："virtual_ipv4":"10.126.126.1/24"
        // 两种都要兼容，否则升级 FFI 后会永远拿不到 IP。
        std::string ip;

        auto objPos = value.find("\"virtual_ipv4\":{");
        if (objPos != std::string::npos) {
            auto addrPos = value.find("\"addr\":", objPos);
            if (addrPos != std::string::npos) {
                addrPos += strlen("\"addr\":");
                auto endPos = value.find_first_of(",}", addrPos);
                try {
                    uint32_t addr = std::stoul(value.substr(addrPos, endPos - addrPos));
                    ip = addrToIp(addr);
                } catch (...) {
                    ip.clear();
                }
            }
        }

        if (ip.empty()) {
            auto strPos = value.find("\"virtual_ipv4\":\"");
            if (strPos != std::string::npos) {
                strPos += strlen("\"virtual_ipv4\":\"");
                auto endPos = value.find('"', strPos);
                std::string cidr = value.substr(strPos, endPos - strPos);
                auto slash = cidr.find('/');
                ip = (slash == std::string::npos) ? cidr : cidr.substr(0, slash);
            }
        }

        if (!ip.empty() && ip != "0.0.0.0") {
            virtualIp_ = ip;
            found = true;
        }

        free_string(pairs[i].key);
        free_string(pairs[i].value);
    }
    return found;
}

bool EasyTierService::fetchNetworkInfos(std::map<std::string, std::string>& outInfo) {
    constexpr int MAX = 64;
    KeyValuePair pairs[MAX] = {};
    int count = collect_network_infos(pairs, MAX);
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i) {
        outInfo[pairs[i].key] = pairs[i].value;
        free_string(pairs[i].key);
        free_string(pairs[i].value);
    }
    return true;
}

static std::string ExtractJsonNumber(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "0";
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    auto end = json.find_first_of(",}]", pos);
    if (end == std::string::npos) return "0";
    return json.substr(pos, end - pos);
}

std::string EasyTierService::getDevicesJson() {
    constexpr int MAX = 64;
    KeyValuePair pairs[MAX] = {};
    int count = collect_network_infos(pairs, MAX);
    if (count <= 0) return "[]";

    std::string result = "[";
    bool first = true;

    for (int i = 0; i < count; ++i) {
        std::string value(pairs[i].value);

        std::string myPeerId;
        auto myPos = value.find("\"my_node_info\":{");
        if (myPos != std::string::npos) {
            std::string mySection = value.substr(myPos);
            myPeerId = ExtractJsonString(mySection, "peer_id");
        }

        auto peersPos = value.find("\"peers\":");
        if (peersPos == std::string::npos) {
            free_string(pairs[i].key);
            free_string(pairs[i].value);
            continue;
        }
        peersPos += strlen("\"peers\":");
        while (peersPos < value.size() && (value[peersPos] == ' ' || value[peersPos] == '\t' ||
               value[peersPos] == '\n' || value[peersPos] == '\r'))
            peersPos++;
        if (peersPos >= value.size() || value[peersPos] != '[') {
            free_string(pairs[i].key);
            free_string(pairs[i].value);
            continue;
        }

        int depth = 0;
        bool inStr = false;
        size_t arrEnd = peersPos;
        for (; arrEnd < value.size(); ++arrEnd) {
            if (inStr) {
                if (value[arrEnd] == '"' && (arrEnd == 0 || value[arrEnd - 1] != '\\'))
                    inStr = false;
            } else {
                if (value[arrEnd] == '"') inStr = true;
                else if (value[arrEnd] == '[') depth++;
                else if (value[arrEnd] == ']') { --depth; if (depth == 0) break; }
            }
        }
        if (arrEnd >= value.size()) {
            free_string(pairs[i].key);
            free_string(pairs[i].value);
            continue;
        }

        std::string arrayContent = value.substr(peersPos + 1, arrEnd - peersPos - 1);
        size_t pos = 0;
        while (pos < arrayContent.size()) {
            while (pos < arrayContent.size() && (arrayContent[pos] == ' ' || arrayContent[pos] == '\t' ||
                   arrayContent[pos] == '\n' || arrayContent[pos] == '\r' || arrayContent[pos] == ','))
                ++pos;
            if (pos >= arrayContent.size() || arrayContent[pos] != '{') break;

            depth = 0;
            inStr = false;
            size_t objStart = pos;
            for (; pos < arrayContent.size(); ++pos) {
                if (inStr) {
                    if (arrayContent[pos] == '"' && (pos == 0 || arrayContent[pos - 1] != '\\'))
                        inStr = false;
                } else {
                    if (arrayContent[pos] == '"') inStr = true;
                    else if (arrayContent[pos] == '{') ++depth;
                    else if (arrayContent[pos] == '}') { --depth; if (depth == 0) { ++pos; break; } }
                }
            }

            std::string peerObj = arrayContent.substr(objStart, pos - objStart);
            std::string peerId = ExtractJsonString(peerObj, "peer_id");
            if (peerId.empty() || peerId == myPeerId) continue;

            std::string ipv4 = ExtractJsonString(peerObj, "ipv4_addr");
            std::string hostname = ExtractJsonString(peerObj, "hostname");
            std::string version = ExtractJsonString(peerObj, "version");
            std::string tunnelProto = ExtractJsonString(peerObj, "tunnel_proto");
            std::string natType = ExtractJsonString(peerObj, "nat_type");
            std::string latMs = ExtractJsonNumber(peerObj, "lat_ms");
            std::string lossRate = ExtractJsonNumber(peerObj, "loss_rate");
            std::string cost = ExtractJsonNumber(peerObj, "cost");

            if (!first) result += ",";
            first = false;

            result += "{\"peer_id\":\"" + JsonEscape(peerId)
                   + "\",\"ipv4_addr\":\"" + JsonEscape(ipv4)
                   + "\",\"hostname\":\"" + JsonEscape(hostname)
                   + "\",\"version\":\"" + JsonEscape(version)
                   + "\",\"tunnel_proto\":\"" + JsonEscape(tunnelProto)
                   + "\",\"nat_type\":\"" + JsonEscape(natType)
                   + "\",\"lat_ms\":" + latMs
                   + ",\"loss_rate\":" + lossRate
                   + ",\"cost\":" + cost
                   + "}";
        }

        free_string(pairs[i].key);
        free_string(pairs[i].value);
    }

    result += "]";
    return result;
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
    // 注意：键名是 relay_network_whitelist（对应 FlagsInConfig）。
    // 写成 foreign_network_whitelist 会被 TOML 解析器判为 unknown field，
    // 导致 parse_config 直接失败、实例起不来（上游 install.sh 也踩过同样的坑）。
    ss << "relay_network_whitelist = \"*\"\n";

    return ss.str();
}

EasyTierService::EasyTierService() = default;

EasyTierService::~EasyTierService() {
    stopEasyTier();
}

bool EasyTierService::init() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!LoadConfigFromFile(config_)) {
            WriteLog("No config file, using defaults");
        } else {
            config_.networkSecret = DecryptSecret(config_.networkSecret);
            config_.sshPassword = DecryptSecret(config_.sshPassword);
        }
        WriteLog("Config: instance=" + config_.instanceName
                 + " network=" + config_.networkName
                 + " autoStart=" + (config_.autoStart ? "yes" : "no"));
    }
    // Auto-start SSH server if enabled (outside lock)
    if (config_.sshEnabled && !config_.sshPassword.empty()) {
        startSshServer(config_.sshPort, config_.sshPassword);
    }

    // Check usbmmidd at startup (for direct desktop feature)
    WriteLog("[Service] Checking usbmmidd...");
    if (!IsUsbmmIdInstalled()) {
        WriteLog("[Service] usbmmidd not installed, attempting install...");
        if (InstallUsbmmId()) {
            WriteLog("[Service] usbmmidd installed successfully");
        } else {
            WriteLog("[Service] usbmmidd install failed (may need bundled driver files in exe dir)");
        }
    } else {
        WriteLog("[Service] usbmmidd is already installed");
    }

    return true;
}

bool EasyTierService::startEasyTier() {
    std::thread staleMonitor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isActive()) return true;
        tomlConfig_ = EasyTierService::MakeConfig(
            config_.instanceName, config_.networkName, config_.networkSecret,
            config_.ipv4, config_.listenPort, config_.peerUrl);
        // 复位上次运行残留的状态（monitor 线程 / FFI 实例）。
        // 这里刻意不调用 stopEasyTier()：那会连带停掉 SSH 服务器，并卸载
        // usbmmidd 虚拟显示器（enableidd 0 / stop / remove 各等 1s），
        // 与"启动 EasyTier"无关，还会把刚装好的驱动立刻拆掉。
        running_ = false;
        active_ = false;
        staleMonitor = std::move(monitorThread_);
    }
    // join 放在锁外：monitor 的轮询间隔是 5 秒，不能让它卡住 mutex_。
    if (staleMonitor.joinable())
        staleMonitor.join();
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

    // 2. 先清空可能残留的实例。上次启动失败时 running_ 仍为 false，
    //    stopEasyTier() 不会清理，导致这里报 "instance already exists"，
    //    之后必须重启进程才能恢复。启动前统一清一次即可自愈。
    retain_network_instance(nullptr, 0);

    if (run_network_instance(tomlConfig.c_str()) != 0) {
        const char* err = nullptr;
        get_error_msg(&err);
        WriteLog("run_network_instance failed: " + (err ? std::string(err) : "unknown"));
        if (err) free_string(err);
        retain_network_instance(nullptr, 0);   // 清理失败的实例，保证下次可重试
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

        // 如果超过 10 秒仍无 IP，打印所有实例的真实状态（含 error_msg）
        if (i == 10) {
            WriteLog("Still no IP after 10s, dumping instance state...");
            constexpr int MAX = 64;
            KeyValuePair pairs[MAX] = {};
            int count = collect_network_infos(pairs, MAX);
            if (count <= 0) {
                WriteLog("collect_network_infos returned " + std::to_string(count));
            }
            for (int j = 0; j < count; ++j) {
                std::string value = pairs[j].value ? pairs[j].value : "";
                std::string err = ExtractJsonString(value, "error_msg");
                std::string vpn = ExtractVpnPortalCfg(value);
                WriteLog(std::string("Instance [")
                         + (pairs[j].key ? pairs[j].key : "?") + "]"
                         + " running=" + (value.find("\"running\":true") != std::string::npos ? "true" : "false")
                         + " error=" + (err.empty() ? "none" : err)
                         + " vpn_portal=" + (vpn.empty() ? "none" : vpn));
                free_string(pairs[j].key);
                free_string(pairs[j].value);
            }
        }
    }

    if (!gotIp) {
        WriteLog("Failed to obtain virtual IP after 60s");
        retain_network_instance(nullptr, 0);   // 清理实例，保证下次启动不会 "instance already exists"
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
    stopSshServer();
    if (running_) {
        running_ = false;
        active_ = false;
        virtualIp_ = "";
        if (monitorThread_.joinable())
            monitorThread_.join();
        retain_network_instance(nullptr, 0);   // <-- 释放 FFI 后台实例
        WriteLog("EasyTier stopped");
    }
    // Safety net: ensure virtual display is removed and physical monitors restored
    RestorePhysicalMonitors(L"");  // re-enable all displays (may fail in Session 0)
    DisableVirtualDisplay();
}

bool EasyTierService::restartEasyTier() {
    // 重启只应重建 EasyTier 实例，不能走 stopEasyTier()：
    // 后者会停掉 SSH 服务器并卸载 usbmmidd 虚拟显示器，
    // 正在进行的远程桌面会话会被直接拆掉。所以这里只停实例本身。
    std::thread staleMonitor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            running_ = false;
            active_ = false;
            virtualIp_ = "";
            staleMonitor = std::move(monitorThread_);
        }
    }
    // join 必须在锁外：monitorLoop 每秒都可能抢 mutex_。
    if (staleMonitor.joinable())
        staleMonitor.join();
    retain_network_instance(nullptr, 0);
    return startEasyTier();
}

EasyTierConfig EasyTierService::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

bool EasyTierService::saveConfigToDisk(const EasyTierConfig& cfg) {
    // 加密写盘复用 common 里的实现，避免客户端/服务端各写一份。
    bool ok = SaveConfigToFileEncrypted(cfg);
    WriteLog(ok ? "Config saved" : "Config SAVE FAILED");
    return ok;
}

ConfigureResult EasyTierService::configureAndRestart(const EasyTierConfig& newConfig, bool restartNow) {
    ConfigureResult res;
    res.wasRunning = isActive();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = newConfig;
    }
    // 1. 先把配置写盘。这一步失败才是真正的"配置保存失败"。
    if (!saveConfigToDisk(newConfig)) {
        res.message = "failed to save config to disk";
        return res;
    }
    res.saved = true;

    // 2. 按新配置调整 SSH 服务器（与实例是否运行无关）。
    if (newConfig.sshEnabled && !newConfig.sshPassword.empty()) {
        startSshServer(newConfig.sshPort, newConfig.sshPassword);
    } else {
        stopSshServer();
    }

    // 3. EasyTier 实例本来就没跑，就不要去启动它。
    //    以前这里无条件 restartEasyTier()，服务处于 stopped 状态时
    //    必然失败，于是整个"应用配置"被误报为失败。
    if (!restartNow) {
        res.message = "config saved (restart not requested)";
        return res;
    }
    if (!res.wasRunning) {
        res.message = "config saved (EasyTier not running, not restarted)";
        WriteLog("[Service] Config saved; EasyTier is not running, skip restart");
        return res;
    }

    res.restarted = restartEasyTier();
    res.message = res.restarted ? "config saved and EasyTier restarted"
                                : "config saved but EasyTier restart failed";
    return res;
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

bool EasyTierService::startSshServer(int port, const std::string& password) {
    if (sshRunning_) {
        WriteLog("SSH server already running");
        return true;
    }
    if (password.empty()) {
        WriteLog("SSH server password is empty, abort start");
        return false;
    }
    if (!sshServer_.start(port, password)) {
        WriteLog("SSH server failed to start on port " + std::to_string(port));
        return false;
    }
    sshPort_ = port;
    sshRunning_ = true;
    WriteLog("SSH server started on port " + std::to_string(port));
    return true;
}

void EasyTierService::stopSshServer() {
    if (!sshRunning_) return;
    sshServer_.stop();
    sshRunning_ = false;
    WriteLog("SSH server stopped");
}

bool EasyTierService::attemptReconnect() {
    active_ = false;
    // 保留同名实例并不会重启它（run_network_instance 仍会报 already exists），
    // 必须先清空所有实例再重新启动。
    retain_network_instance(nullptr, 0);

    if (parse_config(tomlConfig_.c_str()) != 0 ||
        run_network_instance(tomlConfig_.c_str()) != 0) {
        WriteLog("Reconnect failed");
        retain_network_instance(nullptr, 0);
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
    retain_network_instance(nullptr, 0);
    return false;
}