#ifndef DISCOVERY_SERVER_H
#define DISCOVERY_SERVER_H

#include <string>
#include <thread>
#include <atomic>

#define DISCOVERY_MAGIC "JC_DISC"
#define DISCOVERY_PORT 22220

class DiscoveryServer {
public:
    DiscoveryServer();
    ~DiscoveryServer();

    bool start(int port = DISCOVERY_PORT);
    void stop();
    bool isRunning() const { return running_.load(); }

    void setSshPort(int port) { sshPort_ = port; }

private:
    void listenLoop();
    std::string getResponseJson();

    std::thread thread_;
    std::atomic<bool> running_{false};
    int sshPort_ = 2222;
};

#endif