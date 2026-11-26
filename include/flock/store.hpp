#pragma once

#include "flock/store_options.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/run_ref.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
struct BotBusy : Conflict {
    BotBusy() : Conflict("Bot has unfinished work") {}
};
inline constexpr char botPausedMessage[] = "This Bot is paused.";
struct BotPaused : Conflict {
    BotPaused() : Conflict(botPausedMessage) {}
};

class Policy;
class SessionParticipation;
struct ConfiguredSchedule;
struct GitWebhook;
struct InboxTarget;
struct ScheduleImportError;

struct TaskTarget {
    std::string botId;
    Id taskId, waitId, revision, generation;
    static TaskTarget from(const TaskData& task, Id waitId);
};

// Separate CLI and Web connections may use the same database.
class Store {
public:
    explicit Store(std::filesystem::path database, MemorySeed = {}, AutomationModelResolver = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Policy policy();
    bool botDeleted(std::string_view bot) const;
    void deleteBot(const Person&, std::string_view bot);
    bool botPaused(std::string_view bot) const;
    void setBotPaused(const Person&, std::string_view bot, bool paused, const std::function<Seconds()>& clock = unixTime);
    // A change hint for readers, covering writes on this connection and other connections.
    std::pair<std::int64_t, std::int64_t> changeStamp() const;
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class Channels;
    friend class LocalMachines;
    friend class WebIdentity;
    friend class Workflows;
    friend class Notifications;
    friend class Tasks;
    friend class Memory;
    friend class Automations;
    friend class Inbox;
    friend class GitEvents;
    friend class Conversations;
    friend class Runtime;
    friend class Policy;
    friend class T3Connections;
    friend class ExternalMcp;
    friend class SessionParticipation;
    friend class Database;
};
}
