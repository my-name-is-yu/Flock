#pragma once

#include "flock/json.hpp"
#include "flock/person.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace flockv0 {
class Store;
struct T3ConnectRequest;
struct WebCallbacks;
}

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
    explicit T3WebSetup(std::filesystem::path home);
    void configure(WebCallbacks&, Store&, bool live);
    void listeningOn(unsigned port) { port_.store(port); }
private:
    Json status(const Person&);
    bool start(const Person&, const T3ConnectRequest&);

    const std::filesystem::path home_;
    std::atomic<unsigned> port_ = 0;
    std::mutex mutex_;
    bool running_ = false;
    std::string requester_, authorizationUrl_, error_;
    std::jthread worker_;
};

}
