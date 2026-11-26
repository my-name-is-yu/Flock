#pragma once

#include "runtime_components.hpp"
#include "journal_task.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"

#include <string>
#include <string_view>
#include <optional>

namespace flockv0 {
class ConversationStorage;
class Database;
class GitHost;
class GitStorage;
class JournalStorage;
class MemoryStorage;
class PeerStorage;
class SecretStorage;
class TaskEvents;
class TaskRecords;
class WaitStorage;

class runtime::TaskDecisions {
public:
    TaskDecisions(Database& database, ConversationStorage& conversationTurns, GitStorage& gitStorage, JournalStorage& journalStorage,
                  MemoryStorage& memoryStorage, PeerStorage& peerStorage, SecretStorage& secretStorage, TaskEvents& taskEvents,
                  TaskRecords& taskRecords, WaitStorage& waitStorage, TaskUpdates& updates, RunReports& reports, SessionHistory& history,
                  PeerExchanges& peerExchanges, OutboxDispatch& dispatch, const std::string& sessionRead, GitHost* gitHost);

    bool canComplete(const TaskData& task, const Json& event) const;
    struct LearningSource {
        Id reportId;
        std::string runId;
    };
    std::optional<LearningSource> learningSource(const TaskData&, const Json& event);
    void rejectDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view message,
                        bool replay = false);
    void accept(const TaskData& snapshot, const Json& event, const TaskDecision& decision, std::string_view approvedInvocation = {});
    bool continueStage(const TaskData& task);
    bool resumeGitApproval(const TaskData& task);
private:
    void acceptReplyOrCompletion(const TaskData& snapshot, const Json& event, const TaskDecision& decision);
    void complete(TaskData& current, const Json& event, const TaskDecision& decision);
    CompletionProvenance completionProvenance(const TaskData& task, const Json& event);

    Database& database;
    ConversationStorage& conversationTurns;
    GitStorage& gitStorage;
    JournalStorage& journalStorage;
    MemoryStorage& memoryStorage;
    PeerStorage& peerStorage;
    SecretStorage& secretStorage;
    TaskEvents& taskEvents;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    TaskUpdates& updates;
    RunReports& reports;
    SessionHistory& history;
    PeerExchanges& peerExchanges;
    OutboxDispatch& dispatch;
    const std::string& sessionRead;
    GitHost* gitHost;
};
}
