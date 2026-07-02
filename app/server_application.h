#ifndef SERVER_APPLICATION_H
#define SERVER_APPLICATION_H

#include <memory>
#include <string>

class DesktopService;
class TCPServerTransport;
class ServerStatusDialog;

class ServerApplication {
public:
    ServerApplication();
    ~ServerApplication();

    int exec();

private:
    int desktopPort_ = 12345;
    int sshPort_ = 2222;
    std::string sshPassword_;
    std::string myVirtualIp_;
    bool useEasyTier_ = false;

    std::unique_ptr<DesktopService> desktopService_;
    std::unique_ptr<TCPServerTransport> desktopTransport_;
};

#endif
