#ifndef CONNECTION_DIALOG_H
#define CONNECTION_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <string>
#include <vector>

struct ConnectionConfig {
    std::string host;
    int sshPort = 2222;
    std::string username;
    std::string password;

    bool useEasyTier = false;
    std::string easytierServerVip;
};

struct DiscoveredDevice {
    std::string peerId;
    std::string ipv4Addr;
    std::string hostname;
    std::string version;
    std::string tunnelProto;
    std::string natType;
    double latencyMs = 0;
    double lossRate = 0;
    int cost = 0;
};

class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(QWidget* parent = nullptr);
    ConnectionConfig getConfig() const { return config_; }

private slots:
    void onConnect();
    void onEasyTierToggled(bool checked);
    void onManageService();
    void onScanDevices();
    void onDeviceActivated(int row, int column);
    void onScanTimer();

private:
    void createUI();
    void populateDeviceTable(const std::vector<DiscoveredDevice>& devices);
    void doTcpScan(std::vector<DiscoveredDevice>& devices);
    void doEasyTierScan(std::vector<DiscoveredDevice>& devices, std::string& errMsg);

    ConnectionConfig config_;
    bool isScanning_ = false;

    QLineEdit* leHost_;
    QLineEdit* lePort_;
    QLineEdit* leUsername_;
    QLineEdit* lePassword_;

    QCheckBox* chkEasyTier_;
    QLineEdit* leServerVip_;
    QPushButton* btnManageService_;

    QTableWidget* tblDevices_;
    QPushButton* btnScanDevices_;
    QLabel* lblScanStatus_;
    QTimer* scanTimer_;
};

#endif