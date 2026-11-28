#pragma once

#include "flock/json.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Database;
class Policy;
class SecretStorage;
class TaskOperations;
class TaskRecords;
class WaitStorage;
struct InboxTarget;
struct Person;
enum class InboxKind;
enum class WaitReason;

std::optional<InboxKind> inboxWaitKind(WaitReason);

// Records who decided an inbox item and how.
class InboxDecisions {
public:
    InboxDecisions(Database& database, SecretStorage& secretStorage) : database(database), secretStorage(secretStorage) {}

    void record(const Person&, const InboxTarget&, std::string_view decision);
private:
    Database& database;
    SecretStorage& secretStorage;
};

class PolicyApprovals;
class InboxStorage {
public:
    InboxStorage(Database& database, SecretStorage& secretStorage, TaskRecords& taskRecords, WaitStorage& waitStorage, Policy& permissions,
                 TaskOperations& taskOperations, PolicyApprovals& policyApprovals, InboxDecisions& inboxDecisions)
        : database(database), secretStorage(secretStorage), taskRecords(taskRecords), waitStorage(waitStorage), permissions(permissions),
          taskOperations(taskOperations), policyApprovals(policyApprovals), inboxDecisions(inboxDecisions) {}

    Json inbox(const Person&, std::string_view bot) const;
    Json decisionHistory(const Person&, std::string_view bot) const;
    void decideInbox(const Person&, const InboxTarget&, std::string_view decision, const Json& data);
private:
    std::vector<std::string> visibleBoardBots(const Person&, std::string_view bot) const;

    Database& database;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    Policy& permissions;
    TaskOperations& taskOperations;
    PolicyApprovals& policyApprovals;
    InboxDecisions& inboxDecisions;
};
}
