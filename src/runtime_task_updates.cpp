#include "json_fields.hpp"
#include "database.hpp"
#include "journal_storage.hpp"
#include "task_events.hpp"
#include "task_records.hpp"
#include "wait_storage.hpp"
#include "runtime_task_updates.hpp"
#include "journal_task.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/outbox.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
void Runtime::Components::TaskUpdates::saveTask(TaskData& task, std::optional<DecisionCheckpoint> resumedDecision) {
    if (resumedDecision) resumedDecision->generation = task.generation + 1;
    task.pendingDecision = std::move(resumedDecision);
    records.save(task);
}

void Runtime::Components::TaskUpdates::consumeEvent(TaskData& task) {
    task.feedback.reset();
    events.consumeEvent(task);
}

TaskData Runtime::Components::TaskUpdates::snapshot(Id id) {
    std::lock_guard lock(database.mutex);
    return records.load(id);
}

bool Runtime::Components::TaskUpdates::checkpointMatches(const DecisionCheckpoint& checkpoint, const TaskData& task) {
    return checkpoint.eventId == task.eventId && checkpoint.generation == task.generation && checkpoint.revisionId == task.currentRevision;
}

bool Runtime::Components::TaskUpdates::admitWork(TaskData& task, const TaskDecision& decision, bool& charge) {
    charge = task.lastRun || database.query("SELECT 1 FROM peer_exchanges WHERE sender_task=? AND kind='work' LIMIT 1", {task.id}).next();
    if (!charge) return true;
    if (task.allowance.remaining == 0) {
        waits.wait(task, WorkBudgetWait{workBudgetTool(decision.tool), decision.reason, waitUsage(decision.usage)},
                   "This request used its allowance for follow-up work.");
        return false;
    }
    --task.allowance.remaining;
    ++task.allowance.additionalUsed;
    return true;
}

void Runtime::Components::TaskUpdates::establishCriteria(TaskData& task, const Json& args) {
    if (task.currentRevision) return;
    const auto source = task.sourceMessageId.value_or(0);
    if (const auto& proposal = task.criteriaProposal)
        records.addRevision(task, task.intent, proposal->doneWhen, proposal->sourceMessageId.value_or(source));
    else
        records.addRevision(task, task.intent, field(args, "doneWhen"), source);
    task.criteriaProposal.reset();
}

void Runtime::Components::TaskUpdates::recordDecision(const TaskData& task, const Json& event, const TaskDecision& decision,
                                                      std::string_view action) {
    journal.journal(task.id, DecisionEntry{event.at("id").get<Id>(), task.currentRevision, std::string(action), decision,
                                           event.at("evidence"), decision.usage});
}

void Runtime::Components::TaskUpdates::refundUnsentWork(TaskData& task, const Outbox& action) {
    if (action.chargesBudget) {
        ++task.allowance.remaining;
        --task.allowance.additionalUsed;
    }
}
}
