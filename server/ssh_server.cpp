#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "ssh_server.h"
#include <winsock2.h>
#include <libssh/sftp.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <winioctl.h>
#include <intrin.h>

// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE for pre-Win10 SDK compatibility
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

// ==================== Reparse point helpers ====================
// Windows SDK may not define REPARSE_DATA_BUFFER in all versions.
// Define the minimum struct we need for reading symlinks/junctions.

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)
#endif

struct ReparseBuffer {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    // SymbolicLinkReparseBuffer
    struct {
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        ULONG Flags;
        WCHAR PathBuffer[1];
    } SymbolicLinkReparseBuffer;
    // MountPointReparseBuffer
    struct {
        USHORT SubstituteNameOffset;
        USHORT SubstituteNameLength;
        USHORT PrintNameOffset;
        USHORT PrintNameLength;
        WCHAR PathBuffer[1];
    } MountPointReparseBuffer;
};

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

static std::string narrow(const wchar_t* wide, int len) {
    if (!wide || len <= 0) return {};
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wide, len, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return {};
    std::string str(u8len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, len, &str[0], u8len, nullptr, nullptr);
    return str;
}

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

    // Free bind closes the listening socket, causing ssh_bind_accept()
    // in the accept thread to return with an error so the thread exits.
    if (sshbind_) {
        ssh_bind_free(sshbind_);
        sshbind_ = nullptr;
    }

    if (acceptThread_.joinable())
        acceptThread_.detach();
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

// ==================== SFTP error mapping ====================

// libssh only defines these status codes.
// Define additional standard SFTPv6 status codes used internally.
#ifndef SSH_FX_DIR_NOT_EMPTY
#define SSH_FX_DIR_NOT_EMPTY 14
#endif
#ifndef SSH_FX_NO_SPACE_ON_FILESYSTEM
#define SSH_FX_NO_SPACE_ON_FILESYSTEM 15
#endif
#ifndef SSH_FX_INVALID_FILENAME
#define SSH_FX_INVALID_FILENAME 16
#endif
#ifndef SSH_FX_NOT_A_DIRECTORY
#define SSH_FX_NOT_A_DIRECTORY 18
#endif
#ifndef SSH_FX_LOCK_CONFLICT
#define SSH_FX_LOCK_CONFLICT 20
#endif

static uint32_t mapWin32ErrorToFx(DWORD err) {
    switch (err) {
    case ERROR_SUCCESS: return SSH_FX_OK;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return SSH_FX_NO_SUCH_FILE;
    case ERROR_ACCESS_DENIED: return SSH_FX_PERMISSION_DENIED;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return SSH_FX_FILE_ALREADY_EXISTS;
    case ERROR_DIR_NOT_EMPTY: return SSH_FX_DIR_NOT_EMPTY;
    case ERROR_WRITE_PROTECT: return SSH_FX_PERMISSION_DENIED;
    case ERROR_DISK_FULL: return SSH_FX_NO_SPACE_ON_FILESYSTEM;
    case ERROR_INVALID_NAME: return SSH_FX_INVALID_FILENAME;
    case ERROR_BAD_PATHNAME: return SSH_FX_INVALID_FILENAME;
    case ERROR_HANDLE_EOF: return SSH_FX_EOF;
    case ERROR_NOT_SAME_DEVICE: return SSH_FX_NOT_A_DIRECTORY;
    case ERROR_DIRECTORY: return SSH_FX_NOT_A_DIRECTORY;
    case ERROR_SHARING_VIOLATION: return SSH_FX_LOCK_CONFLICT;
    case ERROR_LOCK_VIOLATION: return SSH_FX_LOCK_CONFLICT;
    case ERROR_BROKEN_PIPE: return SSH_FX_CONNECTION_LOST;
    case ERROR_INVALID_HANDLE: return SSH_FX_INVALID_HANDLE;
    default: return SSH_FX_FAILURE;
    }
}

// SSH_FXP_LINK (SFTPv6) - not defined in this libssh version
#ifndef SSH_FXP_LINK
#define SSH_FXP_LINK 21
#endif

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
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Open failed");
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

            bool anyChange = false;
            if (msg->attr) {
                // Handle permissions
                if (msg->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
                    DWORD attrs = GetFileAttributesW(wpath.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES) {
                        if (msg->attr->permissions & 0200) {
                            attrs &= ~FILE_ATTRIBUTE_READONLY;
                        } else {
                            attrs |= FILE_ATTRIBUTE_READONLY;
                        }
                        SetFileAttributesW(wpath.c_str(), attrs);
                        anyChange = true;
                    }
                }
                // Handle mtime/atime
                if (msg->attr->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
                    HANDLE h = CreateFileW(wpath.c_str(), FILE_WRITE_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (h != INVALID_HANDLE_VALUE) {
                        FILETIME ft;
                        ULARGE_INTEGER li;
                        li.QuadPart = (uint64_t)(msg->attr->mtime + 11644473600ULL) * 10000000ULL;
                        ft.dwLowDateTime = li.LowPart;
                        ft.dwHighDateTime = li.HighPart;
                        SetFileTime(h, nullptr, nullptr, &ft);
                        CloseHandle(h);
                        anyChange = true;
                    }
                }
            }
            sftp_reply_status(msg, anyChange ? SSH_FX_OK : SSH_FX_FAILURE, anyChange ? "OK" : "No changes applied");
            break;
        }
        case SSH_FXP_MKDIR: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (CreateDirectoryW(wpath.c_str(), nullptr)) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Mkdir failed");
            }
            break;
        }
        case SSH_FXP_RMDIR: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (RemoveDirectoryW(wpath.c_str())) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Rmdir failed");
            }
            break;
        }
        case SSH_FXP_REMOVE: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) { sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path"); break; }
            if (DeleteFileW(wpath.c_str())) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Remove failed");
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
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Rename failed");
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
        case SSH_FXP_READLINK: {
            std::wstring wpath = sftpPathToWin32(filename);
            if (wpath.empty()) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Invalid path");
                break;
            }
            HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Cannot open reparse point");
                break;
            }
            char rpBuf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE] = {0};
            auto& rp = *(ReparseBuffer*)rpBuf;
            DWORD ret = 0;
            if (DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                rpBuf, sizeof(rpBuf), &ret, nullptr)) {
                std::string target;
                if (rp.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
                    int len = rp.SymbolicLinkReparseBuffer.PrintNameLength / sizeof(WCHAR);
                    int off = rp.SymbolicLinkReparseBuffer.PrintNameOffset / sizeof(WCHAR);
                    target = narrow(&rp.SymbolicLinkReparseBuffer.PathBuffer[off], len);
                    if (target.find("\\??\\") == 0) target = target.substr(4);
                } else if (rp.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
                    int len = rp.MountPointReparseBuffer.PrintNameLength / sizeof(WCHAR);
                    int off = rp.MountPointReparseBuffer.PrintNameOffset / sizeof(WCHAR);
                    target = narrow(&rp.MountPointReparseBuffer.PathBuffer[off], len);
                } else {
                    CloseHandle(h);
                    sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Not a symlink/junction");
                    break;
                }
                for (auto& ch : target) if (ch == '\\') ch = '/';
                sftp_reply_name(msg, target.c_str(), nullptr);
            } else {
                sftp_reply_status(msg, SSH_FX_FAILURE, "IOCTL failed");
            }
            CloseHandle(h);
            break;
        }
        case SSH_FXP_SYMLINK: {
            const char* linkTarget = sftp_client_message_get_data(msg);
            if (!linkTarget || !filename) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Missing args");
                break;
            }
            std::wstring wtarget = widen(linkTarget);
            std::wstring wlink = sftpPathToWin32(filename);
            for (auto& ch : wtarget) if (ch == L'/') ch = L'\\';

            BOOL ok = CreateSymbolicLinkW(wlink.c_str(), wtarget.c_str(),
                (GetFileAttributesW(wtarget.c_str()) & FILE_ATTRIBUTE_DIRECTORY) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
            if (ok) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                DWORD err = GetLastError();
                if (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD) {
                    sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "Symlink creation requires privilege or developer mode");
                } else {
                    sftp_reply_status(msg, mapWin32ErrorToFx(err), "Symlink failed");
                }
            }
            break;
        }
        case SSH_FXP_LINK: {
            const char* existing = sftp_client_message_get_data(msg);
            if (!existing || !filename) {
                sftp_reply_status(msg, SSH_FX_FAILURE, "Missing args");
                break;
            }
            bool isSymlink = (sftp_client_message_get_flags(msg) != 0);
            std::wstring wexisting = sftpPathToWin32(existing);
            std::wstring wlink = sftpPathToWin32(filename);

            if (isSymlink) {
                for (auto& ch : wexisting) if (ch == L'/') ch = L'\\';
                BOOL ok = CreateSymbolicLinkW(wlink.c_str(), wexisting.c_str(),
                    (GetFileAttributesW(wexisting.c_str()) & FILE_ATTRIBUTE_DIRECTORY) ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
                if (ok) {
                    sftp_reply_status(msg, SSH_FX_OK, "OK");
                } else {
                    DWORD err = GetLastError();
                    if (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD)
                        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, "Symlink creation requires privilege or developer mode");
                    else
                        sftp_reply_status(msg, mapWin32ErrorToFx(err), "Symlink failed");
                }
            } else {
                if (CreateHardLinkW(wlink.c_str(), wexisting.c_str(), nullptr)) {
                    sftp_reply_status(msg, SSH_FX_OK, "OK");
                } else {
                    DWORD err = GetLastError();
                    uint32_t fxErr = SSH_FX_FAILURE;
                    if (err == ERROR_NOT_SAME_DEVICE) fxErr = SSH_FX_NOT_A_DIRECTORY;
                    else if (err == ERROR_ACCESS_DENIED) fxErr = SSH_FX_PERMISSION_DENIED;
                    else if (err == ERROR_FILE_EXISTS) fxErr = SSH_FX_FILE_ALREADY_EXISTS;
                    sftp_reply_status(msg, fxErr, "Hard link failed");
                }
            }
            break;
        }
        case SSH_FXP_EXTENDED: {
            const char* subType = sftp_client_message_get_submessage(msg);
            if (!subType) {
                sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Unknown extension");
                break;
            }
            if (strcmp(subType, "statvfs@openssh.com") == 0 || strcmp(subType, "fstatvfs@openssh.com") == 0) {
                // statvfs not supported in this libssh version (no sftp_reply_extended_reply)
                sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "statvfs not implemented");
            }
            else if (strcmp(subType, "fsync@openssh.com") == 0) {
                void* handle_data = sftp_handle(sftp, msg->handle);
                if (!handle_data) {
                    sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                    break;
                }
                if (FlushFileBuffers(static_cast<HANDLE>(handle_data))) {
                    sftp_reply_status(msg, SSH_FX_OK, "OK");
                } else {
                    sftp_reply_status(msg, SSH_FX_FAILURE, "Fsync failed");
                }
            }
            else if (strcmp(subType, "hardlink@openssh.com") == 0) {
                const char* existing = sftp_client_message_get_data(msg);
                if (!existing || !filename) {
                    sftp_reply_status(msg, SSH_FX_FAILURE, "Missing args");
                    break;
                }
                std::wstring wexisting = sftpPathToWin32(existing);
                std::wstring wnewlink = sftpPathToWin32(filename);
                if (CreateHardLinkW(wnewlink.c_str(), wexisting.c_str(), nullptr)) {
                    sftp_reply_status(msg, SSH_FX_OK, "OK");
                } else {
                    DWORD err = GetLastError();
                    uint32_t fxErr = SSH_FX_FAILURE;
                    if (err == ERROR_NOT_SAME_DEVICE) fxErr = SSH_FX_NOT_A_DIRECTORY;
                    else if (err == ERROR_ACCESS_DENIED) fxErr = SSH_FX_PERMISSION_DENIED;
                    else if (err == ERROR_FILE_EXISTS) fxErr = SSH_FX_FILE_ALREADY_EXISTS;
                    sftp_reply_status(msg, fxErr, "Hard link failed");
                }
            }
            else {
                sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Unknown extension");
            }
            break;
        }
        default:
            sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Unknown op");
            break;
        }
        sftp_client_message_free(msg);
    }

    sftp_server_free(sftp);
}

// ==================== Shell Handler ====================

void SshServer::handleShellChannel(ssh_session session, ssh_channel channel,
                                    int cols, int rows) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    HANDLE hPtyInRd = INVALID_HANDLE_VALUE;
    HANDLE hPtyInWr = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutRd = INVALID_HANDLE_VALUE;
    HANDLE hPtyOutWr = INVALID_HANDLE_VALUE;
    HPCON hPC = nullptr;
    PROCESS_INFORMATION pi = {};
    HANDLE hJob = nullptr;
    std::atomic<bool> shellRunning{true};
    wchar_t shellCmd[] = L"cmd.exe /d";
    bool usingConPty = false;
    HMODULE hKernel32 = nullptr;
    auto pCreatePC = (HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*))nullptr;
    auto pClosePC = (VOID(WINAPI*)(HPCON))nullptr;
    std::vector<char> buf(65536);
    std::vector<char> rbuf(65536);

    // Helper: OEMCP→UTF-8 (plain pipe fallback only)
    auto acpToUtf8 = [](const char* src, int srclen) -> std::string {
        if (srclen <= 0) return {};
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, srclen, nullptr, 0) > 0)
            return {src, (size_t)srclen};
        int wn = MultiByteToWideChar(CP_OEMCP, 0, src, srclen, nullptr, 0);
        if (wn <= 0) return {src, (size_t)srclen};
        std::wstring ws(wn, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, src, srclen, &ws[0], wn);
        int u8n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wn, nullptr, 0, nullptr, nullptr);
        if (u8n <= 0) return {src, (size_t)srclen};
        std::string u8(u8n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wn, &u8[0], u8n, nullptr, nullptr);
        return u8;
    };

    // Create pipes
    if (!CreatePipe(&hPtyOutRd, &hPtyOutWr, &sa, 65536)) {
        std::cerr << "[SSH Shell] CreatePipe output failed" << std::endl;
        goto cleanup;
    }
    if (!CreatePipe(&hPtyInRd, &hPtyInWr, &sa, 65536)) {
        std::cerr << "[SSH Shell] CreatePipe input failed" << std::endl;
        goto cleanup;
    }

    // ─── Try ConPTY (Windows 10 1809+) ──────────────────────────────────
    hKernel32 = GetModuleHandleW(L"kernel32.dll");
    pCreatePC = (HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*))
        GetProcAddress(hKernel32, "CreatePseudoConsole");
    pClosePC = (VOID(WINAPI*)(HPCON))
        GetProcAddress(hKernel32, "ClosePseudoConsole");

    if (pCreatePC && pClosePC && cols > 0 && rows > 0) {
        HRESULT hr = pCreatePC({(SHORT)cols, (SHORT)rows}, hPtyInRd, hPtyOutWr, 0, &hPC);
        if (SUCCEEDED(hr)) {
            usingConPty = true;

            // ConPTY has its own copies; close ours to prevent inheritance leaks
            CloseHandle(hPtyInRd); hPtyInRd = INVALID_HANDLE_VALUE;
            CloseHandle(hPtyOutWr); hPtyOutWr = INVALID_HANDLE_VALUE;

            STARTUPINFOEXW si = { sizeof(si) };
            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
            auto attrList = (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
            if (attrList) {
                InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
                UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                          hPC, sizeof(HPCON), nullptr, nullptr);
            }
            si.lpAttributeList = attrList;

            // bInheritHandles=FALSE: ConPTY manages child via attribute list, not pipe inheritance
            if (!CreateProcessW(nullptr, shellCmd, nullptr, nullptr, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT,
                                nullptr, nullptr, &si.StartupInfo, &pi)) {
                std::cerr << "[SSH Shell] CreateProcess: " << GetLastError() << std::endl;
                if (attrList) { DeleteProcThreadAttributeList(attrList); HeapFree(GetProcessHeap(), 0, attrList); }
                pClosePC(hPC); hPC = nullptr;
                goto cleanup;
            }
            if (attrList) { DeleteProcThreadAttributeList(attrList); HeapFree(GetProcessHeap(), 0, attrList); }
        }
    }

    // ─── Plain pipe fallback (ConPTY unavailable or failed) ──────────────
    if (!usingConPty) {
        // Manual inheritance for the two ends inherited by cmd.exe via HANDLE_LIST
        SetHandleInformation(hPtyInRd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(hPtyOutWr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        HANDLE inheritHandles[2] = { hPtyInRd, hPtyOutWr };
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        auto attrList = (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
        if (attrList) {
            InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize);
            UpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inheritHandles, sizeof(inheritHandles), nullptr, nullptr);
        }

        STARTUPINFOEXW si = { sizeof(si) };
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = hPtyInRd;
        si.StartupInfo.hStdOutput = hPtyOutWr;
        si.StartupInfo.hStdError = hPtyOutWr;
        si.lpAttributeList = attrList;

        if (!CreateProcessW(nullptr, shellCmd, nullptr, nullptr, TRUE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                            nullptr, nullptr, &si.StartupInfo, &pi)) {
            std::cerr << "[SSH Shell] CreateProcess: " << GetLastError() << std::endl;
            if (attrList) { DeleteProcThreadAttributeList(attrList); HeapFree(GetProcessHeap(), 0, attrList); }
            goto cleanup;
        }
        if (attrList) { DeleteProcThreadAttributeList(attrList); HeapFree(GetProcessHeap(), 0, attrList); }

        CloseHandle(hPtyInRd); hPtyInRd = INVALID_HANDLE_VALUE;
        CloseHandle(hPtyOutWr); hPtyOutWr = INVALID_HANDLE_VALUE;
    }

    // ─── Job object (common to both paths) ──────────────────────────────
    hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)))
            AssignProcessToJobObject(hJob, pi.hProcess);
    }
    CloseHandle(pi.hThread); pi.hThread = nullptr;

    // ─── I/O loop: raw byte forwarding, no interpretation ──────────────
    while (shellRunning && running_) {
        int rc = ssh_channel_read_nonblocking(channel, buf.data(), buf.size(), 0);
        if (rc > 0) {
            DWORD w = 0;
            if (!WriteFile(hPtyInWr, buf.data(), (DWORD)rc, &w, nullptr))
                std::cerr << "[SSH Shell] WriteFile input: err=" << GetLastError() << std::endl;
        } else if (rc == SSH_ERROR) {
            break;
        }

        DWORD n = 0;
        if (!PeekNamedPipe(hPtyOutRd, nullptr, 0, nullptr, &n, nullptr)) {
            DWORD err = GetLastError();
            if (err != ERROR_BROKEN_PIPE) {
                std::cerr << "[SSH Shell] PeekNamedPipe: " << err << std::endl;
                break;
            }
        } else if (n > 0) {
            DWORD read = 0;
            if (ReadFile(hPtyOutRd, rbuf.data(), (DWORD)min(rbuf.size(), (size_t)n), &read, nullptr) && read > 0) {
                const char* outData = rbuf.data();
                int outLen = (int)read;
                std::string u8;
                if (!usingConPty) {
                    u8 = acpToUtf8(rbuf.data(), (int)read);
                    outData = u8.data();
                    outLen = (int)u8.size();
                }
                if (ssh_channel_write(channel, outData, outLen) != outLen)
                    break;
            }
        }

        if (ssh_channel_is_eof(channel)) {
            std::cerr << "[SSH Shell] Channel EOF" << std::endl;
            break;
        }
        Sleep(10);
    }

cleanup:
    shellRunning = false;
    if (hPC && pClosePC) pClosePC(hPC);
    if (pi.hProcess) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (hJob) CloseHandle(hJob);
    if (hPtyInRd != INVALID_HANDLE_VALUE) CloseHandle(hPtyInRd);
    if (hPtyInWr != INVALID_HANDLE_VALUE) CloseHandle(hPtyInWr);
    if (hPtyOutRd != INVALID_HANDLE_VALUE) CloseHandle(hPtyOutRd);
    if (hPtyOutWr != INVALID_HANDLE_VALUE) CloseHandle(hPtyOutWr);
}
