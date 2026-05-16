#ifndef SFTP_WINDOW_H
#define SFTP_WINDOW_H

#include <QMainWindow>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QMenu>
#include <QProgressDialog>
#include <vector>
#include <string>
#include "../common/ssh_session.h"

class SftpWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit SftpWindow(SshSession* session, QWidget* parent = nullptr);
    ~SftpWindow();
    void navigateTo(const QString& path);

signals:
    void closed();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onGoClicked();
    void onUpClicked();
    void onRefreshClicked();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onItemRightClicked(const QPoint& pos);

private:
    void createUI();
    void populateFileList();
    void downloadFile(int index);
    void uploadFile();
    void deleteItem(int index);
    void mkdirItem();

    SshSession* session_ = nullptr;

    QLineEdit* addressBar_;
    QPushButton* btnUp_;
    QPushButton* btnGo_;
    QPushButton* btnRefresh_;
    QTreeWidget* treeWidget_;
    QStatusBar* statusBar_;
    QProgressDialog* progressDialog_ = nullptr;

    QString currentPath_;
    std::vector<SFtpEntry> entries_;
    bool downloading_ = false;
};

#endif
