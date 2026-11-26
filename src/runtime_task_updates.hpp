#pragma once

#include "database.hpp"
#include "runtime_components.hpp"
#include "task_records.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/outbox.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace flockv0 {
class JournalStorage;
class TaskEvents;
class WaitStorage;

class Runtime::Components::TaskUpdates {
public:
    TaskUpdates(Database& database, JournalStorage& journal, TaskEvents& events, TaskRecords& records, WaitStorage& waits)
        : database(database), journal(journal), events(events), records(records), waits(waits) {}

    TaskData snapshot(Id id);
    void saveTask(TaskData& task, std::optional<DecisionCheckpoint> resumedDecision = std::nullopt);
    void consumeEvent(TaskData& task);
    static bool checkpointMatches(const DecisionCheckpoint& checkpoint, const TaskData& task);

    bool admitWork(TaskData&, const TaskDecision&, bool& charge);
    void establishCriteria(TaskData&, const Json& args);
    void recordDecision(const TaskData&, const Json& event, const TaskDecision&, std::string_view action);
    static void refundUnsentWork(TaskData&, const Outbox&);

    bool changeTask(Id id, const auto& update, TaskData* saved = nullptr) {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        auto task = records.load(id);
        if (!update(task)) return false;
        saveTask(task);
        transaction.commit();
        if (saved) *saved = std::move(task);
        return true;
    }

    bool change(const TaskData& snapshot, const auto& update, TaskData* saved = nullptr) {
        return changeTask(
            snapshot.id,
            [&](TaskData& task) {
                return task.generation == snapshot.generation && task.eventId == snapshot.eventId &&
                       task.currentRevision == snapshot.currentRevision && update(task);
            },
            saved);
    }
private:
    Database& database;
    JournalStorage& journal;
    TaskEvents& events;
    TaskRecords& records;
    WaitStorage& waits;
};
}
