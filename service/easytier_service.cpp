#include "easytier_service.h"
#include "../common/easytier_manager.h"
#include <fstream>
#include <iostream>
#include <string>
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

EasyTierService::EasyTierService() = default;

EasyTierService::~EasyTierService() {
    stopEasyTier();
}

bool EasyTierService::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    // std::cerr<<"Locked mutex from EasyTier init()"<<std::endl;
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
    std::string toml;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mgr_ && mgr_->isActive()) return true;
        toml = EasytierManager::MakeConfig(
            config_.instanceName, config_.networkName, config_.networkSecret,
            config_.ipv4, config_.listenPort, config_.peerUrl);
        // std::cerr<<"config: "<<toml<<std::endl;
    }
    stopEasyTier();            // ← 此时没有锁，不会 crash

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mgr_ && mgr_->isActive()) return true;  // 双重检查，防止并发
        auto mgr = std::make_unique<EasytierManager>(toml);
        if (!mgr->start()) { WriteLog("startEasyTier FAILED"); return false; }
        mgr_ = std::move(mgr);
        WriteLog("EasyTier started, IP=" + mgr_->getVirtualIp());
    }
    return true;
}

void EasyTierService::stopEasyTier() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mgr_) {
        mgr_->stop();
        mgr_.reset();
        retain_network_instance(nullptr, 0);   // <-- 释放 FFI 后台实例
        WriteLog("EasyTier stopped");
    }
}
bool EasyTierService::restartEasyTier() {
    stopEasyTier();
    return startEasyTier();
}

bool EasyTierService::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mgr_ && mgr_->isActive();
}

std::string EasyTierService::getVirtualIp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mgr_ ? mgr_->getVirtualIp() : "";
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
