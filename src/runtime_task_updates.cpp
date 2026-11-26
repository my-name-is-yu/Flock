#include "database.hpp"
#include "journal_storage.hpp"
#include "task_events.hpp"
#include "task_records.hpp"
#include "runtime_task_updates.hpp"
#include "journal_task.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
void runtime::TaskUpdates::saveTask(TaskData& task, std::optional<DecisionCheckpoint> resumedDecision) {
    if (resumedDecision) resumedDecision->generation = task.generation + 1;
    task.pendingDecision = std::move(resumedDecision);
    records.save(task);
}

void runtime::TaskUpdates::consumeEvent(TaskData& task) {
    task.feedback.reset();
    events.consumeEvent(task);
}

TaskData runtime::TaskUpdates::snapshot(Id id) {
    std::lock_guard lock(database.mutex);
    return loadTask(id);
}

TaskData runtime::TaskUpdates::loadTask(Id id) {
    return records.load(id);
}

bool runtime::TaskUpdates::checkpointMatches(const DecisionCheckpoint& checkpoint, const TaskData& task) {
    return checkpoint.eventId == task.eventId && checkpoint.generation == task.generation && checkpoint.revisionId == task.currentRevision;
}

void runtime::TaskUpdates::establishCriteria(TaskData& task, const Json& args) {
    if (task.currentRevision) return;
    const auto source = task.sourceMessageId.value_or(0);
    if (const auto& proposal = task.criteriaProposal)
        records.addRevision(task, task.intent, proposal->doneWhen, proposal->sourceMessageId.value_or(source));
    else
        records.addRevision(task, task.intent, field(args, "doneWhen"), source);
    task.criteriaProposal.reset();
}

void runtime::TaskUpdates::recordDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view action) {
    journal.journal(task.id, DecisionEntry{event.at("id").get<Id>(), task.currentRevision, std::string(action), decision,
                                           event.at("evidence"), decision.usage});
}

}
