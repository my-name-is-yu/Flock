#include "runtime_task_scheduler.hpp"
#include "outbox_storage.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "database.hpp"
#include "task_events.hpp"
#include "task_routing.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
runtime::TaskScheduler::TaskScheduler(Database& database, TaskEvents& taskEvents, TaskRouting& taskRouting, T3& t3, TaskUpdates& updates,
                                      bool requireT3Connection, const std::function<Seconds()>& clock, std::stop_token shutdown)
    : database(database), taskEvents(taskEvents), taskRouting(taskRouting), t3(t3), updates(updates),
      requireT3Connection(requireT3Connection), clock(clock), shutdown(shutdown) {}

std::vector<runtime::TaskScheduler::Work> runtime::TaskScheduler::eligible() {
    std::lock_guard lock(database.mutex);
    auto rows = database.query(R"SQL(
WITH slots AS MATERIALIZED (
  SELECT id,action_id,blocker_id FROM project_route_slots
), runnable AS (
  SELECT id FROM slots WHERE action_id IS NULL AND blocker_id IS NULL
  UNION
  SELECT id FROM tasks WHERE project_id='' AND state NOT IN ('completed','cancelled') AND (
    (state IN ('queued','cancelling') OR (state='deciding' AND environment_id<>'')) OR replanning=1
  )
  UNION
  SELECT task.id FROM tasks task JOIN waits wait ON wait.task_id=task.id AND wait.resolution IS NULL
  WHERE task.state='waiting' AND (
    (task.project_id='' AND wait.reason='settings') OR
    (wait.reason='bot_notes_unavailable' AND task.id IN (SELECT id FROM automation_before_dispatch))
  )
  UNION
  SELECT id FROM tasks WHERE state='queued' AND replanning=1
    AND (pending_environment_id<>environment_id OR (pending_pinned=1 AND pending_project_id<>project_id))
  UNION
  SELECT tasks.id FROM tasks JOIN peer_exchanges ON tasks.id=recipient_task
  WHERE kind='consult' AND state NOT IN ('completed','cancelled')
  UNION
  SELECT tasks.id FROM tasks JOIN peer_exchanges ON tasks.id=recipient_task
  WHERE state='cancelling' AND tasks.active_run IS NULL AND NOT EXISTS (
    SELECT 1 FROM outbox WHERE outbox.id=tasks.action_id AND phase='started'
  )
)
SELECT task.id,NULL AS action_id FROM runnable JOIN tasks task ON task.id=runnable.id
WHERE NOT EXISTS (SELECT 1 FROM task_run_destinations WHERE task_id=task.id AND action_id IS NOT NULL)
AND NOT EXISTS (
  SELECT 1 FROM policy_approvals approval WHERE approval.task=task.id AND turn=0 AND status='pending'
)
UNION ALL
SELECT slot.id,slot.action_id FROM slots slot JOIN tasks task ON task.id=slot.id
WHERE slot.action_id IS NOT NULL AND (task.state='cancelling' OR EXISTS (
  SELECT 1 FROM outbox stop WHERE stop.id=slot.action_id AND stop.stop_operation IS NOT NULL
) OR (
  slot.blocker_id IS NULL AND NOT EXISTS (
    SELECT 1 FROM policy_approvals WHERE operation=?1 || slot.action_id AND status='pending'
  )
))
ORDER BY 1,2)SQL",
                               {runOutboxOperation});
    std::vector<Work> result;
    while (rows.next()) {
        auto action = rows.string(1);
        result.push_back({rows.number(0), action.empty() ? std::nullopt : std::optional{std::move(action)}});
    }
    return result;
}

std::vector<runtime::TaskScheduler::Work> runtime::TaskScheduler::nextBatch(std::size_t slots,
                                                                            const std::function<bool(const Work&)>& isActive) {
    if (slots == 0) return {};
    auto jobs = eligible();
    std::erase_if(jobs, isActive);
    if (jobs.size() > slots) {
        std::rotate(jobs.begin(), std::upper_bound(jobs.begin(), jobs.end(), lastScheduled), jobs.end());
        jobs.resize(slots);
    }
    if (!jobs.empty()) lastScheduled = jobs.back();
    return jobs;
}

bool runtime::TaskScheduler::selectProject(TaskData& task) {
    const auto now = clock();
    if (now < task.t3CheckAt.value_or(0)) return false;
    const auto catalog = taskRouting.readProjectCatalog(t3, task.botId, {task.environmentId, {}, task.gatewayEnvironmentId.value_or("")},
                                                        requireT3Connection);
    return updates.change(
               task,
               [&](TaskData& current) {
                   if (shutdown.stop_requested()) return false;
                   taskRouting.applyProjectCatalog(current, catalog);
                   if (current.state == TaskState::queued && current.projectId.empty()) {
                       if (current.eventId == 0) taskEvents.event(current, "intake", {{"kind", "intake"}});
                       current.state = TaskState::deciding;
                   }
                   current.t3CheckAt = current.state == TaskState::waiting ? std::optional{now + 5} : std::nullopt;
                   return true;
               },
               &task) &&
           !task.projectId.empty();
}

void runtime::TaskScheduler::activate(TaskData task) {
    if (task.projectId.empty() && !selectProject(task)) return;
    updates.change(task, [&](TaskData& current) {
        auto turn = database.query("SELECT 1 FROM task_project_slots WHERE id=? AND blocker_id IS NULL", {current.id});
        if (!turn.next()) return false;
        if (!current.actionId.empty()) {
            current.state = TaskState::sending;
            return true;
        }
        if (current.eventId == 0) taskEvents.event(current, "intake", {{"kind", "intake"}});
        current.state = TaskState::deciding;
        return true;
    });
}
}
