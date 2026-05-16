#ifndef SSH_SESSION_H
#define SSH_SESSION_H

#include <string>
#include <vector>
#include <functional>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

struct SFtpEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    uint64_t modifyTime = 0;
};

class SshSession {
public:
    SshSession();
    ~SshSession();

    SshSession(const SshSession&) = delete;
    SshSession& operator=(const SshSession&) = delete;

    bool connect(const std::string& host, int port,
                 const std::string& username, const std::string& password);
    void disconnect();
    bool isConnected() const { return connected_; }
    std::string getError() const { return error_; }

    // SFTP
    bool sftpInit();
    bool sftpListDir(const std::string& path, std::vector<SFtpEntry>& entries);
    bool sftpDownload(const std::string& remotePath, const std::string& localPath,
                      std::function<bool(int64_t, int64_t)> progress);
    bool sftpUpload(const std::string& localPath, const std::string& remotePath,
                    std::function<bool(int64_t, int64_t)> progress);
    bool sftpDelete(const std::string& path);
    bool sftpMkdir(const std::string& path);
    bool sftpRename(const std::string& oldPath, const std::string& newPath);
    bool sftpFileExists(const std::string& path);

    // Shell (for SSH terminal)
    using ShellDataCallback = std::function<void(const char*, int)>;
    bool openShell(ShellDataCallback onData);
    void writeShell(const char* data, int len);
    void closeShell();
    void setTerminalSize(int cols, int rows);
    void pollChannel(int timeoutMs);

private:
    ssh_session session_ = nullptr;
    sftp_session sftp_ = nullptr;
    ssh_channel shellChannel_ = nullptr;
    bool connected_ = false;
    bool sftpInitialized_ = false;
    std::string error_;
    ShellDataCallback shellOnData_;
};

#endif
