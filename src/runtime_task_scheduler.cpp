#include "runtime_task_scheduler.hpp"
#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
Runtime::TaskScheduler::TaskScheduler(Store::Impl& db) : db(db) {}

std::vector<Runtime::TaskScheduler::Work> Runtime::TaskScheduler::eligible() {
    std::lock_guard lock(db.mutex);
    auto rows = db.query(R"SQL(
WITH slots AS MATERIALIZED (
  SELECT id,action_id,blocker_id FROM project_route_slots
), runnable AS (
  SELECT id FROM slots WHERE action_id IS NULL AND blocker_id IS NULL
  UNION
  SELECT id FROM tasks WHERE project_id='' AND state NOT IN ('completed','cancelled') AND (
    state IN ('queued','cancelling') OR replanning=1
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

std::vector<Runtime::TaskScheduler::Work> Runtime::TaskScheduler::nextBatch(std::size_t slots,
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
}
