#pragma once

#include "flock/core.hpp"

#include <mutex>

namespace flockv0::app {

class SettingsSystem {
public:
    SettingsSystem(const std::filesystem::path& home, std::optional<unsigned> cliPort,
                   std::optional<std::string> cliSessionRead);

    unsigned startupPort(unsigned t3Port) const;
    std::optional<unsigned> recoveryPort(unsigned t3Port) const;
    const std::string& sessionRead() const { return activeSessionRead_; }
    void listenerStarted(unsigned port);
    Json view(unsigned t3Port) const;
    Json change(const Json& patch, unsigned t3Port);

private:
    Json viewLocked(unsigned t3Port) const;

    const std::filesystem::path file_;
    const std::optional<unsigned> cliPort_;
    const bool cliSessionRead_;
    mutable std::mutex mutex_;
    Json saved_;
    Json startupPortSetting_;
    std::string activeSessionRead_;
    std::optional<unsigned> activePort_;
};

void rebindT3Port(Store&, unsigned port);

}
