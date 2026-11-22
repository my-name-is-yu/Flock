#pragma once

#include "flock/core.hpp"
#include "flock/web.hpp"

#include <bitset>
#include <mutex>
#include <optional>

namespace date { class time_zone; }

namespace flockv0::app {
class Cron {
public:
    Cron(std::string expression, std::string timezone);
    bool matches(Seconds utc) const;
private:
    std::bitset<60> minutes_, hours_, days_, months_, weekdays_;
    bool anyDay_, anyWeekday_;
    const date::time_zone* zone_;
};

struct Schedule {
    std::string id, expression, timezone, request;
    Cron cron;
};

struct Bot {
    std::string id, name, environmentId, projectId;
    Json settings = Json::object();
    std::vector<Schedule> schedules;
    std::vector<std::string> allowedBots;
    Intake intake(std::string_view request) const;
};

class Bots {
public:
    explicit Bots(std::filesystem::path directory);
    std::vector<std::string> ids() const;
    Bot get(std::string_view id) const;
    std::optional<Intake> configuredIntake(std::string_view id, std::string_view request) const;
    BotNotes notes(std::string_view id) const;
    std::vector<PeerBot> peers(std::string_view id) const;
    Json cards() const;
    Json create(const Json& profile, const std::optional<AvatarImage>& image = std::nullopt);
    Json updateProfile(std::string_view id, const Json& profile, const std::optional<AvatarImage>& image = std::nullopt);
    std::optional<AvatarImage> avatarImage(std::string_view id) const;
private:
    friend class Scheduler;
    std::optional<Bot> load(std::string_view id, bool scheduledOnly) const;
    std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::filesystem::path path(std::string_view id) const;
    Json cardLocked(std::string_view id) const;
};

class Scheduler {
public:
    using ErrorReporter = std::function<void(std::string_view id, std::string_view error)>;
    Scheduler(const Bots&, Store&, Seconds startedAt, ErrorReporter);
    void tick(Seconds now);
private:
    const Bots& bots_;
    Store& store_;
    Seconds next_;
    ErrorReporter report_;
};
}
