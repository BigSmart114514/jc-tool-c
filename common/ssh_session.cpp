#include "ssh_session.h"
#include <iostream>
#include <cstring>
#include <cstdio>
#include <vector>
#include <fcntl.h>
#include <cerrno>
#include <io.h>
#include <windows.h>
#include <bcrypt.h>

static std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], len);
    return wstr;
}

static std::string narrow(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string str(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    return str;
}

SshSession::SshSession() {}

SshSession::~SshSession() {
    disconnect();
}

bool SshSession::connect(const std::string& host, int port,
                         const std::string& username, const std::string& password) {
    if (connected_) disconnect();

    host_ = host;
    port_ = port;
    username_ = username;
    password_ = password;

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

bool SshSession::reconnect() {
    error_.clear();
    bool hadSftp = sftpInitialized_;
    bool hadShell = (shellChannel_ != nullptr);
    disconnect();

    if (host_.empty()) {
        error_ = "No saved connection parameters";
        return false;
    }

    if (!connect(host_, port_, username_, password_)) {
        return false;
    }

    if (hadSftp) {
        if (!sftpInit()) {
            return false;
        }
    }

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

bool SshSession::ensureConnected() {
    if (connected_ && session_) return true;
    if (host_.empty()) return false;
    return reconnect();
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
    if (!ensureConnected()) return false;
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
        entry.isSymlink = (file->type == SSH_FILEXFER_TYPE_SYMLINK);
        entry.size = file->size;
        entry.modifyTime = file->mtime;
        entry.permissions = file->permissions;
        entry.owner = file->owner ? file->owner : "";
        entry.group = file->group ? file->group : "";

        if (entry.isSymlink) {
            std::string fullPath = path + "/" + file->name;
            char* target = sftp_readlink(sftp_, fullPath.c_str());
            if (target) {
                entry.symlinkTarget = target;
                free(target);
            }
        }

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
    return sftpDownload(remotePath, localPath, 0, progress);
}

bool SshSession::sftpDownload(const std::string& remotePath, const std::string& localPath,
                              int64_t offset, std::function<bool(int64_t, int64_t)> progress) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_file file = sftp_open(sftp_, remotePath.c_str(), O_RDONLY, (mode_t)0);
    if (!file) {
        error_ = "sftp_open (read) failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    if (offset > 0) {
        sftp_seek(file, offset);
    }

    sftp_attributes attr = sftp_stat(sftp_, remotePath.c_str());
    int64_t totalSize = attr ? attr->size : 0;
    if (attr) sftp_attributes_free(attr);

    std::wstring wlocal = widen(localPath);
    FILE* local = _wfopen(wlocal.c_str(), offset > 0 ? L"ab" : L"wb");
    if (!local) {
        error_ = "Cannot open local file for writing: " + localPath + " (errno=" + std::to_string(errno) + ")";
        sftp_close(file);
        return false;
    }

    std::vector<char> buffer(65536);
    int64_t received = offset;
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
        if (offset == 0) {
            remove(localPath.c_str());
        }
        return false;
    }

    return true;
}

bool SshSession::sftpUpload(const std::string& localPath, const std::string& remotePath,
                            std::function<bool(int64_t, int64_t)> progress) {
    return sftpUpload(localPath, remotePath, 0, progress);
}

bool SshSession::sftpUpload(const std::string& localPath, const std::string& remotePath,
                            int64_t offset, std::function<bool(int64_t, int64_t)> progress) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    FILE* local = _wfopen(widen(localPath).c_str(), L"rb");
    if (!local) {
        error_ = "Cannot open local file for reading: " + localPath + " (errno=" + std::to_string(errno) + ")";
        return false;
    }

    if (offset > 0) {
        fseek(local, offset, SEEK_SET);
    }

    int flags = O_WRONLY | O_CREAT;
    if (offset == 0) {
        flags |= O_TRUNC;
    }

    sftp_file file = sftp_open(sftp_, remotePath.c_str(), flags, (mode_t)0644);
    if (!file) {
        error_ = "sftp_open (write) failed: " + std::string(ssh_get_error(session_));
        fclose(local);
        return false;
    }

    if (offset > 0) {
        sftp_seek(file, offset);
    }

    fseek(local, 0, SEEK_END);
    int64_t totalSize = ftell(local);
    fseek(local, offset, SEEK_SET);

    std::vector<char> buffer(65536);
    int64_t sent = offset;
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
        if (offset == 0) {
            sftp_unlink(sftp_, remotePath.c_str());
        }
        return false;
    }

    return true;
}

bool SshSession::sftpDelete(const std::string& path) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_attributes attr = sftp_lstat(sftp_, path.c_str());
    if (!attr) {
        error_ = "sftp_lstat failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    bool isSymlink = (attr->type == SSH_FILEXFER_TYPE_SYMLINK);
    bool isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
    sftp_attributes_free(attr);

    int rc;
    if (isSymlink || !isDir) {
        rc = sftp_unlink(sftp_, path.c_str());
    } else {
        rc = sftp_rmdir(sftp_, path.c_str());
    }

    if (rc != SSH_OK) {
        error_ = std::string(isDir ? "sftp_rmdir" : "sftp_unlink") + " failed: " + ssh_get_error(session_);
        return false;
    }

    return true;
}

bool SshSession::sftpMkdir(const std::string& path) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    int rc = sftp_mkdir(sftp_, path.c_str(), 0755);
    if (rc != SSH_OK) {
        error_ = "sftp_mkdir failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    return true;
}

bool SshSession::sftpRename(const std::string& oldPath, const std::string& newPath) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    int rc = sftp_rename(sftp_, oldPath.c_str(), newPath.c_str());
    if (rc != SSH_OK) {
        error_ = "sftp_rename failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    return true;
}

bool SshSession::sftpFileExists(const std::string& path) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_attributes attr = sftp_stat(sftp_, path.c_str());
    if (attr) {
        sftp_attributes_free(attr);
        return true;
    }
    return false;
}

bool SshSession::sftpFileInfo(const std::string& path, SFtpEntry& info) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_attributes attr = sftp_lstat(sftp_, path.c_str());
    if (!attr) {
        error_ = "sftp_lstat failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    auto pos = path.find_last_of("/\\");
    info.name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    info.isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
    info.isSymlink = (attr->type == SSH_FILEXFER_TYPE_SYMLINK);
    info.size = attr->size;
    info.modifyTime = attr->mtime;
    info.permissions = attr->permissions;
    info.owner = attr->owner ? attr->owner : "";
    info.group = attr->group ? attr->group : "";

    if (info.isSymlink) {
        char* target = sftp_readlink(sftp_, path.c_str());
        if (target) {
            info.symlinkTarget = target;
            free(target);
        }
    }

    sftp_attributes_free(attr);
    return true;
}

bool SshSession::sftpDownloadRecursive(const std::string& remoteDir, const std::string& localDir,
                                       std::function<bool(int64_t, int64_t)> progress) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    if (!CreateDirectoryW(widen(localDir).c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            error_ = "Failed to create local directory: " + localDir + " (error=" + std::to_string(err) + ")";
            return false;
        }
    }

    std::vector<SFtpEntry> entries;
    if (!sftpListDir(remoteDir, entries)) return false;

    for (const auto& entry : entries) {
        std::string childRemote = remoteDir + "/" + entry.name;
        std::string childLocal = localDir + "\\" + entry.name;

        if (entry.isDir) {
            if (!sftpDownloadRecursive(childRemote, childLocal, progress)) return false;
        } else {
            if (!sftpDownload(childRemote, childLocal, progress)) return false;
        }
    }

    return true;
}

bool SshSession::sftpUploadRecursive(const std::string& localDir, const std::string& remoteDir,
                                     std::function<bool(int64_t, int64_t)> progress) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_mkdir(sftp_, remoteDir.c_str(), 0755);

    std::wstring pattern = widen(localDir + "\\*");
    struct _wfinddata_t findData;
    intptr_t hFind = _wfindfirst(pattern.c_str(), &findData);
    if (hFind == -1) {
        error_ = "Failed to list local directory: " + localDir;
        return false;
    }

    do {
        std::wstring wname = findData.name;
        if (wname == L"." || wname == L"..") continue;

        std::string name = narrow(wname);
        std::string childLocal = localDir + "\\" + name;
        std::string childRemote = remoteDir + "/" + name;

        if (findData.attrib & _A_SUBDIR) {
            if (!sftpUploadRecursive(childLocal, childRemote, progress)) {
                _findclose(hFind);
                return false;
            }
        } else {
            if (!sftpUpload(childLocal, childRemote, progress)) {
                _findclose(hFind);
                return false;
            }
        }
    } while (_wfindnext(hFind, &findData) == 0);

    _findclose(hFind);
    return true;
}

bool SshSession::sftpDeleteRecursive(const std::string& path) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_attributes attr = sftp_lstat(sftp_, path.c_str());
    if (!attr) {
        error_ = "sftp_lstat failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    bool isSymlink = (attr->type == SSH_FILEXFER_TYPE_SYMLINK);
    bool isDir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
    sftp_attributes_free(attr);

    if (isDir && !isSymlink) {
        std::vector<SFtpEntry> entries;
        if (!sftpListDir(path, entries)) return false;

        for (const auto& entry : entries) {
            std::string childPath = path + "/" + entry.name;
            if (!sftpDeleteRecursive(childPath)) return false;
        }

        int rc = sftp_rmdir(sftp_, path.c_str());
        if (rc != SSH_OK) {
            error_ = "sftp_rmdir failed: " + std::string(ssh_get_error(session_));
            return false;
        }
    } else {
        int rc = sftp_unlink(sftp_, path.c_str());
        if (rc != SSH_OK) {
            error_ = "sftp_unlink failed: " + std::string(ssh_get_error(session_));
            return false;
        }
    }

    return true;
}

bool SshSession::sftpChecksum(const std::string& remotePath, std::string& sha256hex) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    sftp_file file = sftp_open(sftp_, remotePath.c_str(), O_RDONLY, (mode_t)0);
    if (!file) {
        error_ = "sftp_open (checksum) failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error_ = "BCryptOpenAlgorithmProvider failed";
        sftp_close(file);
        return false;
    }

    DWORD hashObjLen = 0;
    DWORD hashLen = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&hashObjLen, sizeof(hashObjLen), &hashLen, 0);
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&hashLen, sizeof(hashLen), &hashLen, 0);

    std::vector<BYTE> hashObject(hashObjLen);
    std::vector<BYTE> hash(hashLen);

    status = BCryptCreateHash(hAlg, &hHash, hashObject.data(), hashObjLen, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        error_ = "BCryptCreateHash failed";
        BCryptCloseAlgorithmProvider(hAlg, 0);
        sftp_close(file);
        return false;
    }

    std::vector<char> buffer(65536);
    bool ok = true;

    while (true) {
        int n = sftp_read(file, buffer.data(), buffer.size());
        if (n == SSH_ERROR) {
            error_ = "sftp_read error: " + std::string(ssh_get_error(session_));
            ok = false;
            break;
        }
        if (n == 0) break;

        BCryptHashData(hHash, (PBYTE)buffer.data(), n, 0);
    }

    if (ok) {
        status = BCryptFinishHash(hHash, hash.data(), hash.size(), 0);
        if (BCRYPT_SUCCESS(status)) {
            char hex[65] = {0};
            for (DWORD i = 0; i < hashLen && i < 32; i++) {
                sprintf(hex + i * 2, "%02x", hash[i]);
            }
            sha256hex = hex;
        } else {
            error_ = "BCryptFinishHash failed";
            ok = false;
        }
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    sftp_close(file);

    return ok;
}

bool SshSession::sftpSymlink(const std::string& target, const std::string& linkPath) {
    if (!ensureConnected()) return false;
    if (!sftpInit()) return false;

    int rc = sftp_symlink(sftp_, target.c_str(), linkPath.c_str());
    if (rc != SSH_OK) {
        error_ = "sftp_symlink failed: " + std::string(ssh_get_error(session_));
        return false;
    }

    return true;
}

std::string SshSession::sftpReadlink(const std::string& path) {
    if (!ensureConnected()) return "";
    if (!sftpInit()) return "";

    char* target = sftp_readlink(sftp_, path.c_str());
    if (!target) {
        error_ = "sftp_readlink failed: " + std::string(ssh_get_error(session_));
        return "";
    }

    std::string result(target);
    free(target);
    return result;
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
