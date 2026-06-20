#ifndef SERVICE_MANAGER_DIALOG_H
#define SERVICE_MANAGER_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QTimer>
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

private:
    void createUI();
    void loadConfigFromService();
    void setButtonsEnabled(bool enabled);

    QLabel* lblStatusIcon_;
    QLabel* lblStatusText_;
    QLabel* lblIp_;

    QLineEdit* leInstName_;
    QLineEdit* leNetName_;
    QLineEdit* leNetSecret_;
    QLineEdit* leIpv4_;
    QLineEdit* leListenPort_;
    QLineEdit* lePeerUrl_;
    QCheckBox* chkAutoStart_;

    QPushButton* btnInstall_;
    QPushButton* btnUninstall_;
    QPushButton* btnStart_;
    QPushButton* btnStop_;
    QPushButton* btnRestart_;
    QPushButton* btnApply_;

    QTimer* timer_;
    EasyTierControlClient client_;
};

#endif
