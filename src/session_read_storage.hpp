#pragma once

#include "tool_call_result.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace flockv0 {
class Policy;
class ConversationMessages;

class Database;
class SecretStorage;
class SessionParticipationStorage;
class T3;
struct Decision;
struct SessionTarget;
struct WorkSession;

namespace conversation {
struct Message;
}

class ChannelStorage;
class PolicyApprovals;
class SessionReadStorage {
public:
    SessionReadStorage(Database& database, SecretStorage& secretStorage, const SessionParticipationStorage& sessionParticipationStorage,
                       Policy& permissions, ConversationMessages& conversationMessages, ChannelStorage& channelStorage,
                       PolicyApprovals& policyApprovals)
        : database(database), secretStorage(secretStorage), sessionParticipationStorage(sessionParticipationStorage),
          permissions(permissions), conversationMessages(conversationMessages), channelStorage(channelStorage),
          policyApprovals(policyApprovals) {}

    ToolCallResult sessionReadTool(const conversation::Message&, const Decision&, T3&, std::string_view key);
    void authorizeSessionHistory(const conversation::Message&, Json& steps, T3&);
    bool connectedSessionAccess(const conversation::Message&, const SessionTarget&);
    std::optional<WorkSession> resolveWorkSession(const conversation::Message&, std::string_view sessionRef, T3&);
    bool permitsWorkSession(const conversation::Message&, const WorkSession&);
    bool permitsWorkSession(Id sourceMessageId, const WorkSession&);
private:
    class SessionEvidence;
    std::set<std::string> connectedSessionEnvironments(const conversation::Message&);

    Database& database;
    SecretStorage& secretStorage;
    const SessionParticipationStorage& sessionParticipationStorage;
    Policy& permissions;
    ConversationMessages& conversationMessages;
    ChannelStorage& channelStorage;
    PolicyApprovals& policyApprovals;
};
}
