#ifndef SSH_SERVER_H
#define SSH_SERVER_H

#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <libssh/libssh.h>
#include <libssh/server.h>

class SshServer {
public:
    SshServer();
    ~SshServer();

    bool start(int port, const std::string& password);
    void stop();
    bool isRunning() const { return running_; }
    int getPort() const { return port_; }

private:
    void acceptLoop();
    void handleSession(ssh_session session);
    void handleSftpChannel(ssh_session session, ssh_channel channel);
    void handleShellChannel(ssh_session session, ssh_channel channel,
                            int cols, int rows);

    static ssh_bind createBind(int port);
    bool setupHostKey(ssh_bind bind);
    void cleanupKeyFile();
    static int authCallback(ssh_session session, const char* user,
                            const char* password, void* userdata);

    ssh_bind sshbind_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::string password_;
    std::string keyFile_;
    int port_ = 2222;
};

#endif
