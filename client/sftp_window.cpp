#include "sftp_window.h"
#include "../common/ssh_session.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDateTime>
#include <QInputDialog>
#include <QFileInfo>
#include <QApplication>
#include <QClipboard>
#include <QUrl>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QDir>
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
    pathModel_ = new QStringListModel(this);
    pathCompleter_ = new QCompleter(pathModel_, this);
    pathCompleter_->setCaseSensitivity(Qt::CaseInsensitive);
    addressBar_->setCompleter(pathCompleter_);
    toolbarLayout->addWidget(addressBar_);

    btnGo_ = new QPushButton("Go", this);
    btnGo_->setFixedWidth(50);
    connect(btnGo_, &QPushButton::clicked, this, &SftpWindow::onGoClicked);
    toolbarLayout->addWidget(btnGo_);

    btnUpload_ = new QPushButton("Upload", this);
    btnUpload_->setFixedWidth(70);
    connect(btnUpload_, &QPushButton::clicked, this, &SftpWindow::uploadFile);
    toolbarLayout->addWidget(btnUpload_);

    btnMkdir_ = new QPushButton("New Folder", this);
    btnMkdir_->setFixedWidth(90);
    connect(btnMkdir_, &QPushButton::clicked, this, &SftpWindow::mkdirItem);
    toolbarLayout->addWidget(btnMkdir_);

    chkShowHidden_ = new QCheckBox("Show Hidden", this);
    connect(chkShowHidden_, &QCheckBox::toggled, this, &SftpWindow::onShowHiddenToggled);
    toolbarLayout->addWidget(chkShowHidden_);

    btnRefresh_ = new QPushButton("Refresh", this);
    btnRefresh_->setFixedWidth(70);
    connect(btnRefresh_, &QPushButton::clicked, this, &SftpWindow::onRefreshClicked);
    toolbarLayout->addWidget(btnRefresh_);

    mainLayout->addLayout(toolbarLayout);

    searchBar_ = new QLineEdit(this);
    searchBar_->setPlaceholderText("Filter...");
    searchBar_->setClearButtonEnabled(true);
    connect(searchBar_, &QLineEdit::textChanged, this, &SftpWindow::onSearchTextChanged);
    mainLayout->addWidget(searchBar_);

    treeWidget_ = new QTreeWidget(this);
    treeWidget_->setColumnCount(7);
    treeWidget_->setHeaderLabels({"Name", "Size", "Type", "Modified", "Permissions", "Owner", "Group"});
    treeWidget_->setRootIsDecorated(false);
    treeWidget_->setAlternatingRowColors(true);
    treeWidget_->setSortingEnabled(true);
    treeWidget_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    treeWidget_->setContextMenuPolicy(Qt::CustomContextMenu);
    treeWidget_->setDragDropMode(QAbstractItemView::DropOnly);

    treeWidget_->header()->resizeSection(0, 250);
    treeWidget_->header()->resizeSection(1, 80);
    treeWidget_->header()->resizeSection(2, 60);
    treeWidget_->header()->resizeSection(3, 150);
    treeWidget_->header()->resizeSection(4, 100);
    treeWidget_->header()->resizeSection(5, 80);
    treeWidget_->header()->resizeSection(6, 80);
    treeWidget_->header()->setStretchLastSection(true);

    connect(treeWidget_, &QTreeWidget::itemDoubleClicked,
            this, &SftpWindow::onItemDoubleClicked);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested,
            this, &SftpWindow::onItemRightClicked);

    mainLayout->addWidget(treeWidget_);

    statusBar_ = new QStatusBar(this);
    setStatusBar(statusBar_);
    statusBar_->showMessage("Ready");

    setAcceptDrops(true);
}

void SftpWindow::navigateTo(const QString& path) {
    if (!session_ || !session_->isConnected()) return;

    QString normPath = path;
    normPath.replace('\\', '/');

    std::string spath = normPath.toStdString();
    if (spath.empty() || spath == ".") {
        const char* home = getenv("HOME");
        if (home) spath = home;
        else spath = ".";
    }

    currentPath_ = QString::fromStdString(spath);
    addressBar_->setText(currentPath_);
    addRecentPath(currentPath_);
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

        if (!showHidden_ && !entry.name.empty() && entry.name[0] == '.')
            continue;

        QTreeWidgetItem* item = new QTreeWidgetItem(treeWidget_);

        item->setText(0, QString::fromStdString(entry.name));

        if (!entry.isDir) {
            item->setText(1, formatSize(entry.size));
        }

        item->setText(2, entry.isDir ? "Folder" : "File");

        if (entry.modifyTime != 0) {
            QDateTime dt = QDateTime::fromSecsSinceEpoch(entry.modifyTime);
            item->setText(3, dt.toString("yyyy-MM-dd HH:mm:ss"));
        }

        item->setText(4, formatPermissions(entry.permissions, entry.isDir, entry.isSymlink));
        item->setText(5, QString::fromStdString(entry.owner));
        item->setText(6, QString::fromStdString(entry.group));

        item->setData(0, Qt::UserRole, static_cast<qulonglong>(i));
    }

    onSearchTextChanged(searchBar_->text());
}

void SftpWindow::onGoClicked() {
    navigateTo(addressBar_->text());
}

void SftpWindow::onUpClicked() {
    QString p = currentPath_;
    if (p.isEmpty() || p == "/") return;

    p.replace('\\', '/');
    if (p.endsWith(':') || p == "/") {
        navigateTo("/");
        return;
    }
    int pos = p.lastIndexOf('/');
    if (pos <= 0) {
        int colon = p.indexOf(':');
        if (colon > 0) {
            navigateTo(p.left(colon + 1));
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
    QList<QTreeWidgetItem*> selected = treeWidget_->selectedItems();

    if (!item) {
        QMenu menu(this);
        QAction* uploadAction = menu.addAction("Upload File...");
        connect(uploadAction, &QAction::triggered, this, &SftpWindow::uploadFile);
        menu.addSeparator();
        QAction* mkdirAction = menu.addAction("Create Directory...");
        connect(mkdirAction, &QAction::triggered, this, &SftpWindow::mkdirItem);
        menu.addSeparator();
        QAction* pasteAction = menu.addAction("Paste Path");
        connect(pasteAction, &QAction::triggered, [this]() {
            QClipboard* clip = QApplication::clipboard();
            if (clip) {
                QString text = clip->text().trimmed();
                if (!text.isEmpty()) navigateTo(text);
            }
        });
        menu.exec(treeWidget_->viewport()->mapToGlobal(pos));
        return;
    }

    if (!item->isSelected()) {
        treeWidget_->clearSelection();
        item->setSelected(true);
        selected = treeWidget_->selectedItems();
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
        if (selected.size() == 1) {
            QAction* recursiveAction = menu.addAction("Download Recursively...");
            connect(recursiveAction, &QAction::triggered, [this, index]() {
                const auto& e = entries_[index];
                QString remotePath = currentPath_;
                if (!remotePath.endsWith('/')) remotePath += "/";
                remotePath += QString::fromStdString(e.name);
                QString localDir = QFileDialog::getExistingDirectory(this, "Save to directory");
                if (!localDir.isEmpty()) {
                    localDir += "/" + QString::fromStdString(e.name);
                    downloadDirRecursive(remotePath, localDir);
                    statusBar_->showMessage("Recursive download complete");
                    QMessageBox::information(this, "Success", "Recursive download complete!");
                }
            });
        }
        menu.addSeparator();
    } else {
        QAction* downloadAction = menu.addAction("Download");
        connect(downloadAction, &QAction::triggered, [this, index]() {
            downloadFile(index);
        });
        QAction* copyPathAction = menu.addAction("Copy Path");
        connect(copyPathAction, &QAction::triggered, [this, index]() {
            copyPath(index);
        });
        menu.addSeparator();
    }

    // Upload & New Folder available on any item context
    QAction* uploadHereAction = menu.addAction("Upload File...");
    connect(uploadHereAction, &QAction::triggered, this, &SftpWindow::uploadFile);
    QAction* mkdirHereAction = menu.addAction("Create Directory...");
    connect(mkdirHereAction, &QAction::triggered, this, &SftpWindow::mkdirItem);
    menu.addSeparator();

    if (selected.size() > 1) {
        bool hasFile = false;
        for (auto* sel : selected) {
            qulonglong idx = sel->data(0, Qt::UserRole).toULongLong();
            if (idx < entries_.size() && !entries_[idx].isDir) {
                hasFile = true;
                break;
            }
        }
        if (hasFile) {
            QAction* batchDlAction = menu.addAction("Batch Download");
            connect(batchDlAction, &QAction::triggered, this, &SftpWindow::onBatchDownload);
        }
        QAction* batchDelAction = menu.addAction("Batch Delete");
        connect(batchDelAction, &QAction::triggered, this, &SftpWindow::onBatchDelete);
    } else {
        QAction* renameAction = menu.addAction("Rename");
        connect(renameAction, &QAction::triggered, this, &SftpWindow::onRenameItem);

        QAction* deleteAction = menu.addAction("Delete");
        connect(deleteAction, &QAction::triggered, [this, index]() {
            deleteItem(index);
        });

        QAction* propertiesAction = menu.addAction("Properties");
        connect(propertiesAction, &QAction::triggered, this, &SftpWindow::onShowProperties);
    }

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

    if (activeTransferCount_ > 0) {
        QMessageBox::information(this, "Info", "Transfer in progress");
        return;
    }

    QString fileName = QString::fromStdString(entry.name);
    QString savePath = QFileDialog::getSaveFileName(this, "Save File", fileName);
    if (savePath.isEmpty()) return;

    QString remotePath = currentPath_;
    remotePath.replace('\\', '/');
    if (!remotePath.endsWith('/')) remotePath += "/";
    remotePath += fileName;

    activeTransferCount_++;
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

    activeTransferCount_--;

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

void SftpWindow::uploadFiles(const QStringList& localPaths) {
    if (!session_ || !session_->isConnected() || localPaths.isEmpty()) return;

    progressDialog_ = new QProgressDialog("Uploading files...", "Cancel", 0, localPaths.size(), this);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);

    for (int i = 0; i < localPaths.size(); i++) {
        if (progressDialog_->wasCanceled()) break;

        QFileInfo fi(localPaths[i]);
        if (!fi.exists()) continue;

        progressDialog_->setLabelText(QString("Uploading %1/%2: %3")
            .arg(i + 1).arg(localPaths.size()).arg(fi.fileName()));
        progressDialog_->setValue(i);

        QString remotePath = currentPath_;
        remotePath.replace('\\', '/');
        if (!remotePath.endsWith('/')) remotePath += "/";
        remotePath += fi.fileName();

        activeTransferCount_++;
        bool ok = session_->sftpUpload(
            localPaths[i].toStdString(),
            remotePath.toStdString(),
            [this](int64_t sent, int64_t total) -> bool {
                Q_UNUSED(sent) Q_UNUSED(total)
                return !progressDialog_->wasCanceled();
            }
        );
        activeTransferCount_--;

        if (!ok) {
            statusBar_->showMessage(QString("Upload failed: %1").arg(fi.fileName()));
        }
    }

    if (progressDialog_) {
        progressDialog_->close();
        progressDialog_->deleteLater();
        progressDialog_ = nullptr;
    }

    onRefreshClicked();
    statusBar_->showMessage("Upload complete");
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

void SftpWindow::onSearchTextChanged(const QString& text) {
    for (int i = 0; i < treeWidget_->topLevelItemCount(); i++) {
        QTreeWidgetItem* item = treeWidget_->topLevelItem(i);
        bool matches = text.isEmpty() ||
            item->text(0).contains(text, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
}

void SftpWindow::onShowHiddenToggled(bool checked) {
    showHidden_ = checked;
    onRefreshClicked();
}

void SftpWindow::onRenameItem() {
    auto selected = treeWidget_->selectedItems();
    if (selected.isEmpty()) return;

    QTreeWidgetItem* item = selected.first();
    qulonglong index = item->data(0, Qt::UserRole).toULongLong();
    renameItem(static_cast<int>(index));
}

void SftpWindow::renameItem(int index) {
    if (index >= static_cast<int>(entries_.size())) return;
    if (!session_ || !session_->isConnected()) return;

    const auto& entry = entries_[index];

    QString fullPath = currentPath_;
    if (!fullPath.endsWith('/')) fullPath += "/";
    fullPath += QString::fromStdString(entry.name);

    bool ok;
    QString newName = QInputDialog::getText(this, "Rename",
        "New name:", QLineEdit::Normal,
        QString::fromStdString(entry.name), &ok);
    if (!ok || newName.isEmpty() || newName == QString::fromStdString(entry.name)) return;

    QString newPath = currentPath_;
    if (!newPath.endsWith('/')) newPath += "/";
    newPath += newName;

    if (session_->sftpRename(fullPath.toStdString(), newPath.toStdString())) {
        statusBar_->showMessage("Renamed");
        onRefreshClicked();
    } else {
        statusBar_->showMessage("Rename failed");
        QMessageBox::critical(this, "Error",
            "Rename failed: " + QString::fromStdString(session_->getError()));
    }
}

void SftpWindow::onShowProperties() {
    auto selected = treeWidget_->selectedItems();
    if (selected.isEmpty()) return;

    QTreeWidgetItem* item = selected.first();
    qulonglong index = item->data(0, Qt::UserRole).toULongLong();
    showProperties(static_cast<int>(index));
}

void SftpWindow::showProperties(int index) {
    if (index >= static_cast<int>(entries_.size())) return;

    const auto& entry = entries_[index];

    QDialog dlg(this);
    dlg.setWindowTitle("Properties - " + QString::fromStdString(entry.name));
    dlg.setMinimumWidth(400);

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    QFormLayout* form = new QFormLayout();

    form->addRow("Name:", new QLabel(QString::fromStdString(entry.name), &dlg));
    form->addRow("Type:", new QLabel(entry.isDir ? "Folder" : "File", &dlg));

    if (!entry.isDir) {
        form->addRow("Size:", new QLabel(formatSize(entry.size), &dlg));
    }

    if (entry.modifyTime != 0) {
        QDateTime dt = QDateTime::fromSecsSinceEpoch(entry.modifyTime);
        form->addRow("Modified:", new QLabel(dt.toString("yyyy-MM-dd HH:mm:ss"), &dlg));
    }

    form->addRow("Permissions:", new QLabel(
        formatPermissions(entry.permissions, entry.isDir, entry.isSymlink), &dlg));
    form->addRow("Owner:", new QLabel(QString::fromStdString(entry.owner), &dlg));
    form->addRow("Group:", new QLabel(QString::fromStdString(entry.group), &dlg));

    QString fullPath = currentPath_;
    if (!fullPath.endsWith('/')) fullPath += "/";
    fullPath += QString::fromStdString(entry.name);
    form->addRow("Full Path:", new QLabel(fullPath, &dlg));

    if (entry.isSymlink) {
        form->addRow("Symlink Target:", new QLabel(
            QString::fromStdString(entry.symlinkTarget), &dlg));
    }

    layout->addLayout(form);

    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    connect(buttonBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    layout->addWidget(buttonBox);

    dlg.exec();
}

void SftpWindow::copyPath(int index) {
    if (index >= static_cast<int>(entries_.size())) return;

    QString fullPath = currentPath_;
    if (!fullPath.endsWith('/')) fullPath += "/";
    fullPath += QString::fromStdString(entries_[index].name);

    QApplication::clipboard()->setText(fullPath);
    statusBar_->showMessage("Path copied to clipboard");
}

void SftpWindow::onBatchDownload() {
    auto selected = treeWidget_->selectedItems();
    if (selected.isEmpty()) return;

    QString dir = QFileDialog::getExistingDirectory(this, "Save to directory");
    if (dir.isEmpty()) return;

    progressDialog_ = new QProgressDialog("Downloading files...", "Cancel", 0, selected.size(), this);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);

    int successCount = 0;
    for (int i = 0; i < selected.size(); i++) {
        if (progressDialog_->wasCanceled()) break;

        qulonglong idx = selected[i]->data(0, Qt::UserRole).toULongLong();
        if (idx >= entries_.size()) continue;

        const auto& entry = entries_[idx];
        if (entry.isDir) continue;

        progressDialog_->setLabelText(QString("Downloading %1/%2: %3")
            .arg(i + 1).arg(selected.size())
            .arg(QString::fromStdString(entry.name)));
        progressDialog_->setValue(i);

        QString remotePath = currentPath_;
        if (!remotePath.endsWith('/')) remotePath += "/";
        remotePath += QString::fromStdString(entry.name);

        QString localPath = dir + "/" + QString::fromStdString(entry.name);

        activeTransferCount_++;
        bool ok = session_->sftpDownload(
            remotePath.toStdString(),
            localPath.toStdString(),
            [this](int64_t received, int64_t total) -> bool {
                Q_UNUSED(received) Q_UNUSED(total)
                return !progressDialog_->wasCanceled();
            }
        );
        activeTransferCount_--;

        if (ok) successCount++;
    }

    if (progressDialog_) {
        progressDialog_->close();
        progressDialog_->deleteLater();
        progressDialog_ = nullptr;
    }

    statusBar_->showMessage(QString("Downloaded %1 of %2 files").arg(successCount).arg(selected.size()));
    QMessageBox::information(this, "Batch Download",
        QString("Downloaded %1 of %2 files successfully.").arg(successCount).arg(selected.size()));
}

void SftpWindow::onBatchDelete() {
    auto selected = treeWidget_->selectedItems();
    if (selected.isEmpty()) return;

    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Confirm Batch Delete",
        QString("Delete %1 selected item(s)?").arg(selected.size()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    int successCount = 0;
    for (auto* item : selected) {
        qulonglong idx = item->data(0, Qt::UserRole).toULongLong();
        if (idx >= entries_.size()) continue;

        const auto& entry = entries_[idx];
        QString fullPath = currentPath_;
        if (!fullPath.endsWith('/')) fullPath += "/";
        fullPath += QString::fromStdString(entry.name);

        if (session_->sftpDelete(fullPath.toStdString())) {
            successCount++;
        }
    }

    statusBar_->showMessage(QString("Deleted %1 of %2 items").arg(successCount).arg(selected.size()));
    onRefreshClicked();
}

void SftpWindow::onAddBookmark() {
    if (currentPath_.isEmpty()) return;

    if (!bookmarks_.contains(currentPath_)) {
        bookmarks_.append(currentPath_);
        statusBar_->showMessage("Bookmark added");
    } else {
        statusBar_->showMessage("Already bookmarked");
    }
}

void SftpWindow::onGotoBookmark() {
    if (bookmarks_.isEmpty()) {
        QMessageBox::information(this, "Bookmarks", "No bookmarks saved.");
        return;
    }

    bool ok;
    QString selected = QInputDialog::getItem(this, "Go to Bookmark",
        "Select bookmark:", bookmarks_, 0, false, &ok);
    if (ok && !selected.isEmpty()) {
        navigateTo(selected);
    }
}

void SftpWindow::downloadDirRecursive(const QString& remotePath, const QString& localPath) {
    QDir().mkpath(localPath);

    std::vector<SFtpEntry> dirEntries;
    if (!session_->sftpListDir(remotePath.toStdString(), dirEntries)) {
        return;
    }

    for (const auto& entry : dirEntries) {
        if (entry.name == "." || entry.name == "..") continue;

        QString fullRemote = remotePath;
        if (!fullRemote.endsWith('/')) fullRemote += "/";
        fullRemote += QString::fromStdString(entry.name);

        QString fullLocal = localPath + "/" + QString::fromStdString(entry.name);

        if (entry.isDir) {
            downloadDirRecursive(fullRemote, fullLocal);
        } else {
            session_->sftpDownload(
                fullRemote.toStdString(),
                fullLocal.toStdString(),
                nullptr
            );
        }
    }
}

void SftpWindow::processTransferQueue() {
    if (transferQueue_.isEmpty() || activeTransferCount_ > 0) return;

    TransferJob job = transferQueue_.takeFirst();
    activeTransferCount_++;

    bool ok = false;
    if (job.type == TransferJob::Download) {
        ok = session_->sftpDownload(
            job.remotePath.toStdString(),
            job.localPath.toStdString(),
            nullptr
        );
    } else {
        ok = session_->sftpUpload(
            job.localPath.toStdString(),
            job.remotePath.toStdString(),
            nullptr
        );
    }

    activeTransferCount_--;

    Q_UNUSED(ok)

    if (!transferQueue_.isEmpty()) {
        processTransferQueue();
    }
}

void SftpWindow::addRecentPath(const QString& path) {
    recentPaths_.removeAll(path);
    recentPaths_.prepend(path);
    while (recentPaths_.size() > 50) {
        recentPaths_.removeLast();
    }
    pathModel_->setStringList(recentPaths_);
}

QString SftpWindow::formatSize(uint64_t bytes) {
    if (bytes < 1024) {
        return QString::number(bytes) + " B";
    } else if (bytes < 1024ULL * 1024) {
        return QString::number(bytes / 1024.0, 'f', 2) + " KB";
    } else if (bytes < 1024ULL * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 2) + " MB";
    } else {
        return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
    }
}

QString SftpWindow::formatPermissions(uint32_t perm, bool isDir, bool isSymlink) {
    char buf[11];
    buf[0] = isSymlink ? 'l' : (isDir ? 'd' : '-');
    buf[1] = (perm & 0400) ? 'r' : '-';
    buf[2] = (perm & 0200) ? 'w' : '-';
    buf[3] = (perm & 0100) ? ((perm & 04000) ? 's' : 'x') : ((perm & 04000) ? 'S' : '-');
    buf[4] = (perm & 0040) ? 'r' : '-';
    buf[5] = (perm & 0020) ? 'w' : '-';
    buf[6] = (perm & 0010) ? ((perm & 02000) ? 's' : 'x') : ((perm & 02000) ? 'S' : '-');
    buf[7] = (perm & 0004) ? 'r' : '-';
    buf[8] = (perm & 0002) ? 'w' : '-';
    buf[9] = (perm & 0001) ? ((perm & 01000) ? 't' : 'x') : ((perm & 01000) ? 'T' : '-');
    buf[10] = '\0';
    return QString::fromLatin1(buf);
}

void SftpWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_F5:
        onRefreshClicked();
        return;
    case Qt::Key_Delete:
        onBatchDelete();
        return;
    case Qt::Key_F2:
        onRenameItem();
        return;
    case Qt::Key_C:
        if (event->modifiers() & Qt::ControlModifier) {
            auto selected = treeWidget_->selectedItems();
            if (!selected.isEmpty()) {
                QTreeWidgetItem* item = selected.first();
                qulonglong index = item->data(0, Qt::UserRole).toULongLong();
                copyPath(static_cast<int>(index));
            }
            return;
        }
        break;
    case Qt::Key_V:
        if (event->modifiers() & Qt::ControlModifier) {
            QClipboard* clip = QApplication::clipboard();
            if (clip) {
                QString text = clip->text().trimmed();
                if (!text.isEmpty()) navigateTo(text);
            }
            return;
        }
        break;
    default:
        break;
    }
    QMainWindow::keyPressEvent(event);
}

void SftpWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void SftpWindow::dropEvent(QDropEvent* event) {
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasUrls()) {
        QStringList paths;
        for (const QUrl& url : mimeData->urls()) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
        if (!paths.isEmpty()) {
            uploadFiles(paths);
        }
    }
}

void SftpWindow::closeEvent(QCloseEvent* event) {
    hide();
    emit closed();
    event->ignore();
}
