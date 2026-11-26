#pragma once

#include <functional>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "conversation_checkpoint.hpp"
#include "flock/bot_change.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/soul_loader.hpp"
#include "flock/types.hpp"

namespace flockv0 {
class Policy;
class BotAccess;

class Database;
class AttachmentStorage;
class AutomationStorage;
class BotLifecycleStorage;
class BotSettingsStorage;
class BotSetupStorage;
class ChannelStorage;
class ConversationContextStorage;
class ConversationTools;
class GitStorage;
class JournalStorage;
class LocalMachineStorage;
class MemoryStorage;
class ModelCalls;
class PeerStorage;
class PolicyApprovals;
class SecretStorage;
class SessionMessageStorage;
class SessionParticipationStorage;
class SessionReadStorage;
class TaskOperations;
class TaskRecords;
class TaskRouting;
class WaitStorage;
struct WebIdentityState;
class WorkflowStorage;
class GitHost;
class Model;
class T3;
enum class ApprovalWait;
enum class CallOutcome;
enum class ConversationReplyRoute;
enum class SessionExit;
struct BotTools;
struct ConversationCallContext;
struct ConversationInvitationEntry;
struct Decision;
struct TaskDestinations;
struct ModelCall;
struct ModelImage;
struct Person;
struct Resource;
struct SessionOrigin;
struct TaskData;
struct VerifiedChannelMessage;
struct Wait;
struct WorkSession;
namespace conversation {
struct Message;
struct QuestionAnchor;
enum class PrincipalSource;
}

class ConversationStorage {
public:
    ConversationStorage(Database& database, AttachmentStorage& attachments, AutomationStorage& automationStorage,
                        BotLifecycleStorage& botLifecycle, BotAccess& botAccess, BotSettingsStorage& botSettingsStorage,
                        BotSetupStorage& botSetup, ChannelStorage& channelStorage, ConversationContextStorage& conversationContextStorage,
                        ConversationTools& conversationTools, GitStorage& gitStorage, Policy& permissions, JournalStorage& journalStorage,
                        LocalMachineStorage& localMachineStorage, MemoryStorage& memoryStorage, ModelCalls& modelCalls,
                        PeerStorage& peerStorage, PolicyApprovals& policyApprovals, SecretStorage& secretStorage,
                        SessionMessageStorage& sessionMessageStorage, SessionParticipationStorage& sessionParticipationStorage,
                        SessionReadStorage& sessionReadStorage, TaskOperations& taskOperations, TaskRecords& taskRecords,
                        TaskRouting& taskRouting, WaitStorage& waitStorage, WebIdentityState& webIdentity, WorkflowStorage& workflowStorage)
        : database(database), attachments(attachments), automationStorage(automationStorage), botLifecycle(botLifecycle),
          botAccess(botAccess), botSettingsStorage(botSettingsStorage), botSetup(botSetup), channelStorage(channelStorage),
          conversationContextStorage(conversationContextStorage), conversationTools(conversationTools), gitStorage(gitStorage),
          permissions(permissions), journalStorage(journalStorage), localMachineStorage(localMachineStorage), memoryStorage(memoryStorage),
          modelCalls(modelCalls), peerStorage(peerStorage), policyApprovals(policyApprovals), secretStorage(secretStorage),
          sessionMessageStorage(sessionMessageStorage), sessionParticipationStorage(sessionParticipationStorage),
          sessionReadStorage(sessionReadStorage), taskOperations(taskOperations), taskRecords(taskRecords), taskRouting(taskRouting),
          waitStorage(waitStorage), webIdentity(webIdentity), workflowStorage(workflowStorage) {}

    Json receive(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text, Id replyToMessageId,
                 conversation::PrincipalSource, const VerifiedChannelMessage*, const std::function<Intake()>&, const Json& attachments);
    Id enqueueConversation(const Person&, const Intake&, conversation::PrincipalSource, std::string_view clientId, std::string_view text,
                           Id replyTo = 0, const SessionOrigin* origin = nullptr, const ConversationInvitationEntry* invitation = nullptr);
    std::optional<Json> messageReceipt(const Person&, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                                       const Json& attachments) const;
    std::vector<std::pair<std::string, Id>> pendingConversations(unsigned limit, Seconds now) const;
    void runTurn(Id messageId, Model&, T3&, const SoulLoader&, const std::function<Seconds()>&, const BotTools&, GitHost*,
                 const ApplyBotChange&, const ReconcileBotCreation&, const IntakeResolver&, const PeerResolver&, std::stop_token);
    std::optional<Json> applyLocalConversationCall(const conversation::Message&, int step, const Decision&, std::string_view key,
                                                   Seconds now, ApprovalWait, const WorkSession* selectedSession,
                                                   const TaskDestinations& linked, const BotTools&);
    bool isNextTurn(Id messageId) const;
    bool authorizeTurn(const conversation::Message&);
    Person turnActor(const conversation::Message&);
    conversation::Message loadTurnMessage(Id messageId) const;
    Json taskState(const TaskData& task, const conversation::Message&, bool fullContract = false) const;
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, ConversationReplyRoute route,
                    const conversation::QuestionAnchor* question = nullptr);
    TaskData ownTask(Id taskId, const conversation::Message&) const;
    Json resultReference(Id messageId, const conversation::Message&) const;
    bool conversationPending(std::string_view bot) const;
private:
    void deferConversation(Id, Seconds now);
    CallOutcome executeConversationStep(const conversation::Message&, int step, const std::vector<ModelCall>&, std::string_view effectType,
                                        const BotTools&, const ConversationCallContext&);
    CallOutcome executeBotSetup(const conversation::Message&, int step, const Decision&, std::string_view key,
                                const ConversationCallContext&, const Decision* reply = nullptr);
    CallOutcome executeConversationBotChange(const conversation::Message&, int step, const Decision&, std::string_view key,
                                             const ConversationCallContext&);
    CallOutcome executeConversationReply(const conversation::Message&, int step, const Decision&, std::string_view key,
                                         const std::function<Seconds()>&, const Json* setup = nullptr);
    CallOutcome executeLocalConversationCall(const conversation::Message&, int step, const Decision&, std::string_view key,
                                             const ConversationCallContext&, const BotTools&);
    std::optional<Resource> conversationCallResource(const conversation::Message&, const Decision&);
    void saveConversationCallResult(const conversation::Message&, int step, Json result);
    const char* questionAnswerError(const TaskData& task, const conversation::Message&, const std::optional<Wait>&) const;
    Json conversationSteps(const conversation::Message&, int step, std::vector<ModelImage>& images);
    Json turnInput(const conversation::Message&, int step, Seconds now, Json machines, std::vector<ModelImage>& images,
                   const TaskDestinations& linked);
    Json applyTurnTool(const conversation::Message&, int step, const Decision&, Seconds now, const WorkSession* selectedSession,
                       const TaskDestinations& linked, const BotTools&);
    std::map<Id, ConversationModelCheckpoint> conversationCheckpoints;
    Database& database;
    AttachmentStorage& attachments;
    AutomationStorage& automationStorage;
    BotLifecycleStorage& botLifecycle;
    BotAccess& botAccess;
    BotSettingsStorage& botSettingsStorage;
    BotSetupStorage& botSetup;
    ChannelStorage& channelStorage;
    ConversationContextStorage& conversationContextStorage;
    ConversationTools& conversationTools;
    GitStorage& gitStorage;
    Policy& permissions;
    JournalStorage& journalStorage;
    LocalMachineStorage& localMachineStorage;
    MemoryStorage& memoryStorage;
    ModelCalls& modelCalls;
    PeerStorage& peerStorage;
    PolicyApprovals& policyApprovals;
    SecretStorage& secretStorage;
    SessionMessageStorage& sessionMessageStorage;
    SessionParticipationStorage& sessionParticipationStorage;
    SessionReadStorage& sessionReadStorage;
    TaskOperations& taskOperations;
    TaskRecords& taskRecords;
    TaskRouting& taskRouting;
    WaitStorage& waitStorage;
    WebIdentityState& webIdentity;
    WorkflowStorage& workflowStorage;
};
}
