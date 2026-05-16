#include "terminal_widget.h"
#include <QPainter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStyle>
#include <cmath>

TerminalWidget::TerminalWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    font_ = QFont("Consolas", 12);
    font_.setStyleHint(QFont::Monospace);
    recalcCellSize();

    screen_.resize(rows_);
    for (int r = 0; r < rows_; r++) {
        screen_[r].resize(cols_);
    }
    scrollBottom_ = rows_ - 1;
}

TerminalWidget::~TerminalWidget() {}

void TerminalWidget::recalcCellSize() {
    QFontMetrics fm(font_);
    charWidth_ = fm.averageCharWidth();
    if (charWidth_ < 1) charWidth_ = fm.maxWidth();
    charHeight_ = fm.height();
}

void TerminalWidget::write(const QByteArray& data) {
    processData(data);
    update();
}

void TerminalWidget::clearScreen() {
    for (int r = 0; r < rows_; r++) {
        for (int c = 0; c < cols_; c++) {
            screen_[r][c] = CharCell();
        }
    }
    cursorX_ = 0;
    cursorY_ = 0;
    update();
}

void TerminalWidget::scrollUp(int lines) {
    for (int i = 0; i < lines; i++) {
        if (scrollback_.size() < maxScrollback_) {
            scrollback_.append(screen_[scrollTop_]);
        }
        for (int r = scrollTop_; r < scrollBottom_; r++) {
            screen_[r] = screen_[r + 1];
        }
        screen_[scrollBottom_].fill(CharCell());
    }
}

void TerminalWidget::scrollDown(int lines) {
    for (int i = 0; i < lines; i++) {
        for (int r = scrollBottom_; r > scrollTop_; r--) {
            screen_[r] = screen_[r - 1];
        }
        screen_[scrollTop_].fill(CharCell());
    }
}

void TerminalWidget::newLine() {
    cursorX_ = 0;
    if (cursorY_ >= scrollBottom_) {
        scrollUp();
    } else {
        cursorY_++;
    }
}

void TerminalWidget::carriageReturn() {
    cursorX_ = 0;
}

void TerminalWidget::backspace() {
    if (cursorX_ > 0) cursorX_--;
}

void TerminalWidget::tabForward() {
    int tabStop = 8;
    cursorX_ = ((cursorX_ / tabStop) + 1) * tabStop;
    if (cursorX_ >= cols_) cursorX_ = cols_ - 1;
}

void TerminalWidget::moveCursorUp(int n) {
    cursorY_ -= n;
    if (cursorY_ < scrollTop_) cursorY_ = scrollTop_;
}

void TerminalWidget::moveCursorDown(int n) {
    cursorY_ += n;
    if (cursorY_ > scrollBottom_) cursorY_ = scrollBottom_;
}

void TerminalWidget::moveCursorForward(int n) {
    cursorX_ += n;
    if (cursorX_ >= cols_) cursorX_ = cols_ - 1;
}

void TerminalWidget::moveCursorBack(int n) {
    cursorX_ -= n;
    if (cursorX_ < 0) cursorX_ = 0;
}

void TerminalWidget::setCursorPos(int col, int row) {
    cursorX_ = qBound(0, col, cols_ - 1);
    cursorY_ = qBound(scrollTop_, row, scrollBottom_);
}

void TerminalWidget::setScrollRegion(int top, int bottom) {
    scrollTop_ = qBound(0, top, rows_ - 1);
    scrollBottom_ = qBound(0, bottom, rows_ - 1);
    cursorX_ = 0;
    cursorY_ = scrollTop_;
}

void TerminalWidget::eraseInDisplay(int mode) {
    switch (mode) {
    case 0:
        for (int r = cursorY_; r <= scrollBottom_; r++) {
            int start = (r == cursorY_) ? cursorX_ : 0;
            for (int c = start; c < cols_; c++)
                screen_[r][c] = CharCell{};
        }
        break;
    case 1:
        for (int r = scrollTop_; r <= cursorY_; r++) {
            int end = (r == cursorY_) ? cursorX_ : cols_;
            for (int c = 0; c < end; c++)
                screen_[r][c] = CharCell{};
        }
        break;
    case 2:
        for (int r = scrollTop_; r <= scrollBottom_; r++)
            for (int c = 0; c < cols_; c++)
                screen_[r][c] = CharCell{};
        break;
    }
}

void TerminalWidget::eraseInLine(int mode) {
    switch (mode) {
    case 0:
        for (int c = cursorX_; c < cols_; c++)
            screen_[cursorY_][c] = CharCell{};
        break;
    case 1:
        for (int c = 0; c <= cursorX_; c++)
            screen_[cursorY_][c] = CharCell{};
        break;
    case 2:
        for (int c = 0; c < cols_; c++)
            screen_[cursorY_][c] = CharCell{};
        break;
    }
}

void TerminalWidget::deleteChars(int count) {
    auto& row = screen_[cursorY_];
    int n = qMin(count, cols_ - cursorX_);
    for (int c = cursorX_; c < cols_ - n; c++)
        row[c] = row[c + n];
    for (int c = cols_ - n; c < cols_; c++)
        row[c] = CharCell{};
}

void TerminalWidget::insertChars(int count) {
    auto& row = screen_[cursorY_];
    int n = qMin(count, cols_ - cursorX_);
    for (int c = cols_ - 1; c >= cursorX_ + n; c--)
        row[c] = row[c - n];
    for (int c = cursorX_; c < cursorX_ + n; c++)
        row[c] = CharCell{};
}

void TerminalWidget::insertLines(int count) {
    int n = qMin(count, scrollBottom_ - cursorY_ + 1);
    for (int r = scrollBottom_; r >= cursorY_ + n; r--)
        screen_[r] = screen_[r - n];
    for (int r = cursorY_; r < cursorY_ + n; r++)
        screen_[r].fill(CharCell{});
}

void TerminalWidget::deleteLines(int count) {
    int n = qMin(count, scrollBottom_ - cursorY_ + 1);
    for (int r = cursorY_; r <= scrollBottom_ - n; r++)
        screen_[r] = screen_[r + n];
    for (int r = scrollBottom_ - n + 1; r <= scrollBottom_; r++)
        screen_[r].fill(CharCell{});
}

void TerminalWidget::setMode(int mode, bool set) {
    switch (mode) {
    case 25:
        cursorVisible_ = set;
        break;
    }
}

void TerminalWidget::setTerminalSize(int cols, int rows) {
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;

    QVector<QVector<CharCell>> newScreen(rows, QVector<CharCell>(cols));

    for (int r = 0; r < qMin(rows, rows_); r++)
        for (int c = 0; c < qMin(cols, cols_); c++)
            newScreen[r][c] = screen_[r][c];

    screen_ = newScreen;
    cols_ = cols;
    rows_ = rows;
    scrollBottom_ = rows_ - 1;
    cursorX_ = qMin(cursorX_, cols_ - 1);
    cursorY_ = qMin(cursorY_, rows_ - 1);
    update();
}

void TerminalWidget::putChar(QChar ch) {
    ushort uc = ch.unicode();

    if (uc == 0x07) {
    } else if (uc == 0x08) {
        backspace();
    } else if (uc == 0x09) {
        tabForward();
    } else if (uc == 0x0A || uc == 0x0B || uc == 0x0C) {
        newLine();
    } else if (uc == 0x0D) {
        carriageReturn();
    } else if (uc >= 0x20) {
        if (cursorX_ >= cols_) {
            cursorX_ = 0;
            if (cursorY_ >= scrollBottom_) scrollUp();
            else cursorY_++;
        }
        auto& cell = screen_[cursorY_][cursorX_];
        cell.ch = ch;
        cell.fg = curFg_;
        cell.bg = curBg_;
        cell.bold = curBold_;
        cell.underline = curUnderline_;
        cell.reverse = curReverse_;
        cursorX_++;
    }
}

void TerminalWidget::processEscape(QChar ch) {
    char c = ch.toLatin1();   // ASCII-only in escape sequences
    ushort uc = ch.unicode(); // for non-ASCII printable chars
    switch (parseState_) {
    case ParseState::Text:
        if (c == '\x1b') {
            parseState_ = ParseState::ESC;
            parseBuf_.clear();
        } else {
            putChar(ch);
        }
        break;

    case ParseState::ESC:
        if (c == '[') {
            parseState_ = ParseState::CSI;
            parseBuf_.clear();
        } else if (c == ']') {
            parseState_ = ParseState::OSC;
            parseBuf_.clear();
        } else if (c == '7') {
            savedCursorX_ = cursorX_; savedCursorY_ = cursorY_; savedCursorValid_ = true;
            parseState_ = ParseState::Text;
        } else if (c == '8') {
            if (savedCursorValid_) { cursorX_ = savedCursorX_; cursorY_ = savedCursorY_; }
            parseState_ = ParseState::Text;
        } else if (c == 'D') {
            if (cursorY_ >= scrollBottom_) scrollUp(); else cursorY_++;
            parseState_ = ParseState::Text;
        } else if (c == 'M') {
            if (cursorY_ <= scrollTop_) scrollDown(); else cursorY_--;
            parseState_ = ParseState::Text;
        } else if (c == 'c') {
            clearScreen();
            parseState_ = ParseState::Text;
        } else {
            parseState_ = ParseState::Text;
        }
        break;

    case ParseState::CSI: {
        if ((c >= '0' && c <= '9') || c == ';' || c == '?' || c == '>') {
            parseBuf_.append(c);
        } else if (c >= 0x20 && c <= 0x2F) {
            parseBuf_.append(c);
        } else if (c >= 0x40 && c <= 0x7E) {
            QList<QByteArray> params;
            if (c == 'h' || c == 'l') {
                bool set = (c == 'h');
                if (parseBuf_.startsWith('?')) {
                    int mode = parseBuf_.mid(1).toInt();
                    setMode(mode, set);
                }
            } else if (c == 'm') {
                if (parseBuf_.isEmpty()) {
                    curFg_ = 7; curBg_ = 0;
                    curBold_ = false; curUnderline_ = false; curReverse_ = false;
                } else {
                    for (auto& p : parseBuf_.split(';')) {
                        int n = p.toInt();
                        if (n == 0) {
                            curFg_ = 7; curBg_ = 0;
                            curBold_ = false; curUnderline_ = false; curReverse_ = false;
                        } else if (n == 1) curBold_ = true;
                        else if (n == 4) curUnderline_ = true;
                        else if (n == 7) curReverse_ = true;
                        else if (n == 22) curBold_ = false;
                        else if (n == 24) curUnderline_ = false;
                        else if (n == 27) curReverse_ = false;
                        else if (n >= 30 && n <= 37) curFg_ = n - 30;
                        else if (n == 38) { /* extended fg - skip */ }
                        else if (n == 39) curFg_ = 7;
                        else if (n >= 40 && n <= 47) curBg_ = n - 40;
                        else if (n == 49) curBg_ = 0;
                        else if (n >= 90 && n <= 97) curFg_ = n - 90 + 8;
                        else if (n >= 100 && n <= 107) curBg_ = n - 100 + 8;
                    }
                }
            } else if (c == 'A') moveCursorUp(parseBuf_.toInt());
            else if (c == 'B') moveCursorDown(parseBuf_.toInt());
            else if (c == 'C') moveCursorForward(parseBuf_.toInt());
            else if (c == 'D') moveCursorBack(parseBuf_.toInt());
            else if (c == 'H' || c == 'f') {
                auto p = parseBuf_.split(';');
                int row = (p.size() > 0) ? p[0].toInt() - 1 : 0;
                int col = (p.size() > 1) ? p[1].toInt() - 1 : 0;
                setCursorPos(col, row);
            } else if (c == 'J') eraseInDisplay(parseBuf_.toInt());
            else if (c == 'K') eraseInLine(parseBuf_.toInt());
            else if (c == 'L') insertLines(parseBuf_.toInt());
            else if (c == 'M') deleteLines(parseBuf_.toInt());
            else if (c == 'P') deleteChars(parseBuf_.toInt());
            else if (c == '@') insertChars(parseBuf_.toInt());
            else if (c == 's') {
                savedCursorX_ = cursorX_; savedCursorY_ = cursorY_; savedCursorValid_ = true;
            } else if (c == 'u') {
                if (savedCursorValid_) { cursorX_ = savedCursorX_; cursorY_ = savedCursorY_; }
            } else if (c == 'r') {
                auto p = parseBuf_.split(';');
                int top = p.size() > 0 && !p[0].isEmpty() ? p[0].toInt() - 1 : 0;
                int bot = p.size() > 1 && !p[1].isEmpty() ? p[1].toInt() - 1 : rows_ - 1;
                setScrollRegion(top, bot);
            }
            parseState_ = ParseState::Text;
        } else {
            parseState_ = ParseState::Text;
        }
        break;
    }

    case ParseState::OSC:
        if (c == '\x07' || c == '\x1b') {
            auto parts = parseBuf_.split(';');
            if (parts.size() >= 2) {
                int oscNum = parts[0].toInt();
                if (oscNum == 0 || oscNum == 2) {
                    emit titleChanged(QString::fromUtf8(parts.mid(1).join(';')));
                }
            }
            parseState_ = ParseState::Text;
            if (c == '\x1b') {
                parseState_ = ParseState::ESC;
                parseBuf_.clear();
            }
        } else {
            parseBuf_.append(c);
        }
        break;
    }
}

void TerminalWidget::processData(const QByteArray& data) {
    QString text = QString::fromUtf8(data);
    for (const QChar& ch : text) {
        processEscape(ch);
    }
}

void TerminalWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setFont(font_);
    p.setRenderHint(QPainter::TextAntialiasing);

    QColor fg, bg;

    static const QColor palette[16] = {
        QColor("#1e1e1e"), QColor("#cd3131"), QColor("#0dbc79"), QColor("#e5e510"),
        QColor("#2472c8"), QColor("#bc3fbc"), QColor("#11a8cd"), QColor("#e5e5e5"),
        QColor("#666666"), QColor("#f14c4c"), QColor("#23d18b"), QColor("#f5f543"),
        QColor("#3b8eea"), QColor("#d670d6"), QColor("#29b8db"), QColor("#e5e5e5")
    };
    QColor defaultBg("#000000");
    QColor defaultFg("#e0e0e0");

    int startRow = 0;
    int endRow = rows_;

    QRect r = rect();
    p.fillRect(r, defaultBg);

    for (int row = startRow; row < endRow; row++) {
        int y = margin_ + row * charHeight_;
        for (int col = 0; col < cols_; col++) {
            int x = margin_ + col * charWidth_;
            const auto& cell = screen_[row][col];

            uint8_t fgIdx = cell.reverse ? cell.bg : cell.fg;
            uint8_t bgIdx = cell.reverse ? cell.fg : cell.bg;

            if (cell.bold && fgIdx < 8) fgIdx += 8;

            bool isDefaultBg = (cell.bg == 0 && !cell.reverse);
            bg = isDefaultBg ? defaultBg : palette[bgIdx % 16];

            bool isDefaultFg = (cell.fg == 7 && !cell.reverse);
            fg = isDefaultFg ? defaultFg : palette[fgIdx % 16];

            if (cell.ch != ' ') {
                if (bg != defaultBg) {
                    p.fillRect(x, y, charWidth_, charHeight_, bg);
                }
                p.setPen(fg);
                p.drawText(x, y, charWidth_, charHeight_, Qt::AlignCenter, cell.ch);
            }
        }
    }

    // Draw cursor
    if (cursorVisible_ && hasFocus()) {
        int cx = margin_ + cursorX_ * charWidth_;
        int cy = margin_ + cursorY_ * charHeight_;
        p.fillRect(cx, cy, charWidth_, charHeight_, QColor("#e0e0e0"));
        if (cursorX_ < cols_) {
            const auto& cell = screen_[cursorY_][cursorX_];
            p.setPen(QColor("#000000"));
            p.drawText(cx, cy, charWidth_, charHeight_, Qt::AlignCenter, cell.ch);
        }
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent* event) {
    QByteArray result;

    if (event->matches(QKeySequence::Copy)) {
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        QString text = QApplication::clipboard()->text();
        if (!text.isEmpty()) {
            emit dataReady(text.toUtf8());
        }
        return;
    }

    int key = event->key();
    Qt::KeyboardModifiers mod = event->modifiers();
    QString text = event->text();

    if (mod & Qt::ControlModifier && key >= Qt::Key_A && key <= Qt::Key_Z) {
        result.append(static_cast<char>(key - Qt::Key_A + 1));
    } else {
        switch (key) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
            result.append("\r");
            break;
        case Qt::Key_Backspace:
            result.append("\x7f");
            break;
        case Qt::Key_Escape:
            result.append("\x1b");
            break;
        case Qt::Key_Tab:
            result.append("\t");
            break;
        case Qt::Key_Up:    result.append("\x1b[A"); break;
        case Qt::Key_Down:  result.append("\x1b[B"); break;
        case Qt::Key_Right: result.append("\x1b[C"); break;
        case Qt::Key_Left:  result.append("\x1b[D"); break;
        case Qt::Key_Home:  result.append("\x1b[H"); break;
        case Qt::Key_End:   result.append("\x1b[F"); break;
        case Qt::Key_PageUp:   result.append("\x1b[5~"); break;
        case Qt::Key_PageDown: result.append("\x1b[6~"); break;
        case Qt::Key_Insert:   result.append("\x1b[2~"); break;
        case Qt::Key_Delete:
            if (mod & Qt::ControlModifier) result.append("\x1b[3;5~");
            else result.append("\x1b[3~");
            break;
        case Qt::Key_F1:  result.append("\x1bOP"); break;
        case Qt::Key_F2:  result.append("\x1bOQ"); break;
        case Qt::Key_F3:  result.append("\x1bOR"); break;
        case Qt::Key_F4:  result.append("\x1bOS"); break;
        case Qt::Key_F5:  result.append("\x1b[15~"); break;
        case Qt::Key_F6:  result.append("\x1b[17~"); break;
        case Qt::Key_F7:  result.append("\x1b[18~"); break;
        case Qt::Key_F8:  result.append("\x1b[19~"); break;
        case Qt::Key_F9:  result.append("\x1b[20~"); break;
        case Qt::Key_F10: result.append("\x1b[21~"); break;
        case Qt::Key_F11: result.append("\x1b[23~"); break;
        case Qt::Key_F12: result.append("\x1b[24~"); break;
        default:
            if (!text.isEmpty())
                result.append(text.toUtf8());
            break;
        }
    }

    if (!result.isEmpty())
        emit dataReady(result);
}

void TerminalWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    recalcCellSize();
    int newCols = (width() - 2 * margin_) / charWidth_;
    int newRows = (height() - 2 * margin_) / charHeight_;
    if (newCols < 1) newCols = 1;
    if (newRows < 1) newRows = 1;
    if (newCols != cols_ || newRows != rows_) {
        setTerminalSize(newCols, newRows);
    }
}

void TerminalWidget::mousePressEvent(QMouseEvent* event) {
    setFocus();
    if (event->button() == Qt::LeftButton) {
        int col = (event->x() - margin_) / charWidth_;
        int row = (event->y() - margin_) / charHeight_;
        if (col >= 0 && col < cols_ && row >= 0 && row < rows_) {
            selectionActive_ = true;
            selStartRow_ = selEndRow_ = row;
            selStartCol_ = selEndCol_ = col;
        }
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && selectionActive_) {
        selectionActive_ = false;
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent* event) {
    if (selectionActive_ && (event->buttons() & Qt::LeftButton)) {
        int col = (event->x() - margin_) / charWidth_;
        int row = (event->y() - margin_) / charHeight_;
        selEndRow_ = qBound(0, row, rows_ - 1);
        selEndCol_ = qBound(0, col, cols_ - 1);
        update();
    }
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    menu.addAction("Copy", [this]() {
        QString text;
        int r1 = qMin(selStartRow_, selEndRow_);
        int r2 = qMax(selStartRow_, selEndRow_);
        int c1 = (selStartRow_ < selEndRow_) ? selStartCol_ :
                 (selStartRow_ > selEndRow_) ? selEndCol_ : qMin(selStartCol_, selEndCol_);
        int c2 = (selStartRow_ > selEndRow_) ? selStartCol_ :
                 (selStartRow_ < selEndRow_) ? selEndCol_ : qMax(selStartCol_, selEndCol_);
        for (int r = r1; r <= r2; r++) {
            for (int c = c1; c <= (r == r2 ? c2 : cols_ - 1); c++) {
                text += screen_[r][c].ch;
            }
            if (r != r2) text += '\n';
        }
        QApplication::clipboard()->setText(text);
    });
    menu.addAction("Paste", [this]() {
        QString text = QApplication::clipboard()->text();
        if (!text.isEmpty()) emit dataReady(text.toUtf8());
    });
    menu.addSeparator();
    menu.addAction("Clear", this, &TerminalWidget::clearScreen);
    menu.exec(event->globalPos());
}
