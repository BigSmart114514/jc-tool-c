#include "ssh_terminal.h"
#include "../common/ssh_session.h"
#include <Vt102Emulation.h>
#include <TerminalDisplay.h>
#include <ScreenWindow.h>
#include <QVBoxLayout>
#include <QStatusBar>

SshTerminalWindow::SshTerminalWindow(std::unique_ptr<SshSession> session, QWidget* parent)
    : QMainWindow(parent), session_(session.get()), ownedSession_(std::move(session)) {

    setWindowTitle("SSH Terminal");
    resize(900, 600);
    setMinimumSize(400, 200);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    setCentralWidget(central);

    emulation_ = new Konsole::Vt102Emulation();
    emulation_->setImageSize(24, 80);
    emulation_->setHistory(Konsole::HistoryTypeBuffer(1000));
    emulation_->setKeyBindings("default");

    terminal_ = new Konsole::TerminalDisplay(this);
    terminal_->setVTFont(QFont("Consolas", 10));
    terminal_->setScrollBarPosition(QTermWidgetInterface::ScrollBarRight);
    terminal_->setAntialias(true);
    terminal_->setKeyboardCursorShape(Konsole::Emulation::KeyboardCursorShape::BlockCursor);
    terminal_->setBlinkingCursor(true);
    terminal_->setMargin(0);

    auto* screenWindow = emulation_->createWindow();
    terminal_->setScreenWindow(screenWindow);

    layout->addWidget(terminal_);
    terminal_->setFocus();

    statusBar()->showMessage("SSH Terminal connected");

    connect(terminal_, &Konsole::TerminalDisplay::keyPressedSignal,
            emulation_, &Konsole::Emulation::sendKeyEvent);
    connect(terminal_, &Konsole::TerminalDisplay::mouseSignal,
            emulation_, &Konsole::Emulation::sendMouseEvent);
    connect(emulation_, &Konsole::Emulation::sendData,
            this, [this](const char* data, int len) {
        if (session_) session_->writeShell(data, len);
    });
    connect(emulation_, &Konsole::Emulation::titleChanged,
            this, &SshTerminalWindow::onEmulationTitleChanged);
    connect(terminal_, &Konsole::TerminalDisplay::changedContentSizeSignal,
            this, &SshTerminalWindow::onContentSizeChanged);

    if (session_) {
        session_->openShell([this](const char* data, int len) {
            QByteArray ba(data, len);
            QMetaObject::invokeMethod(this, [this, ba]() {
                emulation_->receiveData(ba.constData(), ba.size());
            }, Qt::QueuedConnection);
        });
    }

    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &SshTerminalWindow::onPollTimer);
    pollTimer_->start(50);
}

SshTerminalWindow::~SshTerminalWindow() {
    if (session_) session_->closeShell();
}

void SshTerminalWindow::onPollTimer() {
    if (session_ && session_->isConnected()) {
        session_->pollChannel(0);

        int cols = terminal_->columns();
        int rows = terminal_->lines();
        static int lastCols = 0, lastRows = 0;
        if (cols != lastCols || rows != lastRows) {
            lastCols = cols;
            lastRows = rows;
            emulation_->setImageSize(rows, cols);
            session_->setTerminalSize(cols, rows);
        }
    }
}

void SshTerminalWindow::onEmulationTitleChanged(int titleType, const QString& newTitle) {
    if (titleType == 0 || titleType == 2) {
        setWindowTitle(newTitle + " - SSH Terminal");
    }
}

void SshTerminalWindow::onContentSizeChanged(int height, int width) {
    emulation_->setImageSize(height, width);
    if (session_) {
        session_->setTerminalSize(width, height);
    }
}

void SshTerminalWindow::closeEvent(QCloseEvent* event) {
    if (pollTimer_) { pollTimer_->stop(); pollTimer_ = nullptr; }
    if (session_) { session_->closeShell(); }
    emit closed();
    event->accept();
}
