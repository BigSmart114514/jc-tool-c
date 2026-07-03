#include "client_application.h"
#include "../common/ssh_session.h"
#include "../common/transport_tcp.h"
#include "../common/protocol.h"
#include "../client/control_panel.h"
#include "../client/connection_dialog.h"
#include "../client/user_select_dialog.h"

#include <QApplication>
#include <QMessageBox>
#include <QPointer>
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

int ClientApplication::exec() {
    ConnectionDialog dlg;
    if (dlg.exec() != QDialog::Accepted) return 0;

    ConnectionConfig cfg = dlg.getConfig();

    if (!setupDirectDesktop(cfg)) return 1;

    std::string connectInfo = targetHost_ + ":" + std::to_string(cfg.sshPort)
                              + " (DirectDesktop)";
    std::string modeText = "DirectDesktop";
    if (cfg.useEasyTier) {
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

bool ClientApplication::setupDirectDesktop(const ConnectionConfig& cfg) {
    std::string targetHost = cfg.host;
    if (cfg.useEasyTier && !cfg.easytierServerVip.empty())
        targetHost = cfg.easytierServerVip;
    targetHost_ = targetHost;

    sshSession_ = std::make_unique<SshSession>();
    if (!sshSession_->connect(targetHost, cfg.sshPort, cfg.username, cfg.password)) {
        QMessageBox::critical(nullptr, "SSH 连接失败",
            QString::fromStdString(sshSession_->getError()));
        return false;
    }

    if (!sshSession_->openDirectDesktopChannel()) {
        QMessageBox::critical(nullptr, "DirectDesktop 通道失败",
            QString::fromStdString(sshSession_->getError()));
        return false;
    }

    auto users = sshSession_->readUserList();
    if (users.empty()) {
        std::string err = sshSession_->getError();
        if (err.empty()) err = "服务器无可用用户或用户列表为空";
        QMessageBox::critical(nullptr, "用户列表为空",
            "服务器没有可用的用户会话。\n" + QString::fromStdString(err));
        return false;
    }

    QPointer<UserSelectDialog> selectDlg = new UserSelectDialog(users);
    bool accepted = (selectDlg->exec() == QDialog::Accepted);
    std::string selUser = selectDlg->selectedUser();
    std::string selPass = selectDlg->password();
    delete selectDlg;

    if (!accepted) return false;

    if (!sshSession_->sendLogin(selUser, selPass)) {
        QMessageBox::critical(nullptr, "登录发送失败",
            QString::fromStdString(sshSession_->getError()));
        return false;
    }

    int desktopPort = sshSession_->readDesktopPort();
    if (desktopPort <= 0) {
        QMessageBox::critical(nullptr, "桌面端口获取失败",
            QString::fromStdString(sshSession_->getError()));
        return false;
    }

    sshSession_->sftpInit();

    auto dt = std::make_unique<TCPClientTransport>();
    if (dt->connect(targetHost, desktopPort)) {
        desktopTransportPtr_ = dt.get();
        desktopTransport_ = std::move(dt);
    } else {
        QMessageBox::warning(nullptr, "桌面连接警告",
            "无法连接到桌面服务。桌面功能不可用。");
    }

    return true;
}
