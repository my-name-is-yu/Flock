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
class ConversationMessages;
class ConversationTools;
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
                                ConversationTools& conversationTools, SessionMessageStorage& sessionMessageStorage,
                                BotLifecycleStorage& botLifecycle, TaskOperations& taskOperations, PolicyApprovals& policyApprovals)
        : database(database), path(path), secretStorage(secretStorage), taskRecords(taskRecords), webIdentity(webIdentity),
          permissions(permissions), conversationTurns(conversationTurns), conversationMessages(conversationMessages),
          conversationTools(conversationTools), sessionMessageStorage(sessionMessageStorage), botLifecycle(botLifecycle),
          taskOperations(taskOperations), policyApprovals(policyApprovals) {}

    bool activeSessionOrigin(const conversation::Message&) const;
    bool activeSessionOrigin(std::string_view bot, std::string_view person, const SessionTarget& origin,
                             std::string_view destinationEnvironment, Id message) const;
    bool sessionParticipationStopped(std::string_view bot, const SessionTarget&, Id message) const;
    SessionStop stopSessionParticipation(SessionReadStorage&, const Person&, std::string_view bot, const SessionTarget&, SessionExit,
                                         Id message = 0);
    void stopPausedBotParticipation(std::string_view bot);
    SessionStopResult settleSessionQueueCancellations(T3&, std::stop_token, SessionStop stop = {});
    bool t3EnvironmentConnected(std::string_view environment);
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
    ConversationTools& conversationTools;
    SessionMessageStorage& sessionMessageStorage;
    BotLifecycleStorage& botLifecycle;
    TaskOperations& taskOperations;
    PolicyApprovals& policyApprovals;
    std::mutex sessionQueueCancellationMutex;
};
}
