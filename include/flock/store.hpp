#pragma once

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
    using MemorySeed = std::function<std::string(std::string_view)>;
    struct PlannerModel {
        std::string model, provider;
        std::optional<std::string> reasoningEffort;
        std::optional<Id> modelCallLimit = std::nullopt;
    };
    using AutomationModelResolver = std::function<std::optional<PlannerModel>(std::string_view botId)>;
    using CommitConfiguredSchedules = std::function<void(const std::vector<ConfiguredSchedule>&, const std::function<void()>&)>;
    using EditConfiguredSchedules = std::function<void(const CommitConfiguredSchedules&)>;
    explicit Store(std::filesystem::path database, MemorySeed = {}, AutomationModelResolver = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Policy policy();
    bool botDeleted(std::string_view bot) const;
    void deleteBot(const Person&, std::string_view bot);
    bool botPaused(std::string_view bot) const;
    void setBotPaused(const Person&, std::string_view bot, bool paused, const std::function<Seconds()>& clock = unixTime);
    Json memory(const Person&, std::string_view bot, std::string_view operation, const Json& args);
    Json createAutomation(const Person&, const Intake&, const Json& args, Seconds now);
    Json automations(const Person&, std::string_view bot, Id before = 0, bool activeOnly = false) const;
    Json activeConfiguredSchedules(std::string_view bot) const;
    Json cancelAutomation(const Person&, std::string_view bot, Id id);
    [[nodiscard]] std::vector<ScheduleImportError> syncSchedules(std::string_view bot, const std::vector<ConfiguredSchedule>&, Seconds now);
    [[nodiscard]] std::vector<ScheduleImportError> syncSchedules(std::string_view bot, Seconds now, const EditConfiguredSchedules&);
    void pruneAutomations(const std::vector<std::string>& configuredBots);
    void fireDueAutomations(Seconds now);
    Json receiveWebhook(const Person&, std::string_view source, std::string_view externalEventId, const Json& event, Seconds now);
    Json inbox(const Person&, std::string_view bot = {}) const;
    Json decisionHistory(const Person&, std::string_view bot = {}) const;
    void decideInbox(const Person&, const InboxTarget&, std::string_view decision, const Json& data = Json::object());
    bool receiveGitWebhook(const GitWebhook&);
    void protectGitCredential(std::string value);
    Id submit(const Intake&, const Person* requester = nullptr);
    // Enqueues a Bot message for Runtime. Retrying the same key and payload returns the same receipt.
    Json receive(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text, Id replyToMessageId = 0,
                 const Json& attachments = Json::array());
    // Refers to a person in the current Web identity directory when the Runtime executes the turn.
    Json receiveWeb(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text, Id replyToMessageId = 0,
                    const Json& attachments = Json::array());
    std::optional<Json> receipt(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text,
                                Id replyToMessageId = 0, const Json& attachments = Json::array()) const;
    // Conversation turns paused on the model call limit, newest first.
    Json pausedConversations(std::string_view botId) const;
    // Operations: answer {text}, approve, retry, extend {deadline}, reconcile
    // {threadId?,runId?,questionId?,session?}, cancel. All except cancel require a matching waitId.
    void operate(const Person&, const TaskTarget&, std::string_view operation, const Json& data = Json::object());
    TaskView task(Id taskId) const;
    bool hasTask(Id taskId) const;
    // tasks: newest first, cursor is an exclusive id. history: oldest first, cursor is an exclusive id.
    std::vector<TaskView> tasks(std::string_view botId = {}, Id before = 0, unsigned limit = 50) const;
    Json compactions(std::string_view bot, Id before = 0, unsigned limit = 50) const;
    Json workflows(std::string_view bot, Id before = 0, unsigned limit = 20) const;
    struct ActiveTask {
        TaskData task;
        std::optional<Wait> wait;
        std::optional<TaskBlocker> blockedBy;
    };
    std::vector<ActiveTask> activeTasks(std::string_view botId) const;
    static Wait waitingEvent(const Json& event);
    Json reconciliationData(Id task, Id waitId, Json target, const IntakeResolver&) const;
    // Distinct active state and replanning combinations, grouped by Bot ID.
    std::map<std::string, std::vector<TaskPresence>> activeTaskStatesByBot() const;
    std::vector<TaskView> sessionTasks(std::string_view botId, Id before, unsigned limit) const;
    // Journal position and tasks with a Run start or settled report since that position.
    // On the initial read only the position is needed, before loading the first page.
    std::pair<Id, std::vector<TaskView>> sessionChangesSince(std::string_view botId, std::optional<Id> after) const;
    Json history(Id taskId, Id after = 0, unsigned limit = 50) const;
    // One page of chronological user-visible conversation, across this Bot's tasks.
    enum class ConversationEvents { all, incoming };
    Json conversation(std::string_view botId, Id before = 0, unsigned limit = 50, ConversationEvents events = ConversationEvents::all,
                      std::string_view viewer = {}) const;
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
    struct ConversationChanges {
        Id through;
        std::map<std::string, std::vector<Id>> messages;
    };
    // Original messages affected by replies and instruction delivery since a journal cursor.
    ConversationChanges conversationChangesSince(std::optional<Id> after, std::string_view bot = {}) const;
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
    Json receive(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text, Id replyToMessageId,
                 conversation::PrincipalSource, const VerifiedChannelMessage*, const std::function<Intake()>&,
                 const Json& attachments = Json::array());
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class AutomationStorage;
    friend class MemoryStorage;
    friend class ModelCalls;
    friend class TaskOperations;
    friend class TaskRouting;
    friend class InboxStorage;
    friend class GitStorage;
    friend class WorkflowStorage;
    friend class ChannelStorage;
    friend class Runtime;
    friend class Policy;
    friend class PolicyApprovals;
    friend class T3Connections;
    friend class ExternalMcp;
    friend class SessionParticipation;
    friend class Database;
};
}
