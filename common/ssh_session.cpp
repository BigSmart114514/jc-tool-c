#include "ssh_session.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <cerrno>
#include <windows.h>

static std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);
    return wstr;
}

SshSession::SshSession() {}

SshSession::~SshSession() {
    disconnect();
}

bool SshSession::connect(const std::string& host, int port,
                         const std::string& username, const std::string& password) {
    if (connected_) disconnect();

    session_ = ssh_new();
    if (!session_) {
        error_ = "ssh_new() failed";
        return false;
    }

    ssh_options_set(session_, SSH_OPTIONS_HOST, host.c_str());
    long p = port;
    ssh_options_set(session_, SSH_OPTIONS_PORT, &p);
    ssh_options_set(session_, SSH_OPTIONS_USER, username.c_str());
    long timeoutSec = 10;
    ssh_options_set(session_, SSH_OPTIONS_TIMEOUT, &timeoutSec);

    int rc = ssh_connect(session_);
    if (rc != SSH_OK) {
        error_ = "ssh_connect failed: " + std::string(ssh_get_error(session_));
        ssh_free(session_);
        session_ = nullptr;
        return false;
    }

    rc = ssh_userauth_password(session_, nullptr, password.c_str());
    if (rc != SSH_AUTH_SUCCESS) {
        error_ = "Authentication failed: " + std::string(ssh_get_error(session_));
        ssh_disconnect(session_);
        ssh_free(session_);
        session_ = nullptr;
        return false;
    }

    connected_ = true;
    return true;
}

void SshSession::disconnect() {
    closeShell();

    if (sftp_) {
        sftp_free(sftp_);
        sftp_ = nullptr;
        sftpInitialized_ = false;
    }

    if (session_) {
        if (connected_) {
            ssh_disconnect(session_);
        }
        ssh_free(session_);
        session_ = nullptr;
    }

    connected_ = false;
}

bool SshSession::sftpInit() {
    if (sftpInitialized_) return true;
    if (!connected_ || !session_) return false;

    sftp_ = sftp_new(session_);
    if (!sftp_) {
        error_ = "sftp_new failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    int rc = sftp_init(sftp_);
    if (rc != SSH_OK) {
        error_ = "sftp_init failed: " + std::string(ssh_get_error(session_));
        sftp_free(sftp_);
        sftp_ = nullptr;
        return false;
    }

    sftpInitialized_ = true;
    return true;
}

bool SshSession::sftpListDir(const std::string& path, std::vector<SFtpEntry>& entries) {
    entries.clear();
    if (!sftpInit()) return false;

    sftp_dir dir = sftp_opendir(sftp_, path.c_str());
    if (!dir) {
        error_ = "sftp_opendir failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    while (auto file = sftp_readdir(sftp_, dir)) {
        if (strcmp(file->name, ".") == 0 || strcmp(file->name, "..") == 0) {
            sftp_attributes_free(file);
            continue;
        }

        SFtpEntry entry;
        entry.name = file->name;
        entry.isDir = (file->type == SSH_FILEXFER_TYPE_DIRECTORY);
        entry.size = file->size;
        entry.modifyTime = file->mtime;
        entries.push_back(entry);
        sftp_attributes_free(file);
    }

    if (!sftp_dir_eof(dir)) {
        error_ = "sftp_readdir error: " + std::string(ssh_get_error(session_));
        sftp_closedir(dir);
        return false;
    }

    sftp_closedir(dir);
    return true;
}

bool SshSession::sftpDownload(const std::string& remotePath, const std::string& localPath,
                              std::function<bool(int64_t, int64_t)> progress) {
    if (!sftpInit()) return false;

    sftp_file file = sftp_open(sftp_, remotePath.c_str(), O_RDONLY, (mode_t)0);
    if (!file) {
        error_ = "sftp_open (read) failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    sftp_attributes attr = sftp_stat(sftp_, remotePath.c_str());
    int64_t totalSize = attr ? attr->size : 0;
    if (attr) sftp_attributes_free(attr);

    std::wstring wlocal = widen(localPath);
    FILE* local = _wfopen(wlocal.c_str(), L"wb");
    if (!local) {
        error_ = "Cannot open local file for writing: " + localPath + " (errno=" + std::to_string(errno) + ")";
        sftp_close(file);
        return false;
    }

    std::vector<char> buffer(65536);
    int64_t received = 0;
    bool ok = true;

    while (true) {
        int n = sftp_read(file, buffer.data(), buffer.size());
        if (n == SSH_ERROR) {
            error_ = "sftp_read error: " + std::string(ssh_get_error(session_));
            ok = false;
            break;
        }
        if (n == 0) break;

        fwrite(buffer.data(), 1, n, local);
        received += n;

        if (progress && !progress(received, totalSize)) {
            ok = false;
            break;
        }
    }

    fclose(local);
    sftp_close(file);

    if (!ok) {
        remove(localPath.c_str());
        return false;
    }

    return true;
}

bool SshSession::sftpUpload(const std::string& localPath, const std::string& remotePath,
                            std::function<bool(int64_t, int64_t)> progress) {
    if (!sftpInit()) return false;

    FILE* local = _wfopen(widen(localPath).c_str(), L"rb");
    if (!local) {
        error_ = "Cannot open local file for reading: " + localPath + " (errno=" + std::to_string(errno) + ")";
        return false;
    }

    sftp_file file = sftp_open(sftp_, remotePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, (mode_t)0644);
    if (!file) {
        error_ = "sftp_open (write) failed: " + std::string(ssh_get_error(session_));
        fclose(local);
        return false;
    }

    fseek(local, 0, SEEK_END);
    int64_t totalSize = ftell(local);
    fseek(local, 0, SEEK_SET);

    std::vector<char> buffer(65536);
    int64_t sent = 0;
    bool ok = true;

    while (true) {
        int n = fread(buffer.data(), 1, buffer.size(), local);
        if (n == 0) break;

        int written = sftp_write(file, buffer.data(), n);
        if (written != n) {
            error_ = "sftp_write error: " + std::string(ssh_get_error(session_));
            ok = false;
            break;
        }

        sent += written;
        if (progress && !progress(sent, totalSize)) {
            ok = false;
            break;
        }
    }

    fclose(local);
    sftp_close(file);

    if (!ok) {
        sftp_unlink(sftp_, remotePath.c_str());
        return false;
    }

    return true;
}

bool SshSession::sftpDelete(const std::string& path) {
    if (!sftpInit()) return false;

    sftp_attributes attr = sftp_stat(sftp_, path.c_str());
    if (!attr) {
        error_ = "sftp_stat failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    bool isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
    sftp_attributes_free(attr);

    int rc;
    if (isDir) {
        rc = sftp_rmdir(sftp_, path.c_str());
    } else {
        rc = sftp_unlink(sftp_, path.c_str());
    }

    if (rc != SSH_OK) {
        error_ = (isDir ? "sftp_rmdir" : "sftp_unlink") + std::string(" failed: ") + ssh_get_error(session_);
        return false;
    }

    return true;
}

bool SshSession::sftpMkdir(const std::string& path) {
    if (!sftpInit()) return false;

    int rc = sftp_mkdir(sftp_, path.c_str(), 0755);
    if (rc != SSH_OK) {
        error_ = "sftp_mkdir failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    return true;
}

bool SshSession::sftpRename(const std::string& oldPath, const std::string& newPath) {
    if (!sftpInit()) return false;

    int rc = sftp_rename(sftp_, oldPath.c_str(), newPath.c_str());
    if (rc != SSH_OK) {
        error_ = "sftp_rename failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    return true;
}

bool SshSession::sftpFileExists(const std::string& path) {
    if (!sftpInit()) return false;

    sftp_attributes attr = sftp_stat(sftp_, path.c_str());
    if (attr) {
        sftp_attributes_free(attr);
        return true;
    }
    return false;
}

bool SshSession::openShell(ShellDataCallback onData) {
    if (!connected_ || !session_) return false;
    if (shellChannel_) return true;

    shellChannel_ = ssh_channel_new(session_);
    if (!shellChannel_) {
        error_ = "ssh_channel_new failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    int rc = ssh_channel_open_session(shellChannel_);
    if (rc != SSH_OK) {
        error_ = "ssh_channel_open_session failed: " + std::string(ssh_get_error(session_));
        ssh_channel_free(shellChannel_);
        shellChannel_ = nullptr;
        return false;
    }

    rc = ssh_channel_request_pty_size(shellChannel_, "xterm-256color", 80, 24);
    if (rc != SSH_OK) {
        std::cerr << "[SSH] pty request failed (non-fatal): " << ssh_get_error(session_) << std::endl;
    }

    rc = ssh_channel_request_shell(shellChannel_);
    if (rc != SSH_OK) {
        error_ = "ssh_channel_request_shell failed: " + std::string(ssh_get_error(session_));
        ssh_channel_close(shellChannel_);
        ssh_channel_free(shellChannel_);
        shellChannel_ = nullptr;
        return false;
    }

    shellOnData_ = onData;
    return true;
}

void SshSession::writeShell(const char* data, int len) {
    if (!shellChannel_) return;

    int written = ssh_channel_write(shellChannel_, data, len);
    if (written != len) {
        std::cerr << "[SSH] shell write error: " << ssh_get_error(session_) << std::endl;
    }
}

void SshSession::closeShell() {
    if (shellChannel_) {
        ssh_channel_close(shellChannel_);
        ssh_channel_free(shellChannel_);
        shellChannel_ = nullptr;
    }
    shellOnData_ = nullptr;
}

void SshSession::setTerminalSize(int cols, int rows) {
    if (!shellChannel_) return;
    ssh_channel_change_pty_size(shellChannel_, cols, rows);
}

void SshSession::pollChannel(int timeoutMs) {
    if (!shellChannel_ || !shellOnData_) return;

    std::vector<char> buffer(65536);

    while (true) {
        int rc = ssh_channel_read_nonblocking(shellChannel_, buffer.data(), buffer.size(), 0);
        if (rc == SSH_ERROR) {
            break;
        }
        if (rc == 0) break;

        if (shellOnData_) {
            shellOnData_(buffer.data(), rc);
        }
    }
}
