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

class Database;
class AttachmentStorage;
class BotLifecycleStorage;
class ChannelStorage;
class ChannelConnectors;
class ChannelDeliveryQueue;
class ConversationMessages;
class SessionSendReceipts;
class TurnResults;
class GitStorage;
class JournalStorage;
class ModelCalls;
class SecretStorage;
class SessionParticipationRecords;
class TaskRecords;
class WaitStorage;
enum class ConversationReplyRoute;
struct ConversationBotMessageEntry;
struct ConversationWakeEntry;
struct ConversationInvitationEntry;
struct Person;
struct SessionOrigin;
struct TaskData;
struct VerifiedChannelMessage;
struct Wait;
namespace conversation {
struct Message;
struct QuestionAnchor;
enum class PrincipalSource;
}

class ConversationTurns {
public:
    ConversationTurns(Database& database, AttachmentStorage& attachments, BotLifecycleStorage& botLifecycle, ChannelStorage& channelStorage,
                      GitStorage& gitStorage, Policy& permissions, JournalStorage& journalStorage, ModelCalls& modelCalls,
                      SecretStorage& secretStorage, TaskRecords& taskRecords, WaitStorage& waitStorage,
                      ConversationMessages& conversationMessages, ChannelDeliveryQueue& channelDeliveries,
                      SessionParticipationRecords& sessionParticipations, ChannelConnectors& channelConnectors, TurnResults& turnResults,
                      SessionSendReceipts& sessionSendReceipts)
        : database(database), attachments(attachments), botLifecycle(botLifecycle), channelStorage(channelStorage), gitStorage(gitStorage),
          permissions(permissions), journalStorage(journalStorage), modelCalls(modelCalls), secretStorage(secretStorage),
          taskRecords(taskRecords), waitStorage(waitStorage), conversationMessages(conversationMessages),
          channelDeliveries(channelDeliveries), sessionParticipations(sessionParticipations), channelConnectors(channelConnectors),
          turnResults(turnResults), sessionSendReceipts(sessionSendReceipts) {}

    Json receive(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text, Id replyToMessageId,
                 conversation::PrincipalSource, const VerifiedChannelMessage*, const std::function<Intake()>&, const Json& attachments);
    Id enqueueConversation(const Person&, const Intake&, conversation::PrincipalSource, std::string_view clientId, std::string_view text,
                           Id replyTo = 0, const SessionOrigin* origin = nullptr, const ConversationInvitationEntry* invitation = nullptr,
                           const ConversationBotMessageEntry* botMessage = nullptr);
    // Starts a turn for the Bot that sent `source`, in the same conversation and with the same authority.
    Id enqueueWake(const conversation::Message& source, Json routing, const ConversationWakeEntry&, std::string_view clientId);
    // Wakes the conversation that started a task, with the authority of the message that started it.
    Id enqueueTaskWake(Id sourceMessage, ConversationWakeEntry, std::string_view clientId);
    std::optional<Json> messageReceipt(const Person&, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                                       const Json& attachments) const;
    std::vector<std::pair<std::string, Id>> pendingConversations(unsigned limit, Seconds now) const;
    bool isNextTurn(Id messageId) const;
    bool authorizeTurn(const conversation::Message&);
    Json taskState(const TaskData& task, const conversation::Message&, bool fullContract = false) const;
    // The task this wake reports on, while it waits for the conversation to decide what happens next.
    std::optional<TaskData> wokenTask(const conversation::Message&) const;
    void finishTurn(const conversation::Message&, std::string text, const Json& suggestions, ConversationReplyRoute route,
                    const conversation::QuestionAnchor* question = nullptr);
    TaskData ownTask(Id taskId, const conversation::Message&) const;
    Json resultReference(Id messageId, const conversation::Message&) const;
    bool conversationPending(std::string_view bot) const;
    void deferConversation(Id, Seconds now);
    const char* questionAnswerError(const TaskData& task, const conversation::Message&, const std::optional<Wait>&) const;
private:
    Database& database;
    AttachmentStorage& attachments;
    BotLifecycleStorage& botLifecycle;
    ChannelStorage& channelStorage;
    GitStorage& gitStorage;
    Policy& permissions;
    JournalStorage& journalStorage;
    ModelCalls& modelCalls;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    ConversationMessages& conversationMessages;
    ChannelDeliveryQueue& channelDeliveries;
    SessionParticipationRecords& sessionParticipations;
    ChannelConnectors& channelConnectors;
    TurnResults& turnResults;
    SessionSendReceipts& sessionSendReceipts;
};
}
