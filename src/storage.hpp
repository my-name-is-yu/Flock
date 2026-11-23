#pragma once
#include "flock/policy.hpp"
#include "flock/participation.hpp"
#include "flock/secrets.hpp"
#include "workspace_internal.hpp"
#include "flock/board_inbox.hpp"
#include "flock/git_host.hpp"
#include "flock/channels.hpp"
#include "flock/memory_automation.hpp"
#include <map>
#include <mutex>
#include <sqlite3.h>
#include <utility>
#include <variant>

namespace flockv0 {
namespace conversation { struct Message; }

class StorageError : public std::runtime_error {
public:
    explicit StorageError(sqlite3* db);
    bool contended() const { return code_ == SQLITE_BUSY || code_ == SQLITE_LOCKED; }
private:
    int code_;
};

class Statement {
public:
    Statement(sqlite3*, const char*, const Json& values);
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    bool next();
    Id number(int col) const { return sqlite3_column_int64(stmt_, col); }
    std::string string(int col) const;
    Json json(int col) const { return Json::parse(string(col)); }
private:
    sqlite3_stmt* stmt_ = nullptr;
};
std::string field(const Json&, std::string_view);
bool terminal(const Json&);
Json questionTarget(const Json&);
std::string actionId();
std::string taskIntent(const Json& task);
std::string effectiveDoneWhen(const Json& task);
Json runJson(const RunRef&);
RunRef runRef(const Json&);
Json gitJournal(const GitContext&, std::string_view tool, const GitRepository&, Json result);

enum class RequestJournal { record, skip };

bool modelArgumentsContainProtected(const SecretRedactor&, std::string_view tool, const Json& args);

struct ModelCall : Decision {
    std::string rejection;
};

struct CheckedModelTurn {
    std::vector<ModelCall> calls;
    Json usage;
};

struct ConversationWorkflowOrigin { Id messageId; };
struct ReceivedPeerWork { std::string delegationId, requestId, startKey; };
using WorkflowOrigin = std::variant<ConversationWorkflowOrigin, ReceivedPeerWork>;

struct PeerExchange {
    std::string id, kind, phase, lastMessage;
    Id sender = 0, recipient = 0;
    bool allowsReplan(Id task) const { return recipient == task && kind == "work" && phase == "working"; }
};

struct TaskChange { std::string intent, doneWhen, environment; };

struct ConversationModelFailure {
    std::optional<Seconds> retryFrom;
};

struct ConversationModelCheckpoint {
    Id attempt;
    int step;
    Json usage;
    std::variant<std::vector<ModelCall>,ConversationModelFailure> outcome;
};

class SecretStore {
public:
    explicit SecretStore(const std::filesystem::path& stateDirectory);
    ~SecretStore();
    SecretStore(const SecretStore&) = delete;
    void insert(const SecretRef&, std::string_view value);
    std::string read(const SecretRef&) const;
private:
    int stateDirectory_;
};

Id requiredInteger(const Json&, std::string_view key, Id minimum = 1);
std::string redactT3Tokens(std::string);

struct Store::Impl {
    sqlite3* db = nullptr;
    const std::filesystem::path path;
    const MemorySeed memorySeed;
    mutable std::recursive_mutex mutex;
    SecretStore secrets{path.parent_path()};
    class SessionEvidence;
    std::map<ChannelKind,ChannelConnector> channels;
    std::map<std::string,std::shared_ptr<LocalMachine>,std::less<>> localMachines;
    std::vector<std::string> rootPeople;
    WebIdentityResolver resolveWebIdentity;
    std::string artifactOrigin, webOrigin;
    std::mutex deliveryMutex;
    void openSchema();
    void initializeSchema();
    void initializeChannelSchema();
    void initializeLocalMachineSchema();
    std::optional<Json> messageReceipt(const Person&, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                                       const Json& attachments) const;
    void saveChannelMessage(Id, std::string_view bot, const VerifiedChannelMessage&, bool replay);
    bool channelMessageAuthorized(Id, std::string_view bot) const;
    Json channelContext(Id, std::string_view bot, std::vector<ModelImage>* images = nullptr, std::string_view viewer = {}) const;
    std::optional<ChannelResource> channelAttachment(Id source, Id attachment, std::string_view bot) const;
    Json channelTool(Id, std::string_view bot, const Decision&);
    void channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies);
    bool permitsLocalMachineOwner(std::string_view owner, std::string_view bot) const;
    Json localMachineTool(std::string_view bot, std::string_view scope, const Decision&);
    void queueChannelReply(Id source, Id reply, const Json&);
    void queueChannelTaskUpdate(Id task, Id entry, std::string_view kind, const Json&);
    bool channelDeliveryDue() const;
    void deliverChannels(std::stop_token = {});
    Json channelToolStatus(Id source, std::string_view bot, Json, std::vector<ModelImage>* images = nullptr) const;
    bool permitsChannel(std::string_view bot, const ChannelOrigin&, std::string_view sender, std::string_view person,
                        std::string_view operation) const;
    void initializePolicySchema();
    void initializeParticipationSchema();
    void initializeRootDeliverySchema();
    void initializeSessionHandoffSchema();
    Participation authorizeParticipation(const Person&, Id channel, bool requireConnection = true);
    void initializeMemorySchema();
    void initializeAutomationSchema();
    BotNotes loadNotes(std::string_view bot, const Runtime::SoulLoader&, Seconds now);
    void createMemory(std::string_view key, std::string_view bot, std::string_view id,
                      std::string_view content, Seconds now);
    std::string seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor&, Seconds now);
    Json memory(std::string_view bot, const Person&, std::string_view operation, const Json&,
                Seconds now, bool fromBot = false);
    Json automationTool(std::string_view bot, const Person&, const Json& routing, const Decision&, Seconds now);
    Json createAutomation(const Intake&, const Json&, std::string_view author, Seconds now);
    void syncSchedule(const ConfiguredSchedule&, Seconds now, const SecretRedactor&);
    Json automations(std::string_view bot, Id before) const;
    Json cancelAutomation(std::string_view bot, Id id);
    void fireDueAutomations(Seconds now);
    void fireAutomation(Id, const Statement&, Seconds now);
    SecretRedactor sharedRedactor() const;
    void initializeBoardInboxSchema();
    void initializeGitSchema();
    Json gitTool(GitHost&, std::string_view bot, Id task, const Json& routing, const Decision&,
                 std::string_view invocation, std::optional<Id> generation = {});
    void projectTaskGitReads(GitHost&, const Json& task, Json& event);
    Json saveTaskGitRead(Id task, std::string_view invocation, std::string_view tool, const Json& target, const Json& result);
    Json openGitPr(GitHost&, const GitContext&, const GitOpenPr&, std::string_view invocation, bool newIntent);
    void deliverGitWebhooks();
    std::optional<Id> gitWebhooksDrainedAt;
    bool canReceiveGitWebhook(std::string_view bot, const Json& event);
    bool watchGitPr(Id task, std::string_view bot, const Json& run, Id eventCursor, const GitPullRequest&);
    void decideGitPr(const InboxTarget&, std::string_view decision, const Json& data, const Json& task);
    std::vector<std::string> gitCredentials;
    Id gitRedactionEpoch = 0;
    void gitInboxItem(const Json& task, std::string_view key, std::string_view url,
                      std::string_view title, std::string_view prompt);
    void insertPrReview(const Json& task, std::string_view url, std::string_view title, std::string_view prompt,
                        std::string_view gitKey, std::string_view operationId = {},
                        const Json& candidates = Json::array());
    std::vector<std::string> visibleBoardBots(const Person&, std::string_view bot);
    void recordInboxDecision(const Person&, const InboxTarget&, std::string_view decision);
    void approveMessageModelCall(Id message, Id pause);
    void recordTaskBlocker(Json&, std::string_view reason, Id waitId);
    void withdrawTaskQuestion(Json&, Id waitId);
    SecretRedactor redactor(std::string_view bot, Json* revision = nullptr) const;
    Json accessTool(std::string_view bot, const Person& actor, Id sourceMessage, const Json& routing, const Decision&);
    Json sessionReadTool(std::string_view bot, std::string_view channelScope, const Json& routing, const Decision&, T3&);
    void authorizeSessionHistory(std::string_view bot, std::string_view channelScope, Json& steps, T3&);
    Json readiness(std::string_view bot, const Json& routing, const Json& args, T3&);
    Seconds environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const;
    explicit Impl(std::filesystem::path path, MemorySeed);
    ~Impl();
    Statement query(const char* sql, const Json& args = Json::array()) const { return Statement(db, sql, args); }
    void exec(const char*, const Json& args = Json::array()) const;
    Id dataVersion() const;
    Id last() const { return sqlite3_last_insert_rowid(db); }
    Json load(Id) const;
    Json taskDetails(Id, bool includeBlocker) const;
    Json sessionTask(Id) const;
    void addTaskDetails(Json&) const;
    Json queueBlocker(Id) const;
    void initializeWorkflowSchema();
    void initializeCompactionSchema();
    Json compactions(std::string_view bot, Id before, unsigned limit, Id beforeMessage = 0) const;
    Json conversationHistory(const conversation::Message&, const Json& summaries, std::size_t expandedBytes) const;
    Json contextTool(std::string_view tool, const Json&, const conversation::Message&, int step);
    Id startWorkflow(const WorkflowOrigin&, const Json& args, std::string_view bot, const Json& routing,
                     std::string_view inputArtifactVersion = {});
    Json workflowTool(std::string_view tool, const Json&, std::string_view bot, Id message, int step, const Json& routing);
    Json workflowStatus(Id id, std::string_view bot, Seconds now) const;
    void advanceWorkflows(Seconds now);
    void materializeWorkflowStages(Id scopedWorkflow = 0);
    void appendStageRequirements(std::string& instruction, const Json& task) const;
    void stageObservation(const Json& task, const Observation&, Seconds now);
    bool settleStageReport(Json& task);
    void initializeConversationSchema();
    void initializeModelBudgetSchema();
    void initializeWorkspaceSchema();
    void initializeAttachmentsSchema();
    void saveAttachments(Id, const Json&);
    Json inspectAttachment(std::string_view bot, Id throughMessage, const Json&);
    Json attachmentViews(Id) const;
    Statement scopedAttachment(std::string_view bot, Id throughMessage, const Json& args) const;
    void authorizeBotToolHistory(std::string_view bot, Id message, ModelInput&);
    std::vector<ModelImage> inspectedImages(std::string_view bot, Id message, const Json& steps) const;
    void recoverWorkspace();
    void persistWorkspaceResults();
    void stopWorkspace();
    std::map<Id,std::unique_ptr<WorkspaceJob>> workspaceJobs;
    void validateWorkspace(const ToolContext&, const Decision&);
    Json workspaceTool(const ToolContext&, const Decision&);
    std::optional<Resource> authorizedBotToolScope(std::string_view bot, const Decision&);
    void validateBotTool(const ToolContext&, const Decision&);
    Json executeBotTool(const ToolContext&, const Decision&);
    void saveTurnResults(Id, int step, const Json& results);
    bool runBotTools(const ToolContext&, int step, const std::vector<ModelCall>&, std::stop_token);
    std::string messageModelBudget(Id messageId);
    std::string taskModelBudget(const Json& task);
    std::string ensureModelBudget(std::string key, const Json& config);
    Id reserveModelCall(std::string_view budget, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    Id takeReservedConversationCall(Id messageId);
    Id takeReservedTaskCall(Id event);
    void releaseModelCall(Id attempt);
    Id reserveExtraModelCall(std::string_view budget, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    SecretRedactor modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args,
                                     bool gitNotification) const;
    ModelCall checkModelCall(std::string_view bot, Decision, const std::vector<std::string>& offered,
                             bool gitNotification);
    CheckedModelTurn callModel(std::string_view bot, Model&, const ModelInput&, Id attemptId);
    std::vector<std::pair<std::string,Id>> pendingConversations(unsigned limit, Seconds now) const;
    void deferConversation(Id, Seconds now);
    std::map<Id,ConversationModelCheckpoint> conversationCheckpoints;
    void runTurn(Id messageId, Model&, T3&, const Runtime::SoulLoader&, const std::function<Seconds()>&,
                 const BotTools&, GitHost*, const ApplyBotChange&, const ReconcileBotCreation&, std::stop_token);
    bool runSessionEffect(const conversation::Message&, int step, const Decision&, bool offered, bool hasWebEvidence,
                          T3&, std::stop_token);
    bool isNextTurn(Id messageId) const;
    bool authorizeTurn(const conversation::Message&);
    Person turnActor(const conversation::Message&);
    conversation::Message loadTurnMessage(Id messageId) const;
    const char* questionAnswerError(const Json& task, const conversation::Message&) const;
    Json taskState(const Json& task, const conversation::Message&, bool fullContract = false) const;
    Json conversationSteps(const conversation::Message&, int step, std::vector<ModelImage>& images);
    Json turnInput(const conversation::Message&, int step, Seconds now, Json machines, std::vector<ModelImage>& images);
    Json gitConversationTool(GitHost*, const conversation::Message&, int step, const Decision&);
    bool gitConversationEffectStarted(Id message, int step) const;
    bool gitEffectStarted(std::string_view invocation, Id taskId = 0) const;
    Json applyTurnTool(const conversation::Message&, int step, const Decision&, Seconds now);
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, std::string_view route);
    void pauseTurn(const conversation::Message&);
    Json ownTask(Id taskId, const conversation::Message&) const;
    Json resultReference(Id messageId, const conversation::Message&) const;
    Id startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                             std::string_view environment, Id messageId, const Json& source, bool resume);
    bool acceptInstruction(Json& task, Id messageId, int step, std::string_view instruction,
                           const Json& source, const Json& routing, const TaskChange* change);
    Id addRevision(Json&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    bool conversationPending(std::string_view bot) const;
    void save(Json&);
    Id submit(const Intake&, Id sourceMessageId = 0, const Json& source = nullptr,
              RequestJournal requestJournal = RequestJournal::record, const Json& trigger = nullptr);
    void operate(const Person&, const TaskTarget&, std::string_view, const Json&,
                 Id sourceMessage = 0, std::optional<InboxKind> = {});
    bool waitIsActionable(const Json& task) const;
    Id waitAnchor(Id task, Id wait) const;
    std::optional<PeerExchange> peerExchange(Id) const;
    void initializeSpecialistSchema();
    bool peerWorkRouteWouldCycle(Id sender, std::string_view environment, std::string_view project) const;
    bool replanReady(const Json& task) const;
    void peerMessage(const PeerExchange&, const Json&);
    void requestCancellation(Json&);
    Id journal(Id, std::string_view kind, const Json&);
    Id event(Json&, std::string_view key, Json data);
    Json currentEvent(const Json&) const;
    void wait(Json&, std::string_view reason, Json data = Json::object());
    void clearWait(Json&, std::string_view resolution);
    void consumeEvent(const Json&);
    Json outbox(const Json&) const;
    void unlockQuestion(Json&, std::string_view reason);
};

std::optional<InboxKind> inboxWaitKind(std::string_view reason);

class Transaction {
public:
    enum class Mode { write, read };
    explicit Transaction(sqlite3* db, Mode mode = Mode::write) : db_(db) {
        Statement(db_, mode == Mode::write ? "BEGIN IMMEDIATE" : "BEGIN", Json::array()).next();
    }
    ~Transaction() { if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { Statement(db_, "COMMIT", Json::array()).next(); done_ = true; }
private:
    sqlite3* db_;
    bool done_ = false;
};
}
