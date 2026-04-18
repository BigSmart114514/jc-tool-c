
#ifndef PROTOCOL_H
#define PROTOCOL_H

#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <functional>

#pragma comment(lib, "ws2_32.lib")

// ==================== 配置 ====================
namespace Config {
    constexpr int DEFAULT_DESKTOP_PORT = 12345;
    constexpr int DEFAULT_FILE_PORT = 12346;
    constexpr int BUFFER_SIZE = 65536;
    constexpr int CRF = 28;
    constexpr int FPS = 30;
    constexpr int KEYFRAME_INTERVAL = 120;
}

// ==================== 服务类型 ====================
enum class ServiceType : uint8_t {
    Desktop = 1,
    FileManager = 2
};

// ==================== 远程桌面消息 ====================
namespace Desktop {
    enum class MsgType : uint8_t {
        VideoFrame      = 0x01,
        InputEvent      = 0x02,
        ScreenInfo      = 0x03,
        ClientReady     = 0x04,
        KeyframeRequest = 0x05,
        StreamConfig    = 0x06  // 新增：流配置
    };

    #pragma pack(push, 1)
    struct InputEvent {
        int32_t type;   // 0=鼠标, 1=键盘
        int32_t x;
        int32_t y;
        int32_t key;    // 鼠标: 0=移动,1-6=按键,7=滚轮
        int32_t flags;
    };

    struct ScreenInfo {
        int32_t width;
        int32_t height;
    };
    // --- 新增流配置结构体 ---
    struct StreamConfig {
        int32_t width;               // 指定的长（宽）
        int32_t fps;                 // 指定的FPS
        int32_t keyframeIntervalSec; // 指定的关键帧间隔（秒）
    };
    #pragma pack(pop)
}

// ==================== 文件管理消息 ====================
namespace FileManager {
    enum class MsgType : uint8_t {
        ListDrives      = 0x10,
        ListDir         = 0x11,
        DownloadReq     = 0x12,
        DownloadData    = 0x13,
        UploadReq       = 0x14,
        UploadData      = 0x15,
        Delete          = 0x16,
        CreateDir       = 0x17,
        Response        = 0x1F
    };

    enum class FileType : uint8_t {
        File      = 0,
        Directory = 1,
        Drive     = 2
    };

    enum class Status : uint8_t {
        OK            = 0,
        Error         = 1,
        NotFound      = 2,
        AccessDenied  = 3,
        Transferring  = 4,
        Complete      = 5
    };

    #pragma pack(push, 1)
    struct FileEntry {
        wchar_t name[260];
        uint8_t type;
        uint8_t reserved[3];
        uint64_t size;
        uint64_t modifyTime;
    };

    struct ListHeader {
        uint8_t status;
        uint8_t reserved[3];
        uint32_t count;
    };

    struct TransferHeader {
        uint8_t status;
        uint8_t reserved[3];
        uint32_t chunkSize;
        uint64_t totalSize;
        uint64_t offset;
    };
    #pragma pack(pop)
}

// ==================== 二进制数据 ====================
using BinaryData = std::vector<uint8_t>;

// ==================== 网络工具 ====================
namespace NetUtil {
    inline bool SendAll(SOCKET sock, const void* data, int len) {
        const char* p = static_cast<const char*>(data);
        while (len > 0) {
            int sent = send(sock, p, len, 0);
            if (sent <= 0) return false;
            p += sent;
            len -= sent;
        }
        return true;
    }

    inline bool RecvAll(SOCKET sock, void* buf, int len) {
        char* p = static_cast<char*>(buf);
        while (len > 0) {
            int r = recv(sock, p, len, 0);
            if (r <= 0) return false;
            p += r;
            len -= r;
        }
        return true;
    }

    inline bool InitWinsock() {
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }
}

// ==================== 消息构建器 ====================
namespace MessageBuilder {
    // 远程桌面
    inline BinaryData KeyframeRequest() {
        return { static_cast<uint8_t>(Desktop::MsgType::KeyframeRequest) };
    }

    inline BinaryData StreamConfigMsg(int w, int fps, int kfIntervalSec) {
        BinaryData msg(1 + sizeof(Desktop::StreamConfig));
        msg[0] = static_cast<uint8_t>(Desktop::MsgType::StreamConfig);
        auto* cfg = reinterpret_cast<Desktop::StreamConfig*>(msg.data() + 1);
        cfg->width = w;
        cfg->fps = fps;
        cfg->keyframeIntervalSec = kfIntervalSec;
        return msg;
    }
    
    inline BinaryData VideoFrame(const uint8_t* data, size_t size, bool isKeyframe) {
        BinaryData msg;
        msg.reserve(2 + size);
        msg.push_back(static_cast<uint8_t>(Desktop::MsgType::VideoFrame));
        msg.push_back(isKeyframe ? 1 : 0);
        msg.insert(msg.end(), data, data + size);
        return msg;
    }

    inline BinaryData ScreenInfo(int w, int h) {
        BinaryData msg(1 + sizeof(Desktop::ScreenInfo));
        msg[0] = static_cast<uint8_t>(Desktop::MsgType::ScreenInfo);
        auto* info = reinterpret_cast<Desktop::ScreenInfo*>(msg.data() + 1);
        info->width = w;
        info->height = h;
        return msg;
    }

    inline BinaryData InputEvent(const Desktop::InputEvent& ev) {
        BinaryData msg(1 + sizeof(ev));
        msg[0] = static_cast<uint8_t>(Desktop::MsgType::InputEvent);
        memcpy(msg.data() + 1, &ev, sizeof(ev));
        return msg;
    }

    inline BinaryData ClientReady() {
        return { static_cast<uint8_t>(Desktop::MsgType::ClientReady) };
    }

    // 文件管理
    inline BinaryData FileListRequest(const std::wstring& path) {
        BinaryData msg;
        auto type = path.empty() ? FileManager::MsgType::ListDrives : FileManager::MsgType::ListDir;
        msg.push_back(static_cast<uint8_t>(type));
        
        uint32_t len = static_cast<uint32_t>(path.length() * sizeof(wchar_t));
        msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&len),
                   reinterpret_cast<uint8_t*>(&len) + sizeof(len));
        
        if (len > 0) {
            auto* ptr = reinterpret_cast<const uint8_t*>(path.c_str());
            msg.insert(msg.end(), ptr, ptr + len);
        }
        return msg;
    }

    inline BinaryData DownloadRequest(const std::wstring& path) {
        BinaryData msg;
        msg.push_back(static_cast<uint8_t>(FileManager::MsgType::DownloadReq));
        
        uint32_t len = static_cast<uint32_t>(path.length() * sizeof(wchar_t));
        msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&len),
                   reinterpret_cast<uint8_t*>(&len) + sizeof(len));
        
        auto* ptr = reinterpret_cast<const uint8_t*>(path.c_str());
        msg.insert(msg.end(), ptr, ptr + len);
        return msg;
    }
}

#endif // PROTOCOL_H
