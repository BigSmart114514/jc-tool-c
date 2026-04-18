#include "control_panel.h"
#include <QGroupBox>
#include <QFrame>
#include <iostream>
#include <QApplication>
//#include <QScreen>

ControlPanel::ControlPanel(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle("Remote Control");
    setFixedSize(350, 450);
    createUI();
}

ControlPanel::~ControlPanel() {
    std::lock_guard<std::mutex> lock(windowMtx_);
    if (desktopWindow_) {
        desktopWindow_->close();
        delete desktopWindow_;
    }
    if (fileWindow_) {
        fileWindow_->close();
        delete fileWindow_;
    }
}

void ControlPanel::createUI() {
    QWidget* central = new QWidget(this);
    setCentralWidget(central);

    QVBoxLayout* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(15, 10, 15, 10);
    mainLayout->setSpacing(10);

    // 连接信息组
    QGroupBox* connectionGroup = new QGroupBox("Connection", this);
    QVBoxLayout* connLayout = new QVBoxLayout(connectionGroup);
    
    lblMode_ = new QLabel("Mode: ", this);
    connLayout->addWidget(lblMode_);
    
    lblInfo_ = new QLabel("", this);
    lblInfo_->setWordWrap(true);
    connLayout->addWidget(lblInfo_);
    
    mainLayout->addWidget(connectionGroup);

    // 分隔线
    QFrame* line1 = new QFrame(this);
    line1->setFrameShape(QFrame::HLine);
    line1->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line1);

    // 窗口控制组
    QGroupBox* windowGroup = new QGroupBox("Windows", this);
    QVBoxLayout* windowLayout = new QVBoxLayout(windowGroup);
    
    btnDesktop_ = new QPushButton("Open Desktop", this);
    btnDesktop_->setMinimumHeight(32);
    connect(btnDesktop_, &QPushButton::clicked, this, &ControlPanel::toggleDesktop);
    windowLayout->addWidget(btnDesktop_);
    
    btnFileManager_ = new QPushButton("Open File Manager", this);
    btnFileManager_->setMinimumHeight(32);
    connect(btnFileManager_, &QPushButton::clicked, this, &ControlPanel::toggleFileManager);
    windowLayout->addWidget(btnFileManager_);
    
    mainLayout->addWidget(windowGroup);

    // 分隔线
    QFrame* line2 = new QFrame(this);
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(line2);

    // 输入控制组
    QGroupBox* inputGroup = new QGroupBox("Input Control", this);
    QVBoxLayout* inputLayout = new QVBoxLayout(inputGroup);
    
    chkMouseMove_ = new QCheckBox("Mouse Move", this);
    chkMouseMove_->setChecked(true);
    connect(chkMouseMove_, &QCheckBox::toggled, this, &ControlPanel::onMouseMoveToggled);
    inputLayout->addWidget(chkMouseMove_);
    
    chkMouseClick_ = new QCheckBox("Mouse Click", this);
    chkMouseClick_->setChecked(true);
    connect(chkMouseClick_, &QCheckBox::toggled, this, &ControlPanel::onMouseClickToggled);
    inputLayout->addWidget(chkMouseClick_);
    
    chkKeyboard_ = new QCheckBox("Keyboard", this);
    chkKeyboard_->setChecked(true);
    connect(chkKeyboard_, &QCheckBox::toggled, this, &ControlPanel::onKeyboardToggled);
    inputLayout->addWidget(chkKeyboard_);
    
    mainLayout->addWidget(inputGroup);

    mainLayout->addStretch();

    // 断开连接按钮
    btnDisconnect_ = new QPushButton("Disconnect && Exit", this);
    btnDisconnect_->setMinimumHeight(32);
    connect(btnDisconnect_, &QPushButton::clicked, this, &ControlPanel::onDisconnect);
    mainLayout->addWidget(btnDisconnect_);

    // 状态栏
    statusBar_ = new QStatusBar(this);
    setStatusBar(statusBar_);
    updateStatus("Ready");
}

void ControlPanel::setConfig(const ControlPanelConfig& config) {
    config_ = config;
    lblMode_->setText(QString("Mode: %1").arg(QString::fromStdString(config.modeText)));
    lblInfo_->setText(QString::fromStdString(config.connectInfo));
}

void ControlPanel::setupTransportCallbacks() {
    if (config_.desktopTransport) {
        TransportCallbacks cb;
        cb.onConnected = []() {
            std::cout << "[Desktop Transport] Connected" << std::endl;
        };
        cb.onDisconnected = []() {
            std::cout << "[Desktop Transport] Disconnected" << std::endl;
        };
        cb.onMessage = [this](const BinaryData& data) {
            std::lock_guard<std::mutex> lock(windowMtx_);
            if (desktopWindow_) {
                desktopWindow_->handleMessage(data);
            }
        };
        config_.desktopTransport->setCallbacks(cb);
    }

    if (config_.fileTransport) {
        TransportCallbacks cb;
        cb.onConnected = []() {
            std::cout << "[File Transport] Connected" << std::endl;
        };
        cb.onDisconnected = []() {
            std::cout << "[File Transport] Disconnected" << std::endl;
        };
        cb.onMessage = [this](const BinaryData& data) {
            std::lock_guard<std::mutex> lock(windowMtx_);
            if (fileWindow_) {
                fileWindow_->handleMessage(data);
            }
        };
        config_.fileTransport->setCallbacks(cb);
    }
}

void ControlPanel::updateStatus(const QString& text) {
    statusBar_->showMessage("Status: " + text);
}

bool ControlPanel::ensureDesktopConnected() {
    if (!config_.desktopTransport) {
        QMessageBox::critical(this, "Error", "Desktop transport not configured");
        return false;
    }
    
    if (config_.desktopTransport->isConnected()) {
        return true;
    }
    
    updateStatus("Reconnecting desktop...");
    btnDesktop_->setEnabled(false);
    
    bool result = config_.desktopTransport->reconnect();
    
    btnDesktop_->setEnabled(true);
    
    if (!result) {
        QMessageBox::critical(this, "Connection Error",
            "Failed to reconnect to remote desktop.\n"
            "Please check your network connection and try again.");
        updateStatus("Desktop connection failed");
        return false;
    }
    
    updateStatus("Desktop reconnected");
    return true;
}

bool ControlPanel::ensureFileConnected() {
    if (!config_.fileTransport) {
        QMessageBox::critical(this, "Error", "File transport not configured");
        return false;
    }
    
    if (config_.fileTransport->isConnected()) {
        return true;
    }
    
    updateStatus("Reconnecting file transport...");
    btnFileManager_->setEnabled(false);
    
    bool result = config_.fileTransport->reconnect();
    
    btnFileManager_->setEnabled(true);
    
    if (!result) {
        QMessageBox::critical(this, "Connection Error",
            "Failed to reconnect to file service.\n"
            "Please check your network connection and try again.");
        updateStatus("File connection failed");
        return false;
    }
    
    updateStatus("File transport reconnected");
    return true;
}

void ControlPanel::toggleDesktop() {
    if (desktopOpen_) {
        DesktopWindow* toDelete = nullptr;
        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            toDelete = desktopWindow_;
            desktopWindow_ = nullptr;
        }
        if (toDelete) {
            toDelete->close();
            delete toDelete;
        }
        desktopOpen_ = false;
        btnDesktop_->setText("Open Desktop");
        updateStatus("Desktop closed");
    } else {
        if (!ensureDesktopConnected()) return;

        auto* dw = new DesktopWindow();
        dw->init(config_.desktopTransport);
        dw->setInputToggles(&enableMouseMove_, &enableMouseClick_, &enableKeyboard_);

        connect(dw, &DesktopWindow::closed, this, &ControlPanel::onDesktopWindowClosed);
        connect(dw, &DesktopWindow::openFileManager, this, [this]() {
            if (!fileManagerOpen_) {
                toggleFileManager();
            }
        });

        std::string title = "Remote Desktop [" + config_.modeText + "]";
        dw->setWindowTitle(QString::fromStdString(title));
        dw->show();

        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            desktopWindow_ = dw;
        }

        desktopOpen_ = true;
        btnDesktop_->setText("Close Desktop");
        updateStatus("Desktop opened");

        dw->requestStream();
    }
}

void ControlPanel::toggleFileManager() {
    if (fileManagerOpen_) {
        FileWindow* toDelete = nullptr;
        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            toDelete = fileWindow_;
            fileWindow_ = nullptr;
        }
        if (toDelete) {
            toDelete->close();
            delete toDelete;
        }
        fileManagerOpen_ = false;
        btnFileManager_->setText("Open File Manager");
        updateStatus("File Manager closed");
    } else {
        if (!ensureFileConnected()) return;

        auto* fw = new FileWindow();
        fw->init(config_.fileTransport);

        connect(fw, &FileWindow::closed, this, &ControlPanel::onFileWindowClosed);

        fw->show();
        fw->navigateTo("");

        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            fileWindow_ = fw;
        }

        fileManagerOpen_ = true;
        btnFileManager_->setText("Close File Manager");
        updateStatus("File Manager opened");
    }
}

void ControlPanel::onDesktopWindowClosed() {
    DesktopWindow* toDelete = nullptr;
    {
        std::lock_guard<std::mutex> lock(windowMtx_);
        toDelete = desktopWindow_;
        desktopWindow_ = nullptr;
    }
    if (toDelete) {
        toDelete->deleteLater();
    }
    desktopOpen_ = false;
    btnDesktop_->setText("Open Desktop");
    updateStatus("Desktop closed");
}

void ControlPanel::onFileWindowClosed() {
    FileWindow* toDelete = nullptr;
    {
        std::lock_guard<std::mutex> lock(windowMtx_);
        toDelete = fileWindow_;
        fileWindow_ = nullptr;
    }
    if (toDelete) {
        toDelete->deleteLater();
    }
    fileManagerOpen_ = false;
    btnFileManager_->setText("Open File Manager");
    updateStatus("File Manager closed");
}

void ControlPanel::onDisconnect() {
    auto reply = QMessageBox::question(this, "Confirm",
        "Disconnect and exit?",
        QMessageBox::Yes | QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        {
            std::lock_guard<std::mutex> lock(windowMtx_);
            if (desktopWindow_) {
                desktopWindow_->close();
                delete desktopWindow_;
                desktopWindow_ = nullptr;
            }
            if (fileWindow_) {
                fileWindow_->close();
                delete fileWindow_;
                fileWindow_ = nullptr;
            }
        }

        if (config_.desktopTransport) config_.desktopTransport->disconnect();
        if (config_.fileTransport) config_.fileTransport->disconnect();

        close();
        qApp->quit();
    }
}

void ControlPanel::onMouseMoveToggled(bool checked) {
    enableMouseMove_ = checked;
}

void ControlPanel::onMouseClickToggled(bool checked) {
    enableMouseClick_ = checked;
}

void ControlPanel::onKeyboardToggled(bool checked) {
    enableKeyboard_ = checked;
}