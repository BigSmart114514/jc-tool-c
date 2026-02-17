
#ifndef FILE_SERVICE_H
#define FILE_SERVICE_H

#include "../common/protocol.h"
#include "../common/transport.h"
#include <thread>
#include <atomic>

class FileService {
public:
    FileService();
    ~FileService();

    void setTransport(IServerTransport* transport);
    void start();
    void stop();

private:
    void onClientConnected();
    void onClientDisconnected();
    void onMessage(const BinaryData& data);

    void handleListDrives();
    void handleListDir(const std::wstring& path);
    void handleDownload(const std::wstring& path);

    BinaryData createFileListResponse(FileManager::Status status,
                                       const std::vector<FileManager::FileEntry>& entries);

    IServerTransport* transport_ = nullptr;
    std::atomic<bool> running_{false};
};

#endif // FILE_SERVICE_H
