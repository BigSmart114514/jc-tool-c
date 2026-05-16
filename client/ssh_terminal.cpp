#include "ssh_terminal.h"
#include "terminal_widget.h"
#include "../common/ssh_session.h"
#include <QVBoxLayout>
#include <QStatusBar>
#include <QLabel>

SshTerminalWindow::SshTerminalWindow(std::unique_ptr<SshSession> session, QWidget* parent)
    : QMainWindow(parent), session_(session.get()), ownedSession_(std::move(session)) {

    setWindowTitle("SSH Terminal");
    resize(900, 600);
    setMinimumSize(400, 200);

    terminal_ = new TerminalWidget(this);
    setCentralWidget(terminal_);
    terminal_->setFocus();

    statusBar()->showMessage("SSH Terminal connected");

    connect(terminal_, &TerminalWidget::dataReady, this, &SshTerminalWindow::onDataReady);
    connect(terminal_, &TerminalWidget::titleChanged, this, [this](const QString& title) {
        setWindowTitle(title + " - SSH Terminal");
    });

    // Open SSH shell
    if (session_) {
        session_->openShell([this](const char* data, int len) {
            QByteArray ba(data, len);
            QMetaObject::invokeMethod(this, [this, ba]() {
                terminal_->write(ba);
            }, Qt::QueuedConnection);
        });
    }

    // Poll timer for reading channel data
    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &SshTerminalWindow::onPollTimer);
    pollTimer_->start(50);
}

SshTerminalWindow::~SshTerminalWindow() {
    // ownedSession_ unique_ptr frees SshSession automatically
}

void SshTerminalWindow::onDataReady(const QByteArray& data) {
    if (session_) {
        session_->writeShell(data.constData(), data.size());
    }
}

void SshTerminalWindow::onPollTimer() {
    if (session_ && session_->isConnected()) {
        session_->pollChannel(0);

        int cols = terminal_->cols();
        int rows = terminal_->rows();
        static int lastCols = 0, lastRows = 0;
        if (cols != lastCols || rows != lastRows) {
            lastCols = cols;
            lastRows = rows;
            session_->setTerminalSize(cols, rows);
        }
    }
}

void SshTerminalWindow::closeEvent(QCloseEvent* event) {
    if (pollTimer_) { pollTimer_->stop(); pollTimer_ = nullptr; }
    if (session_) { session_->closeShell(); }
    emit closed();
    event->accept();
}
