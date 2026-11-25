#pragma once
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "workspace_internal.hpp"
#include "flock/board_inbox.hpp"
#include "flock/git_host.hpp"
#include "flock/channels.hpp"
#include "flock/memory_automation.hpp"
#include <map>
#include <mutex>
#include <set>
#include <sqlite3.h>
#include <stop_token>
#include <utility>
#include <variant>

namespace flockv0 {
namespace conversation { struct Message; struct QuestionAnchor; }

struct WorkSession {
    std::string environmentId, threadId, projectId;
};

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
std::string conversationEnvironmentId(const Json& alias);
std::string conversationGatewayEnvironmentId(const Json& alias);
std::string configuredT3Project(const Json& routing, std::string_view environment, std::string_view name = {});
inline constexpr std::string_view runOutboxOperation = "run-outbox/";
bool terminal(const Json&);
Json questionTarget(const Json&);
std::string actionId();
std::string taskIntent(const Json& task);
std::string effectiveDoneWhen(const Json& task);
Json runJson(const RunRef&);
RunRef runRef(const Json&);
Json& destinationRun(Json& task, std::string_view action);
bool destinationRunOutstanding(const Json& run);
void setReportEvidence(Json&, const RunEvidence&);
Json gitJournal(const GitContext&, std::string_view tool, const GitRepository&, Json result);

enum class TaskSource { request, handoff, consultation };

bool modelArgumentsContainProtected(const SecretRedactor&, std::string_view tool, const Json& args);

struct ModelCall : Decision {
    std::string rejection;
};

struct CheckedModelTurn {
    std::vector<ModelCall> calls;
    Json usage;
};


struct PeerExchange {
    std::string id, kind, phase, lastMessage;
    Id sender = 0, recipient = 0;
    bool allowsReplan(Id task) const { return recipient == task && kind == "work" && phase == "working"; }
};

struct TaskRoute { std::string environmentId, projectId, gatewayEnvironmentId; };
struct LinkedTaskDestination { std::string name; TaskRoute route; };
struct LinkedTaskDestinations { std::vector<LinkedTaskDestination> choices; std::vector<std::string> unavailable; };
struct StartDestinations {
    TaskRoute primary;
    Json targets = Json::array();
    std::string commit;
};
struct TaskChange { std::string intent, doneWhen; TaskRoute route; };
struct PendingDestination {
    TaskRoute route;
    bool pinned;
    static std::optional<PendingDestination> read(const Json& task);
    void write(Json& task) const;
};
using ProjectCatalog = std::variant<std::vector<T3Project>,std::string>;

struct ConversationModelFailure {
    std::optional<Seconds> retryFrom;
    ModelErrorCode code = ModelErrorCode::inference_failed;
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

struct PolicyOrigin {
    std::string bot, scope, requester;
    Id anchor = 0, turn = 0, task = 0, revision = 0, generation = 0;
    bool background = false;
};
struct SessionStopResult;
enum class SessionExit { removed, left };
struct SessionStop {
    Id participation = 0, cutoff = 0;
};

class SessionDispatchFileLock {
public:
    enum class Mode { shared, exclusive };
    SessionDispatchFileLock(const std::filesystem::path& database, Mode);
    ~SessionDispatchFileLock();
    SessionDispatchFileLock(const SessionDispatchFileLock&) = delete;
    SessionDispatchFileLock& operator=(const SessionDispatchFileLock&) = delete;
private:
    int fd_;
};

enum class ApprovalGate { allow, waiting, denied, expired, stale };
std::string_view approvalError(ApprovalGate);
enum class GitWatchStart { operationIntent, mutationAdmission };
struct GitPrKey {
    std::string host, repo;
    Id number;
    auto operator<=>(const GitPrKey&) const = default;
};

struct Store::Impl {
    sqlite3* db = nullptr;
    const std::filesystem::path path;
    const MemorySeed memorySeed;
    const AutomationModelResolver automationModel;
    mutable std::recursive_mutex mutex;
    SecretStore secrets{path.parent_path()};
    class SessionEvidence;
    std::map<ChannelKind,ChannelConnector> channels;
    std::map<std::string,std::shared_ptr<LocalMachine>,std::less<>> localMachines;
    std::vector<std::string> rootPeople;
    WebIdentityResolver resolveWebIdentity;
    bool liveInteractiveRoles = false;
    std::string artifactOrigin, webOrigin;
    std::mutex deliveryMutex;
    std::mutex sessionQueueCancellationMutex;
    std::function<Seconds()> policyClock = unixTime;
    void openSchema();
    void initializeSchema();
    bool botDeleted(std::string_view bot) const;
    void initializeChannelSchema();
    void initializeLocalMachineSchema();
    Id enqueueConversation(const Person&, const Intake&, conversation::PrincipalSource,
                           std::string_view clientId, std::string_view text, Id replyTo = 0,
                           const SessionOrigin* origin = nullptr, const Json& invitation = nullptr);
    std::optional<Json> messageReceipt(const Person&, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                                       const Json& attachments) const;
    void saveChannelMessage(Id, std::string_view bot, const VerifiedChannelMessage&, bool replay);
    bool channelMessageAuthorized(Id, std::string_view bot) const;
    Json channelContext(Id, std::string_view bot, std::vector<ModelImage>* images = nullptr, std::string_view viewer = {}) const;
    std::optional<ChannelResource> channelAttachment(Id source, Id attachment, std::string_view bot) const;
    Json channelTool(Id, std::string_view bot, const Decision&);
    void channelApprovalNotice(Id source);
    void channelNotice(Id source, std::string_view bot, std::string_view reason, bool supportsReplies,
                       std::string_view followUp = {});
    void channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies);
    bool permitsLocalMachineOwner(std::string_view owner, std::string_view bot) const;
    Json localMachineTool(std::string_view bot, std::string_view scope, const Decision&);
    std::optional<Resource> channelToolPolicy(Id, std::string_view tool);
    void queueChannelReply(Id source, Id reply, const Json&);
    void queueChannelTaskUpdate(Id task, Id entry, std::string_view kind, const Json&);
    bool channelDeliveryDue() const;
    void deliverChannels(std::stop_token = {});
    Json channelToolStatus(Id source, std::string_view bot, Json, std::vector<ModelImage>* images = nullptr) const;
    bool permitsChannel(std::string_view bot, const ChannelOrigin&, std::string_view sender, std::string_view person,
                        std::string_view operation) const;
    void initializePolicySchema();
    PolicyOrigin policyOrigin(const conversation::Message&);
    PolicyOrigin taskPolicyOrigin(const Json&);
    enum class ApprovalWait { pause, defer };
    ApprovalGate approveAction(const PolicyOrigin&, const Resource&, std::string_view operation,
                               const Json& payload, Seconds now, bool claim = true, ApprovalWait = ApprovalWait::pause);
    void consumePolicyApproval(std::string_view operation);
    bool currentPolicyApproval(Id turn, Id task, Id revision, Id generation, std::string_view operation) const;
    void settlePolicyApprovals(std::optional<Seconds> now = {});
    void decidePolicyApproval(const Person&, const InboxTarget&, std::string_view);
    Json policyApprovalCards(const Person&, std::string_view bot);
    void releasePolicyTurn(Id turn);
    void releasePolicyTask(Id approval, std::string_view resolution);
    void initializeParticipationSchema();
    void initializeExternalMcpSchema();
    Person sessionActor(std::string_view person);
    bool activeSessionOrigin(const conversation::Message&) const;
    bool activeSessionOrigin(std::string_view bot, std::string_view person, const SessionTarget& origin,
                             std::string_view destinationEnvironment, Id message) const;
    bool sessionParticipationStopped(std::string_view bot, const SessionTarget&, Id message) const;
    SessionStop stopSessionParticipation(const Person&, std::string_view bot, const SessionTarget&, SessionExit, Id message = 0);
    SessionStopResult settleSessionQueueCancellations(T3&, std::stop_token, SessionStop stop = {});
    bool connectedSessionAccess(const conversation::Message&, const SessionTarget&);
    bool t3EnvironmentConnected(std::string_view environment);
    void initializeMemorySchema();
    void initializeAutomationSchema();
    void retireAutomations(const Json& botIds);
    BotNotes loadNotes(std::string_view bot, const Runtime::SoulLoader&, Seconds now);
    void createMemory(std::string_view key, std::string_view bot, std::string_view id,
                      std::string_view content, Seconds now);
    std::string seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor&, Seconds now);
    Json memory(std::string_view bot, const Person&, std::string_view operation, const Json&,
                Seconds now, bool fromBot = false);
    bool canReviewPreference(const conversation::Message&);
    bool preferenceReviewPending(Id message, int step) const;
    std::optional<ModelInput> preferenceReviewInput(const conversation::Message&, std::vector<std::string>,
                                                    std::string feedback, Seconds now);
    Json rememberPreference(const conversation::Message&, int step, const Json&, Seconds now);
    void projectMemoryNotice(std::string_view bot, Json& data) const;
    Json automationTool(std::string_view bot, const Person&, const Json& routing, const Decision&, Seconds now);
    Json createAutomation(const Intake&, const Json&, std::string_view author, Seconds now);
    std::vector<ScheduleImportError> stageConfiguredSchedules(
        std::string_view bot, const std::vector<ConfiguredSchedule>&, Seconds now);
    void syncSchedule(const ConfiguredSchedule&, Seconds now, const SecretRedactor&);
    Json automations(std::string_view bot, Id before, bool activeOnly = false) const;
    Json cancelAutomation(std::string_view bot, Id id);
    void fireDueAutomations(Seconds now);
    void fireAutomation(Id, const Statement&, Seconds now);
    Json automationRoute(std::string_view bot, Json routing) const;
    SecretRedactor sharedRedactor() const;
    void initializeBoardInboxSchema();
    Statement actionablePrReviews(std::string_view bot) const;
    void initializeGitSchema();
    Json gitTool(GitHost&, std::string_view bot, Id task, const Json& routing, const Decision&,
                 std::string_view invocation, std::optional<Id> generation = {}, Id turn = 0);
    void projectTaskGitReads(GitHost&, const Json& task, Json& event);
    Json saveTaskGitRead(Id task, std::string_view invocation, std::string_view tool, const Json& target, const Json& result);
    Json openGitPr(GitHost&, const GitContext&, const GitOpenPr&, std::string_view invocation, bool newIntent,
                   std::string_view reason);
    void deliverGitWebhooks();
    std::optional<Id> gitWebhooksDrainedAt;
    Id latestGitWebhookId() const;
    std::multiset<GitPrKey> gitPrMutationsInFlight, gitFeedbackMutationsInFlight;
    class GitPrMutationClaim;
    bool canReceiveGitWebhook(std::string_view bot, const Json& event);
    struct GitPrFollowup {
        enum class Status { available, attempted, limitReached };
        SessionTarget session;
        PolicyOrigin origin;
        std::string operation;
        Json target;
        Status status;
    };
    std::optional<GitPrFollowup> gitPrFollowup(const conversation::Message&, const IntakeResolver&);
    bool currentGitPrCheck(GitHost&, const conversation::Message&);
    std::string gitPrFollowupInstruction(const conversation::Message&, const GitPrFollowup&, const Decision&) const;
    void recordGitPrFollowup(const conversation::Message&, const GitPrFollowup&);
    bool gitPrWatchUnavailable(const GitRepository&, Id pr, Id task) const;
    bool watchGitPr(Id task, std::string_view bot, const Json& run, Id deliveryCursor, const GitPullRequest&,
                    GitWatchStart = GitWatchStart::operationIntent);
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
    Json botSettingsTool(std::string_view bot, const Person& actor, Id sourceMessage, const Json& routing, const Decision&);
    struct CompletedToolCall { Json value; bool consumeApproval = false; };
    using ToolCallResult = std::optional<CompletedToolCall>;
    ToolCallResult sessionReadTool(const conversation::Message&, const Decision&, T3&, std::string_view key);
    void authorizeSessionHistory(const conversation::Message&, Json& steps, T3&);
    std::set<std::string> connectedSessionEnvironments(const conversation::Message&);
    std::optional<WorkSession> resolveWorkSession(const conversation::Message&, std::string_view sessionRef, T3&);
    bool permitsWorkSession(const conversation::Message&, const WorkSession&);
    bool permitsWorkSession(Id sourceMessageId, const WorkSession&);
    Json readiness(std::string_view bot, const Json& routing, const Json& args, T3&);
    Seconds environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const;
    explicit Impl(std::filesystem::path path, MemorySeed, AutomationModelResolver);
    ~Impl();
    Statement query(const char* sql, const Json& args = Json::array()) const { return Statement(db, sql, args); }
    void exec(const char*, const Json& args = Json::array()) const;
    Id dataVersion() const;
    std::pair<Id,Id> changeStamp() const;
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
    Id startWorkflow(Id message, const Json& args, std::string_view bot, const Json& routing);
    Json workflowTool(std::string_view tool, const Json&, std::string_view bot, Id message, int step, const Json& routing);
    Json workflowStatus(Id id, std::string_view bot, Seconds now) const;
    void advanceWorkflows(Seconds now);
    void materializeWorkflowStages();
    void appendStageRequirements(std::string& instruction, const Json& task) const;
    void stageObservation(const Json& task, const Observation&, Seconds now);
    bool settleStageReport(Json& task);
    void initializeConversationSchema();
    void initializeModelBudgetSchema();
    void initializeAttachmentsSchema();
    void saveAttachments(Id, const Json&);
    Json inspectAttachment(std::string_view bot, Id throughMessage, const Json&);
    Json attachmentViews(Id) const;
    Statement scopedAttachment(std::string_view bot, Id throughMessage, const Json& args) const;
    void authorizeBotToolHistory(std::string_view bot, Id message, ModelInput&);
    std::vector<ModelImage> inspectedImages(std::string_view bot, Id message, const Json& steps) const;
    Json workspaceTool(const ToolContext&, const Decision&);
    std::optional<Resource> authorizedBotToolScope(std::string_view bot, const Decision&);
    void validateBotTool(const ToolContext&, const Decision&);
    Json executeBotTool(const ToolContext&, const Decision&);
    void saveTurnResults(Id, int step, const Json& results);
    class ToolBatch;
    std::map<std::pair<Id,int>,std::map<std::size_t,ToolCallResult>> unpersistedToolResults;
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
    CheckedModelTurn callModel(std::string_view bot, Model&, const ModelInput&);
    std::vector<std::pair<std::string,Id>> pendingConversations(unsigned limit, Seconds now) const;
    void deferConversation(Id, Seconds now);
    std::map<Id,ConversationModelCheckpoint> conversationCheckpoints;
    struct SessionSendReceipt {
        std::string bot;
        SessionTarget target;
        std::string title;
        Delivery delivery;
        bool notify;
    };
    std::map<Id,SessionSendReceipt> sessionSendReceipts;
    void recordSessionSendRun(Id, const SessionSendReceipt&);
    void runTurn(Id messageId, Model&, T3&, const Runtime::SoulLoader&, const std::function<Seconds()>&,
                 const BotTools&, GitHost*, const ApplyBotChange&, const ReconcileBotCreation&, const IntakeResolver&,
                 const PeerResolver&, std::stop_token);
    enum class CallOutcome { advance, waiting, stop, finished };
    struct ConversationCallContext {
        T3& t3;
        const std::function<Seconds()>& clock;
        GitHost* gitHost;
        const ApplyBotChange& applyBotChange;
        const ReconcileBotCreation& reconcileBotCreation;
        const IntakeResolver& intake;
        const PeerResolver& peers;
        std::stop_token shutdown;
        bool hasWebEvidence;
    };
    bool runToolBatch(const conversation::Message&, const ToolContext&, int step, const std::vector<ModelCall>&,
                      const ConversationCallContext&);
    CallOutcome executeConversationStep(const conversation::Message&, int step, const std::vector<ModelCall>&,
                                        std::string_view effectType, const BotTools&, const ConversationCallContext&);
    CallOutcome executeConversationBotChange(const conversation::Message&, int step, const Decision&,
                                             std::string_view key, const ConversationCallContext&);
    CallOutcome executeConversationReply(const conversation::Message&, int step, const Decision&,
                                          std::string_view key, const std::function<Seconds()>&);
    CallOutcome executeLocalConversationCall(const conversation::Message&, int step, const Decision&,
                                             std::string_view key, const ConversationCallContext&);
    std::optional<Json> applyLocalConversationCall(const conversation::Message&, int step, const Decision&,
                                                   std::string_view key, Seconds now, ApprovalWait,
                                                   const WorkSession* selectedSession,
                                                   const LinkedTaskDestinations& linked);
    std::optional<Resource> conversationCallResource(const conversation::Message&, const Decision&);
    void saveConversationCallResult(const conversation::Message&, int step, Json result);
    CallOutcome sendSessionMessage(const conversation::Message&, int step, const Decision&, std::string_view key,
                                   const ConversationCallContext&);
    CallOutcome cancelDestinationRun(const conversation::Message&, int step, const Decision&, std::string_view key,
                                     bool offered, bool hasWebEvidence, std::stop_token);
    bool inviteSessionBot(const conversation::Message&, int, const Decision&, T3&, const IntakeResolver&,
                          const PeerResolver&, std::stop_token);
    bool leaveSession(const conversation::Message&, int, const Decision&, T3&, std::stop_token);
    void wakeSessionMessages(T3&, std::stop_token);
    CallOutcome runSessionEffect(const conversation::Message&, int step, const Decision&, std::string_view key,
                                 bool offered, bool hasWebEvidence, T3&, std::stop_token);
    bool isNextTurn(Id messageId) const;
    bool authorizeTurn(const conversation::Message&);
    Person turnActor(const conversation::Message&);
    conversation::Message loadTurnMessage(Id messageId) const;
    const char* questionAnswerError(const Json& task, const conversation::Message&) const;
    Json taskState(const Json& task, const conversation::Message&, bool fullContract = false) const;
    Json conversationSteps(const conversation::Message&, int step, std::vector<ModelImage>& images);
    Json turnInput(const conversation::Message&, int step, Seconds now, Json machines, std::vector<ModelImage>& images,
                   const LinkedTaskDestinations& linked);
    Json gitConversationTool(GitHost*, const conversation::Message&, const Decision&, std::string_view key);
    bool gitEffectStarted(std::string_view invocation, Id taskId = 0) const;
    Json applyTurnTool(const conversation::Message&, int step, const Decision&, Seconds now,
                       const WorkSession* selectedSession, const LinkedTaskDestinations& linked);
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, std::string_view route,
                    const conversation::QuestionAnchor* question = nullptr);
    void pauseTurn(const conversation::Message&);
    Json ownTask(Id taskId, const conversation::Message&) const;
    Json resultReference(Id messageId, const conversation::Message&) const;
    Id startConversationTask(const Json& routing, std::string_view bot, std::string_view request,
                             const TaskRoute&, Id messageId, const Json& source, bool resume,
                             const WorkSession* selectedSession = nullptr);
    bool acceptInstruction(Json& task, Id messageId, int step, std::string_view instruction,
                           const Json& source, const TaskChange* change);
    Id addRevision(Json&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    bool conversationPending(std::string_view bot) const;
    void save(Json&);
    void initializeProjectQueue();
    bool automationBeforeDispatch(Id task) const;
    bool isUndispatchedNotesWait(const Json& task) const;
    bool projectRouteReady(Id task, std::string_view action) const;
    std::string rememberedT3Project(std::string_view bot, std::string_view environment) const;
    void rememberT3Project(std::string_view bot, std::string_view environment, std::string_view project);
    bool projectQuestionOpen(std::string_view bot, std::string_view environment) const;
    ProjectCatalog readProjectCatalog(T3&, std::string_view bot, std::string_view environment, bool requireConnection);
    bool applyProjectCatalog(Json& task, ProjectCatalog catalog);
    std::set<std::string> registeredT3Environments(std::string_view bot);
    std::string taskEnvironment(std::string_view bot, const Json& routing, std::string_view name = {});
    TaskRoute taskRoute(std::string_view bot, const Json& routing, std::string_view name,
                        const Json* movingTask = nullptr);
    StartDestinations startDestinations(const conversation::Message&, const Json& args, bool resume,
                                       const WorkSession* selectedSession, const LinkedTaskDestinations& linked);
    LinkedTaskDestinations discoverLinkedTaskDestinations(T3&, std::string_view bot);
    TaskChange taskChange(const conversation::Message&, const Json& args, const Json& task);
    std::string defaultT3Environment(std::string_view bot, std::string_view configured, std::string_view preferred);
    Id submitRequest(const Intake&, const Person* requester);
    Id submit(const Intake&, Id sourceMessageId = 0, const Json& source = nullptr,
              TaskSource sourceKind = TaskSource::request, const Json& trigger = nullptr);
    void operate(const Person&, const TaskTarget&, std::string_view, const Json&,
                 Id sourceMessage = 0, std::optional<InboxKind> = {});
    bool waitIsActionable(const Json& task) const;
    Id waitAnchor(Id task, Id wait) const;
    std::optional<PeerExchange> peerExchange(Id) const;
    bool peerWorkRouteWouldCycle(Id sender, std::string_view environment, std::string_view project) const;
    bool recipientRouteWouldCycle(Id task, std::string_view environment, std::string_view project) const;
    bool replanReady(const Json& task) const;
    void peerMessage(const PeerExchange&, const Json&);
    void requestCancellation(Json&, SessionStop stop = {});
    Id journal(Id, std::string_view kind, const Json&);
    Id event(Json&, std::string_view key, Json data);
    Json currentEvent(const Json&) const;
    void wait(Json&, std::string_view reason, Json data = Json::object());
    void clearWait(Json&, std::string_view resolution);
    bool recordDestinationReport(Json& task, Json& run, const Observation&);
    void consumeEvent(const Json&);
    Json outbox(const Json&, std::string_view actionId = {}) const;
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
