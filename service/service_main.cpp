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
#include "../common/easytier_control.h"
#include <windows.h>
#include <thread>
#include <iostream>
#include "logging.h"

static EasyTierService g_service;
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static std::thread g_pipeThread;
std::atomic<bool> stopRequested_{false};

static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
static DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD type, LPVOID data, LPVOID ctx);
static void PipeServerLoop();
static void HandleClient(HANDLE hPipe);
static std::string HandleCommand(EasyTierCmd cmd, const std::string& json);
static void SignalShutdown();

static void ReportStatus(DWORD state, DWORD waitHint = 0) {
    if (!g_statusHandle) return;
    SERVICE_STATUS ss = {};
    ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ss.dwCurrentState = state;
    ss.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    ss.dwWaitHint = waitHint;
    SetServiceStatus(g_statusHandle, &ss);
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1 && (wcscmp(argv[1], L"--console") == 0 ||
                     wcscmp(argv[1], L"-c") == 0)) {
        printf("EasyTier Service (console debug mode)\n");
        printf("Pipe: %S\n\n", EASYTIER_PIPE_NAME);
        if (!g_service.init()) { printf("Init failed\n"); return 1; }
        if (g_service.getConfig().autoStart) g_service.startEasyTier();
        g_pipeThread = std::thread(PipeServerLoop);
        printf("Type 'q' to quit, 'start'/'stop'/'restart'/'status' to control\n\n");
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "q" || line == "quit") break;
            if (line == "start")   g_service.startEasyTier();
            if (line == "stop")    g_service.stopEasyTier();
            if (line == "restart") g_service.restartEasyTier();
            if (line == "status")
                printf("  running=%d  ip=%s\n",
                    g_service.isActive(), g_service.getVirtualIp().c_str());
        }
        SignalShutdown();
        if (g_pipeThread.joinable()) g_pipeThread.join();
        g_service.stopEasyTier();
        return 0;
    }
    SERVICE_TABLE_ENTRYW table[] = {
        { L"EasyTier", ServiceMain },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherW(table))
        return GetLastError();
    return 0;
}

static void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(L"EasyTier",
        ServiceCtrlHandler, NULL);
    if (!g_statusHandle) return;

    ReportStatus(SERVICE_START_PENDING, 3000);

    if (!g_service.init()) {
        ReportStatus(SERVICE_STOPPED);
        return;
    }

    g_pipeThread = std::thread(PipeServerLoop);

    if (g_service.getConfig().autoStart)
        g_service.startEasyTier();

    ReportStatus(SERVICE_RUNNING);

    g_pipeThread.join();

    g_service.stopEasyTier();
    ReportStatus(SERVICE_STOPPED);
}

static DWORD WINAPI ServiceCtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        ReportStatus(SERVICE_STOP_PENDING, 5000);
        SignalShutdown();
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void SignalShutdown() {
    stopRequested_ = true;
}

static bool PeekWait(HANDLE hPipe, DWORD minBytes, int intervalMs) {
    while (!stopRequested_) {
        DWORD bytesAvail = 0;
        if (PeekNamedPipe(hPipe, NULL, 0, NULL, &bytesAvail, NULL)) {
            if (bytesAvail >= minBytes) return true;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) return false;
        }
        Sleep(intervalMs);
    }
    return false;
}

static void PipeServerLoop() {
    while (!stopRequested_) {
        HANDLE hPipe = CreateNamedPipeW(EASYTIER_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
            PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(100); continue; }

        if (stopRequested_) { CloseHandle(hPipe); break; }

        // Poll ConnectNamedPipe in non-blocking mode
        while (!stopRequested_) {
            if (ConnectNamedPipe(hPipe, NULL)) break;
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) break;
            if (err == ERROR_NO_DATA) {
                DisconnectNamedPipe(hPipe);
                continue;
            }
            if (err != ERROR_PIPE_LISTENING) break;
            Sleep(100);
        }
        if (stopRequested_) { CloseHandle(hPipe); break; }

        // Handle client in separate thread, immediately accept next connection
        std::thread([hPipe]() {
            HandleClient(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }).detach();
    }
}

static void HandleClient(HANDLE hPipe) {
    while (!stopRequested_) {
        if (!PeekWait(hPipe, sizeof(EasyTierPipeMsg), 100))
            break;

        EasyTierPipeMsg hdr;
        DWORD read;
        if (!ReadFile(hPipe, &hdr, sizeof(hdr), &read, NULL) || read != sizeof(hdr))
            break;
        if (hdr.magic != EASYTIER_MSG_MAGIC || hdr.type != 0)
            break;

        if (stopRequested_) break;

        std::string req;
        if (hdr.dataLen > 0) {
            if (!PeekWait(hPipe, hdr.dataLen, 100))
                break;
            req.resize(hdr.dataLen);
            if (!ReadFile(hPipe, &req[0], hdr.dataLen, &read, NULL) || read != hdr.dataLen)
                break;
        }

        if (stopRequested_) break;

        std::string resp = HandleCommand((EasyTierCmd)hdr.cmd, req);

        EasyTierPipeMsg rhdr;
        rhdr.magic   = EASYTIER_MSG_MAGIC;
        rhdr.type    = 1;
        rhdr.cmd     = hdr.cmd;
        rhdr.seq     = hdr.seq;
        rhdr.dataLen = (uint32_t)resp.size();

        DWORD written;
        WriteFile(hPipe, &rhdr, sizeof(rhdr), &written, NULL);
        if (!resp.empty())
            WriteFile(hPipe, resp.data(), (DWORD)resp.size(), &written, NULL);

        if (hdr.cmd == CMD_SHUTDOWN) break;
    }
}

static std::string HandleCommand(EasyTierCmd cmd, const std::string& json) {
    auto okResp = [](const std::string& extra) {
        return "{\"code\":0" + (extra.empty() ? "" : "," + extra) + "}";
    };
    auto errResp = [](const std::string& msg) {
        return "{\"code\":1,\"message\":\"" + JsonEscape(msg) + "\"}";
    };

    switch (cmd) {
    case CMD_START: {
        bool ok = g_service.startEasyTier();
        if (!ok) return errResp("start failed");
        return okResp("\"ip\":\"" + JsonEscape(g_service.getVirtualIp()) + "\"");
    }
    case CMD_STOP:
        g_service.stopEasyTier();
        return okResp({});

    case CMD_RESTART: {
        bool ok = g_service.restartEasyTier();
        if (!ok) return errResp("restart failed");
        return okResp("\"ip\":\"" + JsonEscape(g_service.getVirtualIp()) + "\"");
    }
    case CMD_STATUS:
        return okResp(
            std::string("\"state\":\"") + (g_service.isActive() ? "running" : "stopped")
            + "\",\"ip\":\"" + JsonEscape(g_service.getVirtualIp()) + "\",\"uptime\":0");

    case CMD_GET_CONFIG: {
        EasyTierConfig cfg = g_service.getConfig();
        return okResp("\"config\":" + ConfigToJson(cfg));
    }
    case CMD_CONFIGURE: {
        auto pos = json.find("\"config\":");
        if (pos == std::string::npos)
            return errResp("missing config");
        pos += strlen("\"config\":");
        int depth = 0;
        size_t end = pos;
        for (; end < json.size(); ++end) {
            if (json[end] == '{') ++depth;
            else if (json[end] == '}') { --depth; if (depth == 0) break; }
        }
        std::string inner = json.substr(pos, end - pos + 1);
        EasyTierConfig cfg;
        if (!JsonToConfig(inner, cfg))
            return errResp("invalid config json");
        bool restart = json.find("\"restart\":true") != std::string::npos;
        bool ok = g_service.configureAndRestart(cfg, restart);
        if (!ok) return errResp("configure failed");
        return okResp("\"ip\":\"" + JsonEscape(g_service.getVirtualIp()) + "\"");
    }
    case CMD_SSH_START: {
        int port = ExtractJsonInt(json, "port");
        if (port <= 0) port = 2222;
        std::string password = ExtractJsonString(json, "password");
        bool ok = g_service.startSshServer(port, password);
        if (!ok) return errResp("ssh start failed");
        return okResp("\"port\":" + std::to_string(port));
    }
    case CMD_SSH_STOP:
        g_service.stopSshServer();
        return okResp({});

    case CMD_SSH_STATUS:
        return okResp(
            std::string("\"running\":") + (g_service.isSshRunning() ? "true" : "false")
            + ",\"port\":" + std::to_string(g_service.getSshPort()));

    case CMD_SHUTDOWN:
        stopRequested_ = true;
        return okResp({});

    default:
        return errResp("unknown command");
    }
}
