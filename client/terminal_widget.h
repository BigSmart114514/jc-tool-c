#ifndef TERMINAL_WIDGET_H
#define TERMINAL_WIDGET_H

#include <QWidget>
#include <QFont>
#include <QVector>
#include <QByteArray>
#include <QPointer>
#include <QMenu>
#include <QClipboard>
#include <QApplication>

struct CharCell {
    QChar ch = L' ';
    uint8_t fg = 7;
    uint8_t bg = 0;
    bool bold = false;
    bool underline = false;
    bool reverse = false;
};

class TerminalWidget : public QWidget {
    Q_OBJECT
public:
    TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget();

    void write(const QByteArray& data);
    void clearScreen();
    void setTerminalSize(int cols, int rows);
    int rows() const { return rows_; }
    int cols() const { return cols_; }

signals:
    void dataReady(const QByteArray& data);
    void titleChanged(const QString& title);
    void terminalClosed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    QSize minimumSizeHint() const override { return QSize(200, 100); }

private:
    void recalcCellSize();
    void scrollUp(int lines = 1);
    void scrollDown(int lines = 1);
    void newLine();
    void carriageReturn();
    void backspace();
    void tabForward();
    void eraseInDisplay(int mode);
    void eraseInLine(int mode);
    void deleteChars(int count);
    void insertChars(int count);
    void insertLines(int count);
    void deleteLines(int count);
    void setCursorPos(int col, int row);
    void moveCursorUp(int n);
    void moveCursorDown(int n);
    void moveCursorForward(int n);
    void moveCursorBack(int n);
    void setScrollRegion(int top, int bottom);
    void setMode(int mode, bool set);

    void processData(const QByteArray& data);
    void putChar(QChar ch);
    void processEscape(QChar ch);

    QFont font_;
    int charWidth_ = 0;
    int charHeight_ = 0;
    int margin_ = 4;

    int rows_ = 24;
    int cols_ = 80;
    int scrollTop_ = 0;
    int scrollBottom_ = 24;

    QVector<QVector<CharCell>> screen_;
    QVector<QVector<CharCell>> scrollback_;
    int maxScrollback_ = 1000;
    int scrollOffset_ = 0;

    int cursorX_ = 0;
    int cursorY_ = 0;
    bool cursorVisible_ = true;
    bool cursorBlink_ = false;

    uint8_t curFg_ = 7;
    uint8_t curBg_ = 0;
    bool curBold_ = false;
    bool curUnderline_ = false;
    bool curReverse_ = false;

    enum class ParseState { Text, ESC, CSI, OSC };
    ParseState parseState_ = ParseState::Text;
    QByteArray parseBuf_;
    bool selectionActive_ = false;
    int selStartRow_ = 0, selStartCol_ = 0;
    int selEndRow_ = 0, selEndCol_ = 0;

    bool savedCursorValid_ = false;
    int savedCursorX_ = 0;
    int savedCursorY_ = 0;
};

#endif
