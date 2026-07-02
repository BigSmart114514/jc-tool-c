#include "client_application.h"
#include "../common/ssh_session.h"
#include "../common/transport_tcp.h"
#include "../common/protocol.h"
#include "../client/control_panel.h"
#include "../client/connection_dialog.h"

#include <QApplication>
#include <QMessageBox>
#include <iostream>

ClientApplication::ClientApplication() = default;

ClientApplication::~ClientApplication() {
    if (desktopTransportPtr_) {
        desktopTransportPtr_->disconnect();
    }
    if (sshSession_) {
        sshSession_->disconnect();
    }
}

bool ClientApplication::setupConnections(const ConnectionConfig& cfg) {
    std::string targetHost = cfg.host;
    if (cfg.useEasyTier && !cfg.easytierServerVip.empty()) {
        targetHost = cfg.easytierServerVip;
    }
    targetHost_ = targetHost;

    sshSession_ = std::make_unique<SshSession>();
    if (!sshSession_->connect(targetHost, cfg.sshPort, cfg.username, cfg.password)) {
        QMessageBox::critical(nullptr, "SSH Connection Failed",
            QString::fromStdString(sshSession_->getError()));
        return false;
    }

    if (!sshSession_->sftpInit()) {
        QMessageBox::warning(nullptr, "SFTP Init Warning",
            "SFTP initialization failed. File manager may not work.");
    }

    auto dt = std::make_unique<TCPClientTransport>();
    if (dt->connect(targetHost, cfg.desktopPort)) {
        desktopTransportPtr_ = dt.get();
        desktopTransport_ = std::move(dt);
    } else {
        QMessageBox::warning(nullptr, "Desktop Warning",
            "Could not connect to desktop service. Desktop will be unavailable.");
    }

    return true;
}

int ClientApplication::exec() {
    ConnectionDialog dlg;
    if (dlg.exec() != QDialog::Accepted) return 0;

    ConnectionConfig cfg = dlg.getConfig();
    if (!setupConnections(cfg)) return 1;

    std::string connectInfo = targetHost_ + ":" + std::to_string(cfg.sshPort);
    std::string modeText = "SSH+SFTP";
    if (cfg.useEasyTier) {
        modeText = "EasyTier + SSH+SFTP";
        connectInfo = "SSH: " + targetHost_ + ":" + std::to_string(cfg.sshPort)
                     + " | via EasyTier Service";
    }

    ControlPanelConfig panelConfig;
    panelConfig.sshSession = sshSession_.get();
    panelConfig.desktopTransport = desktopTransportPtr_;
    panelConfig.modeText = modeText;
    panelConfig.connectInfo = connectInfo;
    panelConfig.sshHost = targetHost_;
    panelConfig.sshPort = cfg.sshPort;
    panelConfig.sshUser = cfg.username;
    panelConfig.sshPassword = cfg.password;

    ControlPanel controlPanel;
    controlPanel.setConfig(panelConfig);
    controlPanel.setupDesktopTransportCallbacks();
    controlPanel.show();

    int result = qApp->exec();
    return result;
}
