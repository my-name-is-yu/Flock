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

class PolicyApprovals;
class InboxStorage {
public:
    InboxStorage(Database& database, SecretStorage& secretStorage, TaskRecords& taskRecords, WaitStorage& waitStorage, Policy& permissions,
                 TaskOperations& taskOperations, PolicyApprovals& policyApprovals)
        : database(database), secretStorage(secretStorage), taskRecords(taskRecords), waitStorage(waitStorage), permissions(permissions),
          taskOperations(taskOperations), policyApprovals(policyApprovals) {}

    Json inbox(const Person&, std::string_view bot) const;
    Json decisionHistory(const Person&, std::string_view bot) const;
    void decideInbox(const Person&, const InboxTarget&, std::string_view decision, const Json& data);
    void recordInboxDecision(const Person&, const InboxTarget&, std::string_view decision);
private:
    std::vector<std::string> visibleBoardBots(const Person&, std::string_view bot) const;

    Database& database;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    Policy& permissions;
    TaskOperations& taskOperations;
    PolicyApprovals& policyApprovals;
};
}
