#ifndef CLIENT_APPLICATION_H
#define CLIENT_APPLICATION_H

#include <memory>
#include <string>
#include "../common/protocol.h"

class SshSession;
class TCPClientTransport;
class ITransport;
struct ConnectionConfig;

class ClientApplication {
public:
    ClientApplication();
    ~ClientApplication();

    int exec();

private:
    bool setupConnections(const ConnectionConfig& cfg);

    std::unique_ptr<SshSession> sshSession_;
    std::unique_ptr<TCPClientTransport> desktopTransport_;
    ITransport* desktopTransportPtr_ = nullptr;
    std::string targetHost_;
};

#endif
