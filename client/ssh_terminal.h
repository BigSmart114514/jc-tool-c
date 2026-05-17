#ifndef SSH_TERMINAL_H
#define SSH_TERMINAL_H

#include <QMainWindow>
#include <QTimer>
#include <QCloseEvent>
#include <memory>

namespace Konsole {
class Vt102Emulation;
class TerminalDisplay;
}

class SshSession;

class SshTerminalWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit SshTerminalWindow(std::unique_ptr<SshSession> session, QWidget* parent = nullptr);
    ~SshTerminalWindow();

signals:
    void closed();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onPollTimer();
    void onEmulationTitleChanged(int titleType, const QString& newTitle);
    void onContentSizeChanged(int height, int width);

private:
    SshSession* session_ = nullptr;
    std::unique_ptr<SshSession> ownedSession_;
    Konsole::Vt102Emulation* emulation_ = nullptr;
    Konsole::TerminalDisplay* terminal_ = nullptr;
    QTimer* pollTimer_ = nullptr;
};

#endif
