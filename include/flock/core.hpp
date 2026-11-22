#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
using Json = nlohmann::json;
using Id = std::int64_t;
using Seconds = std::int64_t;

Seconds unixTime();
std::string bounded(std::string_view text, std::size_t bytes);
std::string actionId();

struct Conflict : std::runtime_error { using std::runtime_error::runtime_error; };

// Initial allowance from an intake's saved config. Each approval adds one attempt.
Id modelCallLimit(const Json& config);

// Routing and execution settings are saved at intake. Current instructions are loaded for each turn.
struct Intake {
    std::string botId, request, environmentId, projectId;
    Json config = Json::object();
};
using IntakeResolver = std::function<Intake(std::string_view botId, std::string_view request)>;
// Prepare a reconciliation from a Store::task snapshot and the current Bot configuration.
Json reconciliationData(const Json& task, Id waitId, Json target, const IntakeResolver& intake);

struct BotNotes { std::string soul, memory; };
struct PeerBot { std::string name; Intake intake; };

struct Decision {
    std::string tool;
    Json args = Json::object();
    std::string reason;
};
struct ModelTurn {
    std::vector<Decision> calls;
    Json usage = Json::object();
};
struct ModelImage {
    Id messageId;
    std::string attachmentId, mimeType, dataBase64;
};
struct ModelInput {
    Json task, event, history;
    BotNotes notes;
    std::vector<std::string> tools;
    std::string feedback;
    Json evidence = Json::array();
    Json config = Json::object(); // Adapter settings only. Never serialize into the prompt.
    bool needsDoneWhen = false, canForwardUserMessage = false;
    std::vector<ModelImage> images{};
};
struct ModelError : std::runtime_error {
    ModelError(std::string message, bool retryable, Json measurements = Json::object())
        : std::runtime_error(std::move(message)), transient(retryable), usage(std::move(measurements)) {}
    bool transient;
    Json usage;
};
class Model {
public:
    virtual ~Model() = default;
    virtual ModelTurn decide(const ModelInput&) = 0;
};

struct RunRef {
    std::string threadId, runId, link;
};
struct Question {
    std::string requestId;
    Json fields = Json::array();
};
// T3 trims ordinary input and labels without explicit values using JavaScript String.trim.
std::string_view trimSessionQuestionText(std::string_view text);
// Validate a model answer against the observed fields and encode it for Session submission.
Json sessionQuestionAnswers(const Json& fields, const Json& answers);
struct Observation {
    enum class Kind { Running, Question, ExternalInput, Finished, Unavailable } kind = Kind::Running;
    Question question;
    std::string status, report, resultRunId, itemId;
    bool truncated = false;
    bool settled = false; // True on Finished only after delegated work has settled.
    std::string detail;
};
inline constexpr std::size_t maxSessionPromptBytes = 120000;
std::string sessionPrompt(const std::string& actionId, const std::string& instruction);

struct Command {
    std::string actionId, tool, environmentId, projectId, instruction, authGeneration;
    Json config = Json::object(), answers = Json::object();
    RunRef target;
    std::string questionId;
    RunRef candidate; // User-specified reconciliation hint. T3 must verify ownership.
    std::string candidateQuestionId;
    std::string ownerBotId;
};
struct Delivery {
    enum class Kind { Accepted, Unknown, Auth, Settings, QuestionGone, NotAccepted } kind = Kind::Unknown;
    RunRef run;
    std::string detail;
    bool definitelyUnsent = false;
};
struct ReadRequest {
    std::string environmentId, threadId, runId, itemId, cursor, view = "messages";
    std::size_t offset = 0;
    Json config = Json::object();
};
struct Excerpt {
    std::string text, threadId, runId, itemId, nextCursor;
    std::size_t nextOffset = 0;
    bool more = false;
};
inline constexpr unsigned sessionPageLimit = 20;

struct SessionTarget {
    std::string environmentId, threadId;
    bool operator==(const SessionTarget&) const = default;
};
struct SessionPreview {
    std::string text;
    bool truncated = false;
};
struct SessionMetadata {
    std::string title, project, projectId, status, createdAt, updatedAt;
    std::vector<SessionPreview> messages;
    bool moreMessages = false;
};
struct SessionAttachment {
    std::string id, name, mimeType;
    std::optional<std::string> content;
    bool truncated = false;
};
struct SessionEvent {
    std::string id, time, kind, status, title, text;
    std::vector<SessionAttachment> attachments;
    bool textTruncated = false, attachmentsTruncated = false;
};
struct SessionEventFilter {
    std::string kind, fromTime, toTime;
    unsigned limit = 10;
    std::optional<Id> afterPosition;
    std::function<bool(const SessionEvent&)> matchText;
};
struct SessionEventPage {
    std::vector<SessionEvent> items;
    std::optional<Id> nextPosition;
    bool truncated = false;
};
struct SessionCounts {
    Id messages = 0, runs = 0;
};

class T3 {
public:
    virtual ~T3() = default;
    virtual std::string authGeneration(const std::string& environmentId, const Json& config) = 0;
    // Local/preflight checks only. Throw ModelError for a settings failure and report auth separately.
    virtual std::optional<Delivery> preflight(const Command&) { return std::nullopt; }
    virtual Delivery send(const Command&) = 0;
    virtual Delivery reconcile(const Command&) = 0;
    virtual Observation observe(const std::string& environmentId, const Json& config, const RunRef&) = 0;
    virtual void interrupt(const std::string& environmentId, const Json& config, const RunRef&) = 0;
    virtual Excerpt read(const ReadRequest&) { throw std::runtime_error("Session reading is unavailable"); }
    virtual std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool) { return {}; }
    virtual std::optional<SessionEventPage> sessionEvents(const SessionTarget&, const SessionEventFilter&) { return {}; }
    virtual std::optional<SessionEvent> sessionEvent(const SessionTarget&, std::string_view) { return {}; }
    virtual std::optional<SessionCounts> sessionCounts(const SessionTarget&) { return {}; }
    virtual std::optional<SessionAttachment> sessionAttachment(const SessionTarget&, std::string_view,
                                                               std::string_view) { return {}; }
};

// Store methods use SQLite transactions and may be called through separate CLI/Web connections.
class GitHost;
struct GitWebhook;
class Access;
struct Person;
struct ConfiguredSchedule;
struct ScheduleImportError;
struct InboxTarget;
struct BotChange;
struct TaskTarget {
    std::string botId;
    Id taskId, waitId, revision, generation;
    static TaskTarget from(const Json& task, Id waitId);
};
using ApplyBotChange = std::function<void(const BotChange&)>;
struct ChannelConnector;
class LocalMachine;
enum class ChannelKind;
struct ChannelResource;
struct VerifiedChannelMessage;
class Store {
public:
    using MemorySeed = std::function<std::string(std::string_view)>;
    explicit Store(std::filesystem::path database, MemorySeed = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Access access();
    Json memory(const Person&, std::string_view bot, std::string_view operation, const Json& args);
    Json createAutomation(const Person&, const Intake&, const Json& args, Seconds now);
    Json automations(const Person&, std::string_view bot, Id before = 0) const;
    Json cancelAutomation(const Person&, std::string_view bot, Id id);
    [[nodiscard]] std::vector<ScheduleImportError> syncSchedules(
        std::string_view bot, const std::vector<ConfiguredSchedule>&, Seconds now);
    void pruneAutomations(const std::vector<std::string>& configuredBots);
    void fireDueAutomations(Seconds now);
    Json receiveWebhook(const Person&, std::string_view source, std::string_view externalEventId,
                        const Json& event, Seconds now);
    Json inbox(const Person&, std::string_view bot = {}) const;
    Json decisionHistory(const Person&, std::string_view bot = {}) const;
    void decideInbox(const Person&, const InboxTarget&, std::string_view decision,
                     const Json& data = Json::object(), const ApplyBotChange& = {});
    Id requestPrReview(Id task, Id revision, Id generation, std::string_view url,
                       std::string_view title, std::string_view prompt);
    bool receiveGitWebhook(const GitWebhook&);
    void protectGitCredential(std::string value);
    Id submit(const Intake&);
    // Enqueues a Bot message for Runtime. Retrying the same key and payload returns the same receipt.
    Json receive(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text,
                 Id replyToMessageId = 0, const Json& attachments = Json::array());
    std::optional<Json> receipt(const Person&, std::string_view botId, std::string_view clientMessageId,
                                std::string_view text, Id replyToMessageId = 0, const Json& attachments = Json::array()) const;
    // Conversation turns paused on the model call limit, newest first.
    Json pausedConversations(std::string_view botId) const;
    // Operations: answer {text}, approve, retry, extend {deadline}, reconcile
    // {threadId?,runId?,questionId?,session?}, cancel. All except cancel require a matching waitId.
    void operate(const Person&, const TaskTarget&, std::string_view operation, const Json& data = Json::object());
    Json task(Id taskId) const;
    bool hasTask(Id taskId) const;
    // tasks: newest first, cursor is an exclusive id. history: oldest first, cursor is an exclusive id.
    Json tasks(std::string_view botId = {}, Id before = 0, unsigned limit = 50) const;
    Json compactions(std::string_view bot, Id before = 0, unsigned limit = 50) const;
    Json workflows(std::string_view bot, Id before = 0, unsigned limit = 20) const;
    Json activeTasks(std::string_view botId) const;
    // Distinct active state and replanning combinations, grouped by Bot ID.
    Json activeTaskStatesByBot() const;
    Json sessionTasks(std::string_view botId, Id before, unsigned limit) const;
    // Journal position and tasks with a Run start or settled report since that position.
    // On the initial read only the position is needed, before loading the first page.
    std::pair<Id,Json> sessionChangesSince(std::string_view botId, std::optional<Id> after) const;
    Json history(Id taskId, Id after = 0, unsigned limit = 50) const;
    // Latest completion or Session report for the Task's current revision.
    Json latestTaskResult(Id taskId) const;
    // One page of chronological user-visible conversation, across this Bot's tasks.
    enum class ConversationEvents { all, incoming };
    Json conversation(std::string_view botId, Id before = 0, unsigned limit = 50,
                      ConversationEvents events = ConversationEvents::all, std::string_view viewer = {}) const;
    // Same chronological page for either participant. Each exchange is journaled under both tasks.
    Json peerConversation(std::string_view bot, std::string_view peer, Id before = 0, unsigned limit = 50,
                          Id from = 0, std::string_view viewer = {}) const;
    Id peerMessageAnchor(std::string_view bot, std::string_view peer, Id event) const;
    std::vector<std::pair<std::string,std::string>> peerExchangePairs() const;
    bool hasPeerExchange(std::string_view bot, std::string_view peer) const;
    // A change hint for readers, covering writes on this connection and other connections.
    std::pair<std::int64_t,std::int64_t> changeStamp() const;
    struct ConversationChanges {
        Id through;
        std::map<std::string,std::vector<Id>> messages;
    };
    // Original messages affected by replies and instruction delivery since a journal cursor.
    ConversationChanges conversationChangesSince(std::optional<Id> after, std::string_view bot = {}) const;
    void connectChannel(ChannelConnector);
    void artifactBaseUrl(std::string url);
    Json receiveChannel(ChannelKind, std::string_view credential, const Json& event, const IntakeResolver& routing);
    std::optional<ChannelResource> artifact(std::string_view id) const;
    std::optional<ChannelResource> attachment(const Person&, Id id) const;
    void setRootPeople(std::vector<std::string> ids);
    void setWebOrigin(std::string origin);
    void connectLocalMachine(std::string id, std::shared_ptr<LocalMachine>);
    Json localConnections(const Person&) const;
    void decideLocalConnection(const Person&, Id id, bool approve);
    void disconnectLocalConnection(const Person&, Id id);
    Json runLocalMachineOperation(std::string_view environment, const RunRef& authenticatedRun,
                                  std::string_view machineId, std::string_view operation,
                                  const std::filesystem::path& path);
    const std::filesystem::path& path() const;

private:
    Json receive(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text,
                 Id replyToMessageId, const VerifiedChannelMessage*, const std::function<Intake()>&,
                 const Json& attachments = Json::array());
    struct Impl;
    std::unique_ptr<Impl> p_;
    friend class Runtime;
    friend class Access;
};

struct BotTools;
struct RuntimeOptions {
    std::function<Seconds()> clock = unixTime;
    // "off", "on_demand", "prefill". Prefill uses the same bounded T3 history reader.
    std::string sessionRead = "off";
    // Resolve the peers this sender can use, with each peer's own execution settings.
    std::function<std::vector<PeerBot>(std::string_view)> peers = {};
    std::function<void(std::string_view)> automationError = {};
    std::shared_ptr<const BotTools> botTools = {};
    std::shared_ptr<GitHost> gitHost = {};
};
class Runtime {
public:
    using SoulLoader = std::function<std::string(std::string_view)>;
    Runtime(Store&, Model&, T3&, SoulLoader, RuntimeOptions = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void runConversations();
    // tick() completes a step for each eligible project. tickConcurrent() schedules
    // eligible projects independently and returns immediately.
    void tick();
    void tickConcurrent();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace flockv0
