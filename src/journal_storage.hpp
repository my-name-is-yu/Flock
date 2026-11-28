#pragma once

#include <optional>
#include <string_view>
#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class ChannelDeliveryQueue;
class Database;
struct CompletionEntry;
struct ConversationInterruptEntry;
struct ConversationMessageEntry;
struct ConversationReplyEntry;
struct DecisionEntry;
struct DeliveredInstructionEntry;
struct GitPrFollowupEntry;
struct OutboxInstruction;
struct RunReportEntry;
struct RunStop;
struct SendStartedEntry;
struct SessionStop;
struct SettledRunReport;
struct TaskRevisionEntry;
struct ToolErrorEntry;
struct UserAnswerEntry;
struct Wait;
enum class JournalKind;

class JournalStorage {
public:
    JournalStorage(Database& database, ChannelDeliveryQueue& channelDeliveries)
        : database(database), channelDeliveries(channelDeliveries) {}

    Id journal(Id, const Wait&);
    Id journal(Id, const CompletionEntry&);
    Id journal(Id, const ToolErrorEntry&);
    Id journal(Id, const DecisionEntry&);
    Id journalCancelRequested(Id, SessionStop);
    Id journalCancelled(Id);
    Id journal(Id, const UserAnswerEntry&);
    Id journal(Id, const OutboxInstruction&);
    Id journal(Id, const DeliveredInstructionEntry&);
    Id journal(Id, const TaskRevisionEntry&);
    Id journal(Id, const SendStartedEntry&);
    Id journal(Id, const RunReportEntry&);
    Id journal(Id, const GitPrFollowupEntry&);
    Id journalRunSettled(Id, const SettledRunReport&);
    Id journalConversationMessage(std::string_view bot, const ConversationMessageEntry&);
    Id journalConversationHandoff(std::string_view bot, Id sourceMessage, std::string_view peerBot, Id peerMessage);
    Id journalConversationReply(std::optional<Id> task, std::string_view bot, const ConversationReplyEntry&);
    Id journalInterrupt(Id, const RunStop&);
    Id journalInterrupt(Id, const ConversationInterruptEntry&);
private:
    Id writeRunReportJournal(Id, JournalKind, const RunReportEntry&);
private:
    Database& database;
    ChannelDeliveryQueue& channelDeliveries;
};
}
