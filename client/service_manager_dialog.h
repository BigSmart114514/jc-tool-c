#ifndef SERVICE_MANAGER_DIALOG_H
#define SERVICE_MANAGER_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTimer>
#include <functional>
#include "../common/easytier_control.h"

class ServiceManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServiceManagerDialog(QWidget* parent = nullptr);
    ~ServiceManagerDialog();

private slots:
    void refreshStatus();
    void onInstallService();
    void onUninstallService();
    void onStart();
    void onStop();
    void onRestart();
    void onApplyConfig();
    void onSshStart();
    void onSshStop();

private:
    void createUI();
    void runAsync(std::function<void()> fn);
    bool waitForPipe(EasyTierControlClient& cli, int maxAttempts = 20, int pauseMs = 300);
    void loadConfigFromService();
    void setButtonsEnabled(bool enabled);
    void refreshSshStatus();

    QLabel* lblStatusIcon_;
    QLabel* lblStatusText_;
    QLabel* lblIp_;

    QPushButton* btnInstall_;
    QPushButton* btnUninstall_;
    QPushButton* btnStart_;
    QPushButton* btnStop_;
    QPushButton* btnRestart_;
    QPushButton* btnApply_;

    // SSH section
    QLabel* lblSshStatus_;
    QPushButton* btnSshStart_;
    QPushButton* btnSshStop_;
    QLineEdit* leSshPort_;
    QLineEdit* leSshPassword_;

    // EasyTier config section
    QLineEdit* leInstName_;
    QLineEdit* leNetName_;
    QLineEdit* leNetSecret_;
    QLineEdit* leIpv4_;
    QLineEdit* leListenPort_;
    QLineEdit* lePeerUrl_;
    QCheckBox* chkAutoStart_;

    QTimer* timer_;
    EasyTierControlClient client_;
};

#endif
