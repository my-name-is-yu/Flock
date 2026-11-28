#pragma once

#include "session_stop.hpp"
#include "flock/intake.hpp"
#include "flock/types.hpp"

#include <filesystem>
#include <mutex>
#include <stop_token>
#include <string_view>

namespace flockv0 {
class Policy;
class ConversationStorage;
class SessionParticipationRecords;
class SessionSendReceipts;
class ConversationMessages;
class TurnResults;
class SessionMessageStorage;

class Database;
class SecretStorage;
class SessionReadStorage;
class T3;
class TaskRecords;
struct Decision;
struct Person;
struct SessionStopResult;
struct SessionTarget;
struct WebIdentityState;

namespace conversation {
struct Message;
}

class BotLifecycleStorage;
class TaskOperations;
class PolicyApprovals;
class SessionParticipationStorage {
public:
    SessionParticipationStorage(Database& database, const std::filesystem::path& path, SecretStorage& secretStorage,
                                TaskRecords& taskRecords, const WebIdentityState& webIdentity, Policy& permissions,
                                ConversationStorage& conversationTurns, ConversationMessages& conversationMessages,
                                TurnResults& turnResults, SessionMessageStorage& sessionMessageStorage, BotLifecycleStorage& botLifecycle,
                                TaskOperations& taskOperations, PolicyApprovals& policyApprovals,
                                SessionParticipationRecords& sessionParticipations, SessionSendReceipts& sessionSendReceipts)
        : database(database), path(path), secretStorage(secretStorage), taskRecords(taskRecords), webIdentity(webIdentity),
          permissions(permissions), conversationTurns(conversationTurns), conversationMessages(conversationMessages),
          turnResults(turnResults), sessionMessageStorage(sessionMessageStorage), botLifecycle(botLifecycle),
          taskOperations(taskOperations), policyApprovals(policyApprovals), sessionParticipations(sessionParticipations),
          sessionSendReceipts(sessionSendReceipts) {}

    SessionStop stopSessionParticipation(SessionReadStorage&, const Person&, std::string_view bot, const SessionTarget&, SessionExit,
                                         Id message = 0);
    void stopPausedBotParticipation(std::string_view bot);
    SessionStopResult settleSessionQueueCancellations(T3&, std::stop_token, SessionStop stop = {});
    bool inviteSessionBot(const conversation::Message&, int, const Decision&, T3&, const IntakeResolver&, const PeerResolver&,
                          std::stop_token);
    bool leaveSession(SessionReadStorage&, const conversation::Message&, int, const Decision&, T3&, std::stop_token);
private:
    void interruptPausedSessionRuns(T3&, std::stop_token);

    Database& database;
    const std::filesystem::path& path;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    const WebIdentityState& webIdentity;
    Policy& permissions;
    ConversationStorage& conversationTurns;
    ConversationMessages& conversationMessages;
    TurnResults& turnResults;
    SessionMessageStorage& sessionMessageStorage;
    BotLifecycleStorage& botLifecycle;
    TaskOperations& taskOperations;
    PolicyApprovals& policyApprovals;
    SessionParticipationRecords& sessionParticipations;
    SessionSendReceipts& sessionSendReceipts;
    std::mutex sessionQueueCancellationMutex;
};
}
