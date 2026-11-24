#pragma once
#include "flock/policy.hpp"

#include "flock/bot_profile.hpp"
#include "flock/memory_automation.hpp"

#include <mutex>
#include <optional>

namespace flockv0::app {
struct Bot {
    std::string id, name, environmentId, projectId;
    Json settings = Json::object();
    std::vector<std::string> allowedBots;
    std::string role;
    Intake intake(std::string_view request) const;
};

class Bots {
public:
    using ScheduleReporter = std::function<void(std::string_view,std::string_view)>;

    explicit Bots(std::filesystem::path directory);
    std::vector<std::string> ids() const;
    Bot get(std::string_view id) const;
    std::string soul(std::string_view id) const;
    std::string initialMemory(std::string_view id) const;
    std::vector<PeerBot> peers(std::string_view id) const;
    Json cards() const;
    Json skillCards() const;
    void apply(const BotChange&);
    bool reconcileCreation(const BotChange&);
    Json create(const Json& profile, const std::optional<AvatarImage>& image = std::nullopt,
                const Policy::PublishOwnedBot& publishOwned = {});
    Json updateProfile(std::string_view id, const Json& profile, const std::optional<AvatarImage>& image = std::nullopt);
    Json updateRunSettings(Store&, std::string_view id, const Json& settings, Seconds now, const ScheduleReporter&);
    std::optional<AvatarImage> avatarImage(std::string_view id) const;
    void syncSchedules(Store&, Seconds now, const ScheduleReporter&) const;
private:
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::mutex runSettingsMutex_;
    std::filesystem::path path(std::string_view id) const;
    Json cardLocked(std::string_view id) const;
    Json cardFromDirectory(std::string_view id, const std::filesystem::path&) const;
    void syncBotSchedules(Store&, std::string_view id, Seconds now, const ScheduleReporter&) const;
};

}
