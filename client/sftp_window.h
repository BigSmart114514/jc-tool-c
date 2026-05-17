#ifndef SFTP_WINDOW_H
#define SFTP_WINDOW_H

#include <QMainWindow>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QStatusBar>
#include <QMenu>
#include <QProgressDialog>
#include <QCompleter>
#include <QStringListModel>
#include <QList>
#include <QStringList>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <vector>
#include <string>
#include "../common/ssh_session.h"

struct TransferJob {
    enum Type { Download, Upload };
    Type type;
    QString localPath;
    QString remotePath;
    QString name;
};

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
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onGoClicked();
    void onUpClicked();
    void onRefreshClicked();
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onItemRightClicked(const QPoint& pos);
    void onSearchTextChanged(const QString& text);
    void onShowHiddenToggled(bool checked);
    void onRenameItem();
    void onShowProperties();
    void onBatchDownload();
    void onBatchDelete();
    void onAddBookmark();
    void onGotoBookmark();

private:
    void createUI();
    void populateFileList();
    void downloadFile(int index);
    void uploadFile();
    void uploadFiles(const QStringList& localPaths);
    void deleteItem(int index);
    void mkdirItem();
    void renameItem(int index);
    void showProperties(int index);
    void copyPath(int index);
    void downloadDirRecursive(const QString& remotePath, const QString& localPath);
    void processTransferQueue();
    void addRecentPath(const QString& path);

    static QString formatSize(uint64_t bytes);
    static QString formatPermissions(uint32_t perm, bool isDir, bool isSymlink);

    SshSession* session_ = nullptr;

    QLineEdit* addressBar_;
    QLineEdit* searchBar_;
    QCheckBox* chkShowHidden_;
    QPushButton* btnUp_;
    QPushButton* btnGo_;
    QPushButton* btnUpload_;
    QPushButton* btnMkdir_;
    QPushButton* btnRefresh_;
    QTreeWidget* treeWidget_;
    QStatusBar* statusBar_;
    QProgressDialog* progressDialog_ = nullptr;
    QCompleter* pathCompleter_;
    QStringListModel* pathModel_;

    QString currentPath_;
    std::vector<SFtpEntry> entries_;

    bool showHidden_ = false;
    QStringList recentPaths_;
    QStringList bookmarks_;
    QList<TransferJob> transferQueue_;
    int activeTransferCount_ = 0;
};

#endif
