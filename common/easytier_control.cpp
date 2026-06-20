#include "easytier_control.h"
#include <vector>
#include <sstream>
#include <wincrypt.h>

std::string EncryptSecret(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    DATA_BLOB in = { (DWORD)plaintext.size(), (BYTE*)plaintext.data() };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, NULL, NULL, NULL, NULL,
        CRYPTPROTECT_LOCAL_MACHINE, &out))
        return "";
    std::string encrypted((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);

    DWORD needed = 0;
    CryptBinaryToStringA((BYTE*)encrypted.data(), (DWORD)encrypted.size(),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &needed);
    std::string b64(needed, '\0');
    CryptBinaryToStringA((BYTE*)encrypted.data(), (DWORD)encrypted.size(),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0], &needed);
    if (needed > 0) b64.resize(needed - 1);
    return b64;
}

std::string DecryptSecret(const std::string& encryptedBase64) {
    if (encryptedBase64.empty()) return "";
    DWORD needed = 0;
    CryptStringToBinaryA(encryptedBase64.data(), (DWORD)encryptedBase64.size(),
        CRYPT_STRING_BASE64, NULL, &needed, NULL, NULL);
    if (needed == 0) return "";
    std::vector<BYTE> decoded(needed);
    CryptStringToBinaryA(encryptedBase64.data(), (DWORD)encryptedBase64.size(),
        CRYPT_STRING_BASE64, decoded.data(), &needed, NULL, NULL);

    DATA_BLOB in = { needed, decoded.data() };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
        CRYPTPROTECT_LOCAL_MACHINE, &out))
        return "";
    std::string result((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

const wchar_t* GetEasyTierDataDir() {
    static wchar_t dir[MAX_PATH] = { 0 };
    if (dir[0] == 0) {
        wcscpy_s(dir, L"C:\\ProgramData\\EasyTier");
        CreateDirectoryW(dir, NULL);
    }
    return dir;
}

const wchar_t* GetEasyTierConfigPath() {
    static wchar_t path[MAX_PATH] = { 0 };
    if (path[0] == 0) {
        wcscpy_s(path, GetEasyTierDataDir());
        wcscat_s(path, L"\\service_config.json");
    }
    return path;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

static std::string ExtractJsonString(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    std::string result;
    while (pos < json.size()) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            result += json[pos + 1];
            pos += 2;
        } else if (json[pos] == '"') {
            break;
        } else {
            result += json[pos];
            ++pos;
        }
    }
    return result;
}

static int ExtractJsonInt(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    int sign = 1;
    if (json[pos] == '-') { sign = -1; ++pos; }
    int val = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        val = val * 10 + (json[pos] - '0');
        ++pos;
    }
    return val * sign;
}

static bool ExtractJsonBool(const std::string& json, const char* key) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    return json.compare(pos, 4, "true") == 0;
}

std::string ConfigToJson(const EasyTierConfig& config) {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"instanceName\":\"" << JsonEscape(config.instanceName) << "\",\n"
       << "  \"networkName\":\"" << JsonEscape(config.networkName) << "\",\n"
       << "  \"networkSecret\":\"" << JsonEscape(config.networkSecret) << "\",\n"
       << "  \"ipv4\":\"" << JsonEscape(config.ipv4) << "\",\n"
       << "  \"listenPort\":" << config.listenPort << ",\n"
       << "  \"peerUrl\":\"" << JsonEscape(config.peerUrl) << "\",\n"
       << "  \"autoStart\":" << (config.autoStart ? "true" : "false") << "\n"
       << "}";
    return ss.str();
}

bool JsonToConfig(const std::string& json, EasyTierConfig& config) {
    if (json.empty() || json[0] != '{') return false;
    config.instanceName  = ExtractJsonString(json, "instanceName");
    config.networkName   = ExtractJsonString(json, "networkName");
    config.networkSecret = ExtractJsonString(json, "networkSecret");
    config.ipv4          = ExtractJsonString(json, "ipv4");
    config.listenPort    = ExtractJsonInt(json, "listenPort");
    config.peerUrl       = ExtractJsonString(json, "peerUrl");
    config.autoStart     = ExtractJsonBool(json, "autoStart");
    return true;
}

bool SaveConfigToFile(const EasyTierConfig& config) {
    CreateDirectoryW(GetEasyTierDataDir(), NULL);

    std::string json = ConfigToJson(config);
    HANDLE hFile = CreateFileW(GetEasyTierConfigPath(), GENERIC_WRITE,
        0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    bool ok = WriteFile(hFile, json.data(), (DWORD)json.size(), &written, NULL)
              && written == json.size();
    CloseHandle(hFile);
    return ok;
}

bool LoadConfigFromFile(EasyTierConfig& config) {
    HANDLE hFile = CreateFileW(GetEasyTierConfigPath(), GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(hFile, NULL);
    if (size == 0 || size > 65536) { CloseHandle(hFile); return false; }
    std::string json(size, '\0');
    DWORD read;
    bool ok = ReadFile(hFile, &json[0], size, &read, NULL) && read == size;
    CloseHandle(hFile);
    if (!ok) return false;
    json.resize(read);
    return JsonToConfig(json, config);
}

EasyTierControlClient::EasyTierControlClient()
    : hPipe_(INVALID_HANDLE_VALUE), seq_(0) {}

EasyTierControlClient::~EasyTierControlClient() { disconnect(); }

bool EasyTierControlClient::connect(DWORD timeoutMs) {
    if (hPipe_ != INVALID_HANDLE_VALUE) return true;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        hPipe_ = CreateFileW(EASYTIER_PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe_ != INVALID_HANDLE_VALUE) break;
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(EASYTIER_PIPE_NAME, 200)) continue;
        } else if (err == ERROR_FILE_NOT_FOUND) {
            Sleep(200);
            continue;
        } else {
            return false;
        }
    }
    if (hPipe_ == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe_, &mode, NULL, NULL);
    return true;
}

void EasyTierControlClient::disconnect() {
    if (hPipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hPipe_);
        hPipe_ = INVALID_HANDLE_VALUE;
    }
}

bool EasyTierControlClient::sendRaw(const void* data, uint32_t len) {
    DWORD written;
    return WriteFile(hPipe_, data, len, &written, NULL) && written == len;
}

bool EasyTierControlClient::recvRaw(void* data, uint32_t len) {
    DWORD read;
    return ReadFile(hPipe_, data, len, &read, NULL) && read == len;
}

bool EasyTierControlClient::sendRequest(EasyTierCmd cmd,
    const std::string& requestJson, std::string& responseJson)
{
    auto tryOnce = [&]() -> bool {
        if (hPipe_ == INVALID_HANDLE_VALUE) return false;
        EasyTierPipeMsg hdr;
        hdr.magic   = EASYTIER_MSG_MAGIC;
        hdr.type    = 0;
        hdr.cmd     = (uint32_t)cmd;
        hdr.seq     = ++seq_;
        hdr.dataLen = (uint32_t)requestJson.size();

        if (!sendRaw(&hdr, sizeof(hdr))) return false;
        if (!requestJson.empty() && !sendRaw(requestJson.data(), (uint32_t)requestJson.size()))
            return false;

        EasyTierPipeMsg resp;
        if (!recvRaw(&resp, sizeof(resp))) return false;
        if (resp.magic != EASYTIER_MSG_MAGIC || resp.type != 1) return false;

        responseJson.clear();
        if (resp.dataLen > 0) {
            responseJson.resize(resp.dataLen);
            if (!recvRaw(&responseJson[0], resp.dataLen)) return false;
        }
        return true;
    };

    if (tryOnce()) return true;

    // 服务端已在处理完上一个请求后断开了管道，重连一次再试
    disconnect();
    if (!connect(EASYTIER_PIPE_TIMEOUT)) return false;
    return tryOnce();
}

bool EasyTierControlClient::start() {
    std::string resp;
    return sendRequest(CMD_START, "{}", resp);
}

bool EasyTierControlClient::stop() {
    std::string resp;
    return sendRequest(CMD_STOP, "{}", resp);
}

bool EasyTierControlClient::restart() {
    std::string resp;
    return sendRequest(CMD_RESTART, "{}", resp);
}

bool EasyTierControlClient::getStatus(std::string& state, std::string& ip, uint32_t& uptime) {
    std::string resp;
    if (!sendRequest(CMD_STATUS, "{}", resp)) return false;
    state = ExtractJsonString(resp, "state");
    ip = ExtractJsonString(resp, "ip");
    uptime = (uint32_t)ExtractJsonInt(resp, "uptime");
    return true;
}

bool EasyTierControlClient::getConfig(EasyTierConfig& config) {
    std::string resp;
    if (!sendRequest(CMD_GET_CONFIG, "{}", resp)) return false;

    auto configPos = resp.find("\"config\":{");
    if (configPos == std::string::npos) return false;
    configPos += strlen("\"config\":");
    int depth = 0;
    size_t configEnd = configPos;
    for (; configEnd < resp.size(); ++configEnd) {
        if (resp[configEnd] == '{') ++depth;
        else if (resp[configEnd] == '}') { --depth; if (depth == 0) break; }
    }
    std::string inner = resp.substr(configPos, configEnd - configPos + 1);
    return JsonToConfig(inner, config);
}

bool EasyTierControlClient::configure(const EasyTierConfig& config, bool restartNow) {
    std::string configJson = ConfigToJson(config);
    std::string request = "{\"config\":" + configJson + ",\"restart\":"
        + (restartNow ? "true" : "false") + "}";
    std::string resp;
    return sendRequest(CMD_CONFIGURE, request, resp);
}

bool EasyTierControlClient::shutdown() {
    std::string resp;
    return sendRequest(CMD_SHUTDOWN, "{}", resp);
}
