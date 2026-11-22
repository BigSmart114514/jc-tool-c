#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "ssh_server.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <libssh/sftp.h>
#include <libssh/callbacks.h>
#include <iostream>
#include <vector>
#include <map>
#include <set>
#include <cwctype>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <winioctl.h>
#include <intrin.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <sstream>
#include "../common/direct_desktop_protocol.h"
#include "../server/virtual_display.h"
#include "logging.h"

#pragma comment(lib, "userenv.lib")

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

// SFTP handles can refer to either an open file or a directory enumeration.
// Tagging them keeps SSH_FXP_CLOSE from calling CloseHandle() on a
// FindHandleData* and makes sure directory enumerations are FindClose()d.
struct SftpHandle {
    bool isDirectory;
    HANDLE file;             // valid when isDirectory == false
    FindHandleData* dir;     // valid when isDirectory == true
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

    // Connect dummy socket to unblock ssh_bind_accept() so the accept thread can exit
    if (port_ > 0) {
        SOCKET tmp = socket(AF_INET, SOCK_STREAM, 0);
        if (tmp != INVALID_SOCKET) {
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((u_short)port_);
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            connect(tmp, (sockaddr*)&addr, sizeof(addr));
            closesocket(tmp);
        }
    }

    if (acceptThread_.joinable())
        acceptThread_.join();

    if (sshbind_) {
        ssh_bind_free(sshbind_);
        sshbind_ = nullptr;
    }
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
        WriteLog("[SSH Server] Key exchange failed");
        ssh_free(session);
        return;
    }

    WriteLog("[SSH Server] Session authenticated");

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
                    WriteLog("[SSH Server] Failed to accept channel");
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
                WriteLog("[SSH Server] Shell request received, launching handler...");
                try {
                    handleShellChannel(session, currentChannel, ptyCols, ptyRows);
                } catch (const std::exception& e) {
                    WriteLog(std::string("[SSH Server] Shell exception: ") + e.what());
                } catch (...) {
                    WriteLog("[SSH Server] Shell unknown exception");
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
                    WriteLog("[SSH Server] SFTP request received");
                    try {
                        handleSftpChannel(session, currentChannel);
                    } catch (const std::exception& e) {
                        WriteLog(std::string("[SSH Server] SFTP exception: ") + e.what());
                    } catch (...) {
                        WriteLog("[SSH Server] SFTP unknown exception");
                    }
                    currentChannel = nullptr;
                    ssh_disconnect(session);
                    ssh_free(session);
                    return;
                } else if (sub && strcmp(sub, DIRECT_DESKTOP_SUBSYSTEM) == 0) {
                    ssh_message_channel_request_reply_success(msg);
                    WriteLog("[SSH Server] DirectDesktop subsystem request received");
                    try {
                        handleDirectDesktopChannel(session, currentChannel);
                    } catch (const std::exception& e) {
                        WriteLog(std::string("[SSH Server] DirectDesktop exception: ") + e.what());
                    } catch (...) {
                        WriteLog("[SSH Server] DirectDesktop unknown exception");
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

            auto* hnd = new SftpHandle{};
            hnd->isDirectory = false;
            hnd->file = h;
            ssh_string handle = sftp_handle_alloc(sftp, hnd);
            sftp_reply_handle(msg, handle);
            ssh_string_free(handle);
            break;
        }
        case SSH_FXP_CLOSE: {
            auto* hnd = static_cast<SftpHandle*>(sftp_handle(sftp, msg->handle));
            if (hnd) {
                if (hnd->isDirectory) {
                    if (hnd->dir) {
                        if (!hnd->dir->isDrivesList && hnd->dir->hFind != INVALID_HANDLE_VALUE)
                            FindClose(hnd->dir->hFind);
                        delete hnd->dir;
                    }
                } else if (hnd->file != INVALID_HANDLE_VALUE) {
                    CloseHandle(hnd->file);
                }
                delete hnd;
                sftp_handle_remove(sftp, hnd);
            }
            sftp_reply_status(msg, SSH_FX_OK, "OK");
            break;
        }
        case SSH_FXP_READ: {
            auto* hnd = static_cast<SftpHandle*>(sftp_handle(sftp, msg->handle));
            if (!hnd || hnd->isDirectory) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            HANDLE h = hnd->file;
            LARGE_INTEGER pos;
            pos.QuadPart = (LONGLONG)msg->offset;
            if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Seek failed");
                break;
            }
            uint32_t len = msg->len;
            std::vector<char> buf(len > 65536 ? 65536 : len);
            DWORD read = 0;
            if (ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr)) {
                if (read == 0) {
                    sftp_reply_status(msg, SSH_FX_EOF, "EOF");
                } else {
                    sftp_reply_data(msg, buf.data(), read);
                }
            } else {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Read failed");
            }
            break;
        }
        case SSH_FXP_WRITE: {
            auto* hnd = static_cast<SftpHandle*>(sftp_handle(sftp, msg->handle));
            if (!hnd || hnd->isDirectory) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            HANDLE h = hnd->file;
            LARGE_INTEGER pos;
            pos.QuadPart = (LONGLONG)msg->offset;
            if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Seek failed");
                break;
            }
            // libssh does not populate msg->len for SSH_FXP_WRITE; the payload
            // lives in msg->data and its length must be queried from the string.
            const char* data = static_cast<const char*>(ssh_string_data(msg->data));
            size_t total = ssh_string_len(msg->data);
            size_t done = 0;
            bool ok = (data != nullptr);
            while (ok && done < total) {
                DWORD chunk = (DWORD)((total - done) > 0x10000 ? 0x10000 : (total - done));
                DWORD written = 0;
                if (!WriteFile(h, data + done, chunk, &written, nullptr) || written == 0) {
                    ok = false;
                    break;
                }
                done += written;
            }
            if (ok) {
                sftp_reply_status(msg, SSH_FX_OK, "OK");
            } else {
                sftp_reply_status(msg, mapWin32ErrorToFx(GetLastError()), "Write failed");
            }
            break;
        }
        case SSH_FXP_FSTAT: {
            auto* hnd = static_cast<SftpHandle*>(sftp_handle(sftp, msg->handle));
            if (!hnd || hnd->isDirectory) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            HANDLE h = hnd->file;
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
                dirInfo->hFind = INVALID_HANDLE_VALUE;
                dirInfo->driveMask = GetLogicalDrives();
                dirInfo->driveIndex = -1;
                dirInfo->first = true;
                auto* hnd = new SftpHandle{};
                hnd->isDirectory = true;
                hnd->dir = dirInfo;
                ssh_string handle = sftp_handle_alloc(sftp, hnd);
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
                auto* hnd = new SftpHandle{};
                hnd->isDirectory = true;
                hnd->dir = dirInfo;
                ssh_string handle = sftp_handle_alloc(sftp, hnd);
                sftp_reply_handle(msg, handle);
                ssh_string_free(handle);
            }
            break;
        }
        case SSH_FXP_READDIR: {
            auto* hnd = static_cast<SftpHandle*>(sftp_handle(sftp, msg->handle));
            if (!hnd || !hnd->isDirectory || !hnd->dir) {
                sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle");
                break;
            }
            FindHandleData* dirInfo = hnd->dir;

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

// ==================== Environment block merging ====================

static std::map<std::wstring, std::wstring> parseEnvBlock(const wchar_t* block) {
    std::map<std::wstring, std::wstring> result;
    if (!block) return result;
    const wchar_t* p = block;
    while (*p) {
        const wchar_t* eq = wcschr(p, L'=');
        if (eq && eq > p) {
            std::wstring key(p, eq - p);
            std::wstring value(eq + 1);
            for (auto& ch : key) ch = towupper(ch);
            result[std::move(key)] = std::move(value);
        }
        p += wcslen(p) + 1;
    }
    return result;
}


static std::vector<std::wstring> splitPath(const std::wstring& path) {
    std::vector<std::wstring> entries;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring entry = path.substr(start, end - start);
        while (!entry.empty() && entry.back() == L'\\')
            entry.pop_back();
        if (!entry.empty())
            entries.push_back(std::move(entry));
        start = end + 1;
    }
    return entries;
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
    wchar_t shellCmd[] = L"powershell.exe -NoLogo";
    bool usingConPty = false;
    HMODULE hConPty = nullptr;
    HMODULE hDll = nullptr;
    auto pCreatePC = (HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*))nullptr;
    auto pClosePC = (VOID(WINAPI*)(HPCON))nullptr;
    auto pResizePC = (HRESULT(WINAPI*)(HPCON, COORD))nullptr;
    std::vector<char> buf(65536);
    std::vector<char> rbuf(65536);
    std::map<std::wstring, std::wstring> userEnvMap;

    // Parse user environment into a map for later application to child process
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    WriteLog(std::string("[SSH Shell] sessionId=") + std::to_string(sessionId));
    if (sessionId != 0xFFFFFFFF) {
        HANDLE hUserToken = NULL;
        if (WTSQueryUserToken(sessionId, &hUserToken)) {
            void* userEnv = NULL;
            if (CreateEnvironmentBlock(&userEnv, hUserToken, FALSE)) {
                userEnvMap = parseEnvBlock((const wchar_t*)userEnv);
                WriteLog(std::string("[SSH Shell] user env parsed: ") + std::to_string(userEnvMap.size()) + " vars");
                DestroyEnvironmentBlock(userEnv);
            } else {
                WriteLog("[SSH Shell] CreateEnvironmentBlock failed");
            }
            CloseHandle(hUserToken);
        } else {
            WriteLog("[SSH Shell] WTSQueryUserToken failed");
        }
    } else {
        WriteLog("[SSH Shell] no active console session");
    }

    // Create pipes
    if (!CreatePipe(&hPtyOutRd, &hPtyOutWr, &sa, 65536)) {
        WriteLog("[SSH Shell] CreatePipe output failed");
        goto cleanup;
    }
    if (!CreatePipe(&hPtyInRd, &hPtyInWr, &sa, 65536)) {
        WriteLog("[SSH Shell] CreatePipe input failed");
        goto cleanup;
    }

    // ─── Try ConPTY (conpty.dll first, fallback to kernel32) ────────────
    hConPty = LoadLibraryW(L"conpty.dll");
    hDll = hConPty ? hConPty : GetModuleHandleW(L"kernel32.dll");
    pCreatePC = (HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*))
        GetProcAddress(hDll, "CreatePseudoConsole");
    pClosePC = (VOID(WINAPI*)(HPCON))
        GetProcAddress(hDll, "ClosePseudoConsole");
    pResizePC = (HRESULT(WINAPI*)(HPCON, COORD))
        GetProcAddress(hDll, "ResizePseudoConsole");
    {
        std::string log = "[SSH Shell] ConPTY from: ";
        log += (hConPty ? "conpty.dll" : "kernel32.dll");
        WriteLog(log);
    }

    if (pCreatePC && pClosePC && cols > 0 && rows > 0) {
        HRESULT hr = pCreatePC({(SHORT)cols, (SHORT)rows}, hPtyInRd, hPtyOutWr, 0, &hPC);
        {
            char hex[16]; sprintf_s(hex, "%08lx", (long)hr);
            WriteLog(std::string("[SSH Shell] CreatePseudoConsole hr=0x") + hex);
        }
        if (SUCCEEDED(hr)) {
            usingConPty = true;

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

            // Apply user env to current process so child inherits it
            {
                struct EnvEntry { std::wstring key; std::wstring value; bool existed; };
                std::vector<EnvEntry> envBackup;
                for (const auto& [key, val] : userEnvMap) {
                    wchar_t buf[32767];
                    DWORD ret = GetEnvironmentVariableW(key.c_str(), buf, 32767);
                    bool existed = (ret > 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND);
                    envBackup.push_back({key, existed ? std::wstring(buf) : L"", existed});
                    if (key == L"PATH") {
                        std::wstring curPath = (ret > 0) ? buf : L"";
                        auto sysPath = splitPath(curPath);
                        auto userPath = splitPath(val);
                        std::set<std::wstring> pathSet;
                        for (auto& e : sysPath) {
                            std::wstring u = e;
                            for (auto& ch : u) ch = towupper(ch);
                            pathSet.insert(std::move(u));
                        }
                        std::wstring merged;
                        for (size_t i = 0; i < sysPath.size(); ++i) {
                            if (i > 0) merged += L';';
                            merged += sysPath[i];
                        }
                        for (auto& entry : userPath) {
                            std::wstring u = entry;
                            for (auto& ch : u) ch = towupper(ch);
                            if (pathSet.find(u) == pathSet.end()) {
                                if (!merged.empty()) merged += L';';
                                merged += entry;
                                pathSet.insert(std::move(u));
                            }
                        }
                        SetEnvironmentVariableW(L"PATH", merged.c_str());
                    } else {
                        SetEnvironmentVariableW(key.c_str(), val.c_str());
                    }
                }
                WriteLog(std::string("[SSH Shell] Applied ") + std::to_string(userEnvMap.size()) + " user env vars");

                // bInheritHandles=FALSE: ConPTY manages child via attribute list, not pipe inheritance
                if (!CreateProcessW(nullptr, shellCmd, nullptr, nullptr, FALSE,
                                    EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP,
                                    nullptr, nullptr, &si.StartupInfo, &pi)) {
                    DWORD gle = GetLastError();
                    WriteLog(std::string("[SSH Shell] CreateProcessW failed gle=") + std::to_string(gle));
                    for (const auto& e : envBackup) {
                        SetEnvironmentVariableW(e.key.c_str(), e.existed ? e.value.c_str() : nullptr);
                    }
                    if (attrList) { DeleteProcThreadAttributeList(attrList); HeapFree(GetProcessHeap(), 0, attrList); }
                    pClosePC(hPC); hPC = nullptr;
                    goto cleanup;
                }
                WriteLog("[SSH Shell] CreateProcessW succeeded");
                for (const auto& e : envBackup) {
                    SetEnvironmentVariableW(e.key.c_str(), e.existed ? e.value.c_str() : nullptr);
                }
                WriteLog("[SSH Shell] Restored original env");
            }
            if (attrList) { DeleteProcThreadAttributeList(attrList); HeapFree(GetProcessHeap(), 0, attrList); }
            CloseHandle(hPtyInRd); hPtyInRd = INVALID_HANDLE_VALUE;
            CloseHandle(hPtyOutWr); hPtyOutWr = INVALID_HANDLE_VALUE;
        }
    }

    // ─── ConPTY is required; no plain pipe fallback ─────────────────────
    if (!usingConPty) {
        WriteLog("[SSH Shell] ConPTY unavailable or failed");
        goto cleanup;
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

    // Make session non-blocking so we can poll for window-change messages
    ssh_set_blocking(session, 0);

    WriteLog("[SSH Shell] Successfully entered I/O loop!");

    while (shellRunning && running_) {
        // ───【轮询：窗口大小变化】───
        ssh_message msg;
        while ((msg = ssh_message_get(session)) != nullptr) {
            if (ssh_message_type(msg) == SSH_REQUEST_CHANNEL &&
                ssh_message_subtype(msg) == SSH_CHANNEL_REQUEST_WINDOW_CHANGE) {
                int newCols = ssh_message_channel_request_pty_width(msg);
                int newRows = ssh_message_channel_request_pty_height(msg);
                if (pResizePC) {
                    pResizePC(hPC, {(SHORT)newCols, (SHORT)newRows});
                    WriteLog(std::string("[SSH Shell] Resized to ") + std::to_string(newCols) + "x" + std::to_string(newRows));
                }
                ssh_message_channel_request_reply_success(msg);
            } else {
                ssh_message_reply_default(msg);
            }
            ssh_message_free(msg);
        }

        // ───【第一道防线：Windows Terminal 哲学，只看进程死没死】───
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            WriteLog(std::string("[SSH Shell] Shell exited. code=") + std::to_string(exitCode));
            break; // 这是唯一合法的退出理由！
        }

        // ───【处理输入（SSH 读 -> 管道 写）】───
        int rc = ssh_channel_read_nonblocking(channel, buf.data(), buf.size(), 0);
        if (rc > 0) {
            DWORD w = 0;
            if (!WriteFile(hPtyInWr, buf.data(), (DWORD)rc, &w, nullptr)) {
                DWORD err = GetLastError();
                // 拦截 232 和 109：此时管道正在抖动，千万不要 break！
                if (err == ERROR_NO_DATA || err == ERROR_BROKEN_PIPE) {
                    Sleep(10);
                    continue; // 丢弃当前写入，等待管道恢复
                } else {
                    WriteLog(std::string("[SSH Shell] WriteFile fatal err=") + std::to_string(err));
                    break;
                }
            }
        } else if (rc == SSH_ERROR) {
            WriteLog("[SSH Shell] SSH client disconnected.");
            break; // 客户端主动断开
        }

        // ───【处理输出（管道 读 -> SSH 写）】───
        DWORD n = 0;
        if (!PeekNamedPipe(hPtyOutRd, nullptr, 0, nullptr, &n, nullptr)) {
            DWORD err = GetLastError();
            // 拦截 232 和 109：此时管道正在抖动，千万不要 break！
            if (err == ERROR_NO_DATA || err == ERROR_BROKEN_PIPE) {
                Sleep(10);
                continue; // 忽略报错，等待管道恢复并吐出新的提示符
            } else {
                WriteLog(std::string("[SSH Shell] PeekNamedPipe fatal err=") + std::to_string(err));
                break;
            }
        }

        // 正常读取并转发给 SSH
        if (n > 0) {
            DWORD read = 0;
            if (ReadFile(hPtyOutRd, rbuf.data(), (DWORD)min(rbuf.size(), (size_t)n), &read, nullptr) && read > 0) {
                if (ssh_channel_write(channel, rbuf.data(), (int)read) != (int)read) {
                    WriteLog("[SSH Shell] SSH channel write failed.");
                    break;
                }
            }
        }

        if (ssh_channel_is_eof(channel)) {
            WriteLog("[SSH Shell] Channel EOF");
            break;
        }
        
        Sleep(10); // 降低 CPU 占用
    }
    WriteLog("[SSH Shell] Exiting I/O loop normally.");

cleanup:
    WriteLog(std::string("[SSH Shell] Entering cleanup. LastError=") + std::to_string(GetLastError()));
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

// ─── Helper: get path to RemoteControl.exe in same dir as EasyTierService ──
static std::wstring GetRemoteControlPath() {
    wchar_t path[MAX_PATH + 1] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = L'\0';
    return std::wstring(path) + L"RemoteControl.exe";
}

// ─── Security attributes for cross-session access ─────────────────────────
static SECURITY_ATTRIBUTES MakeAllowAllSA() {
    static SECURITY_DESCRIPTOR sd = {};
    static bool init = false;
    if (!init) {
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        init = true;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    return sa;
}

// ─── Helper: pick a dynamic port ──────────────────────────────────────────
// 注意：这里只是"挑一个当前空闲的端口"，关闭 socket 后它可能被别的进程抢走。
// 真正的可用性由子进程 bind/listen 成功后再上报，父进程不会提前认为它就绪。
static int PickDynamicPort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return 12346;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return 12346;
    }
    int len = sizeof(addr);
    getsockname(s, (sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    closesocket(s);
    return port;
}

// ─── Port persistence helpers (per-user) ──────────────────────────────────
static std::wstring GetPortFilePath(const std::string& username) {
    wchar_t progData[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PROGRAMDATA", progData, MAX_PATH))
        wcscpy_s(progData, L"C:\\ProgramData");
    std::wstring dir = std::wstring(progData) + L"\\EasyTier";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\dd_" + widen(username.c_str()) + L".port";
}

static void SaveRunningPort(const std::string& username, int port) {
    std::wstring path = GetPortFilePath(username);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string data = std::to_string(port);
    DWORD written;
    WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
}

static int LoadRunningPort(const std::string& username) {
    std::wstring path = GetPortFilePath(username);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32] = {};
    DWORD read;
    if (!ReadFile(h, buf, (DWORD)sizeof(buf) - 1, &read, nullptr)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    return atoi(buf);
}

static void RemoveRunningPort(const std::string& username) {
    DeleteFileW(GetPortFilePath(username).c_str());
}

// ─── Enumerate users with active sessions ────────────────────────────────
static std::vector<UserListEntry> EnumerateUsers() {
    std::vector<UserListEntry> entries;
    std::map<std::string, bool> userMap;

    WTS_SESSION_INFO* sessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount))
        return entries;

    for (DWORD i = 0; i < sessionCount; ++i) {
        if (sessions[i].State == WTSIdle || sessions[i].State == WTSListen)
            continue;
        wchar_t* userName = nullptr;
        DWORD userNameSize = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId,
                                         WTSUserName, &userName, &userNameSize)) {
            if (userName && *userName) {
                std::string name = narrow(userName);
                bool active = (sessions[i].State == WTSActive);
                auto it = userMap.find(name);
                if (it == userMap.end())
                    userMap[name] = active;
                else if (active)
                    it->second = true;
            }
            if (userName) WTSFreeMemory(userName);
        }
    }
    WTSFreeMemory(sessions);

    for (const auto& pair : userMap)
        entries.push_back({pair.first, pair.second});
    return entries;
}

// ─── DirectDesktop handler ────────────────────────────────────────────────
void SshServer::handleDirectDesktopChannel(ssh_session session, ssh_channel channel) {
    WriteLog("[DirectDesktop] Handler started");
    ssh_set_blocking(session, 0);

    std::vector<char> buf(4096);
    std::vector<uint8_t> recvBuf;
    std::string username, password;
    DWORD deadline;

    // ── 1. Enumerate users & send list ──────────────────────────────────────
    auto users = EnumerateUsers();
    auto userListMsg = BuildUserList(users);
    if (ssh_channel_write(channel, userListMsg.data(), (int)userListMsg.size()) != (int)userListMsg.size()) {
        WriteLog("[DirectDesktop] Failed to send user list");
        return;
    }
    WriteLog(std::string("[DirectDesktop] Sent ") + std::to_string(users.size()) + " users");

    // ── 2. Wait for login request (60s timeout) ─────────────────────────────
    deadline = GetTickCount() + 60000;
    bool gotCreds = false;
    while (running_ && GetTickCount() < deadline) {
        int rc = ssh_channel_read_nonblocking(channel, buf.data(), (int)buf.size(), 0);
        if (rc > 0) {
            recvBuf.insert(recvBuf.end(), buf.data(), buf.data() + rc);
            if (recvBuf.size() >= 3 && recvBuf[0] == DD_C2S_CREDENTIAL_RESPONSE) {
                if (ParseCredentialResponse(recvBuf.data(), recvBuf.size(), username, password)) {
                    gotCreds = true;
                    break;
                }
            }
        } else if (rc == SSH_ERROR) {
            WriteLog("[DirectDesktop] SSH error reading login");
            return;
        }
        ssh_message msg;
        while ((msg = ssh_message_get(session)) != nullptr) ssh_message_free(msg);
        if (ssh_channel_is_eof(channel)) {
            WriteLog("[DirectDesktop] Channel EOF before login");
            return;
        }
        Sleep(100);
    }
    if (!gotCreds) {
        WriteLog("[DirectDesktop] Login timeout");
        auto err = BuildError("Login exchange timeout");
        ssh_channel_write(channel, err.data(), (int)err.size());
        return;
    }
    WriteLog(std::string("[DirectDesktop] Login for user: ") + username);

    // ── 3. Check if already running (per-user event) ────────────────────────
    std::wstring runningEventName = L"Global\\DD_Running_" + widen(username.c_str());
    HANDLE hRunningEvent = OpenEventW(SYNCHRONIZE, FALSE, runningEventName.c_str());
    bool isReused = false;

    if (hRunningEvent) {
        int existingPort = LoadRunningPort(username);
        if (existingPort > 0) {
            WriteLog(std::string("[DirectDesktop] Reusing session on port ") + std::to_string(existingPort));
            auto result = BuildLoginResult(true);
            ssh_channel_write(channel, result.data(), (int)result.size());
            auto info = BuildDesktopInfo(existingPort, 0, 0);
            ssh_channel_write(channel, info.data(), (int)info.size());
            isReused = true;
        } else {
            WriteLog("[DirectDesktop] Running event stale, starting fresh");
            CloseHandle(hRunningEvent);
            hRunningEvent = nullptr;
        }
    }

    // ── 4. Launch new session (if not reused) ───────────────────────────────
    HANDLE hProcess = nullptr;
    std::wstring virtualOutput;
    bool virtualDisplayEnabled = false;

    if (!isReused) {
        // 4a. Authenticate & get token
        HANDLE hLaunchToken = nullptr;
        bool authOk = false;

        // Find if user has an active session
        WTS_SESSION_INFO* sessions = nullptr;
        DWORD sessionCount = 0;
        bool userHasSession = false;
        DWORD userSessionId = 0xFFFFFFFF;
        if (WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount)) {
            for (DWORD i = 0; i < sessionCount; ++i) {
                wchar_t* u = nullptr;
                DWORD us = 0;
                if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId,
                                                 WTSUserName, &u, &us)) {
                    if (u && *u && narrow(u) == username) {
                        userSessionId = sessions[i].SessionId;
                        if (sessions[i].State == WTSActive || sessions[i].State == WTSConnected) {
                            userHasSession = true;
                        }
                    }
                    if (u) WTSFreeMemory(u);
                }
            }
            WTSFreeMemory(sessions);
        }

        if (userHasSession) {
            // User already logged in — get their token
            if (WTSQueryUserToken(userSessionId, &hLaunchToken)) {
                authOk = true;
                WriteLog("[DirectDesktop] User has active session, using WTS token");
            }
        } else if (!password.empty()) {
            // User not logged in — validate password via LogonUser
            HANDLE hLogonToken = nullptr;
            if (LogonUserW(widen(username.c_str()).c_str(), L".", widen(password.c_str()).c_str(),
                           LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &hLogonToken)) {
                // Try to assign to console session
                DWORD csid = WTSGetActiveConsoleSessionId();
                if (csid != 0xFFFFFFFF)
                    SetTokenInformation(hLogonToken, TokenSessionId, &csid, sizeof(csid));
                hLaunchToken = hLogonToken;
                authOk = true;
                WriteLog("[DirectDesktop] User not logged in, LogonUser OK");
            } else {
                DWORD gle = GetLastError();
                WriteLog(std::string("[DirectDesktop] LogonUser failed: ") + std::to_string(gle));
            }
        }

        if (!authOk) {
            WriteLog("[DirectDesktop] Auth failed for " + username);
            auto result = BuildLoginResult(false);
            ssh_channel_write(channel, result.data(), (int)result.size());
            return;
        }

        WriteLog("[DirectDesktop] Auth OK");
        auto result = BuildLoginResult(true);
        ssh_channel_write(channel, result.data(), (int)result.size());

        // 4b. Enable virtual display
        virtualDisplayEnabled = EnableVirtualDisplay(virtualOutput);
        if (!virtualDisplayEnabled) {
            WriteLog("[DirectDesktop] Virtual display unavailable, starting with the existing desktop");
        } else if (virtualOutput.empty()) {
            WriteLog("[DirectDesktop] Looking for the virtual output in the user session");
            virtualOutput = L"auto-virtual";
        }
        WriteLog("[DirectDesktop] Virtual display: " + narrow(virtualOutput));

        // 4c. Pick port & create ready pipe
        int desktopPort = PickDynamicPort();
        DWORD pid = GetCurrentProcessId();
        SECURITY_ATTRIBUTES sa = MakeAllowAllSA();

        std::wstring pipeName = std::wstring(DIRECT_DESKTOP_READY_PIPE_PREFIX)
                                + std::to_wstring(pid) + L"_" + std::to_wstring(desktopPort);
        HANDLE hReadyPipe = CreateNamedPipeW(pipeName.c_str(),
            PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
            1, 4096, 4096, 30000, &sa);

        // 4d. Create per-user running event
        hRunningEvent = CreateEventW(&sa, TRUE, FALSE, runningEventName.c_str());

        if (hReadyPipe == INVALID_HANDLE_VALUE || !hRunningEvent) {
            WriteLog("[DirectDesktop] Pipe/event creation failed");
            auto err = BuildError("Internal error");
            ssh_channel_write(channel, err.data(), (int)err.size());
            if (hRunningEvent) CloseHandle(hRunningEvent);
            if (hReadyPipe != INVALID_HANDLE_VALUE) CloseHandle(hReadyPipe);
            if (hLaunchToken) CloseHandle(hLaunchToken);
            if (virtualDisplayEnabled) DisableVirtualDisplay();
            return;
        }

        // 4e. Launch RemoteControl.exe
        std::wstring exePath = GetRemoteControlPath();
        std::wstringstream cmdLine;
        cmdLine << L"\"RemoteControl.exe\" --direct-desktop --port " << desktopPort
                << L" --ready-pipe \"" << pipeName << L"\""
                << L" --exit-event \"" << runningEventName << L"\"";
        if (!virtualOutput.empty())
            cmdLine << L" --capture-output \"" << virtualOutput << L"\"";
        std::wstring command = cmdLine.str();

        STARTUPINFOW si = { sizeof(si) };
        si.lpDesktop = L"winsta0\\default";
        PROCESS_INFORMATION pi = {};
        BOOL created = CreateProcessAsUserW(hLaunchToken,
            exePath.c_str(), command.data(),
            nullptr, nullptr, FALSE,
            CREATE_NEW_CONSOLE | CREATE_NO_WINDOW,
            nullptr, nullptr, &si, &pi);
        DWORD gle = created ? 0 : GetLastError();
        CloseHandle(hLaunchToken);

        if (!created) {
            WriteLog(std::string("[DirectDesktop] CreateProcessAsUser failed: ") + std::to_string(gle));
            auto err = BuildError("Failed to launch RemoteControl");
            ssh_channel_write(channel, err.data(), (int)err.size());
            CloseHandle(hRunningEvent);
            CloseHandle(hReadyPipe);
            if (virtualDisplayEnabled) DisableVirtualDisplay();
            return;
        }
        CloseHandle(pi.hThread);
        hProcess = pi.hProcess;

        // 4f. Wait for the child's status report.
        //
        // 同时监听三件事：状态管道、子进程退出、超时。
        // 之前用 PIPE_NOWAIT，却把 ConnectNamedPipe 的返回值当成"已连接"，
        // 未连接时返回的是 ERROR_PIPE_LISTENING，于是永远不去读，
        // 就算子进程早就写了状态也只能白等到超时。
        // 现在用 PeekNamedPipe 判断有没有数据，不再依赖连接状态标志。
        bool ready = false;
        int reportedWidth = 0;
        int reportedHeight = 0;
        std::string childStatus;
        DWORD lastPeekError = 0;
        ConnectNamedPipe(hReadyPipe, nullptr);
        const ULONGLONG readyDeadline = GetTickCount64() + 20000;
        while (GetTickCount64() < readyDeadline) {
            DWORD available = 0;
            if (!PeekNamedPipe(hReadyPipe, nullptr, 0, nullptr, &available, nullptr)) {
                const DWORD peekError = GetLastError();
                if (peekError == ERROR_BROKEN_PIPE) break;
                // 未连接/尚未写入都属于"还没到"，不是错误，继续等。
                lastPeekError = peekError;
            } else if (available > 0) {
                char pipeBuf[256] = {};
                DWORD read = 0;
                if (ReadFile(hReadyPipe, pipeBuf, sizeof(pipeBuf) - 1, &read, nullptr) && read > 0) {
                    childStatus.assign(pipeBuf, read);
                    while (!childStatus.empty() &&
                           (childStatus.back() == '\r' || childStatus.back() == '\n' ||
                            childStatus.back() == '\0'))
                        childStatus.pop_back();
                    if (childStatus.rfind("OK ", 0) == 0) {
                        std::istringstream ss(childStatus.substr(3));
                        int reportedPort = 0;
                        ss >> reportedPort >> reportedWidth >> reportedHeight;
                        if (reportedPort > 0) {
                            desktopPort = reportedPort;
                            ready = true;
                        }
                    }
                    break;
                }
            }

            if (WaitForSingleObject(hProcess, 50) != WAIT_TIMEOUT) break;
        }
        CloseHandle(hReadyPipe);
        if (!ready && childStatus.empty() && lastPeekError)
            WriteLog("[DirectDesktop] Status pipe last error: " + std::to_string(lastPeekError));

        if (!ready) {
            DWORD exitCode = STILL_ACTIVE;
            const bool exited = GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE;
            WriteLog("[DirectDesktop] RemoteControl did not report ready"
                     + std::string(exited ? ", exit=" + std::to_string(exitCode) : ", still running")
                     + (childStatus.empty() ? std::string() : ", status=" + childStatus));

            std::string reason = "desktop process did not start";
            if (childStatus.rfind("ERR ", 0) == 0) reason = childStatus.substr(4);
            else if (exited) reason = "desktop process exited with code " + std::to_string(exitCode);

            SetEvent(hRunningEvent);
            TerminateProcess(hProcess, 1);
            CloseHandle(hProcess); hProcess = nullptr;
            CloseHandle(hRunningEvent); hRunningEvent = nullptr;
            RemoveRunningPort(username);
            auto err = BuildError(reason + " (see C:\\ProgramData\\EasyTier\\desktop.log)");
            ssh_channel_write(channel, err.data(), (int)err.size());
            if (virtualDisplayEnabled) DisableVirtualDisplay();
            return;
        }
        WriteLog("[DirectDesktop] RemoteControl ready, status: " + childStatus);

        // 4g. Save port & send desktop info
        SaveRunningPort(username, desktopPort);
        auto info = BuildDesktopInfo(desktopPort, reportedWidth, reportedHeight);
        ssh_channel_write(channel, info.data(), (int)info.size());
        WriteLog(std::string("[DirectDesktop] Streaming on port ") + std::to_string(desktopPort));
    }

    // ── 5. Monitor channel (process, screen-off, EOF) ───────────────────────
    while (running_) {
        if (hProcess && WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
            WriteLog("[DirectDesktop] RemoteControl process exited");
            break;  // process died → cleanup
        }
        if (ssh_channel_is_eof(channel)) {
            WriteLog("[DirectDesktop] Client disconnected");
            if (isReused) goto keep_alive;  // don't own the session
            break;  // owner → cleanup
        }

        int rc = ssh_channel_read_nonblocking(channel, buf.data(), (int)buf.size(), 0);
        if (rc > 0) {
            if (buf[0] == DD_C2S_SCREEN_OFF) {
                WriteLog("[DirectDesktop] Screen-off request via SSH (handled via TCP)");
            } else if (buf[0] == DD_C2S_SHUTDOWN) {
                WriteLog("[DirectDesktop] Shutdown requested via SSH");
                break;  // explicit shutdown → cleanup
            }
        } else if (rc == SSH_ERROR) {
            WriteLog("[DirectDesktop] Channel error");
            if (isReused) goto keep_alive;
            break;
        }

        ssh_message msg;
        while ((msg = ssh_message_get(session)) != nullptr) ssh_message_free(msg);
        Sleep(500);
    }

    // ── 6. Cleanup (process exit or explicit shutdown) ───────────────────────
    WriteLog("[DirectDesktop] Cleaning up...");
    if (hRunningEvent) {
        SetEvent(hRunningEvent);
        if (hProcess) {
            WaitForSingleObject(hProcess, 5000);
            TerminateProcess(hProcess, 0);
            CloseHandle(hProcess);
        }
        RemoveRunningPort(username);
        CloseHandle(hRunningEvent);
    } else if (hProcess) {
        CloseHandle(hProcess);
    }
    if (virtualDisplayEnabled) DisableVirtualDisplay();
    WriteLog("[DirectDesktop] Cleanup complete");
    return;

    // ── Keep-alive path (client disconnected, process stays) ─────────────────
keep_alive:
    WriteLog("[DirectDesktop] Session preserved for reconnection");
    // Don't signal event, don't kill process, don't remove port, don't disable display
    return;
}
