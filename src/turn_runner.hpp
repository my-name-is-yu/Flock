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
class ConversationMessages;
class ConversationTools;
class TurnResults;
class GitStorage;
class LocalMachineStorage;
class MemoryStorage;
class ModelCalls;
class PolicyApprovals;
class SecretStorage;
class SessionMessageStorage;
class SessionParticipationStorage;
class SessionReadStorage;
class TaskOperations;
class TaskRecords;
class TaskRouting;
class WaitStorage;
class GitHost;
class Model;
class T3;
enum class ApprovalWait;
enum class CallOutcome;
struct BotTools;
struct ConversationCallContext;
struct Decision;
struct TaskDestinations;
struct ModelCall;
struct ModelImage;
struct Resource;
struct TaskData;
struct WorkSession;
namespace conversation {
struct Message;
}

class ConversationTurns;

// Runs conversation turns: asks the model, carries out its tool calls, and continues until the turn ends.
class TurnRunner {
public:
    TurnRunner(Database& database, AttachmentStorage& attachments, AutomationStorage& automationStorage, BotLifecycleStorage& botLifecycle,
               BotAccess& botAccess, BotSettingsStorage& botSettingsStorage, BotSetupStorage& botSetup, ChannelStorage& channelStorage,
               ConversationContextStorage& conversationContextStorage, ConversationTools& conversationTools, GitStorage& gitStorage,
               Policy& permissions, LocalMachineStorage& localMachineStorage, MemoryStorage& memoryStorage, ModelCalls& modelCalls,
               PolicyApprovals& policyApprovals, SecretStorage& secretStorage, SessionMessageStorage& sessionMessageStorage,
               SessionParticipationStorage& sessionParticipationStorage, SessionReadStorage& sessionReadStorage,
               TaskOperations& taskOperations, TaskRecords& taskRecords, TaskRouting& taskRouting, WaitStorage& waitStorage,
               ConversationMessages& conversationMessages, TurnResults& turnResults, ConversationTurns& conversationTurns)
        : database(database), attachments(attachments), automationStorage(automationStorage), botLifecycle(botLifecycle),
          botAccess(botAccess), botSettingsStorage(botSettingsStorage), botSetup(botSetup), channelStorage(channelStorage),
          conversationContextStorage(conversationContextStorage), conversationTools(conversationTools), gitStorage(gitStorage),
          permissions(permissions), localMachineStorage(localMachineStorage), memoryStorage(memoryStorage), modelCalls(modelCalls),
          policyApprovals(policyApprovals), secretStorage(secretStorage), sessionMessageStorage(sessionMessageStorage),
          sessionParticipationStorage(sessionParticipationStorage), sessionReadStorage(sessionReadStorage), taskOperations(taskOperations),
          taskRecords(taskRecords), taskRouting(taskRouting), waitStorage(waitStorage), conversationMessages(conversationMessages),
          turnResults(turnResults), conversationTurns(conversationTurns) {}

    void runTurn(Id messageId, Model&, T3&, const SoulLoader&, const std::function<Seconds()>&, const BotTools&, GitHost*,
                 const ApplyBotChange&, const ReconcileBotCreation&, const IntakeResolver&, const PeerResolver&, std::stop_token);
    std::optional<Json> applyLocalConversationCall(const conversation::Message&, int step, const Decision&, std::string_view key,
                                                   Seconds now, ApprovalWait, const WorkSession* selectedSession,
                                                   const TaskDestinations& linked, const BotTools&);
    Json applyWokenTaskDecision(const conversation::Message&, const Decision&, Seconds now);
    struct WorkLearningSource {
        Id report;
        std::string runId;
    };
    // The finished Run whose report the Bot may learn from when it completes the woken task.
    std::optional<WorkLearningSource> wokenLearningSource(const TaskData&);
private:
    CallOutcome executeConversationStep(const conversation::Message&, int step, const std::vector<ModelCall>&, std::string_view effectType,
                                        const BotTools&, const ConversationCallContext&);
    CallOutcome executeBotSetup(const conversation::Message&, int step, const Decision&, std::string_view key,
                                const ConversationCallContext&, const Decision* reply = nullptr);
    CallOutcome executeConversationBotChange(const conversation::Message&, int step, const Decision&, std::string_view key,
                                             const ConversationCallContext&);
    CallOutcome executeMessageBot(const conversation::Message&, int step, const Decision&, const ConversationCallContext&);
    CallOutcome executeConversationReply(const conversation::Message&, int step, const Decision&, std::string_view key,
                                         const std::function<Seconds()>&, const Json* setup = nullptr);
    CallOutcome executeLocalConversationCall(const conversation::Message&, int step, const Decision&, std::string_view key,
                                             const ConversationCallContext&, const BotTools&);
    std::optional<Resource> conversationCallResource(const conversation::Message&, const Decision&);
    void saveConversationCallResult(const conversation::Message&, int step, Json result);
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
    LocalMachineStorage& localMachineStorage;
    MemoryStorage& memoryStorage;
    ModelCalls& modelCalls;
    PolicyApprovals& policyApprovals;
    SecretStorage& secretStorage;
    SessionMessageStorage& sessionMessageStorage;
    SessionParticipationStorage& sessionParticipationStorage;
    SessionReadStorage& sessionReadStorage;
    TaskOperations& taskOperations;
    TaskRecords& taskRecords;
    TaskRouting& taskRouting;
    WaitStorage& waitStorage;
    ConversationMessages& conversationMessages;
    TurnResults& turnResults;
    ConversationTurns& conversationTurns;
};
}
