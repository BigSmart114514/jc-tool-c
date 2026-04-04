#ifndef FILE_WINDOW_H
#define FILE_WINDOW_H

#include <QMainWindow>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QMenu>
#include <QProgressDialog>
#include <vector>
#include <mutex>
#include "../common/protocol.h"
#include "../common/transport.h"

class FileWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit FileWindow(QWidget* parent = nullptr);
    ~FileWindow();

    void init(ITransport* transport);
    void handleMessage(const BinaryData& data);
    void navigateTo(const QString& path);

signals:
    void closed();
    void fileListReady();
    void downloadProgress(int percent);
    void downloadComplete(bool success);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onGoClicked();
    void onUpClicked();
    void onRefreshClicked();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onItemRightClicked(const QPoint& pos);
    void onFileListReady();
    void onDownloadProgress(int percent);
    void onDownloadComplete(bool success);

private:
    void createUI();
    void populateFileList();
    void downloadFile(int index);
    void showContextMenu(const QPoint& pos);

    // UI组件
    QLineEdit* addressBar_;
    QPushButton* btnUp_;
    QPushButton* btnGo_;
    QPushButton* btnRefresh_;
    QTreeWidget* treeWidget_;
    QStatusBar* statusBar_;
    QProgressDialog* progressDialog_ = nullptr;

    // 数据
    ITransport* transport_ = nullptr;
    QString currentPath_;
    std::vector<FileManager::FileEntry> files_;
    std::mutex filesMtx_;

    // 下载状态
    QString downloadPath_;
    HANDLE downloadFile_ = INVALID_HANDLE_VALUE;
    uint64_t downloadTotal_ = 0;
    uint64_t downloadReceived_ = 0;
    bool downloading_ = false;
};

#endif // FILE_WINDOW_H