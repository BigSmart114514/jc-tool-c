#include "ssh_terminal.h"
#include "../common/ssh_session.h"
#include <Vt102Emulation.h>
#include <TerminalDisplay.h>
#include <ScreenWindow.h>
#include <CharacterColor.h>
#include <QVBoxLayout>
#include <QStatusBar>

// Windows Terminal Campbell color scheme
static const Konsole::ColorEntry campbellColorTable[TABLE_COLORS] = {
    // Normal: Default fore, Default back
    Konsole::ColorEntry(QColor(0xCC,0xCC,0xCC), false),
    Konsole::ColorEntry(QColor(0x0C,0x0C,0x0C), true),
    // ANSI 0-7: Black, Red, Green, Yellow, Blue, Magenta, Cyan, White
    Konsole::ColorEntry(QColor(0x0C,0x0C,0x0C), false),
    Konsole::ColorEntry(QColor(0xC5,0x0F,0x1F), false),
    Konsole::ColorEntry(QColor(0x13,0xA1,0x0E), false),
    Konsole::ColorEntry(QColor(0xC1,0x9C,0x00), false),
    Konsole::ColorEntry(QColor(0x00,0x37,0xDA), false),
    Konsole::ColorEntry(QColor(0x88,0x17,0x98), false),
    Konsole::ColorEntry(QColor(0x3A,0x96,0xDD), false),
    Konsole::ColorEntry(QColor(0xCC,0xCC,0xCC), false),
    // Intense: Default fore, Default back
    Konsole::ColorEntry(QColor(0xF2,0xF2,0xF2), false),
    Konsole::ColorEntry(QColor(0x0C,0x0C,0x0C), true),
    // ANSI 8-15: Bright Black, Red, Green, Yellow, Blue, Magenta, Cyan, White
    Konsole::ColorEntry(QColor(0x76,0x76,0x76), false),
    Konsole::ColorEntry(QColor(0xE7,0x48,0x56), false),
    Konsole::ColorEntry(QColor(0x16,0xC6,0x0C), false),
    Konsole::ColorEntry(QColor(0xF9,0xF1,0xA5), false),
    Konsole::ColorEntry(QColor(0x3B,0x78,0xFF), false),
    Konsole::ColorEntry(QColor(0xB4,0x00,0x9E), false),
    Konsole::ColorEntry(QColor(0x61,0xD6,0xD6), false),
    Konsole::ColorEntry(QColor(0xF2,0xF2,0xF2), false),
};

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
    terminal_->setVTFont(QFont("Consolas", 11));
    terminal_->setScrollBarPosition(QTermWidgetInterface::ScrollBarRight);
    terminal_->setAntialias(true);
    terminal_->setKeyboardCursorShape(Konsole::Emulation::KeyboardCursorShape::BlockCursor);
    terminal_->setBlinkingCursor(true);
    terminal_->setMargin(0);
    terminal_->setColorTable(campbellColorTable);

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
    // Forward mouse to remote when TUI app requests it (DECSET 1000/1002/1006)
    // Note: qtermwidget's setUsesMouse(true) means "local selection mode",
    // so we INVERT: when program requests mouse → forwarding mode (setUsesMouse(false))
    connect(emulation_, &Konsole::Emulation::programUsesMouseChanged,
            this, [this](bool usesMouse) {
        terminal_->setUsesMouse(!usesMouse);
    });
    terminal_->setUsesMouse(!emulation_->programUsesMouse());

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

    // Sync initial PTY size after widget is laid out
    QTimer::singleShot(0, this, [this]() {
        int cols = terminal_->columns();
        int rows = terminal_->lines();
        if (cols > 0 && rows > 0) {
            emulation_->setImageSize(rows, cols);
            if (session_) session_->setTerminalSize(cols, rows);
        }
    });
}

SshTerminalWindow::~SshTerminalWindow() {
    if (session_) session_->closeShell();
}

void SshTerminalWindow::onPollTimer() {
    if (session_ && session_->isConnected()) {
        session_->pollChannel(0);

        // Safety net: correct size if emulation and widget are out of sync
        int cols = terminal_->columns();
        int rows = terminal_->lines();
        if (cols > 0 && rows > 0) {
            QSize cur = emulation_->imageSize();
            if (cur.width() != cols || cur.height() != rows) {
                emulation_->setImageSize(rows, cols);
                session_->setTerminalSize(cols, rows);
            }
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
