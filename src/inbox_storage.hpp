#pragma once

#include "flock/json.hpp"
#include "flock/store.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Database;
class GitStorage;
class JournalStorage;
class SecretStorage;
class ModelCalls;
class TaskOperations;
class Statement;
class TaskRecords;
class WaitStorage;
struct InboxTarget;
struct Person;
struct TaskData;
enum class InboxKind;
enum class WaitReason;

std::optional<InboxKind> inboxWaitKind(WaitReason);

class InboxStorage {
public:
    InboxStorage(Database& database, SecretStorage& secretStorage, TaskRecords& taskRecords, WaitStorage& waitStorage,
                 JournalStorage& journalStorage, Store::Impl& remaining, ModelCalls& modelCalls, TaskOperations& taskOperations)
        : database(database), secretStorage(secretStorage), taskRecords(taskRecords), waitStorage(waitStorage),
          journalStorage(journalStorage), remaining(remaining), modelCalls(modelCalls), taskOperations(taskOperations) {}

    Json inbox(const Person&, std::string_view bot) const;
    Json decisionHistory(const Person&, std::string_view bot) const;
    void decideInbox(GitStorage&, const Person&, const InboxTarget&, std::string_view decision, const Json& data);
    void recordInboxDecision(const Person&, const InboxTarget&, std::string_view decision);
    void insertPrReview(const TaskData&, std::string_view url, std::string_view title, std::string_view prompt, std::string_view gitKey,
                        std::string_view operationId = {}, const Json& candidates = Json::array());
    Statement actionablePrReviews(std::string_view bot) const;
private:
    std::vector<std::string> visibleBoardBots(const Person&, std::string_view bot) const;

    Database& database;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    JournalStorage& journalStorage;
    Store::Impl& remaining;
    ModelCalls& modelCalls;
    TaskOperations& taskOperations;
};
}
