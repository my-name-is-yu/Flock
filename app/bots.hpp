#pragma once

#include "flock/memory_automation.hpp"
#include "flock/web.hpp"

#include <mutex>
#include <optional>

namespace flockv0::app {
struct Bot {
    std::string id, name, environmentId, projectId;
    Json settings = Json::object(), avatar;
    std::vector<std::string> allowedBots;
    Intake intake(std::string_view request) const;
};

class Bots {
public:
    explicit Bots(std::filesystem::path directory);
    std::vector<std::string> ids() const;
    Bot get(std::string_view id) const;
    std::string soul(std::string_view id) const;
    std::string initialMemory(std::string_view id) const;
    std::vector<PeerBot> peers(std::string_view id) const;
    Json cards() const;
    void apply(const BotChange&);
    Json create(const Json& profile, const std::optional<AvatarImage>& image = std::nullopt);
    Json updateProfile(std::string_view id, const Json& profile, const std::optional<AvatarImage>& image = std::nullopt);
    std::optional<AvatarImage> avatarImage(std::string_view id) const;
    void syncSchedules(Store&, Seconds now, const std::function<void(std::string_view,std::string_view)>& report) const;
private:
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::filesystem::path path(std::string_view id) const;
    Json cardLocked(std::string_view id) const;
    Json readConfig(std::string_view id) const;
    static Bot configuredBot(std::string_view id, const Json&);
};

}
