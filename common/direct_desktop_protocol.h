#ifndef DIRECT_DESKTOP_PROTOCOL_H
#define DIRECT_DESKTOP_PROTOCOL_H

#define NOMINMAX
#include <cstdint>
#include <string>
#include <vector>

// Custom SSH subsystem name
#define DIRECT_DESKTOP_SUBSYSTEM "direct-desktop@jc-tool-c"

// Message types on the SSH custom channel
// Server → Client
enum DirectDesktopMsgS2C : uint8_t {
    DD_S2C_CREDENTIAL_REQUEST = 0x01,
    DD_S2C_LOGIN_RESULT       = 0x02,
    DD_S2C_DESKTOP_INFO       = 0x03,
    DD_S2C_ERROR              = 0x04,
    DD_S2C_USER_LIST          = 0x05,
};

// Client → Server
enum DirectDesktopMsgC2S : uint8_t {
    DD_C2S_CREDENTIAL_RESPONSE = 0x81,
    DD_C2S_SCREEN_OFF          = 0x82,
    DD_C2S_SHUTDOWN            = 0x83,
};

#pragma pack(push, 1)
struct DirectDesktopInfo {
    int32_t port;
    int32_t width;
    int32_t height;
};
#pragma pack(pop)

struct UserListEntry {
    std::string name;
    bool hasActiveSession;
};

inline std::vector<uint8_t> BuildCredentialRequest() {
    return { DD_S2C_CREDENTIAL_REQUEST };
}

inline std::vector<uint8_t> BuildLoginResult(bool success) {
    return { DD_S2C_LOGIN_RESULT, static_cast<uint8_t>(success ? 0 : 1) };
}

inline std::vector<uint8_t> BuildDesktopInfo(int port, int width, int height) {
    std::vector<uint8_t> msg(1 + sizeof(DirectDesktopInfo));
    msg[0] = DD_S2C_DESKTOP_INFO;
    auto* info = reinterpret_cast<DirectDesktopInfo*>(msg.data() + 1);
    info->port = port;
    info->width = width;
    info->height = height;
    return msg;
}

inline std::vector<uint8_t> BuildError(const std::string& err) {
    std::vector<uint8_t> msg(1 + err.size());
    msg[0] = DD_S2C_ERROR;
    if (!err.empty())
        memcpy(msg.data() + 1, err.data(), err.size());
    return msg;
}

inline std::vector<uint8_t> BuildUserList(const std::vector<UserListEntry>& entries) {
    size_t totalSize = 1; // type byte
    for (const auto& e : entries) {
        totalSize += 1 + e.name.size() + 1; // nameLen + name + activeFlag
    }
    std::vector<uint8_t> msg(totalSize);
    msg[0] = DD_S2C_USER_LIST;
    size_t pos = 1;
    for (const auto& e : entries) {
        uint8_t nameLen = static_cast<uint8_t>(e.name.size() > 255 ? 255 : e.name.size());
        msg[pos++] = nameLen;
        if (nameLen > 0) {
            memcpy(msg.data() + pos, e.name.data(), nameLen);
            pos += nameLen;
        }
        msg[pos++] = e.hasActiveSession ? 1 : 0;
    }
    return msg;
}

inline std::vector<UserListEntry> ParseUserList(const uint8_t* data, size_t len) {
    std::vector<UserListEntry> entries;
    if (!data || len < 1 || data[0] != DD_S2C_USER_LIST) return entries;
    size_t pos = 1;
    while (pos < len) {
        if (pos + 2 > len) break;
        uint8_t nameLen = data[pos++];
        if (pos + nameLen + 1 > len) break;
        UserListEntry e;
        if (nameLen > 0)
            e.name.assign(reinterpret_cast<const char*>(data + pos), nameLen);
        pos += nameLen;
        e.hasActiveSession = (data[pos++] != 0);
        entries.push_back(std::move(e));
    }
    return entries;
}

inline std::vector<uint8_t> BuildScreenOff() {
    return { DD_C2S_SCREEN_OFF };
}

inline bool ParseCredentialResponse(const uint8_t* data, size_t len,
                                     std::string& outUser, std::string& outPass) {
    if (!data || len < 3) return false;
    // data[0] = DD_C2S_CREDENTIAL_RESPONSE
    // data[1] = username_len
    // data[2..2+len-1] = username
    // after username: password_len
    // after that: password
    size_t pos = 1;
    uint8_t userLen = data[pos++];
    if (pos + userLen > len) return false;
    outUser.assign(reinterpret_cast<const char*>(data + pos), userLen);
    pos += userLen;
    if (pos >= len) return false;
    uint8_t passLen = data[pos++];
    if (pos + passLen > len) return false;
    outPass.assign(reinterpret_cast<const char*>(data + pos), passLen);
    return true;
}

// Named pipe used between EasyTierService and DirectDesktop
#define DIRECT_DESKTOP_READY_PIPE_PREFIX L"\\\\.\\pipe\\DD_Ready_"

#endif
