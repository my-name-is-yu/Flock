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
class ConversationMessages;

class Database;
class SecretStorage;
class TaskRecords;
class WaitStorage;
struct InboxTarget;
struct Person;
struct Resource;
struct TaskData;
struct WebIdentityState;

namespace conversation {
struct Message;
}

class BotLifecycleStorage;
class ChannelConnectors;
class ChannelNotices;
class InboxDecisions;
class PolicyApprovals {
public:
    PolicyApprovals(Database& database, const SecretStorage& secretStorage, const WebIdentityState& webIdentity, TaskRecords& taskRecords,
                    WaitStorage& waitStorage, Policy& permissions, ConversationMessages& conversationMessages,
                    BotLifecycleStorage& botLifecycle, ChannelConnectors& channelConnectors, ChannelNotices& channelNotices,
                    InboxDecisions& inboxDecisions)
        : database(database), secretStorage(secretStorage), webIdentity(webIdentity), taskRecords(taskRecords), waitStorage(waitStorage),
          permissions(permissions), conversationMessages(conversationMessages), botLifecycle(botLifecycle),
          channelConnectors(channelConnectors), channelNotices(channelNotices), inboxDecisions(inboxDecisions) {}

    Seconds now() const { return clock(); }
    void setClock(const std::function<Seconds()>& value) { clock = value; }
    PolicyOrigin policyOrigin(const conversation::Message&);
    PolicyOrigin taskPolicyOrigin(const TaskData&);
    ApprovalGate approveAction(const PolicyOrigin&, const Resource&, std::string_view operation, const Json& payload, Seconds now,
                               bool claim = true, ApprovalWait = ApprovalWait::pause);
    void consumePolicyApproval(std::string_view operation);
    void keepUnsentTaskApproval(const TaskData&, std::string_view action, Id readyGeneration, bool sendStarted, Seconds now);
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
    ConversationMessages& conversationMessages;
    BotLifecycleStorage& botLifecycle;
    ChannelConnectors& channelConnectors;
    ChannelNotices& channelNotices;
    InboxDecisions& inboxDecisions;
    std::function<Seconds()> clock = unixTime;
};
}
