#ifndef EASYTIER_CONTROL_H
#define EASYTIER_CONTROL_H

#include <string>
#include <cstdint>
#include <windows.h>

#define EASYTIER_PIPE_NAME L"\\\\.\\pipe\\EasyTierControl"
#define EASYTIER_PIPE_TIMEOUT 5000
#define EASYTIER_MSG_MAGIC 0x00495445

enum EasyTierCmd : uint32_t {
    CMD_START      = 1,
    CMD_STOP       = 2,
    CMD_RESTART    = 3,
    CMD_STATUS     = 4,
    CMD_GET_CONFIG = 5,
    CMD_CONFIGURE  = 6,
    CMD_SHUTDOWN   = 7,
    CMD_SSH_START  = 10,
    CMD_SSH_STOP   = 11,
    CMD_SSH_STATUS = 12,
};

#pragma pack(push, 1)
struct EasyTierPipeMsg {
    uint32_t magic;
    uint32_t type;
    uint32_t cmd;
    uint32_t seq;
    uint32_t dataLen;
};
#pragma pack(pop)

struct EasyTierConfig {
    std::string instanceName = "jc-client";
    std::string networkName  = "jc-tool-vpn";
    std::string networkSecret;
    std::string ipv4;
    int listenPort = 11012;
    std::string peerUrl = "tcp://225284.xyz:11010";
    bool autoStart = false;
    // SSH server config
    bool sshEnabled = false;
    int sshPort = 2222;
    std::string sshPassword;
};

std::string EncryptSecret(const std::string& plaintext);
std::string DecryptSecret(const std::string& encryptedBase64);

// JSON extraction helpers (used by service_main.cpp as well)
std::string ExtractJsonString(const std::string& json, const char* key);
int ExtractJsonInt(const std::string& json, const char* key);
bool ExtractJsonBool(const std::string& json, const char* key);

const wchar_t* GetEasyTierDataDir();
const wchar_t* GetEasyTierConfigPath();

bool SaveConfigToFile(const EasyTierConfig& config);
bool LoadConfigFromFile(EasyTierConfig& config);

std::string ConfigToJson(const EasyTierConfig& config);
bool JsonToConfig(const std::string& json, EasyTierConfig& config);
std::string JsonEscape(const std::string& s);

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

class EasyTierControlClient {
public:
    EasyTierControlClient();
    ~EasyTierControlClient();

    bool connect(DWORD timeoutMs = EASYTIER_PIPE_TIMEOUT);
    void disconnect();
    bool isConnected() const { return hPipe_ != INVALID_HANDLE_VALUE; }
    ConnectionState state() const { return state_; }
    std::string lastError() const { return lastError_; }

    bool sendRequest(EasyTierCmd cmd, const std::string& requestJson,
                     std::string& responseJson);

    bool start();
    bool stop();
    bool restart();
    bool getStatus(std::string& state, std::string& ip, uint32_t& uptime);
    bool getConfig(EasyTierConfig& config);
    bool configure(const EasyTierConfig& config, bool restartNow);
    bool shutdown();
    bool sshStart(int port, const std::string& password);
    bool sshStop();
    bool sshStatus(bool& running, int& port);

private:
    bool sendRaw(const void* data, uint32_t len);
    bool recvRaw(void* data, uint32_t len);
    HANDLE hPipe_;
    uint32_t seq_;
    ConnectionState state_ = ConnectionState::Disconnected;
    std::string lastError_;
};

#endif
