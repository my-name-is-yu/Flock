#pragma once
#include "flock/store_options.hpp"
#include "flock/bot_change.hpp"
#include "flock/bot_profile.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
struct Person;
}

namespace flockv0::app {
struct Bot {
    std::string id, name, environmentId, projectId;
    Json settings = Json::object();
    std::string role;
    Intake intake(std::string_view request) const;
};

class Bots {
public:
    using RememberProject = std::function<void(std::string_view bot, std::string_view environment, std::string_view project)>;
    using ScheduleReporter = std::function<void(std::string_view, std::string_view)>;

    explicit Bots(std::filesystem::path directory);
    std::vector<std::string> ids() const;
    Bot get(std::string_view id) const;
    std::optional<PlannerModel> plannerModel(std::string_view id) const;
    std::string soul(std::string_view id) const;
    std::string initialMemory(std::string_view id) const;
    std::vector<PeerBot> peers(Store&, std::string_view id) const;
    Json cards() const;
    Json skillCards() const;
    void apply(const BotChange&);
    bool reconcileCreation(const BotChange&);
    Json create(const Json& profile, const std::optional<AvatarImage>& image = std::nullopt, const PublishOwnedBot& publishOwned = {},
                const RememberProject& rememberProject = {});
    Json updateProfile(std::string_view id, const Json& profile, const std::optional<AvatarImage>& image = std::nullopt);
    Json updateRunSettings(Store&, std::string_view id, const Json& settings, Seconds now, const ScheduleReporter&,
                           const Json& modelCatalog = Json::array(), const Person* editor = nullptr);
    Json saveSetup(Store*, std::string_view id, const BotSetupEdit&, Seconds, const ScheduleReporter&, const Json& modelCatalog,
                   const std::optional<AvatarImage>& image = std::nullopt, const Person* editor = nullptr);
    std::optional<AvatarImage> avatarImage(std::string_view id) const;
    void syncSchedules(Store&, Seconds now, const ScheduleReporter&) const;
private:
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::optional<Bot> tryGet(std::string_view id) const;
    std::filesystem::path path(std::string_view id) const;
    std::string readOptionalNotes(std::string_view id, std::string_view filename, std::size_t limit) const;
    Json cardLocked(std::string_view id) const;
    Json cardFromDirectory(std::string_view id, const std::filesystem::path&) const;
    void syncBotSchedules(Store&, std::string_view id, Seconds now, const ScheduleReporter&) const;
};

}
