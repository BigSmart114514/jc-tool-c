#include "server_status_dialog.h"
#include <QVBoxLayout>
#include <QPushButton>

ServerStatusDialog::ServerStatusDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("Server Running");
    setMinimumSize(400, 250);
    createUI();
}

void ServerStatusDialog::createUI() {
    QVBoxLayout* layout = new QVBoxLayout(this);

    lblConnection_ = new QLabel();
    layout->addWidget(lblConnection_);

    lblDesktopPort_ = new QLabel();
    layout->addWidget(lblDesktopPort_);

    lblSshPort_ = new QLabel();
    layout->addWidget(lblSshPort_);

    lblSshAuth_ = new QLabel();
    layout->addWidget(lblSshAuth_);

    lblResolution_ = new QLabel();
    layout->addWidget(lblResolution_);

    layout->addStretch();

    QPushButton* btnStop = new QPushButton("Stop Server");
    btnStop->setStyleSheet("background-color: #c9302c; color: white; padding: 8px;");
    layout->addWidget(btnStop);

    connect(btnStop, &QPushButton::clicked, this, [this]() {
        hide();
        emit stopRequested();
    });
}

void ServerStatusDialog::setInfo(const std::string& virtualIp, bool useEasyTier,
                                  int desktopPort, int sshPort, int width, int height) {
    if (useEasyTier) {
        lblConnection_->setText(
            QString("Virtual IP: %1 (EasyTier Service)").arg(virtualIp.c_str()));
    } else {
        lblConnection_->setText("Mode: Direct TCP (no VPN)");
    }
    lblDesktopPort_->setText(QString("Desktop Port: %1").arg(desktopPort));
    lblSshPort_->setText(QString("SSH Port: %1 (via Service)").arg(sshPort));
    lblSshAuth_->setText("SSH Auth: Password (configured in Service)");
    lblResolution_->setText(QString("Resolution: %1x%2").arg(width).arg(height));
}
