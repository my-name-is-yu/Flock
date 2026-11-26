#pragma once

#include "policy_origin.hpp"
#include "policy_approval_types.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <functional>
#include <optional>
#include <string_view>

namespace flockv0 {
class Policy;
class ConversationStorage;

class Database;
class SecretStorage;
class TaskRecords;
class WaitStorage;
struct InboxTarget;
struct Person;
struct Resource;
struct TaskData;
struct TaskRoute;
class SuggestionStorage;
struct WebIdentityState;

namespace conversation {
struct Message;
}

class BotLifecycleStorage;
class ChannelStorage;
class InboxStorage;
class PolicyApprovals {
public:
    PolicyApprovals(Database& database, const SecretStorage& secretStorage, const WebIdentityState& webIdentity, TaskRecords& taskRecords,
                    WaitStorage& waitStorage, Policy& permissions, ConversationStorage& conversationTurns,
                    BotLifecycleStorage& botLifecycle, ChannelStorage& channelStorage, InboxStorage& inboxStorage,
                    const SuggestionStorage& suggestions)
        : database(database), secretStorage(secretStorage), webIdentity(webIdentity), taskRecords(taskRecords), waitStorage(waitStorage),
          permissions(permissions), conversationTurns(conversationTurns), botLifecycle(botLifecycle), channelStorage(channelStorage),
          inboxStorage(inboxStorage), suggestions(suggestions) {}

    Seconds now() const { return clock(); }
    void setClock(const std::function<Seconds()>& value) { clock = value; }
    PolicyOrigin policyOrigin(const conversation::Message&);
    PolicyOrigin taskPolicyOrigin(const TaskData&);
    ApprovalGate approveAction(const PolicyOrigin&, const Resource&, std::string_view operation, const Json& payload, Seconds now,
                               bool claim = true, ApprovalWait = ApprovalWait::pause);
    ApprovalGate approveTaskLaunch(const TaskData&, const TaskRoute&, std::string_view operation, const Json& payload, Seconds now,
                                   bool claim);
    void consumePolicyApproval(std::string_view operation);
    void settlePolicyApprovals(std::optional<Seconds> now = {});
    void decidePolicyApproval(const Person&, const InboxTarget&, std::string_view);
    Json policyApprovalCards(const Person&, std::string_view bot);
    void releasePolicyTurn(Id turn);
private:
    bool currentPolicyApproval(Id turn, Id task, Id revision, Id generation, std::string_view operation) const;
    void releasePolicyTask(Id approval, std::string_view resolution);

    Database& database;
    const SecretStorage& secretStorage;
    const WebIdentityState& webIdentity;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    Policy& permissions;
    ConversationStorage& conversationTurns;
    BotLifecycleStorage& botLifecycle;
    ChannelStorage& channelStorage;
    InboxStorage& inboxStorage;
    const SuggestionStorage& suggestions;
    std::function<Seconds()> clock = unixTime;
};
}
