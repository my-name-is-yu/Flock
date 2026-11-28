#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string_view>
#include "flock/json.hpp"
#include "flock/types.hpp"
#include "session_send_receipt.hpp"

namespace flockv0 {
class Policy;

class Database;
class BotLifecycleStorage;
class ConversationStorage;
class ConversationMessages;
class ConversationTools;
class GitStorage;
class JournalStorage;
class OutboxStorage;
class PolicyApprovals;
class SecretStorage;
class SessionParticipationRecords;
class SessionReadStorage;
class TaskRecords;
class TaskRunReports;
class T3;
enum class CallOutcome;
struct ConversationCallContext;
struct Decision;
namespace conversation {
struct Message;
}

class SessionMessageStorage {
public:
    SessionMessageStorage(Database& database, BotLifecycleStorage& botLifecycle, ConversationStorage& conversationTurns,
                          ConversationMessages& conversationMessages, ConversationTools& conversationTools, GitStorage& gitStorage,
                          const Policy& permissions, const std::filesystem::path& path, JournalStorage& journalStorage,
                          OutboxStorage& outboxStorage, PolicyApprovals& policyApprovals, SecretStorage& secretStorage,
                          SessionParticipationRecords& sessionParticipations, SessionReadStorage& sessionReadStorage,
                          TaskRecords& taskRecords, TaskRunReports& taskRunReports)
        : database(database), botLifecycle(botLifecycle), conversationTurns(conversationTurns), conversationMessages(conversationMessages),
          conversationTools(conversationTools), gitStorage(gitStorage), permissions(permissions), path(path),
          journalStorage(journalStorage), outboxStorage(outboxStorage), policyApprovals(policyApprovals), secretStorage(secretStorage),
          sessionParticipations(sessionParticipations), sessionReadStorage(sessionReadStorage), taskRecords(taskRecords),
          taskRunReports(taskRunReports) {}

    bool persistPendingReceipt(Id message);
    void forgetPendingReceipt(Id message);
    void recordPendingRuns(const SessionTarget&);
    void reconcilePausedSessionSends(T3&, std::stop_token);
    CallOutcome sendSessionMessage(const conversation::Message&, int step, const Decision&, std::string_view key,
                                   const ConversationCallContext&);
    CallOutcome cancelDestinationRun(const conversation::Message&, int step, const Decision&, std::string_view key, bool offered,
                                     std::stop_token);
    void wakeSessionMessages(T3&, std::stop_token);
    CallOutcome runSessionEffect(const conversation::Message&, int step, const Decision&, std::string_view key, bool offered, T3&,
                                 std::stop_token);
private:
    struct SessionSend;
    std::map<Id, SessionSendReceipt> sessionSendReceipts;
    void saveSessionSendStep(const SessionSend&, Json result);
    void addressSessionSend(SessionSend&);
    std::optional<CallOutcome> prepareSessionSend(SessionSend&);
    void probeSessionSendTarget(SessionSend&);
    std::optional<CallOutcome> approveSessionSend(SessionSend&);
    std::optional<CallOutcome> startSessionSend(const SessionSend&);
    void deliverSessionSend(SessionSend&);
    void persistSessionSendReceipt(Id message, const SessionSendReceipt&, std::optional<Seconds> interruptAt = {});
    void saveSessionSendResult(std::string_view bot, Id message, int step, Json result);
    bool recordSessionSendRun(Id, const SessionSendReceipt&, std::optional<Seconds> interruptAt = {});
    Database& database;
    BotLifecycleStorage& botLifecycle;
    ConversationStorage& conversationTurns;
    ConversationMessages& conversationMessages;
    ConversationTools& conversationTools;
    GitStorage& gitStorage;
    const Policy& permissions;
    const std::filesystem::path& path;
    JournalStorage& journalStorage;
    OutboxStorage& outboxStorage;
    PolicyApprovals& policyApprovals;
    SecretStorage& secretStorage;
    SessionParticipationRecords& sessionParticipations;
    SessionReadStorage& sessionReadStorage;
    TaskRecords& taskRecords;
    TaskRunReports& taskRunReports;
};
}
