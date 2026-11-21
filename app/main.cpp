#include <iostream>
#include <string>
#include <cstdio>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <QApplication>
#include <QDialog>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>

#include "../common/protocol.h"
#include "../common/transport_tcp.h"
#include "../common/direct_desktop_protocol.h"
#include "../server/desktop_service.h"
#include "../server/virtual_display.h"
#include "../client/service_manager_dialog.h"
#include "client_application.h"

// ─── Headless direct-desktop mode ──────────────────────────────────────────

static void Log(const char* msg) {
    std::cout << "[DD] " << msg << std::endl;
    OutputDebugStringA(msg);
}

static void Log(const std::string& msg) { Log(msg.c_str()); }

static std::string Narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// 父进程（服务）与本进程之间的状态通道。
// 子进程把启动结果写回管道：成功是 "OK <port> <w> <h>"，
// 失败是 "ERR <原因>"。父进程据此既能拿到端口，也能知道失败原因，
// 而不是只能干等超时。
static void SendStatus(const std::wstring& pipeName, const std::string& status) {
    if (pipeName.empty()) return;

    HANDLE hPipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40; ++attempt) {
        hPipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) {
            Log("Failed to connect status pipe: " + std::to_string(GetLastError()));
            return;
        }
        WaitNamedPipeW(pipeName.c_str(), 250);
    }
    if (hPipe == INVALID_HANDLE_VALUE) {
        Log("Status pipe unavailable after retries");
        return;
    }

    DWORD written = 0;
    if (!WriteFile(hPipe, status.data(), (DWORD)status.size(), &written, nullptr)
        || written != status.size()) {
        Log("Status pipe write failed: " + std::to_string(GetLastError()));
    }
    CloseHandle(hPipe);
    Log("Status reported to service: " + status);
}

static int RunDirectDesktop(int argc, char* argv[]) {
    int port = 0;
    std::wstring readyPipe;
    std::wstring captureOutput;
    std::wstring exitEvent;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--direct-desktop") continue;
        std::string val = (i + 1 < argc && argv[i+1][0] == '-') ? "" : (++i < argc ? argv[i] : "");
        if (val.empty() && arg.find('=') != std::string::npos) {
            auto eq = arg.find('=');
            val = arg.substr(eq + 1);
            arg = arg.substr(0, eq);
        }
        if (arg == "--port") port = std::stoi(val);
        else if (arg == "--ready-pipe") readyPipe.assign(val.begin(), val.end());
        else if (arg == "--capture-output") captureOutput.assign(val.begin(), val.end());
        else if (arg == "--exit-event") exitEvent.assign(val.begin(), val.end());
    }

    if (port <= 0) {
        Log("Missing --port");
        SendStatus(readyPipe, "ERR missing port argument");
        return 1;
    }

    // 子进程没有控制台（CREATE_NO_WINDOW），stdout 默认丢失。
    // 把 stdout/stderr 落到和 service.log 同目录的文件，失败时可查。
    {
        wchar_t programData[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"PROGRAMDATA", programData, MAX_PATH);
        std::wstring dir = n ? std::wstring(programData) : std::wstring(L"C:\\ProgramData");
        dir += L"\\EasyTier";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring file = dir + L"\\desktop.log";
        std::string logFile = Narrow(file);
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
            DeleteFileW(file.c_str());
        if (!logFile.empty()) {
            std::freopen(logFile.c_str(), "w", stdout);
            std::freopen(logFile.c_str(), "a", stderr);
        }
    }
    Log("Direct desktop starting on port " + std::to_string(port));

    HANDLE hExitEvent = nullptr;
    if (!exitEvent.empty()) {
        hExitEvent = OpenEventW(SYNCHRONIZE, FALSE, exitEvent.c_str());
        if (!hExitEvent) Log("OpenEventW failed: " + std::to_string(GetLastError()));
    }

    if (!NetUtil::InitWinsock()) {
        Log("WSAStartup failed");
        SendStatus(readyPipe, "ERR WSAStartup failed");
        return 1;
    }

    auto service = std::make_unique<DesktopService>();
    if (!captureOutput.empty()) service->setCaptureOutput(captureOutput);

    if (!service->init()) {
        Log("DesktopService init failed (screen capture or encoder)");
        SendStatus(readyPipe, "ERR screen capture or encoder init failed");
        WSACleanup();
        return 1;
    }

    auto transport = std::make_unique<TCPServerTransport>(port);
    if (!transport->start()) {
        Log("TCPServerTransport start failed on port " + std::to_string(port));
        SendStatus(readyPipe, "ERR cannot listen on port " + std::to_string(port));
        WSACleanup();
        return 1;
    }

    service->setTransport(transport.get());
    service->start();

    Log("Streaming on port " + std::to_string(port)
        + " (" + std::to_string(service->getWidth()) + "x" + std::to_string(service->getHeight()) + ")");

    // 只有真正开始 listen 之后才报告 OK，父进程收到的端口一定是可连接的。
    SendStatus(readyPipe, "OK " + std::to_string(port) + " "
                          + std::to_string(service->getWidth()) + " "
                          + std::to_string(service->getHeight()));

    Log("Waiting for exit signal...");
    if (hExitEvent) {
        WaitForSingleObject(hExitEvent, INFINITE);
        CloseHandle(hExitEvent);
    } else {
        // No exit event (shouldn't happen with DACL fix), poll for activity
        int idleSeconds = 0;
        while (idleSeconds < 30) {
            if (transport->hasClient()) { idleSeconds = 0; }
            else { ++idleSeconds; }
            Sleep(1000);
        }
        Log("No exit event, shutdown after idle timeout");
    }

    Log("Shutting down...");
    service->stop();
    transport->stop();
    WSACleanup();
    return 0;
}

// ─── Normal GUI entry ──────────────────────────────────────────────────────

static bool isElevated() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;
    TOKEN_ELEVATION te = {};
    DWORD dwSize = 0;
    bool ok = GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &dwSize);
    CloseHandle(hToken);
    return ok && te.TokenIsElevated;
}

static void relaunchAsAdmin() {
    wchar_t exe[MAX_PATH + 1] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = nullptr;
    sei.nShow = SW_SHOW;
    ShellExecuteExW(&sei);
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--direct-desktop") == 0)
            return RunDirectDesktop(argc, argv);
    }

    // GUI mode: auto-elevate if not already admin
    if (!isElevated()) {
        relaunchAsAdmin();
        return 0;
    }

    // Suppress Qt DPI warning when manifest already declares PerMonitorV2
    _putenv("QT_LOGGING_RULES=qt.qpa.window=false");

    QApplication app(argc, argv);

    if (!NetUtil::InitWinsock()) {
        QMessageBox::critical(nullptr, "Error", "WSAStartup failed!");
        return 1;
    }

    QDialog startupDlg;
    startupDlg.setWindowTitle("JC Tool");
    startupDlg.setFixedSize(300, 200);
    QVBoxLayout layout(&startupDlg);

    QLabel* label = new QLabel("Select application mode:");
    label->setAlignment(Qt::AlignCenter);
    layout.addWidget(label);

    QPushButton* btnClient = new QPushButton("Start as Client");
    QPushButton* btnServiceMgr = new QPushButton("Service Manager...");
    btnClient->setMinimumHeight(35);
    btnServiceMgr->setMinimumHeight(35);
    layout.addWidget(btnClient);
    layout.addSpacing(4);
    layout.addWidget(btnServiceMgr);

    QObject::connect(btnClient, &QPushButton::clicked, [&](){ startupDlg.accept(); });
    QObject::connect(btnServiceMgr, &QPushButton::clicked, [&]() {
        ServiceManagerDialog dlg;
        dlg.exec();
    });

    if (startupDlg.exec() != QDialog::Accepted) {
        WSACleanup();
        return 0;
    }

    ClientApplication clientApp;
    int result = clientApp.exec();

    WSACleanup();
    return result;
}
