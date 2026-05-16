#ifndef CONNECTION_DIALOG_H
#define CONNECTION_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <string>

struct ConnectionConfig {
    // SSH (to our embedded SSH server)
    std::string host;
    int sshPort = 2222;
    std::string username;
    std::string password;

    // Desktop (separate TCP transport)
    int desktopPort = 12345;

    // EasyTier
    bool useEasyTier = false;
    std::string easytierInstanceName;
    std::string easytierNetworkName;
    std::string easytierNetworkSecret;
    std::string easytierIpv4;
    int easytierListenPort = 11012;
    std::string easytierPeerUrl;
};

class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget* parent = nullptr);
    ConnectionConfig getConfig() const { return config_; }

private slots:
    void onConnect();
    void onEasyTierToggled(bool checked);

private:
    void createUI();

    ConnectionConfig config_;

    QLineEdit* leHost_;
    QLineEdit* lePort_;
    QLineEdit* leUsername_;
    QLineEdit* lePassword_;
    QLineEdit* leDesktopPort_;

    QCheckBox* chkEasyTier_;
    QLineEdit* leInstName_;
    QLineEdit* leNetName_;
    QLineEdit* leNetSecret_;
    QLineEdit* leIpv4_;
    QLineEdit* leListenPort_;
    QLineEdit* lePeerUrl_;
};

#endif
