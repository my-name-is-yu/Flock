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

class LocalMachine;
class Policy;
class SessionParticipation;
struct ChannelConnector;
struct ChannelResource;
struct ConfiguredSchedule;
struct GitWebhook;
struct InboxTarget;
struct ScheduleImportError;
struct VerifiedChannelMessage;
enum class ChannelKind;
namespace conversation {
enum class PrincipalSource;
}

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
    Json workflows(std::string_view bot, Id before = 0, unsigned limit = 20) const;
    // Same chronological page for either participant.
    Json peerConversation(std::string_view bot, std::string_view peer, Id before = 0, unsigned limit = 50, Id from = 0,
                          std::string_view viewer = {}) const;
    Id peerMessageAnchor(std::string_view bot, std::string_view peer, Id event) const;
    std::vector<std::pair<std::string, std::string>> peerExchangePairs() const;
    bool hasPeerExchange(std::string_view bot, std::string_view peer) const;
    // A change hint for readers, covering writes on this connection and other connections.
    std::pair<std::int64_t, std::int64_t> changeStamp() const;
    struct BotViewStamp {
        std::pair<Id, Id> database;
        std::size_t gitCredentialCount;
        bool operator==(const BotViewStamp&) const = default;
    };
    // No reusable version when the view can depend on live channel permissions or vault files.
    std::optional<BotViewStamp> botViewStamp() const;
    struct NotificationEvents {
        Id through;
        Json events;
        std::map<std::string, Id> scope;
        bool more;
    };
    NotificationEvents notificationEventsSince(const Person&, const std::vector<std::string>& bots, std::optional<Id> after,
                                               const std::map<std::string, Id>& previousScope) const;
    void connectChannel(ChannelConnector);
    void artifactBaseUrl(std::string url);
    Json receiveChannel(ChannelKind, std::string_view credential, const Json& event, const IntakeResolver& routing);
    std::optional<ChannelResource> artifact(std::string_view id) const;
    std::optional<ChannelResource> attachment(const Person&, Id id) const;
    void setRootPeople(std::vector<std::string> ids);
    void setWebIdentityResolver(WebIdentityResolver, bool liveInteractiveRoles = false);
    void setWebOrigin(std::string origin);
    void connectLocalMachine(std::string id, std::shared_ptr<LocalMachine>);
    Json localConnections(const Person&) const;
    void decideLocalConnection(const Person&, Id id, bool approve);
    void disconnectLocalConnection(const Person&, Id id);
    Json runLocalMachineOperation(std::string_view environment, const RunRef& authenticatedRun, std::string_view machineId,
                                  std::string_view operation, const std::filesystem::path& path);
    const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
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
