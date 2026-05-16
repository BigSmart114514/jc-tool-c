#include "sftp_window.h"
#include "../common/ssh_session.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QInputDialog>
#include <QFileInfo>
#include <cstdlib>
#include <iostream>

SftpWindow::SftpWindow(SshSession* session, QWidget* parent)
    : QMainWindow(parent), session_(session) {

    setWindowTitle("SFTP File Manager");
    resize(900, 600);
    createUI();
}

SftpWindow::~SftpWindow() {}

void SftpWindow::createUI() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(5, 5, 5, 5);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();

    btnUp_ = new QPushButton("Up", this);
    btnUp_->setFixedWidth(50);
    connect(btnUp_, &QPushButton::clicked, this, &SftpWindow::onUpClicked);
    toolbarLayout->addWidget(btnUp_);

    addressBar_ = new QLineEdit(this);
    addressBar_->setPlaceholderText("Remote path...");
    toolbarLayout->addWidget(addressBar_);

    btnGo_ = new QPushButton("Go", this);
    btnGo_->setFixedWidth(50);
    connect(btnGo_, &QPushButton::clicked, this, &SftpWindow::onGoClicked);
    toolbarLayout->addWidget(btnGo_);

    btnRefresh_ = new QPushButton("Refresh", this);
    btnRefresh_->setFixedWidth(70);
    connect(btnRefresh_, &QPushButton::clicked, this, &SftpWindow::onRefreshClicked);
    toolbarLayout->addWidget(btnRefresh_);

    mainLayout->addLayout(toolbarLayout);

    treeWidget_ = new QTreeWidget(this);
    treeWidget_->setColumnCount(4);
    treeWidget_->setHeaderLabels({"Name", "Size", "Type", "Modified"});
    treeWidget_->setRootIsDecorated(false);
    treeWidget_->setAlternatingRowColors(true);
    treeWidget_->setSelectionMode(QAbstractItemView::SingleSelection);
    treeWidget_->setContextMenuPolicy(Qt::CustomContextMenu);

    treeWidget_->header()->resizeSection(0, 300);
    treeWidget_->header()->resizeSection(1, 120);
    treeWidget_->header()->resizeSection(2, 100);
    treeWidget_->header()->resizeSection(3, 180);

    connect(treeWidget_, &QTreeWidget::itemDoubleClicked,
            this, &SftpWindow::onItemDoubleClicked);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested,
            this, &SftpWindow::onItemRightClicked);

    mainLayout->addWidget(treeWidget_);

    statusBar_ = new QStatusBar(this);
    setStatusBar(statusBar_);
    statusBar_->showMessage("Ready");
}

void SftpWindow::navigateTo(const QString& path) {
    if (!session_ || !session_->isConnected()) return;

    QString normPath = path;
    normPath.replace('\\', '/');  // normalize backslashes

    std::string spath = normPath.toStdString();
    if (spath.empty() || spath == ".") {
        const char* home = getenv("HOME");
        if (home) spath = home;
        else spath = ".";
    }

    currentPath_ = QString::fromStdString(spath);
    addressBar_->setText(currentPath_);
    statusBar_->showMessage("Loading...");

    if (!session_->sftpListDir(spath, entries_)) {
        statusBar_->showMessage("Error: " + QString::fromStdString(session_->getError()));
        return;
    }

    populateFileList();
    statusBar_->showMessage(QString("%1 items").arg(entries_.size()));
}

void SftpWindow::populateFileList() {
    treeWidget_->clear();

    for (size_t i = 0; i < entries_.size(); i++) {
        const auto& entry = entries_[i];
        QTreeWidgetItem* item = new QTreeWidgetItem(treeWidget_);

        item->setText(0, QString::fromStdString(entry.name));

        if (!entry.isDir) {
            QString sizeStr;
            if (entry.size < 1024) {
                sizeStr = QString::number(entry.size) + " B";
            } else if (entry.size < 1024LL * 1024) {
                sizeStr = QString::number(entry.size / 1024.0, 'f', 2) + " KB";
            } else if (entry.size < 1024LL * 1024 * 1024) {
                sizeStr = QString::number(entry.size / (1024.0 * 1024.0), 'f', 2) + " MB";
            } else {
                sizeStr = QString::number(entry.size / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
            }
            item->setText(1, sizeStr);
        }

        item->setText(2, entry.isDir ? "Folder" : "File");

        if (entry.modifyTime != 0) {
            QDateTime dt = QDateTime::fromSecsSinceEpoch(entry.modifyTime);
            item->setText(3, dt.toString("yyyy-MM-dd HH:mm:ss"));
        }

        item->setData(0, Qt::UserRole, static_cast<qulonglong>(i));
    }
}

void SftpWindow::onGoClicked() {
    navigateTo(addressBar_->text());
}

void SftpWindow::onUpClicked() {
    QString p = currentPath_;
    if (p.isEmpty() || p == "/") return;

    p.replace('\\', '/');
    // Drive root like "C:" → go to "/"
    if (p.endsWith(':') || p == "/") {
        navigateTo("/");
        return;
    }
    int pos = p.lastIndexOf('/');
    if (pos <= 0) {
        // Keep the drive letter path, e.g. "C:" from "C:/Windows"
        int colon = p.indexOf(':');
        if (colon > 0) {
            navigateTo(p.left(colon + 1));  // "C:"
        } else {
            navigateTo("/");
        }
    } else {
        navigateTo(p.left(pos));
    }
}

void SftpWindow::onRefreshClicked() {
    navigateTo(currentPath_);
}

void SftpWindow::onItemDoubleClicked(QTreeWidgetItem* item, int) {
    if (!item) return;

    qulonglong index = item->data(0, Qt::UserRole).toULongLong();
    if (index >= entries_.size()) return;

    const auto& entry = entries_[index];
    if (!entry.isDir) return;

    QString entryName = QString::fromStdString(entry.name);
    QString newPath;

    if (currentPath_ == "/") {
        // At drives view: "C:" → navigate directly
        newPath = entryName;
    } else {
        newPath = currentPath_;
        newPath.replace('\\', '/');
        if (!newPath.endsWith('/')) newPath += "/";
        newPath += entryName;
    }
    navigateTo(newPath);
}

void SftpWindow::onItemRightClicked(const QPoint& pos) {
    QTreeWidgetItem* item = treeWidget_->itemAt(pos);
    if (!item) {
        QMenu menu(this);
        QAction* uploadAction = menu.addAction("Upload File...");
        connect(uploadAction, &QAction::triggered, this, &SftpWindow::uploadFile);
        menu.addSeparator();
        QAction* mkdirAction = menu.addAction("Create Directory...");
        connect(mkdirAction, &QAction::triggered, this, &SftpWindow::mkdirItem);
        menu.exec(treeWidget_->viewport()->mapToGlobal(pos));
        return;
    }

    qulonglong index = item->data(0, Qt::UserRole).toULongLong();
    if (index >= entries_.size()) return;

    const auto& entry = entries_[index];
    QMenu menu(this);

    if (entry.isDir) {
        QAction* openAction = menu.addAction("Open");
        connect(openAction, &QAction::triggered, [this, item]() {
            onItemDoubleClicked(item, 0);
        });
        menu.addSeparator();
    } else {
        QAction* downloadAction = menu.addAction("Download");
        connect(downloadAction, &QAction::triggered, [this, index]() {
            downloadFile(index);
        });
        menu.addSeparator();
    }

    QAction* deleteAction = menu.addAction("Delete");
    connect(deleteAction, &QAction::triggered, [this, index]() {
        deleteItem(index);
    });

    menu.exec(treeWidget_->viewport()->mapToGlobal(pos));
}

void SftpWindow::downloadFile(int index) {
    if (index >= static_cast<int>(entries_.size())) return;
    if (!session_ || !session_->isConnected()) return;

    const auto& entry = entries_[index];
    if (entry.isDir) {
        QMessageBox::information(this, "Info", "Cannot download a directory");
        return;
    }

    if (downloading_) {
        QMessageBox::information(this, "Info", "Download in progress");
        return;
    }

    QString fileName = QString::fromStdString(entry.name);
    QString savePath = QFileDialog::getSaveFileName(this, "Save File", fileName);
    if (savePath.isEmpty()) return;

    QString remotePath = currentPath_;
    remotePath.replace('\\', '/');
    if (!remotePath.endsWith('/')) remotePath += "/";
    remotePath += fileName;

    downloading_ = true;
    statusBar_->showMessage("Downloading...");
    progressDialog_ = new QProgressDialog("Downloading...", "Cancel", 0, 100, this);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);
    progressDialog_->setValue(0);

    bool ok = session_->sftpDownload(
        remotePath.toStdString(),
        savePath.toStdString(),
        [this](int64_t received, int64_t total) -> bool {
            if (total > 0) {
                int pct = static_cast<int>((received * 100) / total);
                QMetaObject::invokeMethod(this, [this, pct]() {
                    if (progressDialog_) progressDialog_->setValue(pct);
                    statusBar_->showMessage(QString("Downloading... %1%").arg(pct));
                }, Qt::QueuedConnection);
            }
            return !progressDialog_->wasCanceled();
        }
    );

    if (progressDialog_) {
        progressDialog_->close();
        progressDialog_->deleteLater();
        progressDialog_ = nullptr;
    }

    downloading_ = false;

    if (ok) {
        statusBar_->showMessage("Download complete");
        QMessageBox::information(this, "Success", "Download complete!");
    } else {
        statusBar_->showMessage("Download failed");
        QMessageBox::critical(this, "Error", "Download failed: " + QString::fromStdString(session_->getError()));
    }
}

void SftpWindow::uploadFile() {
    if (!session_ || !session_->isConnected()) return;

    QString localPath = QFileDialog::getOpenFileName(this, "Select file to upload");
    if (localPath.isEmpty()) return;

    QFileInfo fi(localPath);
    QString remotePath = currentPath_;
    remotePath.replace('\\', '/');
    if (!remotePath.endsWith('/')) remotePath += "/";
    remotePath += fi.fileName();

    statusBar_->showMessage("Uploading...");
    progressDialog_ = new QProgressDialog("Uploading...", "Cancel", 0, 100, this);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);
    progressDialog_->setValue(0);

    bool ok = session_->sftpUpload(
        localPath.toStdString(),
        remotePath.toStdString(),
        [this](int64_t sent, int64_t total) -> bool {
            if (total > 0) {
                int pct = static_cast<int>((sent * 100) / total);
                QMetaObject::invokeMethod(this, [this, pct]() {
                    if (progressDialog_) progressDialog_->setValue(pct);
                    statusBar_->showMessage(QString("Uploading... %1%").arg(pct));
                }, Qt::QueuedConnection);
            }
            return !progressDialog_->wasCanceled();
        }
    );

    if (progressDialog_) {
        progressDialog_->close();
        progressDialog_->deleteLater();
        progressDialog_ = nullptr;
    }

    if (ok) {
        statusBar_->showMessage("Upload complete");
        onRefreshClicked();
    } else {
        statusBar_->showMessage("Upload failed");
        QMessageBox::critical(this, "Error", "Upload failed: " + QString::fromStdString(session_->getError()));
    }
}

void SftpWindow::deleteItem(int index) {
    if (index >= static_cast<int>(entries_.size())) return;
    if (!session_ || !session_->isConnected()) return;

    const auto& entry = entries_[index];
    QString fullPath = currentPath_;
    fullPath.replace('\\', '/');
    if (!fullPath.endsWith('/')) fullPath += "/";
    fullPath += QString::fromStdString(entry.name);

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Confirm Delete",
        QString("Delete %1?").arg(fullPath),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    if (session_->sftpDelete(fullPath.toStdString())) {
        statusBar_->showMessage("Deleted");
        onRefreshClicked();
    } else {
        statusBar_->showMessage("Delete failed");
        QMessageBox::critical(this, "Error",
            "Delete failed: " + QString::fromStdString(session_->getError()));
    }
}

void SftpWindow::mkdirItem() {
    if (!session_ || !session_->isConnected()) return;

    bool ok;
    QString name = QInputDialog::getText(this, "Create Directory",
        "Directory name:", QLineEdit::Normal, "", &ok);
    if (!ok || name.isEmpty()) return;

    QString fullPath = currentPath_;
    fullPath.replace('\\', '/');
    if (!fullPath.endsWith('/')) fullPath += "/";
    fullPath += name;

    if (session_->sftpMkdir(fullPath.toStdString())) {
        statusBar_->showMessage("Directory created");
        onRefreshClicked();
    } else {
        statusBar_->showMessage("Create directory failed");
        QMessageBox::critical(this, "Error",
            "Create directory failed: " + QString::fromStdString(session_->getError()));
    }
}

void SftpWindow::closeEvent(QCloseEvent* event) {
    hide();
    emit closed();
    event->ignore();
}
