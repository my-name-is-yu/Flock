#include "runtime_task_updates.hpp"

namespace flockv0 {
void Runtime::TaskUpdates::saveTask(TaskData& task, std::optional<DecisionCheckpoint> resumedDecision) {
    if (resumedDecision) resumedDecision->generation = task.generation + 1;
    task.pendingDecision = std::move(resumedDecision);
    db.save(task);
}

void Runtime::TaskUpdates::consumeEvent(TaskData& task) {
    task.feedback.reset();
    db.consumeEvent(task);
}

TaskData Runtime::TaskUpdates::snapshot(Id id) {
    std::lock_guard lock(db.mutex);
    return db.load(id);
}

bool Runtime::TaskUpdates::checkpointMatches(const DecisionCheckpoint& checkpoint, const TaskData& task) {
    return checkpoint.eventId == task.eventId && checkpoint.generation == task.generation && checkpoint.revisionId == task.currentRevision;
}
}
