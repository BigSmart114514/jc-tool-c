#ifndef SERVER_STATUS_DIALOG_H
#define SERVER_STATUS_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <string>

class ServerStatusDialog : public QDialog {
    Q_OBJECT
public:
    explicit ServerStatusDialog(QWidget* parent = nullptr);

    void setInfo(const std::string& virtualIp, bool useEasyTier,
                 int desktopPort, int sshPort, int width, int height);

signals:
    void stopRequested();

private:
    void createUI();

    QLabel* lblConnection_;
    QLabel* lblDesktopPort_;
    QLabel* lblSshPort_;
    QLabel* lblSshAuth_;
    QLabel* lblResolution_;
};

#endif
