#pragma once

#include "database.hpp"
#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace flockv0 {
class JournalStorage;
class TaskEvents;
class TaskRecords;

class runtime::TaskUpdates {
public:
    TaskUpdates(Database& database, JournalStorage& journal, TaskEvents& events, TaskRecords& records)
        : database(database), journal(journal), events(events), records(records) {}

    TaskData snapshot(Id id);
    void saveTask(TaskData& task);
    void consumeEvent(TaskData& task);

    void establishCriteria(TaskData&, const Json& args);
    void recordDecision(const TaskData&, const Json& event, const TaskDecision&, std::string_view action);

    bool changeTask(Id id, const auto& update, TaskData* saved = nullptr) {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        auto task = loadTask(id);
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
    TaskData loadTask(Id id);

    Database& database;
    JournalStorage& journal;
    TaskEvents& events;
    TaskRecords& records;
};
}
