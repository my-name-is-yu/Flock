#pragma once
#include "task_interaction_storage.hpp"
#include "database.hpp"
#include "json_fields.hpp"
#include "wait_storage.hpp"
#include "task_records.hpp"
#include "task_events.hpp"
#include "journal_storage.hpp"
#include "attachment_storage.hpp"
#include "bot_lifecycle_storage.hpp"
#include "peer_storage.hpp"
#include "outbox_storage.hpp"
#include "run_ref_storage.hpp"
#include "secret_storage.hpp"
#include "web_identity_state.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "workspace_internal.hpp"
#include "wait_storage.hpp"
#include "flock/board_inbox.hpp"
#include "flock/git_host.hpp"
#include "flock/channels.hpp"
#include "flock/memory_automation.hpp"
#include "flock/outbox.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/bot_change.hpp"
#include "flock/json.hpp"
#include "flock/model_error.hpp"
#include "flock/t3_command.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox_instruction.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

namespace flockv0 {
class Model;
class T3;
struct BotNotes;
struct ModelImage;
struct ModelInput;
struct Observation;
struct T3Project;

namespace conversation {
struct Message;
struct QuestionAnchor;
}

struct ModelBudget {
    std::string id;
    Id initialLimit;
};

inline bool hasDestinationSelector(const Json& args) {
    return args.is_object() && (args.contains("destination") || args.contains("environmentId") || args.contains("projectId") ||
                                args.contains("gatewayEnvironmentId"));
}
std::string conversationEnvironmentId(const Json& alias);
std::string conversationGatewayEnvironmentId(const Json& alias);
std::string configuredT3Project(const Json& routing, std::string_view environment, std::string_view name = {});
inline constexpr std::string_view runOutboxOperation = "run-outbox/";
inline constexpr std::string_view runStopOperation = "run-stop/";
std::string effectiveDoneWhen(const TaskData& task);
DestinationRun& destinationRun(TaskData& task, std::string_view action);
bool destinationStopRequested(const std::optional<Outbox>& action, const RunRef&);
RunEvidence boundedReportEvidence(const RunEvidence&);
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

struct LinkedTaskDestinations {
    std::vector<RunTarget> choices;
    std::vector<std::string> unavailable;
};
struct StartDestinations {
    TaskRoute primary;
    std::vector<RunTarget> targets;
    std::string commit;
};
struct TaskChange {
    std::string intent, doneWhen;
    TaskRoute route;
};
using ProjectCatalog = std::variant<std::vector<T3Project>, std::string>;

struct ConversationModelFailure {
    std::optional<Seconds> retryFrom;
    ModelErrorCode code = ModelErrorCode::inference_failed;
};

struct ConversationModelCheckpoint {
    Id attempt;
    int step;
    Json usage;
    std::variant<std::vector<ModelCall>, ConversationModelFailure> outcome;
};

Id requiredInteger(const Json&, std::string_view key, Id minimum = 1);
std::string redactT3Tokens(std::string);

struct PolicyOrigin {
    std::string bot, scope, requester;
    Id anchor = 0, turn = 0, task = 0, revision = 0, generation = 0;
    bool background = false;
};
struct CompletionEntry;
struct ToolErrorEntry;
struct DecisionEntry;
struct ConversationInvitationEntry;
struct ConversationMessageEntry;
struct ConversationReplyEntry;
enum class ConversationReplyRoute;
struct SessionStopResult;
struct TaskRevisionEntry;
struct DeliveredInstructionEntry;
struct UserAnswerEntry;
struct SendStartedEntry;
struct RunReportEntry;
struct ConversationInterruptEntry;
struct BotMessageEntry;
struct PeerEvidenceEntry;
struct PrReviewRequestedEntry;
struct GitPrFollowupEntry;
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

struct Store::Impl : Database {
    OutboxStorage outboxStorage{*this};
    PeerStorage peerStorage{*this};
    JournalStorage journalStorage{*this, *this};
    TaskEvents taskEvents{*this, *this};
    WaitStorage waitStorage{*this, *this, journalStorage, outboxStorage, peerStorage};
    TaskRecords taskRecords{*this, journalStorage, outboxStorage, waitStorage};
    const std::filesystem::path path;
    const MemorySeed memorySeed;
    const AutomationModelResolver automationModel;
    SecretStorage secretStorage{*this, path.parent_path()};
    AttachmentStorage attachments{*this};
    BotLifecycleStorage botLifecycle{*this};
    class SessionEvidence;
    std::map<ChannelKind, ChannelConnector> channels;
    std::map<std::string, std::shared_ptr<LocalMachine>, std::less<>> localMachines;
    std::vector<std::string> rootPeople;
    WebIdentityState webIdentity;
    std::string artifactOrigin, webOrigin;
    std::mutex deliveryMutex;
    std::mutex sessionQueueCancellationMutex;
    std::function<Seconds()> policyClock = unixTime;
    Id enqueueConversation(const Person&, const Intake&, conversation::PrincipalSource, std::string_view clientId, std::string_view text,
                           Id replyTo = 0, const SessionOrigin* origin = nullptr, const ConversationInvitationEntry* invitation = nullptr);
    std::optional<Json> messageReceipt(const Person&, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                                       const Json& attachments) const;
    void saveChannelMessage(Id, std::string_view bot, const VerifiedChannelMessage&, bool replay);
    bool channelMessageAuthorized(Id, std::string_view bot) const;
    Json channelContext(Id, std::string_view bot, std::vector<ModelImage>* images = nullptr, std::string_view viewer = {}) const;
    std::optional<ChannelResource> channelAttachment(Id source, Id attachment, std::string_view bot) const;
    Json channelTool(Id, std::string_view bot, const Decision&);
    void channelApprovalNotice(Id source);
    void channelNotice(Id source, std::string_view bot, std::string_view reason, bool supportsReplies, std::string_view followUp = {});
    void channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies);
    bool permitsLocalMachineOwner(std::string_view owner, std::string_view bot) const;
    Json localMachineTool(std::string_view bot, std::string_view scope, const Decision&);
    std::optional<Resource> channelToolPolicy(Id, std::string_view tool);
    void queueChannelReply(Id source, Id reply, std::string_view text, const Json& suggestions = Json::array());
    void queueChannelTaskUpdate(Id task, Id entry, std::string_view text, std::span<const std::string> suggestions = {});
    bool channelDeliveryDue() const;
    void deliverChannels(std::stop_token = {});
    void dropPausedBotDeliveries(std::string_view bot);
    Json channelToolStatus(Id source, std::string_view bot, Json, std::vector<ModelImage>* images = nullptr) const;
    bool permitsChannel(std::string_view bot, const ChannelOrigin&, std::string_view sender, std::string_view person,
                        std::string_view operation) const;
    PolicyOrigin policyOrigin(const conversation::Message&);
    PolicyOrigin taskPolicyOrigin(const TaskData&);
    enum class ApprovalWait { pause, defer };
    ApprovalGate approveAction(const PolicyOrigin&, const Resource&, std::string_view operation, const Json& payload, Seconds now,
                               bool claim = true, ApprovalWait = ApprovalWait::pause);
    void consumePolicyApproval(std::string_view operation);
    bool currentPolicyApproval(Id turn, Id task, Id revision, Id generation, std::string_view operation) const;
    void settlePolicyApprovals(std::optional<Seconds> now = {});
    void decidePolicyApproval(const Person&, const InboxTarget&, std::string_view);
    Json policyApprovalCards(const Person&, std::string_view bot);
    void releasePolicyTurn(Id turn);
    void releasePolicyTask(Id approval, std::string_view resolution);
    Person sessionActor(std::string_view person);
    bool activeSessionOrigin(const conversation::Message&) const;
    bool activeSessionOrigin(std::string_view bot, std::string_view person, const SessionTarget& origin,
                             std::string_view destinationEnvironment, Id message) const;
    bool sessionParticipationStopped(std::string_view bot, const SessionTarget&, Id message) const;
    SessionStop stopSessionParticipation(const Person&, std::string_view bot, const SessionTarget&, SessionExit, Id message = 0);
    void stopPausedBotParticipation(std::string_view bot);
    void interruptPausedSessionRuns(T3&, std::stop_token);
    SessionStopResult settleSessionQueueCancellations(T3&, std::stop_token, SessionStop stop = {});
    bool connectedSessionAccess(const conversation::Message&, const SessionTarget&);
    bool t3EnvironmentConnected(std::string_view environment);
    void retireAutomations(const Json& botIds);
    BotNotes loadNotes(std::string_view bot, const std::function<std::string(std::string_view)>&, Seconds now);
    std::optional<SessionMemory> sessionMemory(const Command&, Seconds now);
    void createMemory(std::string_view key, std::string_view bot, std::string_view id, std::string_view content, Seconds now);
    std::string seedPrivateMemory(std::string_view key, std::string_view bot, const SecretRedactor&, Seconds now);
    Json memory(std::string_view bot, const Person&, std::string_view operation, const Json&, Seconds now, bool fromBot = false);
    bool canReviewPreference(const conversation::Message&);
    bool preferenceReviewPending(Id message, int step) const;
    std::optional<ModelInput> preferenceReviewInput(const conversation::Message&, std::vector<std::string>, std::string feedback,
                                                    Seconds now);
    Json rememberPreference(const conversation::Message&, int step, const Json&, Seconds now);
    void projectMemoryNotice(std::string_view bot, Json& data) const;
    Json automationTool(std::string_view bot, const Person&, const Json& routing, const Decision&, Seconds now);
    Json createAutomation(const Intake&, const Json&, std::string_view author, Seconds now);
    std::vector<ScheduleImportError> stageConfiguredSchedules(std::string_view bot, const std::vector<ConfiguredSchedule>&, Seconds now);
    void syncSchedule(const ConfiguredSchedule&, Seconds now, const SecretRedactor&);
    Json automations(std::string_view bot, Id before, bool activeOnly = false) const;
    Json cancelAutomation(std::string_view bot, Id id);
    void fireDueAutomations(Seconds now);
    void skipDueAutomations(std::string_view bot, Seconds now);
    std::vector<Id> dueAutomations(Seconds now, std::string_view bot = {}) const;
    void processDueAutomation(Id, Seconds now, bool skip = false);
    static void applyPlannerModel(Json& config, const PlannerModel&);
    Json automationRoute(std::string_view bot, Json routing) const;
    Statement actionablePrReviews(std::string_view bot) const;
    Json gitTool(GitHost&, std::string_view bot, Id task, std::string_view environment, std::string_view project, const Decision&,
                 std::string_view invocation, std::optional<Id> generation = {}, Id turn = 0);
    void projectTaskGitReads(GitHost&, const TaskData& task, Json& event);
    Json saveTaskGitRead(Id task, std::string_view invocation, std::string_view tool, const Json& target, const Json& result);
    Json openGitPr(GitHost&, const GitContext&, const GitOpenPr&, std::string_view invocation, bool newIntent, std::string_view reason);
    void deliverGitWebhooks();
    void skipPendingGitWebhooks(std::string_view bot);
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
    void decideGitPr(const InboxTarget&, std::string_view decision, const Json& data, const TaskData& task);
    void gitInboxItem(const TaskData& task, std::string_view key, std::string_view url, std::string_view title, std::string_view prompt);
    void insertPrReview(const TaskData& task, std::string_view url, std::string_view title, std::string_view prompt,
                        std::string_view gitKey, std::string_view operationId = {}, const Json& candidates = Json::array());
    std::vector<std::string> visibleBoardBots(const Person&, std::string_view bot);
    void recordInboxDecision(const Person&, const InboxTarget&, std::string_view decision);
    void approveMessageModelCall(const Person&, std::string_view bot, Id message, Id pause);
    void recordTaskBlocker(TaskData&, std::string_view reason, Id waitId);
    void withdrawTaskQuestion(TaskData&, Id waitId);
    Json botSettingsTool(std::string_view bot, const Person& actor, Id sourceMessage, const Json& routing, const Decision&);
    struct CompletedToolCall {
        Json value;
        bool consumeApproval = false;
    };
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
    Json compactions(std::string_view bot, Id before, unsigned limit, Id beforeMessage = 0) const;
    Json conversationHistory(const conversation::Message&, const Json& summaries, std::size_t expandedBytes) const;
    Json contextTool(std::string_view tool, const Json&, const conversation::Message&, int step);
    Id startWorkflow(Id message, const Json& args, std::string_view bot, const Json& routing);
    Json workflowTool(std::string_view tool, const Json&, std::string_view bot, Id message, int step, const Json& routing);
    Json workflowStatus(Id id, std::string_view bot, Seconds now) const;
    void advanceWorkflows(Seconds now);
    void materializeWorkflowStages();
    void appendStageRequirements(std::string& instruction, const TaskData& task) const;
    void stageObservation(const TaskData& task, const Observation&, Seconds now);
    bool settleStageReport(TaskData& task);
    void authorizeBotToolHistory(std::string_view bot, Id message, ModelInput&);
    Json workspaceTool(const ToolContext&, const Decision&);
    std::optional<Resource> authorizedBotToolScope(std::string_view bot, const Decision&);
    void validateBotTool(const ToolContext&, const Decision&);
    Json executeBotTool(const ToolContext&, const Decision&);
    void saveTurnResults(Id, int step, const Json& results);
    class ToolBatch;
    std::map<std::pair<Id, int>, std::map<std::size_t, ToolCallResult>> unpersistedToolResults;
    ModelBudget messageModelBudget(Id messageId) const;
    ModelBudget taskModelBudget(Id taskId) const;
    Id reserveModelCall(const ModelBudget&, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    Id takeReservedConversationCall(Id messageId);
    Id takeReservedTaskCall(Id event);
    void releaseModelCall(Id attempt);
    Id approveModelBudget(const Person&, const ModelBudget&, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    void resumeSharedModelBudgetWaits(const Person&, std::string_view budget, Id approvedMessage, Id approvedTask);
    void resumeMessageModelCall(std::string_view bot, Id message, Id pause, Id attempt);
    SecretRedactor modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args, bool gitNotification) const;
    ModelCall checkModelCall(std::string_view bot, Decision, const std::vector<std::string>& offered, bool gitNotification);
    CheckedModelTurn callModel(std::string_view bot, Model&, const ModelInput&);
    std::vector<std::pair<std::string, Id>> pendingConversations(unsigned limit, Seconds now) const;
    void deferConversation(Id, Seconds now);
    std::map<Id, ConversationModelCheckpoint> conversationCheckpoints;
    struct SessionSendReceipt {
        std::string bot;
        SessionTarget target;
        std::string title;
        Delivery delivery;
        int step = -1;
        Id taskId = 0;
        std::string actionId, requestId;
        bool notify = true;
    };
    std::map<Id, SessionSendReceipt> sessionSendReceipts;
    bool recordSessionSendRun(Id, const SessionSendReceipt&, std::optional<Seconds> interruptAt = {});
    void saveSessionSendResult(std::string_view bot, Id message, int step, Json result);
    void persistSessionSendReceipt(Id message, const SessionSendReceipt&, std::optional<Seconds> interruptAt = {});
    void reconcilePausedSessionSends(T3&, std::stop_token);
    bool advanceDestinationSessionMessageRun(TaskData&, DestinationRun&);
    void runTurn(Id messageId, Model&, T3&, const std::function<std::string(std::string_view)>&, const std::function<Seconds()>&,
                 const BotTools&, GitHost*, const ApplyBotChange&, const ReconcileBotCreation&, const IntakeResolver&, const PeerResolver&,
                 std::stop_token);
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
    CallOutcome executeConversationStep(const conversation::Message&, int step, const std::vector<ModelCall>&, std::string_view effectType,
                                        const BotTools&, const ConversationCallContext&);
    CallOutcome executeConversationBotChange(const conversation::Message&, int step, const Decision&, std::string_view key,
                                             const ConversationCallContext&);
    CallOutcome executeConversationReply(const conversation::Message&, int step, const Decision&, std::string_view key,
                                         const std::function<Seconds()>&);
    CallOutcome executeLocalConversationCall(const conversation::Message&, int step, const Decision&, std::string_view key,
                                             const ConversationCallContext&);
    std::optional<Json> applyLocalConversationCall(const conversation::Message&, int step, const Decision&, std::string_view key,
                                                   Seconds now, ApprovalWait, const WorkSession* selectedSession,
                                                   const LinkedTaskDestinations& linked);
    std::optional<Resource> conversationCallResource(const conversation::Message&, const Decision&);
    void saveConversationCallResult(const conversation::Message&, int step, Json result);
    CallOutcome sendSessionMessage(const conversation::Message&, int step, const Decision&, std::string_view key,
                                   const ConversationCallContext&);
    CallOutcome cancelDestinationRun(const conversation::Message&, int step, const Decision&, std::string_view key, bool offered,
                                     bool hasWebEvidence, std::stop_token);
    bool inviteSessionBot(const conversation::Message&, int, const Decision&, T3&, const IntakeResolver&, const PeerResolver&,
                          std::stop_token);
    bool leaveSession(const conversation::Message&, int, const Decision&, T3&, std::stop_token);
    void wakeSessionMessages(T3&, std::stop_token);
    CallOutcome runSessionEffect(const conversation::Message&, int step, const Decision&, std::string_view key, bool offered,
                                 bool hasWebEvidence, T3&, std::stop_token);
    bool isNextTurn(Id messageId) const;
    bool authorizeTurn(const conversation::Message&);
    Person turnActor(const conversation::Message&);
    conversation::Message loadTurnMessage(Id messageId) const;
    const char* questionAnswerError(const TaskData& task, const conversation::Message&, const std::optional<Wait>&) const;
    Json taskState(const TaskData& task, const conversation::Message&, bool fullContract = false) const;
    Json conversationSteps(const conversation::Message&, int step, std::vector<ModelImage>& images);
    Json turnInput(const conversation::Message&, int step, Seconds now, Json machines, std::vector<ModelImage>& images,
                   const LinkedTaskDestinations& linked);
    Json gitConversationTool(GitHost*, const conversation::Message&, const Decision&, std::string_view key);
    bool gitEffectStarted(std::string_view invocation, Id taskId = 0) const;
    Json applyTurnTool(const conversation::Message&, int step, const Decision&, Seconds now, const WorkSession* selectedSession,
                       const LinkedTaskDestinations& linked);
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, ConversationReplyRoute route,
                    const conversation::QuestionAnchor* question = nullptr);
    void pauseTurn(const conversation::Message&);
    TaskData ownTask(Id taskId, const conversation::Message&) const;
    Json resultReference(Id messageId, const conversation::Message&) const;
    Id startConversationTask(const Json& routing, std::string_view bot, std::string_view request, const TaskRoute&, Id messageId,
                             const std::optional<OutboxSource>& source, bool resume, const WorkSession* selectedSession = nullptr);
    bool acceptInstruction(TaskData& task, Id messageId, int step, std::string_view instruction, std::optional<OutboxSource> source,
                           const TaskChange* change);
    bool conversationPending(std::string_view bot) const;
    bool automationBeforeDispatch(Id task) const;
    bool isUndispatchedNotesWait(const TaskData& task) const;
    bool projectRouteReady(Id task, std::string_view action) const;
    std::string rememberedT3Project(std::string_view bot, std::string_view environment) const;
    void rememberT3Project(std::string_view bot, std::string_view environment, std::string_view project);
    bool projectQuestionOpen(std::string_view bot, std::string_view environment) const;
    ProjectCatalog readProjectCatalog(T3&, std::string_view bot, std::string_view environment, bool requireConnection);
    bool applyProjectCatalog(TaskData& task, ProjectCatalog catalog);
    std::set<std::string> registeredT3Environments(std::string_view bot);
    std::string taskEnvironment(std::string_view bot, const Json& routing, std::string_view name = {});
    TaskRoute taskRoute(std::string_view bot, const Json& routing, std::string_view name, const TaskData* movingTask = nullptr,
                        const LinkedTaskDestinations* linked = nullptr);
    StartDestinations startDestinations(const conversation::Message&, const Json& args, bool resume, const WorkSession* selectedSession,
                                        const LinkedTaskDestinations& linked);
    LinkedTaskDestinations discoverLinkedTaskDestinations(T3&, std::string_view bot);
    TaskChange taskChange(const conversation::Message&, const Json& args, const TaskData& task, const LinkedTaskDestinations& linked);
    std::string defaultT3Environment(std::string_view bot, std::string_view configured, std::string_view preferred);
    Id submitRequest(const Intake&, const Person* requester);
    Id submit(const Intake&, Id sourceMessageId = 0, const OutboxSource* source = nullptr, TaskSource sourceKind = TaskSource::request,
              std::optional<TaskTrigger> trigger = {}, std::optional<std::string> requester = {});
    void operate(const Person&, const TaskTarget&, std::string_view, const Json&, Id sourceMessage = 0, std::optional<InboxKind> = {});
    void requestCancellation(TaskData&, SessionStop stop = {});
    bool recordDestinationReport(TaskData& task, DestinationRun& run, const Observation&);
};

std::optional<InboxKind> inboxWaitKind(WaitReason reason);

}
