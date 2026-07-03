#include <iostream>
#include <string>
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

static void SignalReady(const std::wstring& pipeName, int port) {
    HANDLE hPipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        Log("Failed to connect ready pipe: " + std::to_string(GetLastError()));
        return;
    }
    std::string msg = std::to_string(port);
    DWORD written;
    WriteFile(hPipe, msg.data(), (DWORD)msg.size(), &written, nullptr);
    CloseHandle(hPipe);
    Log("Ready signal sent");
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

    if (port <= 0) { Log("Missing --port"); return 1; }

    HANDLE hExitEvent = nullptr;
    if (!exitEvent.empty()) {
        hExitEvent = OpenEventW(SYNCHRONIZE, FALSE, exitEvent.c_str());
        if (!hExitEvent) Log("OpenEventW failed: " + std::to_string(GetLastError()));
    }

    if (!NetUtil::InitWinsock()) { Log("WSAStartup failed"); return 1; }

    auto service = std::make_unique<DesktopService>();
    if (!captureOutput.empty()) service->setCaptureOutput(captureOutput);

    if (!service->init()) { Log("DesktopService init failed"); WSACleanup(); return 1; }

    auto transport = std::make_unique<TCPServerTransport>(port);
    if (!transport->start()) { Log("TCPServerTransport start failed"); WSACleanup(); return 1; }

    service->setTransport(transport.get());
    service->start();

    Log("Streaming on port " + std::to_string(port)
        + " (" + std::to_string(service->getWidth()) + "x" + std::to_string(service->getHeight()) + ")");

    if (!readyPipe.empty()) SignalReady(readyPipe, port);

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
