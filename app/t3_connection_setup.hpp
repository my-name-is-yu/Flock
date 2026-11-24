#pragma once

#include "flock/web.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace flockv0::app {

struct T3SetupResult {
    std::string environmentId;
    unsigned port;
    bool portChanged;
    std::vector<std::string> configuredProviders;
};

T3SetupResult connectT3(const std::filesystem::path& home, const T3ConnectRequest& request, unsigned port,
                        const std::function<void(const std::string&)>& authorize, std::stop_token stop = {},
                        const std::optional<Person>& requester = {});
void disconnectT3(Store&, const std::optional<std::string>& environment);

class T3WebSetup {
public:
    T3WebSetup(std::filesystem::path home, std::function<void()> onConnected);
    void configure(WebCallbacks&, Store&, bool live);
    void listeningOn(unsigned port) { port_.store(port); }

private:
    Json status(const Person&);
    bool start(const Person&, const T3ConnectRequest&);

    const std::filesystem::path home_;
    const std::function<void()> onConnected_;
    std::atomic<unsigned> port_ = 0;
    std::mutex mutex_;
    bool running_ = false;
    std::string requester_, authorizationUrl_, error_;
    std::jthread worker_;
};

}
