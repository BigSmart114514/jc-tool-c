#include "ssh_server.h"
#include <libssh/sftp.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <windows.h>

// File type bits for SFTP permissions encoding (from sys/stat.h)
#ifndef S_IFDIR
#define S_IFDIR 0x4000
#endif
#ifndef S_IFREG
#define S_IFREG 0x8000
#endif

// ==================== String conversion helpers ====================

static std::wstring widen(const char* utf8) {
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wstr[0], len);
    return wstr;
}

static std::string narrow(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string str(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &str[0], len, nullptr, nullptr);
    return str;
}

static std::string narrow(const std::wstring& ws) { return narrow(ws.c_str()); }

// ==================== SFTP internal types ====================

// Convert SFTP path to Win32 path:
//   "/C:/Windows" → "C:\Windows"
//   "C:/Windows"  → "C:\Windows"
//   "/"           → ""  (drives listing sentinel)
//   "."           → "."
static std::wstring sftpPathToWin32(const char* sftp) {
    if (!sftp || !*sftp) return L"";
    std::wstring w = widen(sftp);
    // "/" → drives listing
    if (w == L"/") return L"";
    // Replace forward slashes with backslashes
    for (auto& ch : w) { if (ch == L'/') ch = L'\\'; }
    // "/C:..." → remove leading backslash from drive paths
    if (w.size() >= 2 && w[0] == L'\\' && iswalpha(w[1]) && w[2] == L':') {
        w = w.substr(1);
    }
    // "C:dir" → "C:\dir" (add backslash after drive letter if needed)
    if (w.size() >= 2 && iswalpha(w[0]) && w[1] == L':') {
        if (w.size() == 2) w += L'\\';  // "C:" → "C:\"
        else if (w[2] != L'\\') w.insert(2, L"\\");
    }
    return w;
}

struct FindHandleData {
    HANDLE hFind;
    bool first;
    bool isDrivesList;     // virtual drives listing (A:, B:, C: ...)
    WIN32_FIND_DATAW firstData;
    DWORD driveMask;       // bitmask from GetLogicalDrives()
    int driveIndex;        // current drive index being enumerated
};

static sftp_attributes allocAttrs() {
    return (sftp_attributes)calloc(1, sizeof(struct sftp_attributes_struct));
}

static void winTimeToUnix(const FILETIME& ft, uint32_t& out) {
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    // Win32 epoch (1601-01-01) -> Unix epoch (1970-01-01)
    out = (uint32_t)((li.QuadPart / 10000000ULL) - 11644473600ULL);
}

static void winTimeToUnix(LARGE_INTEGER li, uint32_t& out) {
    out = (uint32_t)(((uint64_t)li.QuadPart / 10000000ULL) - 11644473600ULL);
}

static void attrFromFindData(const WIN32_FIND_DATAW& ffd,
                              const char* utfName,
                              sftp_attributes attr) {
    attr->name = strdup(utfName);
    attr->size = ((uint64_t)ffd.nFileSizeHigh << 32) | ffd.nFileSizeLow;
    attr->permissions = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        ? (0755 | S_IFDIR) : (0644 | S_IFREG);
    attr->type = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 ? SSH_FILEXFER_TYPE_DIRECTORY : SSH_FILEXFER_TYPE_REGULAR;
    winTimeToUnix(ffd.ftLastWriteTime, attr->mtime);
    attr->flags = SSH_FILEXFER_ATTR_SIZE
                | SSH_FILEXFER_ATTR_PERMISSIONS
                | SSH_FILEXFER_ATTR_ACMODTIME
                | SSH_FILEXFER_ATTR_UIDGID;
    attr->uid = 0;
    attr->gid = 0;
    attr->atime = attr->mtime;
}

static void attrFromFileInfo(const std::string& utfName,
                              sftp_attributes attr) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    std::wstring wpath = widen(utfName.c_str());
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data)) {
        return;
    }
    attr->name = strdup(utfName.c_str());
    attr->size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    attr->permissions = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        ? (0755 | S_IFDIR) : (0644 | S_IFREG);
    attr->type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 ? SSH_FILEXFER_TYPE_DIRECTORY : SSH_FILEXFER_TYPE_REGULAR;
    winTimeToUnix(data.ftLastWriteTime, attr->mtime);
    attr->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS
                | SSH_FILEXFER_ATTR_ACMODTIME | SSH_FILEXFER_ATTR_UIDGID;
    attr->uid = 0;
    attr->gid = 0;
    attr->atime = attr->mtime;
}

// ==================== SshServer ====================

SshServer::SshServer() {}

SshServer::~SshServer() {
    stop();
}

ssh_bind SshServer::createBind(int port) {
    ssh_bind bind = ssh_bind_new();
    if (!bind) return nullptr;

    long p = port;
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &p);
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, "0.0.0.0");

    return bind;
}

bool SshServer::setupHostKey(ssh_bind bind) {
    // Persistent key file path (next to the exe)
    char keyPath[MAX_PATH + 1] = {0};
    GetModuleFileNameA(nullptr, keyPath, MAX_PATH);
    char* slash = strrchr(keyPath, '\\');
    if (slash) *slash = '\0';
    strcat_s(keyPath, "\\ssh_host_key_rsa");

    // Check if key already exists
    bool keyExists = (GetFileAttributesA(keyPath) != INVALID_FILE_ATTRIBUTES);

    if (keyExists) {
        std::cout << "[SSH Server] Loading existing host key" << std::endl;
    } else {
        std::cout << "[SSH Server] Generating new RSA host key (2048 bit)..." << std::endl;
        ssh_key key = nullptr;
        int rc = ssh_pki_generate(SSH_KEYTYPE_RSA, 2048, &key);
        if (rc != SSH_OK || !key) {
            std::cerr << "[SSH Server] Failed to generate RSA host key" << std::endl;
            return false;
        }

        rc = ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, keyPath);
        ssh_key_free(key);
        if (rc != SSH_OK) {
            std::cerr << "[SSH Server] Failed to export key to file" << std::endl;
            return false;
        }
    }

    keyFile_ = keyPath;

    int rc = ssh_bind_options_set(bind, SSH_BIND_OPTIONS_RSAKEY, keyPath);
    if (rc != SSH_OK) {
        std::cerr << "[SSH Server] Failed to set RSA key path" << std::endl;
        keyFile_.clear();
        return false;
    }

    return true;
}

void SshServer::cleanupKeyFile() {
    // Persistent key — do NOT delete on stop. Only delete on explicit request.
}

bool SshServer::start(int port, const std::string& password) {
    if (running_) return false;

    password_ = password;
    port_ = port;

    sshbind_ = createBind(port);
    if (!sshbind_) return false;

    if (!setupHostKey(sshbind_)) {
        ssh_bind_free(sshbind_);
        sshbind_ = nullptr;
        return false;
    }

    if (ssh_bind_listen(sshbind_) != SSH_OK) {
        std::cerr << "[SSH Server] ssh_bind_listen failed: "
                  << ssh_get_error(sshbind_) << std::endl;
        ssh_bind_free(sshbind_);
        sshbind_ = nullptr;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&SshServer::acceptLoop, this);
    std::cout << "[SSH Server] Listening on port " << port << std::endl;
    return true;
}

void SshServer::stop() {
    running_ = false;
    if (sshbind_) {
        ssh_bind_free(sshbind_);
        sshbind_ = nullptr;
    }
    if (acceptThread_.joinable()) acceptThread_.join();
}

void SshServer::acceptLoop() {
    while (running_) {
        ssh_session session = ssh_new();
        if (!session) continue;

        if (ssh_bind_accept(sshbind_, session) != SSH_OK) {
            ssh_free(session);
            continue;
        }

        std::cout << "[SSH Server] Accepted connection" << std::endl;

        if (!running_) {
            ssh_free(session);
            break;
        }

        std::thread(&SshServer::handleSession, this, session).detach();
    }
}

void SshServer::handleSession(ssh_session session) {
    // Key exchange
    if (ssh_handle_key_exchange(session) != SSH_OK) {
        std::cerr << "[SSH Server] Key exchange failed" << std::endl;
        ssh_free(session);
        return;
    }

    bool authenticated = false;
    ssh_channel currentChannel = nullptr;
    int ptyCols = 80, ptyRows = 24;

    while (running_) {
        ssh_message msg;
        do {
            msg = ssh_message_get(session);
        } while (!msg && running_);
        if (!msg) break;

        switch (ssh_message_type(msg)) {
        case SSH_REQUEST_AUTH:
            if (ssh_message_subtype(msg) == SSH_AUTH_METHOD_PASSWORD) {
                const char* pass = ssh_message_auth_password(msg);
                if (pass && password_ == pass) {
                    authenticated = true;
                    ssh_message_auth_reply_success(msg, 0);
                } else {
                    ssh_message_reply_default(msg);
                }
            } else {
                ssh_message_reply_default(msg);
            }
            break;

        case SSH_REQUEST_CHANNEL_OPEN:
            if (ssh_message_subtype(msg) == SSH_CHANNEL_SESSION) {
                currentChannel = ssh_message_channel_request_open_reply_accept(msg);
                if (!currentChannel) {
                    std::cerr << "[SSH Server] Failed to accept channel" << std::endl;
                }
            } else {
                ssh_message_reply_default(msg);
            }
            break;

        case SSH_REQUEST_CHANNEL:
            if (!currentChannel) {
                ssh_message_reply_default(msg);
                break;
            }
            if (ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_PTY) {
                ptyCols = ssh_message_channel_request_pty_width(msg);
                ptyRows = ssh_message_channel_request_pty_height(msg);
                ssh_message_channel_request_reply_success(msg);
            }
            else if (ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_SHELL) {
                ssh_message_channel_request_reply_success(msg);
                std::cerr << "[SSH Server] Shell request received, launching handler..." << std::endl;
                try {
                    handleShellChannel(session, currentChannel, ptyCols, ptyRows);
                } catch (const std::exception& e) {
                    std::cerr << "[SSH Server] Shell exception: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[SSH Server] Shell unknown exception" << std::endl;
                }
                currentChannel = nullptr;
                ssh_disconnect(session);
                ssh_free(session);
                return;
            }
            else if (ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_SUBSYSTEM) {
                const char* sub = ssh_message_channel_request_subsystem(msg);
                if (sub && strcmp(sub, "sftp") == 0) {
                    ssh_message_channel_request_reply_success(msg);
                    std::cerr << "[SSH Server] SFTP request received" << std::endl;
                    try {
                        handleSftpChannel(session, currentChannel);
                    } catch (const std::exception& e) {
                        std::cerr << "[SSH Server] SFTP exception: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[SSH Server] SFTP unknown exception" << std::endl;
                    }
                    currentChannel = nullptr;
                    ssh_disconnect(session);
                    ssh_free(session);
                    return;
                } else {
                    ssh_message_reply_default(msg);
                }
            }
            else {
                ssh_message_reply_default(msg);
            }
            break;

        default:
            ssh_message_reply_default(msg);
            break;
        }
        ssh_message_free(msg);
    }

    ssh_disconnect(session);
    ssh_free(session);
}

// ==================== SFTP Server ====================

void SshServer::handleSftpChannel(ssh_session session, ssh_channel channel) {
    sftp_session sftp = sftp_server_new(session, channel);
    if (!sftp) {
        std::cerr << "[SFTP] sftp_server_new failed" << std::endl;
        return;
    }

    if (sftp_server_init(sftp) < 0) {
        std::cerr << "[SFTP] sftp_server_init failed" << std::endl;
        sftp_server_free(sftp);
        return;
    }

    while (running_) {
        sftp_client_message msg = sftp_get_client_message(sftp);
        if (!msg) break;

        uint8_t type = sftp_client_message_get_type(msg);
        const char* filename = sftp_client_message_get_filename(msg);

        switch (type) {
        case SSH_FXP_REALPATH: {
            const char* rp = filename ? filename : ".";
            // "/" is the virtual drives root
            if (strcmp(rp, "/") == 0) {
                sftp_attributes attr = allocAttrs();
                attr->name = strdup("/");
                attr->type = SSH_FILEXFER_TYPE_DIRECTORY;
                attr->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS
                            | SSH_FILEXFER_ATTR_ACMODTIME;
                sftp_reply_name(msg, "/", attr);
                sftp_attributes_free(attr);
                break;
            }
            std::wstring wpath = sftpPathToWin32(rp);
            if (wpath.empty()) {
                sftp_reply_name(msg, "/", nullptr);
                break;
            }
            // Use GetFullPathNameW to resolve relative → absolute
            wchar_t fullW[MAX_PATH];
            wchar_t* filePart = nullptr;
            DWORD rclen = GetFullPathNameW(wpath.c_str(), MAX_PATH, fullW, &filePart);
            if (rclen > 0 && rclen < MAX_PATH) {
                std::string utf8 = narrow(fullW);
                // Convert backslashes to forward slashes for client compatibility
                for (auto& ch : utf8) { if (ch == '\\') ch = '/'; }
                sftp_attributes attr = allocAttrs();
                attr->name = strdup(utf8.c_str());
                attr->type = SSH_FILEXFER_TYPE_DIRECTORY;
                attr->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS
                            | SSH_FILEXFER_ATTR_ACMODTIME;
                sftp_reply_name(msg, utf8.c_str(), attr);
                sftp_attributes_free(attr);
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Realpath failed");
            }
            break;
        }
        case SSH_FXP_STAT:
        case SSH_FXP_LSTAT: {
            const char* sp = filename ? filename : "";
            // Handle "/" as virtual drives root
            if (strcmp(sp, "/") == 0) {
                sftp_attributes attr = allocAttrs();
                attr->name = strdup("/");
                attr->type = SSH_FILEXFER_TYPE_DIRECTORY;
                attr->permissions = 0755;
                attr->flags = SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_UIDGID;
                attr->uid = 0; attr->gid = 0;
                sftp_reply_attr(msg, attr);
                sftp_attributes_free(attr);
                break;
            }
            std::string winPath = narrow(sftpPathToWin32(sp));
            sftp_attributes attr = allocAttrs();
            attrFromFileInfo(winPath, attr);
            if (attr->name) {
                sftp_reply_attr(msg, attr);
            } else {
                sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "Not found");
            }
            sftp_attributes_free(attr);
            break;
        }
        case SSH_FXP_OPEN: {
            uint32_t flags = sftp_client_message_get_flags(msg);
            DWORD access = 0, creation = 0;
            if (flags & SSH_FXF_READ) access |= GENERIC_READ;
            if (flags & SSH_FXF_WRITE) {
                access |= GENERIC_WRITE;
                if (flags & SSH_FXF_APPEND) access |= FILE_APPEND_DATA;
            }
            if (flags & SSH_FXF_CREAT) {
                if (flags & SSH_FXF_EXCL) creation = CREATE_NEW;
                else if (flags & SSH_FXF_TRUNC) creation = CREATE_ALWAYS;
                else creation = OPEN_ALWAYS;
            } else {
                creation = OPEN_EXISTING;
            }
            if (!(flags & SSH_FXF_READ) && !(flags & SSH_FXF_WRITE)) {
                creation = OPEN_EXISTING;
            }

            std::wstring wpath = sftpPathToWin32(filename);
            HANDLE h = CreateFileW(wpath.c_str(), access,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                uint32_t fxErr = SSH_FX_FAILURE;
                if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
                    fxErr = SSH_FX_NO_SUCH_FILE;
                else if (err == ERROR_ACCESS_DENIED)
                    fxErr = SSH_FX_PERMISSION_DENIED;
                else if (err == ERROR_FILE_EXISTS)
                    fxErr = SSH_FX_FILE_ALREADY_EXISTS;
                sftp_reply_status(msg, fxErr, "Open failed");
                break;
            }

            ssh_string handle = sftp_handle_alloc(sftp, h);
            sftp_reply_handle(msg, handle);
            ssh_string_free(handle);
            break;
        }
        case SSH_FXP_CLOSE: {
            void* handle_data = sftp_handle(sftp, msg->handle);
            if (handle_data) {
                CloseHandle(static_cast<HANDLE>(handle_data));
                sftp_handle_remove(sftp, handle_data);
            }
            sftp_reply_status(msg, SSH_FX_OK, "OK");
            break;
        }
        case SSH_FXP_READ: {
            void* handle_data = sftp_handle(sftp, msg->handle);
            if (!handle_data) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            HANDLE h = static_cast<HANDLE>(handle_data);
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)(msg->offset & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)((msg->offset >> 32) & 0xFFFFFFFF);
            uint32_t len = msg->len;
            std::vector<char> buf(len > 65536 ? 65536 : len);
            len = (uint32_t)buf.size();
            DWORD read = 0;
            if (ReadFile(h, buf.data(), len, &read, &ov)) {
                sftp_reply_data(msg, buf.data(), read);
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_HANDLE_EOF || read == 0) {
                    sftp_reply_status(msg, SSH_FX_EOF, "EOF");
                } else {
                    sftp_reply_status(msg, SSH_FX_FAILURE, "Read failed");
                }
            }
            break;
        }
        case SSH_FXP_WRITE: {
            void* handle_data = sftp_handle(sftp, msg->handle);
            if (!handle_data) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            HANDLE h = static_cast<HANDLE>(handle_data);
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)(msg->offset & 0xFFFFFFFF);
            ov.OffsetHigh = (DWORD)((msg->offset >> 32) & 0xFFFFFFFF);
            const char* data = sftp_client_message_get_data(msg);
            uint32_t len = msg->len;
            DWORD written = 0;
            if (WriteFile(h, data, len, &written, &ov)) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Write failed");
            }
            break;
        }
        case SSH_FXP_FSTAT: {
            void* handle_data = sftp_handle(sftp, msg->handle);
            if (!handle_data) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            HANDLE h = static_cast<HANDLE>(handle_data);
            sftp_attributes attr = allocAttrs();
            FILE_STANDARD_INFO stdInfo;
            FILE_BASIC_INFO basicInfo;
            attr->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS
                        | SSH_FILEXFER_ATTR_ACMODTIME | SSH_FILEXFER_ATTR_UIDGID;
            attr->uid = 0; attr->gid = 0;
            if (GetFileInformationByHandleEx(h, FileStandardInfo,
                                             &stdInfo, sizeof(stdInfo))) {
                attr->size = stdInfo.EndOfFile.QuadPart;
            }
            if (GetFileInformationByHandleEx(h, FileBasicInfo,
                                             &basicInfo, sizeof(basicInfo))) {
                winTimeToUnix(basicInfo.ChangeTime, attr->mtime);
                attr->type = (basicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                             ? SSH_FILEXFER_TYPE_DIRECTORY
                             : SSH_FILEXFER_TYPE_REGULAR;
                attr->permissions = (basicInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                                    ? 0755 : 0644;
            }
            sftp_reply_attr(msg, attr);
            sftp_attributes_free(attr);
            break;
        }
        case SSH_FXP_SETSTAT: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (msg->attr) {
                if (msg->attr->permissions & 0200) {
                    SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_NORMAL);
                } else {
                    SetFileAttributesW(wpath.c_str(), FILE_ATTRIBUTE_READONLY);
                }
            }
            sftp_reply_status(msg, SSH_FX_OK, "OK");
            break;
        }
        case SSH_FXP_MKDIR: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (CreateDirectoryW(wpath.c_str(), nullptr)) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                DWORD err = GetLastError();
                uint32_t fxErr = (err == ERROR_ALREADY_EXISTS)
                                 ? SSH_FX_FILE_ALREADY_EXISTS : SSH_FX_FAILURE;
                sftp_reply_status(msg, fxErr, "Mkdir failed");
            }
            break;
        }
        case SSH_FXP_RMDIR: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (RemoveDirectoryW(wpath.c_str())) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Rmdir failed");
            }
            break;
        }
        case SSH_FXP_REMOVE: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (DeleteFileW(wpath.c_str())) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Remove failed");
            }
            break;
        }
        case SSH_FXP_RENAME: {
            const char* newname = sftp_client_message_get_data(msg);
            if (!newname) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Rename missing newname");
                break;
            }
            std::wstring oldp = sftpPathToWin32(filename);
            std::wstring newp = sftpPathToWin32(newname);
            if (oldp.empty() || newp.empty()) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break;
            }
            if (MoveFileW(oldp.c_str(), newp.c_str())) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Rename failed");
            }
            break;
        }
        case SSH_FXP_OPENDIR: {
            std::wstring wpath = sftpPathToWin32(filename);
            // "" → list drives
            bool listDrives = wpath.empty();
            if (listDrives) {
                auto* dirInfo = new FindHandleData();
                dirInfo->isDrivesList = true;
                dirInfo->driveMask = GetLogicalDrives();
                dirInfo->driveIndex = -1;
                dirInfo->first = true;
                ssh_string handle = sftp_handle_alloc(sftp, dirInfo);
                sftp_reply_handle(msg, handle);
                ssh_string_free(handle);
            } else {
                // Append backslash for drive roots like "C:" → "C:\"
                std::wstring searchPath = wpath;
                if (searchPath.size() == 2 && searchPath[1] == L':')
                    searchPath += L"\\*";
                else
                    searchPath += L"\\*";
                WIN32_FIND_DATAW ffd;
                HANDLE h = FindFirstFileW(searchPath.c_str(), &ffd);
                if (h == INVALID_HANDLE_VALUE) {
                    sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, "Not found");
                    break;
                }
                auto* dirInfo = new FindHandleData();
                dirInfo->isDrivesList = false;
                dirInfo->hFind = h;
                dirInfo->first = true;
                dirInfo->firstData = ffd;
                ssh_string handle = sftp_handle_alloc(sftp, dirInfo);
                sftp_reply_handle(msg, handle);
                ssh_string_free(handle);
            }
            break;
        }
        case SSH_FXP_READDIR: {
            void* handle_data = sftp_handle(sftp, msg->handle);
            if (!handle_data) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            auto* dirInfo = static_cast<FindHandleData*>(handle_data);

            // Handle virtual drives listing
            if (dirInfo->isDrivesList) {
                DWORD mask = dirInfo->driveMask;
                bool added = false;
                for (int i = dirInfo->driveIndex + 1; i < 26; i++) {
                    if (mask & (1 << i)) {
                        dirInfo->driveIndex = i;
                        char driveName[] = { (char)('A' + i), ':', '\0' };
                        sftp_attributes attr = allocAttrs();
                        attr->name = strdup(driveName);
                        attr->type = SSH_FILEXFER_TYPE_DIRECTORY;
                        attr->permissions = 0755 | S_IFDIR;
                        attr->flags = SSH_FILEXFER_ATTR_PERMISSIONS
                                    | SSH_FILEXFER_ATTR_UIDGID;
                        attr->uid = 0; attr->gid = 0;
                        std::string longname = std::string("drwxr-xr-x 0 0 0 0 ") + driveName;
                        sftp_reply_names_add(msg, driveName, longname.c_str(), attr);
                        sftp_attributes_free(attr);
                        added = true;
                        break; // return one entry per READDIR call
                    }
                }
                if (added) {
                    sftp_reply_names(msg);
                } else {
                    sftp_reply_status(msg, SSH_FX_EOF, "EOF");
                }
                break;
            }

            // Regular directory listing
            bool added = false;
            WIN32_FIND_DATAW ffd;
            bool ok;
            if (dirInfo->first) {
                dirInfo->first = false;
                ffd = dirInfo->firstData;
                ok = true;
            } else {
                ok = FindNextFileW(dirInfo->hFind, &ffd);
            }

            while (ok) {
                if (wcscmp(ffd.cFileName, L".") != 0
                    && wcscmp(ffd.cFileName, L"..") != 0) {
                    std::string name = narrow(ffd.cFileName);
                    sftp_attributes attr = allocAttrs();
                    attrFromFindData(ffd, name.c_str(), attr);
                    std::string longname = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY)
                                           ? "drwxr-xr-x" : "-rw-r--r--";
                    char perms[12];
                    snprintf(perms, sizeof(perms), "%s", longname.c_str());
                    longname = perms;
                    longname += " 0 0 0 ";
                    char sizeStr[32];
                    snprintf(sizeStr, sizeof(sizeStr), "%llu",
                             (unsigned long long)attr->size);
                    longname += sizeStr;
                    char timeStr[32];
                    snprintf(timeStr, sizeof(timeStr), " %u", attr->mtime);
                    longname += timeStr;
                    longname += " ";
                    longname += name;

                    sftp_reply_names_add(msg, name.c_str(),
                                         longname.c_str(), attr);
                    sftp_attributes_free(attr);
                    added = true;
                }
                ok = FindNextFileW(dirInfo->hFind, &ffd);
            }

            if (added) {
                sftp_reply_names(msg);
            } else {
                sftp_reply_status(msg, SSH_FX_EOF, "EOF");
            }
            break;
        }
        case SSH_FXP_READLINK:
        case SSH_FXP_SYMLINK:
            sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Not supported");
            break;
        default:
            sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Unknown op");
            break;
        }
        sftp_client_message_free(msg);
    }

    sftp_server_free(sftp);
}

// ==================== ConPTY Shell Handler ====================

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x20016
#endif

typedef HRESULT (WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT (WINAPI *ResizePseudoConsoleFn)(HPCON, COORD);
typedef void (WINAPI *ClosePseudoConsoleFn)(HPCON);

static bool tryConPty(HANDLE hIn, HANDLE hOut, SHORT cols, SHORT rows, HPCON* phPty) {
    auto fn = (CreatePseudoConsoleFn)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "CreatePseudoConsole");
    if (!fn) return false;
    COORD size = {cols > 0 ? cols : 80, rows > 0 ? rows : 24};
    return SUCCEEDED(fn(size, hIn, hOut, 0, phPty));
}

static void closeConPty(HPCON hPty) {
    auto fn = (ClosePseudoConsoleFn)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "ClosePseudoConsole");
    if (fn) fn(hPty);
}

void SshServer::handleShellChannel(ssh_session session, ssh_channel channel,
                                    int cols, int rows) {
    std::cerr << "[SSH Shell] Starting shell channel" << std::endl;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hPtyInRd = INVALID_HANDLE_VALUE;
    HANDLE hPtyInWr = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutRd = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutWr = INVALID_HANDLE_VALUE;
    HPCON hPty = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};
    std::atomic<bool> shellRunning{true};
    wchar_t shellExe[] = L"cmd.exe";
    std::thread reader;
    bool readerStarted = false;
    bool useConPty = false;

    // Helper: convert pipe output to UTF-8. Data may already be UTF-8
    // (modern Windows ConPTY) or need ACP→UTF-8 conversion (legacy).
    auto acpToUtf8 = [](const char* src, int srclen) -> std::string {
        if (srclen <= 0) return {};
        // Check if already valid UTF-8
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, srclen, nullptr, 0) > 0)
            return {src, (size_t)srclen};
        // Not valid UTF-8 → convert from system ACP (e.g. GBK on Chinese Windows)
        int wn = MultiByteToWideChar(CP_ACP, 0, src, srclen, nullptr, 0);
        if (wn <= 0) return {src, (size_t)srclen};
        std::wstring ws(wn, L'\0');
        MultiByteToWideChar(CP_ACP, 0, src, srclen, &ws[0], wn);
        int u8n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wn, nullptr, 0, nullptr, nullptr);
        if (u8n <= 0) return {src, (size_t)srclen};
        std::string u8(u8n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wn, &u8[0], u8n, nullptr, nullptr);
        return u8;
    };

    // Helper: convert UTF-8 input to system ACP before writing to pipe
    auto utf8ToAcp = [](const char* src, int srclen) -> std::string {
        if (srclen <= 0) return {};
        int wn = MultiByteToWideChar(CP_UTF8, 0, src, srclen, nullptr, 0);
        if (wn <= 0) return {src, (size_t)srclen};
        std::wstring ws(wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, src, srclen, &ws[0], wn);
        int acpn = WideCharToMultiByte(CP_ACP, 0, ws.c_str(), wn, nullptr, 0, nullptr, nullptr);
        if (acpn <= 0) return {src, (size_t)srclen};
        std::string acp(acpn, '\0');
        WideCharToMultiByte(CP_ACP, 0, ws.c_str(), wn, &acp[0], acpn, nullptr, nullptr);
        return acp;
    };

    // Create pipes
    if (!CreatePipe(&hPtyOutRd, &hPtyOutWr, &sa, 0)) {
        std::cerr << "[SSH Shell] CreatePipe output failed" << std::endl;
        goto cleanup;
    }
    if (!CreatePipe(&hPtyInRd, &hPtyInWr, &sa, 0)) {
        std::cerr << "[SSH Shell] CreatePipe input failed" << std::endl;
        goto cleanup;
    }

    // Try ConPTY first; fall back to plain pipe
    useConPty = tryConPty(hPtyInRd, hPtyOutWr, (SHORT)cols, (SHORT)rows, &hPty);

    if (useConPty) {
        // ─── ConPTY path ─────────────────────────────────────────
        std::cerr << "[SSH Shell] Using ConPTY" << std::endl;
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
        if (!si.lpAttributeList) goto cleanup;
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize))
            { free(si.lpAttributeList); goto cleanup; }
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPty, sizeof(HPCON), nullptr, nullptr))
            { DeleteProcThreadAttributeList(si.lpAttributeList); free(si.lpAttributeList); goto cleanup; }

        if (!CreateProcessW(nullptr, shellExe, nullptr, nullptr, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                            &si.StartupInfo, &pi)) {
            std::cerr << "[SSH Shell] ConPTY CreateProcess: " << GetLastError() << std::endl;
            DeleteProcThreadAttributeList(si.lpAttributeList);
            free(si.lpAttributeList);
            goto cleanup;
        }
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        CloseHandle(pi.hThread); pi.hThread = nullptr;
        CloseHandle(hPtyInRd); hPtyInRd = INVALID_HANDLE_VALUE;
        CloseHandle(hPtyOutWr); hPtyOutWr = INVALID_HANDLE_VALUE;

        reader = std::thread([&]() {
            std::vector<char> rbuf(65536);
            while (shellRunning && running_) {
                DWORD n = 0;
                if (ReadFile(hPtyOutRd, rbuf.data(), (DWORD)rbuf.size(), &n, nullptr)) {
                    if (n > 0) {
                        std::string u8 = acpToUtf8(rbuf.data(), n);
                        ssh_channel_write(channel, u8.data(), u8.size());
                    }
                } else {
                    if (GetLastError() != ERROR_BROKEN_PIPE)
                        std::cerr << "[SSH Shell] ReadFile: " << GetLastError() << std::endl;
                    break;
                }
            }
        });
        readerStarted = true;

        std::vector<char> buf(65536);
        while (shellRunning && running_) {
            int rc = ssh_channel_read_nonblocking(channel, buf.data(), buf.size(), 0);
            if (rc > 0) {
                DWORD w = 0;
                std::string acp = utf8ToAcp(buf.data(), rc);
                WriteFile(hPtyInWr, acp.data(), (DWORD)acp.size(), &w, nullptr);
            } else if (rc == SSH_ERROR) {
                break;
            } else {
                Sleep(20);
            }
        }
        std::cerr << "[SSH Shell] ConPTY loop done" << std::endl;

    } else {
        // ─── Fallback: plain pipe path ─────────────────────────
        std::cerr << "[SSH Shell] Plain pipe fallback" << std::endl;
        STARTUPINFOW si = { sizeof(si), 0 };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hPtyInRd;
        si.hStdOutput = hPtyOutWr;
        si.hStdError = hPtyOutWr;

        if (!CreateProcessW(nullptr, shellExe, nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::cerr << "[SSH Shell] CreateProcess: " << GetLastError() << std::endl;
            goto cleanup;
        }
        std::cerr << "[SSH Shell] cmd.exe PID=" << pi.dwProcessId << std::endl;
        CloseHandle(pi.hThread); pi.hThread = nullptr;
        CloseHandle(hPtyInRd); hPtyInRd = INVALID_HANDLE_VALUE;
        CloseHandle(hPtyOutWr); hPtyOutWr = INVALID_HANDLE_VALUE;

        reader = std::thread([&]() {
            std::vector<char> rbuf(65536);
            while (shellRunning && running_) {
                DWORD n = 0;
                if (ReadFile(hPtyOutRd, rbuf.data(), (DWORD)rbuf.size(), &n, nullptr)) {
                    if (n > 0) {
                        std::string u8 = acpToUtf8(rbuf.data(), n);
                        ssh_channel_write(channel, u8.data(), u8.size());
                    }
                } else {
                    if (GetLastError() != ERROR_BROKEN_PIPE)
                        std::cerr << "[SSH Shell] ReadFile: " << GetLastError() << std::endl;
                    break;
                }
            }
        });
        readerStarted = true;

        std::vector<char> buf(65536);
        while (shellRunning && running_) {
            int rc = ssh_channel_read_nonblocking(channel, buf.data(), buf.size(), 0);
            if (rc > 0) {
                DWORD w = 0;
                std::string acp = utf8ToAcp(buf.data(), rc);
                WriteFile(hPtyInWr, acp.data(), (DWORD)acp.size(), &w, nullptr);
            } else if (rc == SSH_ERROR) {
                break;
            } else {
                Sleep(20);
            }
        }
        std::cerr << "[SSH Shell] Pipe loop done" << std::endl;
    }

cleanup:
    std::cerr << "[SSH Shell] Cleanup" << std::endl;
    shellRunning = false;
    // Close read pipe to unblock reader thread before joining
    if (hPtyOutRd != INVALID_HANDLE_VALUE) {
        CloseHandle(hPtyOutRd);
        hPtyOutRd = INVALID_HANDLE_VALUE;
    }
    if (readerStarted && reader.joinable()) reader.join();
    if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (hPty != INVALID_HANDLE_VALUE) closeConPty(hPty);
    if (hPtyInRd != INVALID_HANDLE_VALUE) CloseHandle(hPtyInRd);
    if (hPtyInWr != INVALID_HANDLE_VALUE) CloseHandle(hPtyInWr);
    if (hPtyOutRd != INVALID_HANDLE_VALUE) CloseHandle(hPtyOutRd);
    if (hPtyOutWr != INVALID_HANDLE_VALUE) CloseHandle(hPtyOutWr);
    std::cerr << "[SSH Shell] Done" << std::endl;
}
