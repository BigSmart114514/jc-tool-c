#include "server_application.h"
#include "../server/desktop_service.h"
#include "../common/transport_tcp.h"
#include "../common/easytier_control.h"
#include "../client/server_settings_dialog.h"
#include "../client/server_status_dialog.h"
#include "../client/service_manager_dialog.h"

#include <QApplication>
#include <QMessageBox>
#include <iostream>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

ServerApplication::ServerApplication() = default;

ServerApplication::~ServerApplication() {
    if (desktopService_) desktopService_->stop();
    if (desktopTransport_) desktopTransport_->stop();
    timeEndPeriod(1);
}

int ServerApplication::exec() {
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    ServerSettingsDialog settingsDlg;
    if (settingsDlg.exec() != QDialog::Accepted) {
        timeEndPeriod(1);
        return 0;
    }

    ServerSettings settings = settingsDlg.getSettings();
    useEasyTier_ = settings.useEasyTier;
    desktopPort_ = settings.desktopPort;
    sshPort_ = settings.sshPort;
    sshPassword_ = settings.sshPassword;

    // Step 1: Connect to EasyTierService (required for SSH)
    EasyTierControlClient svcClient;
    if (!svcClient.connect(3000)) {
        QMessageBox::StandardButton btn = QMessageBox::critical(nullptr,
            "Service Required",
            "EasyTierService is not running.\n\n"
            "SSH access requires this service. The server cannot start without it.\n\n"
            "Open Service Manager to install/start it?",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (btn == QMessageBox::Yes) {
            ServiceManagerDialog mgrDlg;
            mgrDlg.exec();
            if (!svcClient.connect(3000)) {
                QMessageBox::critical(nullptr, "Still Not Available",
                    "EasyTierService is still not available.\n\n"
                    "Server startup cancelled.");
                timeEndPeriod(1);
                return 1;
            }
        } else {
            timeEndPeriod(1);
            return 1;
        }
    }

    // Step 2: Start SSH
    svcClient.sshStart(sshPort_, sshPassword_);

    // Step 3: Start EasyTier VPN if enabled
    if (useEasyTier_) {
        std::string state, ip;
        uint32_t uptime;
        if (svcClient.getStatus(state, ip, uptime) && state == "running") {
            myVirtualIp_ = ip;
        } else {
            if (svcClient.start()) {
                Sleep(2000);
                svcClient.getStatus(state, ip, uptime);
                myVirtualIp_ = ip;
            }
            if (myVirtualIp_.empty()) {
                QMessageBox::StandardButton btn = QMessageBox::question(nullptr,
                    "VPN Not Available",
                    "Could not get a virtual IP from EasyTier.\n\n"
                    "Start without VPN? (SSH will use Direct TCP)",
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (btn != QMessageBox::Yes) {
                    timeEndPeriod(1);
                    return 1;
                }
                useEasyTier_ = false;
            }
        }
    }

    if (!useEasyTier_) {
        myVirtualIp_ = "Direct TCP";
    }

    // Step 4: Start desktop service
    auto service = std::make_unique<DesktopService>();
    if (!service->init()) {
        QMessageBox::critical(nullptr, "Error", "Desktop service init failed!");
        return 1;
    }

    auto transport = std::make_unique<TCPServerTransport>(desktopPort_);
    if (!transport->start()) {
        QMessageBox::critical(nullptr, "TCP Error", "Failed to start TCP transport.");
        return 1;
    }

    service->setTransport(transport.get());
    service->start();

    desktopService_ = std::move(service);
    desktopTransport_ = std::move(transport);

    // Step 5: Show status dialog
    ServerStatusDialog dialog;
    dialog.setInfo(myVirtualIp_, useEasyTier_, desktopPort_, sshPort_,
                   desktopService_->getWidth(), desktopService_->getHeight());

    QObject::connect(&dialog, &ServerStatusDialog::stopRequested, [this]() {
        std::cout << "[Server] Stop requested..." << std::endl;
        desktopService_->stop();
        desktopTransport_->stop();
        qApp->quit();
    });
    dialog.show();

    qApp->exec();
    return 0;
}
