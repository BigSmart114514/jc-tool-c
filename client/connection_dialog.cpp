#include "connection_dialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QMessageBox>

ConnectionDialog::ConnectionDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Connect to Remote Host");
    setMinimumWidth(450);
    createUI();
}

void ConnectionDialog::createUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    // SSH connection group
    QGroupBox* sshGroup = new QGroupBox("SSH Connection");
    QFormLayout* sshForm = new QFormLayout(sshGroup);

    leHost_ = new QLineEdit("127.0.0.1");
    lePort_ = new QLineEdit("2222");
    leUsername_ = new QLineEdit("root");
    lePassword_ = new QLineEdit();
    lePassword_->setEchoMode(QLineEdit::Password);

    sshForm->addRow("Host:", leHost_);
    sshForm->addRow("Port:", lePort_);
    sshForm->addRow("Username:", leUsername_);
    sshForm->addRow("Password:", lePassword_);

    QLabel* pwdNote = new QLabel("SSH/SFTP/Shell 共享同一密码");
    pwdNote->setStyleSheet("color: #888; font-size: 11px;");
    sshForm->addRow("", pwdNote);
    mainLayout->addWidget(sshGroup);

    // Desktop streaming group
    QGroupBox* desktopGroup = new QGroupBox("Remote Desktop");
    QFormLayout* desktopForm = new QFormLayout(desktopGroup);
    leDesktopPort_ = new QLineEdit("12345");
    desktopForm->addRow("Desktop port:", leDesktopPort_);
    mainLayout->addWidget(desktopGroup);

    // EasyTier group
    QGroupBox* etGroup = new QGroupBox("EasyTier VPN (Optional)");
    QVBoxLayout* etLayout = new QVBoxLayout(etGroup);

    chkEasyTier_ = new QCheckBox("Use EasyTier virtual network");
    connect(chkEasyTier_, &QCheckBox::toggled, this, &ConnectionDialog::onEasyTierToggled);
    etLayout->addWidget(chkEasyTier_);

    QFormLayout* etForm = new QFormLayout();
    leInstName_ = new QLineEdit("jc-client");
    leNetName_ = new QLineEdit("jc-tool-vpn");
    leNetSecret_ = new QLineEdit();
    leNetSecret_->setPlaceholderText("Enter network key");
    leIpv4_ = new QLineEdit();
    leIpv4_->setPlaceholderText("Leave empty for auto-assign");
    leListenPort_ = new QLineEdit("11012");
    lePeerUrl_ = new QLineEdit("tcp://225284.xyz:11010");

    etForm->addRow("Instance name:", leInstName_);
    etForm->addRow("Network name:", leNetName_);
    etForm->addRow("Network key:", leNetSecret_);
    etForm->addRow("Virtual IP:", leIpv4_);
    etForm->addRow("Listen port:", leListenPort_);
    etForm->addRow("Peer URL:", lePeerUrl_);
    etLayout->addLayout(etForm);

    onEasyTierToggled(false);
    mainLayout->addWidget(etGroup);

    // Connect button
    QPushButton* btnConnect = new QPushButton("Connect");
    btnConnect->setMinimumHeight(36);
    btnConnect->setStyleSheet("background-color: #337ab7; color: white; font-weight: bold;");
    connect(btnConnect, &QPushButton::clicked, this, &ConnectionDialog::onConnect);
    mainLayout->addWidget(btnConnect);

    QPushButton* btnCancel = new QPushButton("Cancel");
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    mainLayout->addWidget(btnCancel);
}

void ConnectionDialog::onEasyTierToggled(bool checked) {
    leInstName_->setEnabled(checked);
    leNetName_->setEnabled(checked);
    leNetSecret_->setEnabled(checked);
    leIpv4_->setEnabled(checked);
    leListenPort_->setEnabled(checked);
    lePeerUrl_->setEnabled(checked);
}

void ConnectionDialog::onConnect() {
    if (leHost_->text().isEmpty()) {
        QMessageBox::warning(this, "Validation", "Host cannot be empty");
        return;
    }
    if (leUsername_->text().isEmpty()) {
        QMessageBox::warning(this, "Validation", "Username cannot be empty");
        return;
    }
    if (lePassword_->text().isEmpty()) {
        QMessageBox::warning(this, "Validation", "Password cannot be empty");
        return;
    }

    config_.host = leHost_->text().toStdString();
    config_.sshPort = lePort_->text().toInt();
    config_.username = leUsername_->text().toStdString();
    config_.password = lePassword_->text().toStdString();

    config_.desktopPort = leDesktopPort_->text().toInt();

    config_.useEasyTier = chkEasyTier_->isChecked();
    config_.easytierInstanceName = leInstName_->text().toStdString();
    config_.easytierNetworkName = leNetName_->text().toStdString();
    config_.easytierNetworkSecret = leNetSecret_->text().toStdString();
    config_.easytierIpv4 = leIpv4_->text().toStdString();
    config_.easytierListenPort = leListenPort_->text().toInt();
    config_.easytierPeerUrl = lePeerUrl_->text().toStdString();

    accept();
}
