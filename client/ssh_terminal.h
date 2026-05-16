#ifndef SSH_TERMINAL_H
#define SSH_TERMINAL_H

#include <QMainWindow>
#include <QMainWindow>
#include <QTimer>
#include <QCloseEvent>
#include <memory>

class SshSession;
class TerminalWidget;

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
    void onDataReady(const QByteArray& data);
    void onPollTimer();

private:
    SshSession* session_ = nullptr;
    std::unique_ptr<SshSession> ownedSession_;
    TerminalWidget* terminal_ = nullptr;
    QTimer* pollTimer_ = nullptr;
};

#endif
