#pragma once

#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace flockv0 {
class Store;
}

namespace flockv0::app {

class SettingsSystem {
public:
    SettingsSystem(const std::filesystem::path& home, std::optional<unsigned> cliPort);

    unsigned startupPort(unsigned t3Port) const;
    unsigned t3CliPort(unsigned requestedPort, unsigned t3Port) const;
    std::optional<unsigned> recoveryPort(unsigned t3Port) const;
    const std::optional<std::string>& remoteAddress() const { return activeRemoteAddress_; }
    void listenerStarted(unsigned port);
    Json view(unsigned t3Port) const;
    Json change(const Json& patch, unsigned t3Port);
private:
    Json viewLocked(unsigned t3Port) const;

    const std::filesystem::path file_;
    const std::optional<unsigned> cliPort_;
    mutable std::mutex mutex_;
    Json saved_;
    Json startupPortSetting_;
    std::optional<std::string> activeRemoteAddress_;
    std::optional<unsigned> activePort_;
};

void rebindT3Port(Store&, unsigned port);

}
