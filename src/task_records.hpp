#pragma once

#include <optional>
#include <string_view>
#include "flock/task.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
class JournalStorage;
class OutboxStorage;
class WaitStorage;
class Statement;
struct CompletionEntry;

class TaskRecords {
public:
    TaskRecords(Database& database, JournalStorage& journalStorage, OutboxStorage& outboxStorage, WaitStorage& waitStorage)
        : database(database), journalStorage(journalStorage), outboxStorage(outboxStorage), waitStorage(waitStorage) {}

    TaskData load(Id) const;
    std::optional<std::string> externalWaitingNote(const TaskData&) const;
    TaskView taskDetails(Id, bool includeBlocker) const;
    TaskView sessionTask(Id) const;
    void addTaskDetails(TaskView&) const;
    std::optional<TaskBlocker> queueBlocker(Id) const;
    Id addRevision(TaskData&, std::string_view intent, std::string_view doneWhen, Id sourceMessageId = 0);
    TaskData readTask(const Statement&, int col = 0) const;
    Id insertTask(const TaskData&);
    void save(TaskData&);
    Id complete(TaskData&, const CompletionEntry&);
    void saveDecisionCheckpoint(TaskData&, DecisionCheckpoint);
private:
    Database& database;
    JournalStorage& journalStorage;
    OutboxStorage& outboxStorage;
    WaitStorage& waitStorage;
};

void requireInstructionAcknowledgments(const TaskData&, const Json& args);
void waitForExternalEvent(TaskData&, const Json& args, Seconds now);
}
