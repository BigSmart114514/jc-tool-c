
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
    constexpr int BUFFER_SIZE = 65536;
    constexpr int VIDEO_BITRATE = 10000000;
    constexpr int FPS = 30;
    constexpr int KEYFRAME_INTERVAL = 120;
}

// ==================== 服务类型 ====================
enum class ServiceType : uint8_t {
    Desktop = 1
};

// ==================== 远程桌面消息 ====================
namespace Desktop {
    enum class MsgType : uint8_t {
        VideoFrame      = 0x01,
        InputEvent      = 0x02,
        ScreenInfo      = 0x03,
        ClientReady     = 0x04,
        KeyframeRequest = 0x05,
        StreamConfig    = 0x06,  // 流配置
        ClientDisconnect = 0x07, // 客户端断开通知
        AudioData       = 0x08,  // 音频数据（AAC帧）
        AudioConfig     = 0x09,  // 音频配置（AudioSpecificConfig）
        AudioEnable     = 0x0A   // 客户端→服务器：启用/禁用音频
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
    struct StreamConfig {
        int32_t width;
        int32_t fps;
        int32_t keyframeIntervalSec;
    };

    struct AudioConfigMsg {
        int32_t sampleRate;
        uint8_t channels;
        uint8_t asc[2]; // AudioSpecificConfig for AAC LC
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

    inline BinaryData ClientDisconnect() {
        return { static_cast<uint8_t>(Desktop::MsgType::ClientDisconnect) };
    }

    inline BinaryData AudioConfig(int sampleRate, int channels, const uint8_t* asc, int ascLen) {
        BinaryData msg(1 + sizeof(int32_t) + 1 + 1 + ascLen);
        msg[0] = static_cast<uint8_t>(Desktop::MsgType::AudioConfig);
        int32_t sr = sampleRate;
        memcpy(msg.data() + 1, &sr, sizeof(sr));
        msg[5] = static_cast<uint8_t>(channels);
        msg[6] = static_cast<uint8_t>(ascLen);
        if (ascLen > 0) memcpy(msg.data() + 7, asc, ascLen);
        return msg;
    }

    inline BinaryData AudioData(const uint8_t* aacData, size_t aacSize) {
        BinaryData msg(1 + aacSize);
        msg[0] = static_cast<uint8_t>(Desktop::MsgType::AudioData);
        if (aacSize > 0) memcpy(msg.data() + 1, aacData, aacSize);
        return msg;
    }

    inline BinaryData AudioEnableMsg(bool enabled) {
        BinaryData msg(2);
        msg[0] = static_cast<uint8_t>(Desktop::MsgType::AudioEnable);
        msg[1] = enabled ? 1 : 0;
        return msg;
    }

}

#endif // PROTOCOL_H
