// SFTP 链路诊断工具：
//   1. 单会话 SFTP 基本功能（列目录 / 上传 / 下载 / 删除）
//   2. 同一 SSH 会话上先开一个长生命周期通道（shell），再开 SFTP
//      —— 复现“DirectDesktop 会话里再开 SFTP 会卡死”的场景
//   3. 反复列目录，观察服务端是否泄漏目录句柄
//
// 用法：SftpProbe.exe [port]

#include "../service/ssh_server.h"
#include "../common/ssh_session.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

static const char* kHost = "127.0.0.1";
static const char* kUser = "probe";
static const char* kPass = "probe-pass";

static void armWatchdog(int seconds, const char* what) {
    std::thread([seconds, what]() {
        Sleep(static_cast<DWORD>(seconds) * 1000);
        std::cout << "[Probe] TIMEOUT after " << seconds << "s while " << what
                  << " -> channel request was never answered" << std::endl;
        std::cout.flush();
        ExitProcess(99);
    }).detach();
}

static std::string tempPath(const char* name) {
    char dir[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, dir);
    std::string p = std::string(dir) + "sftp_probe_" + std::to_string(GetCurrentProcessId()) + "_" + name;
    return p;
}

static bool writeLocal(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

static std::string readLocal(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static DWORD handleCount() {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 2223;

    SshServer server;
    if (!server.start(port, kPass)) {
        std::cerr << "[Probe] failed to start SSH server on port " << port << std::endl;
        return 2;
    }
    std::cout << "[Probe] server listening on " << port << std::endl;

    // ── 1. 单会话 SFTP ───────────────────────────────────────────────────
    int rc = 0;
    {
        SshSession s;
        if (!s.connect(kHost, port, kUser, kPass)) {
            std::cerr << "[Probe] connect failed: " << s.getError() << std::endl;
            server.stop();
            return 3;
        }

        std::vector<SFtpEntry> entries;
        if (!s.sftpListDir("/", entries)) {
            std::cerr << "[Probe] sftpListDir(/) failed: " << s.getError() << std::endl;
            rc = 4;
        } else {
            std::cout << "[Probe] list(/) ok, " << entries.size() << " entries" << std::endl;
        }

        // 上传 / 下载校验
        std::string localUp = tempPath("up.bin");
        std::string localDown = tempPath("down.bin");
        std::string remote = "C:/ProgramData/EasyTier/sftp_probe_up.bin";
        std::string payload;
        payload.reserve(3 * 1024 * 1024);
        for (int i = 0; i < 3 * 1024 * 1024; i++) payload.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
        writeLocal(localUp, payload);

        if (!s.sftpUpload(localUp, remote, nullptr)) {
            std::cerr << "[Probe] upload failed: " << s.getError() << std::endl;
            rc = 5;
        } else if (!s.sftpDownload(remote, localDown, nullptr)) {
            std::cerr << "[Probe] download failed: " << s.getError() << std::endl;
            rc = 6;
        } else {
            std::string got = readLocal(localDown);
            if (got != payload) {
                std::cerr << "[Probe] round-trip mismatch: sent=" << payload.size()
                          << " got=" << got.size() << std::endl;
                rc = 7;
            } else {
                std::cout << "[Probe] upload/download round-trip ok (" << got.size() << " bytes)" << std::endl;
            }
        }
        s.sftpDelete(remote);
        DeleteFileA(localUp.c_str());
        DeleteFileA(localDown.c_str());

        // 目录句柄泄漏检查：反复列目录
        DWORD before = handleCount();
        for (int i = 0; i < 100; i++) {
            std::vector<SFtpEntry> tmp;
            s.sftpListDir("/", tmp);
        }
        DWORD after = handleCount();
        std::cout << "[Probe] handles before=" << before << " after 100 listdir=" << after
                  << " delta=" << (long long)after - (long long)before << std::endl;

        s.disconnect();
    }

    // ── 2. 第二条独立连接 ────────────────────────────────────────────────
    // 客户端现在为 SFTP 单独建一条 SSH 连接（DirectDesktop 会长期占用自己的
    // 通道）。这里验证：第一条连接挂着长生命周期 shell 时，第二条连接依然能
    // 正常初始化 SFTP 并列出目录。
    {
        armWatchdog(12, "opening SFTP on a second connection");
        SshSession busy;
        if (!busy.connect(kHost, port, kUser, kPass)) {
            std::cerr << "[Probe] connect(busy) failed: " << busy.getError() << std::endl;
            server.stop();
            return 3;
        }
        bool shellOk = busy.openShell([](const char*, int) {});
        std::cout << "[Probe] busy session openShell = " << shellOk << std::endl;
        Sleep(1000);

        SshSession sftpOnly;
        if (!sftpOnly.connect(kHost, port, kUser, kPass)) {
            std::cerr << "[Probe] connect(sftp) failed: " << sftpOnly.getError() << std::endl;
            busy.disconnect();
            server.stop();
            return 3;
        }
        std::vector<SFtpEntry> entries;
        bool listOk = sftpOnly.sftpListDir("/", entries);
        std::cout << "[Probe] second-connection list(/) = " << listOk
                  << " entries=" << entries.size() << std::endl;
        if (!listOk) rc = 8;
        sftpOnly.disconnect();
        busy.disconnect();
    }

    server.stop();
    std::cout << "[Probe] done, rc=" << rc << std::endl;
    return rc;
}
