#include "file_service.h"
#include <iostream>

FileService::FileService() {}

FileService::~FileService() {
    stop();
}

void FileService::setTransport(IServerTransport* transport) {
    transport_ = transport;

    TransportCallbacks callbacks;
    callbacks.onConnected = [this]() { onClientConnected(); };
    callbacks.onDisconnected = [this]() { onClientDisconnected(); };
    callbacks.onMessage = [this](const BinaryData& data) { onMessage(data); };

    transport_->setCallbacks(callbacks);
}

void FileService::start() {
    running_ = true;
    std::cout << "[File] Service started" << std::endl;
}

void FileService::stop() {
    running_ = false;
}

void FileService::onClientConnected() {
    std::cout << "[File] Client connected" << std::endl;
}

void FileService::onClientDisconnected() {
    std::cout << "[File] Client disconnected" << std::endl;
}

void FileService::onMessage(const BinaryData& data) {
    if (data.empty() || !transport_) return;

    auto type = static_cast<FileManager::MsgType>(data[0]);

    switch (type) {
        case FileManager::MsgType::ListDrives:
            handleListDrives();
            break;

        case FileManager::MsgType::ListDir: {
            if (data.size() < 5) break;
            uint32_t pathLen;
            memcpy(&pathLen, data.data() + 1, sizeof(pathLen));

            std::wstring path;
            if (pathLen > 0 && data.size() >= 5 + pathLen) {
                path.resize(pathLen / sizeof(wchar_t));
                memcpy(&path[0], data.data() + 5, pathLen);
            }
            handleListDir(path);
            break;
        }

        case FileManager::MsgType::DownloadReq: {
            if (data.size() < 5) break;
            uint32_t pathLen;
            memcpy(&pathLen, data.data() + 1, sizeof(pathLen));

            std::wstring path;
            if (pathLen > 0 && data.size() >= 5 + pathLen) {
                path.resize(pathLen / sizeof(wchar_t));
                memcpy(&path[0], data.data() + 5, pathLen);
            }
            handleDownload(path);
            break;
        }

        default:
            break;
    }
}

void FileService::handleListDrives() {
    std::vector<FileManager::FileEntry> entries;
    DWORD drives = GetLogicalDrives();

    for (wchar_t c = L'A'; c <= L'Z'; c++) {
        if (drives & (1 << (c - L'A'))) {
            FileManager::FileEntry entry = {};
            swprintf(entry.name, 260, L"%c:", c);
            entry.type = static_cast<uint8_t>(FileManager::FileType::Drive);
            entries.push_back(entry);
        }
    }

    auto response = createFileListResponse(FileManager::Status::OK, entries);
    if (transport_) transport_->send(response);
}

void FileService::handleListDir(const std::wstring& path) {
    std::vector<FileManager::FileEntry> entries;
    std::wstring searchPath = path;

    if (searchPath.empty() || searchPath.back() != L'\\') {
        searchPath += L"\\";
    }
    searchPath += L"*";

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        auto response = createFileListResponse(FileManager::Status::NotFound, {});
        if (transport_) transport_->send(response);
        return;
    }

    do {
        if (wcscmp(findData.cFileName, L".") == 0) continue;

        FileManager::FileEntry entry = {};
        wcsncpy(entry.name, findData.cFileName, 259);

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            entry.type = static_cast<uint8_t>(FileManager::FileType::Directory);
        } else {
            entry.type = static_cast<uint8_t>(FileManager::FileType::File);
            entry.size = (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) | findData.nFileSizeLow;
        }

        ULARGE_INTEGER time;
        time.LowPart = findData.ftLastWriteTime.dwLowDateTime;
        time.HighPart = findData.ftLastWriteTime.dwHighDateTime;
        entry.modifyTime = time.QuadPart;

        entries.push_back(entry);
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);

    auto response = createFileListResponse(FileManager::Status::OK, entries);
    if (transport_) transport_->send(response);
}

void FileService::handleDownload(const std::wstring& path) {
    if (!transport_) return;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        BinaryData response;
        response.push_back(static_cast<uint8_t>(FileManager::MsgType::DownloadData));
        FileManager::TransferHeader header = {};
        header.status = static_cast<uint8_t>(FileManager::Status::NotFound);
        auto* ptr = reinterpret_cast<uint8_t*>(&header);
        response.insert(response.end(), ptr, ptr + sizeof(header));
        transport_->send(response);
        return;
    }

    LARGE_INTEGER fileSize;
    GetFileSizeEx(hFile, &fileSize);
    uint64_t totalSize = fileSize.QuadPart;
    uint64_t offset = 0;

    std::wcout << L"[File] Download: " << path << L" (" << totalSize << L" bytes)" << std::endl;

    constexpr size_t CHUNK_SIZE = 32768;
    std::vector<uint8_t> buffer(CHUNK_SIZE);
    DWORD bytesRead;

    while (offset < totalSize && running_ && transport_ && transport_->hasClient()) {
        if (!ReadFile(hFile, buffer.data(), static_cast<DWORD>(CHUNK_SIZE), &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }

        BinaryData response;
        response.push_back(static_cast<uint8_t>(FileManager::MsgType::DownloadData));

        FileManager::TransferHeader header = {};
        header.totalSize = totalSize;
        header.offset = offset;
        header.chunkSize = bytesRead;

        offset += bytesRead;
        header.status = (offset >= totalSize)
            ? static_cast<uint8_t>(FileManager::Status::Complete)
            : static_cast<uint8_t>(FileManager::Status::Transferring);

        auto* ptr = reinterpret_cast<uint8_t*>(&header);
        response.insert(response.end(), ptr, ptr + sizeof(header));
        response.insert(response.end(), buffer.begin(), buffer.begin() + bytesRead);

        if (!transport_->send(response)) {
            std::cerr << "[File] Send failed during download" << std::endl;
            break;
        }

        Sleep(1);
    }

    CloseHandle(hFile);
    std::cout << "[File] Download complete" << std::endl;
}

BinaryData FileService::createFileListResponse(FileManager::Status status,
                                                const std::vector<FileManager::FileEntry>& entries) {
    BinaryData response;
    response.push_back(static_cast<uint8_t>(FileManager::MsgType::Response));

    FileManager::ListHeader header = {};
    header.status = static_cast<uint8_t>(status);
    header.count = static_cast<uint32_t>(entries.size());

    auto* hdrPtr = reinterpret_cast<uint8_t*>(&header);
    response.insert(response.end(), hdrPtr, hdrPtr + sizeof(header));

    for (const auto& entry : entries) {
        auto* entryPtr = reinterpret_cast<const uint8_t*>(&entry);
        response.insert(response.end(), entryPtr, entryPtr + sizeof(entry));
    }

    return response;
}