#pragma once

#include "flock/web.hpp"
#include "flock/workspace_tools.hpp"

#include <map>
#include <mutex>

namespace flockv0::app {
class SettingsConnections {
public:
    explicit SettingsConnections(const std::filesystem::path& home);

    std::string githubToken() const;
    std::string githubWebhookSecret() const;
    void configureChannels(Store&, WebCallbacks&) const;
    std::shared_ptr<const BotTools> loadTools() const;

    Json view() const;
    Json change(std::string_view section, const Json& input);
    Json botChannels(std::string_view botId) const;
    Json changeBotChannel(std::string_view botId, const Json& input, const Policy&);

private:
    struct Check {
        bool passed;
        Seconds finishedAt;
    };

    Json viewLocked() const;
    Json botChannelsLocked(std::string_view botId) const;
    void saveLocked(std::string_view section, Json desired, Json savedSection = nullptr);

    std::filesystem::path home_;
    std::filesystem::path file_;
    Json persisted_;
    Json desired_;
    Json active_;
    Json loadedTools_;
    bool hostChannels_ = false;
    bool hostTools_ = false;
    std::optional<Check> githubCheck_;
    std::map<std::pair<std::string,std::string>,Check> bridgeChecks_;
    mutable std::mutex mutex_;
};
}
