#include "connection_dialog.h"
#include "service_manager_dialog.h"
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
    QGroupBox* etGroup = new QGroupBox("EasyTier VPN (via System Service)");
    QVBoxLayout* etLayout = new QVBoxLayout(etGroup);

    chkEasyTier_ = new QCheckBox("Route traffic through EasyTier VPN");
    connect(chkEasyTier_, &QCheckBox::toggled, this, &ConnectionDialog::onEasyTierToggled);
    etLayout->addWidget(chkEasyTier_);

    QFormLayout* etForm = new QFormLayout();
    leServerVip_ = new QLineEdit();
    leServerVip_->setPlaceholderText("Server's VPN virtual IP (e.g. 10.144.0.1)");
    etForm->addRow("Server Virtual IP:", leServerVip_);

    btnManageService_ = new QPushButton("Manage EasyTier Service...");
    connect(btnManageService_, &QPushButton::clicked, this, &ConnectionDialog::onManageService);
    etForm->addRow("", btnManageService_);

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
    leServerVip_->setEnabled(checked);
    btnManageService_->setEnabled(checked);
}

void ConnectionDialog::onManageService() {
    ServiceManagerDialog dlg(this);
    dlg.exec();
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
    config_.easytierServerVip = leServerVip_->text().toStdString();

    accept();
}
