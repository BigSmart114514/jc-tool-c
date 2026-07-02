#ifndef SERVER_SETTINGS_DIALOG_H
#define SERVER_SETTINGS_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <atomic>
#include <string>
#include "../common/easytier_control.h"

struct ServerSettings {
    bool useEasyTier = false;
    int desktopPort = 12345;
    int sshPort = 2222;
    std::string sshPassword;
};

class ServerSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServerSettingsDialog(QWidget* parent = nullptr);
    ServerSettings getSettings() const;

private slots:
    void onEasyTierToggled(bool checked);
    void onManageService();
    void onStartService();
    void refreshEasyTierStatus();

private:
    void createUI();
    void runAsync(std::function<void()> fn);

    ServerSettings settings_;

    QLabel* lblStatusIcon_;
    QLabel* lblStatusText_;
    QLabel* lblIp_;
    QPushButton* btnStartService_;
    QPushButton* btnManageService_;

    QCheckBox* chkEasyTier_;

    QLineEdit* leSshPort_;
    QLineEdit* leSshPassword_;

    QLineEdit* leDesktopPort_;
    QTimer* refreshTimer_;
    std::atomic<bool> refreshBusy_{false};
};

#endif
