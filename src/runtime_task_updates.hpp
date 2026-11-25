#pragma once

#include "storage.hpp"

namespace flockv0 {
class Runtime::TaskUpdates {
public:
    explicit TaskUpdates(Store::Impl& db) : db(db) {}

    TaskData snapshot(Id id);
    void saveTask(TaskData& task, std::optional<DecisionCheckpoint> resumedDecision = std::nullopt);
    void consumeEvent(TaskData& task);
    static bool checkpointMatches(const DecisionCheckpoint& checkpoint, const TaskData& task);

    bool admitWork(TaskData&, const TaskDecision&, bool& charge);
    void establishCriteria(TaskData&, const Json& args);
    void recordDecision(const TaskData&, const Json& event, const TaskDecision&, std::string_view action);
    static void refundUnsentWork(TaskData&, const Outbox&);

    bool changeTask(Id id, const auto& update, TaskData* saved = nullptr) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        auto task = db.load(id);
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
    Store::Impl& db;
};
}
