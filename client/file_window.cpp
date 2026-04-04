#include "file_window.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QDateTime>
#include <iostream>

FileWindow::FileWindow(QWidget* parent)
    : QMainWindow(parent) {
    
    setWindowTitle("Remote File Manager");
    resize(800, 600);
    
    createUI();
    
    connect(this, &FileWindow::fileListReady, this, &FileWindow::onFileListReady);
    connect(this, &FileWindow::downloadProgress, this, &FileWindow::onDownloadProgress);
    connect(this, &FileWindow::downloadComplete, this, &FileWindow::onDownloadComplete);
}

FileWindow::~FileWindow() {
    if (downloadFile_ != INVALID_HANDLE_VALUE) {
        CloseHandle(downloadFile_);
    }
}

void FileWindow::createUI() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(5, 5, 5, 5);

    // 工具栏
    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    
    btnUp_ = new QPushButton("Up", this);
    btnUp_->setFixedWidth(50);
    connect(btnUp_, &QPushButton::clicked, this, &FileWindow::onUpClicked);
    toolbarLayout->addWidget(btnUp_);
    
    addressBar_ = new QLineEdit(this);
    toolbarLayout->addWidget(addressBar_);
    
    btnGo_ = new QPushButton("Go", this);
    btnGo_->setFixedWidth(50);
    connect(btnGo_, &QPushButton::clicked, this, &FileWindow::onGoClicked);
    toolbarLayout->addWidget(btnGo_);
    
    btnRefresh_ = new QPushButton("Refresh", this);
    btnRefresh_->setFixedWidth(70);
    connect(btnRefresh_, &QPushButton::clicked, this, &FileWindow::onRefreshClicked);
    toolbarLayout->addWidget(btnRefresh_);
    
    mainLayout->addLayout(toolbarLayout);

    // 文件列表
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
            this, &FileWindow::onItemDoubleClicked);
    connect(treeWidget_, &QTreeWidget::customContextMenuRequested,
            this, &FileWindow::onItemRightClicked);
    
    mainLayout->addWidget(treeWidget_);

    // 状态栏
    statusBar_ = new QStatusBar(this);
    setStatusBar(statusBar_);
    statusBar_->showMessage("Ready");
}

void FileWindow::init(ITransport* transport) {
    transport_ = transport;
}

void FileWindow::handleMessage(const BinaryData& data) {
    if (data.empty()) return;

    auto type = static_cast<FileManager::MsgType>(data[0]);

    switch (type) {
        case FileManager::MsgType::Response: {
            if (data.size() < 1 + sizeof(FileManager::ListHeader)) break;

            auto* header = reinterpret_cast<const FileManager::ListHeader*>(data.data() + 1);

            std::lock_guard<std::mutex> lock(filesMtx_);
            files_.clear();

            if (header->status == static_cast<uint8_t>(FileManager::Status::OK) && header->count > 0) {
                auto* entries = reinterpret_cast<const FileManager::FileEntry*>(
                    data.data() + 1 + sizeof(FileManager::ListHeader));

                size_t expectedSize = 1 + sizeof(FileManager::ListHeader) + 
                                     header->count * sizeof(FileManager::FileEntry);
                if (data.size() >= expectedSize) {
                    for (uint32_t i = 0; i < header->count; i++) {
                        files_.push_back(entries[i]);
                    }
                }
            }

            emit fileListReady();
            break;
        }

        case FileManager::MsgType::DownloadData: {
            if (data.size() < 1 + sizeof(FileManager::TransferHeader)) break;

            auto* header = reinterpret_cast<const FileManager::TransferHeader*>(data.data() + 1);

            if (header->status == static_cast<uint8_t>(FileManager::Status::NotFound) ||
                header->status == static_cast<uint8_t>(FileManager::Status::Error)) {
                if (downloadFile_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(downloadFile_);
                    downloadFile_ = INVALID_HANDLE_VALUE;
                    QFile::remove(downloadPath_);
                }
                downloading_ = false;
                emit downloadComplete(false);
                break;
            }

            if (!downloading_) break;

            if (header->chunkSize > 0 && downloadFile_ != INVALID_HANDLE_VALUE) {
                const uint8_t* chunkData = data.data() + 1 + sizeof(FileManager::TransferHeader);
                DWORD written;
                WriteFile(downloadFile_, chunkData, header->chunkSize, &written, nullptr);
                downloadReceived_ += written;
            }

            if (header->totalSize > 0) {
                int progress = static_cast<int>((downloadReceived_ * 100) / header->totalSize);
                emit downloadProgress(progress);
            }

            if (header->status == static_cast<uint8_t>(FileManager::Status::Complete)) {
                if (downloadFile_ != INVALID_HANDLE_VALUE) {
                    CloseHandle(downloadFile_);
                    downloadFile_ = INVALID_HANDLE_VALUE;
                }
                downloading_ = false;
                emit downloadComplete(true);
            }
            break;
        }

        default:
            break;
    }
}

void FileWindow::navigateTo(const QString& path) {
    if (!transport_ || !transport_->isConnected()) return;
    
    currentPath_ = path;
    addressBar_->setText(path);
    statusBar_->showMessage("Loading...");
    
    auto request = MessageBuilder::FileListRequest(path.toStdWString());
    transport_->send(request);
}

void FileWindow::onGoClicked() {
    navigateTo(addressBar_->text());
}

void FileWindow::onUpClicked() {
    if (currentPath_.isEmpty()) return;
    
    int pos = currentPath_.lastIndexOf('\\');
    if (pos == -1) {
        navigateTo("");
    } else if (pos == 2 && currentPath_.length() == 3) {
        navigateTo("");
    } else {
        navigateTo(currentPath_.left(pos));
    }
}

void FileWindow::onRefreshClicked() {
    navigateTo(currentPath_);
}

void FileWindow::onFileListReady() {
    populateFileList();
}

void FileWindow::populateFileList() {
    treeWidget_->clear();
    
    std::lock_guard<std::mutex> lock(filesMtx_);
    
    for (const auto& entry : files_) {
        QTreeWidgetItem* item = new QTreeWidgetItem(treeWidget_);
        
        // 名称
        item->setText(0, QString::fromWCharArray(entry.name));
        
        // 大小
        if (static_cast<FileManager::FileType>(entry.type) == FileManager::FileType::File) {
            QString sizeStr;
            if (entry.size < 1024) {
                sizeStr = QString::number(entry.size) + " B";
            } else if (entry.size < 1024 * 1024) {
                sizeStr = QString::number(entry.size / 1024.0, 'f', 2) + " KB";
            } else if (entry.size < 1024ULL * 1024 * 1024) {
                sizeStr = QString::number(entry.size / (1024.0 * 1024.0), 'f', 2) + " MB";
            } else {
                sizeStr = QString::number(entry.size / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
            }
            item->setText(1, sizeStr);
        }
        
        // 类型
        QString typeStr;
        switch (static_cast<FileManager::FileType>(entry.type)) {
            case FileManager::FileType::Drive:     typeStr = "Drive"; break;
            case FileManager::FileType::Directory: typeStr = "Folder"; break;
            default:                               typeStr = "File"; break;
        }
        item->setText(2, typeStr);
        
        // 时间
        if (entry.modifyTime != 0) {
            FILETIME ft;
            ft.dwLowDateTime = static_cast<DWORD>(entry.modifyTime & 0xFFFFFFFF);
            ft.dwHighDateTime = static_cast<DWORD>(entry.modifyTime >> 32);
            
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&ft, &ft);
            FileTimeToSystemTime(&ft, &st);
            
            QDateTime dt(QDate(st.wYear, st.wMonth, st.wDay),
                        QTime(st.wHour, st.wMinute, st.wSecond));
            item->setText(3, dt.toString("yyyy-MM-dd HH:mm:ss"));
        }
        
        // 存储索引
        item->setData(0, Qt::UserRole, static_cast<int>(&entry - &files_[0]));
    }
    
    statusBar_->showMessage(QString("%1 items").arg(files_.size()));
}

void FileWindow::onItemDoubleClicked(QTreeWidgetItem* item, int column) {
    if (!item) return;
    
    int index = item->data(0, Qt::UserRole).toInt();
    FileManager::FileEntry entry;
    
    {
        std::lock_guard<std::mutex> lock(filesMtx_);
        if (index < 0 || index >= static_cast<int>(files_.size())) return;
        entry = files_[index];
    }
    
    auto type = static_cast<FileManager::FileType>(entry.type);
    QString name = QString::fromWCharArray(entry.name);
    
    if (type == FileManager::FileType::Drive) {
        navigateTo(name + "\\");
    } else if (type == FileManager::FileType::Directory) {
        if (name == "..") {
            onUpClicked();
        } else {
            QString newPath = currentPath_;
            if (!newPath.isEmpty() && !newPath.endsWith('\\')) {
                newPath += "\\";
            }
            newPath += name;
            navigateTo(newPath);
        }
    }
}

void FileWindow::onItemRightClicked(const QPoint& pos) {
    QTreeWidgetItem* item = treeWidget_->itemAt(pos);
    if (!item) return;
    
    int index = item->data(0, Qt::UserRole).toInt();
    FileManager::FileEntry entry;
    
    {
        std::lock_guard<std::mutex> lock(filesMtx_);
        if (index < 0 || index >= static_cast<int>(files_.size())) return;
        entry = files_[index];
    }
    
    QMenu menu(this);
    auto type = static_cast<FileManager::FileType>(entry.type);
    
    if (type == FileManager::FileType::File) {
        QAction* downloadAction = menu.addAction("Download");
        connect(downloadAction, &QAction::triggered, [this, index]() {
            downloadFile(index);
        });
        menu.addSeparator();
    } else {
        QAction* openAction = menu.addAction("Open");
        connect(openAction, &QAction::triggered, [this, item]() {
            onItemDoubleClicked(item, 0);
        });
        menu.addSeparator();
    }
    
    menu.addAction("Refresh", this, &FileWindow::onRefreshClicked);
    
    menu.exec(treeWidget_->viewport()->mapToGlobal(pos));
}

void FileWindow::downloadFile(int index) {
    FileManager::FileEntry entry;
    {
        std::lock_guard<std::mutex> lock(filesMtx_);
        if (index < 0 || index >= static_cast<int>(files_.size())) return;
        entry = files_[index];
    }
    
    if (static_cast<FileManager::FileType>(entry.type) != FileManager::FileType::File) return;
    if (downloading_) {
        QMessageBox::information(this, "Info", "Download in progress");
        return;
    }
    
    QString fileName = QString::fromWCharArray(entry.name);
    QString savePath = QFileDialog::getSaveFileName(this, "Save File", fileName);
    if (savePath.isEmpty()) return;
    
    downloadFile_ = CreateFileW(savePath.toStdWString().c_str(),
                                GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (downloadFile_ == INVALID_HANDLE_VALUE) {
        QMessageBox::critical(this, "Error", "Cannot create file");
        return;
    }
    
    downloadPath_ = savePath;
    downloadTotal_ = entry.size;
    downloadReceived_ = 0;
    downloading_ = true;
    
    // 显示进度对话框
    progressDialog_ = new QProgressDialog("Downloading...", "Cancel", 0, 100, this);
    progressDialog_->setWindowModality(Qt::WindowModal);
    progressDialog_->setMinimumDuration(0);
    progressDialog_->setValue(0);
    
    statusBar_->showMessage("Downloading...");
    
    QString remotePath = currentPath_;
    if (!remotePath.isEmpty() && !remotePath.endsWith('\\')) {
        remotePath += "\\";
    }
    remotePath += fileName;
    
    auto request = MessageBuilder::DownloadRequest(remotePath.toStdWString());
    transport_->send(request);
}

void FileWindow::onDownloadProgress(int percent) {
    if (progressDialog_) {
        progressDialog_->setValue(percent);
    }
    statusBar_->showMessage(QString("Downloading... %1%").arg(percent));
}

void FileWindow::onDownloadComplete(bool success) {
    if (progressDialog_) {
        progressDialog_->close();
        delete progressDialog_;
        progressDialog_ = nullptr;
    }
    
    if (success) {
        statusBar_->showMessage("Download complete");
        QMessageBox::information(this, "Success", "Download complete!");
    } else {
        statusBar_->showMessage("Download failed");
        QMessageBox::critical(this, "Error", "Download failed!");
    }
}

void FileWindow::closeEvent(QCloseEvent* event) {
    hide();
    emit closed();
    event->ignore(); // 不真正关闭，只隐藏
}