#include "storage.hpp"

namespace flockv0 {
void Store::Impl::initializeProjectQueue() {
    const auto projectQueueInstalled = [this] {
        return query("SELECT 1 FROM sqlite_schema WHERE type='view'"
                     " AND name IN ('task_project_slots','automation_before_dispatch')"
                     " GROUP BY type HAVING count(*)=2").next();
    };
    if (projectQueueInstalled()) return;
    Transaction install(db);
    if (projectQueueInstalled()) return;
    constexpr auto sql = R"SQL(
CREATE VIEW IF NOT EXISTS automation_before_dispatch AS
SELECT task.id FROM tasks task
WHERE task.project_id<>'' AND json_type(task.data,'$.trigger')='object'
  AND json_extract(task.data,'$.activeRun') IS NULL AND json_extract(task.data,'$.lastRun') IS NULL
  AND json_type(task.data,'$.runTargets') IS NULL AND json_type(task.data,'$.runResults') IS NULL
  AND NOT EXISTS (SELECT 1 FROM outbox WHERE task_id=task.id);
DROP VIEW IF EXISTS project_route_queue;
CREATE VIEW project_route_queue AS
WITH candidates AS (
  SELECT task.id,task.state AS slot_state,task.project_id,task.environment_id,NULL AS action_id
  FROM tasks task
  WHERE json_type(task.data,'$.runTargets') IS NULL AND json_type(task.data,'$.runResults') IS NULL
    AND task.project_id<>''
    AND task.state NOT IN ('completed','cancelled')
    AND NOT (task.state='waiting' AND EXISTS (
      SELECT 1 FROM automation_before_dispatch undispatched
      JOIN waits wait ON wait.task_id=undispatched.id AND wait.resolution IS NULL
      WHERE undispatched.id=task.id AND json_extract(wait.data,'$.reason')='bot_notes_unavailable'
    ))
  UNION ALL
  SELECT task.id,CASE WHEN outbox.phase IN ('started','accepted') THEN task.state ELSE 'queued' END,
         json_extract(route.value,'$.projectId'),json_extract(route.value,'$.environmentId'),outbox.id
  FROM tasks task
  CROSS JOIN json_each(coalesce(json_extract(task.data,'$.runResults'),json_extract(task.data,'$.runTargets'))) route
  LEFT JOIN outbox ON outbox.task_id=task.id AND outbox.id=json_extract(route.value,'$.actionId')
  WHERE task.state NOT IN ('completed','cancelled')
    AND coalesce(json_extract(route.value,'$.status'),'queued') IN ('queued','running','confirmation_pending')
)
SELECT task.id,task.environment_id,task.project_id,task.action_id,
       ROW_NUMBER() OVER (
         PARTITION BY task.environment_id,task.project_id
         ORDER BY COALESCE(sender.slot_state,task.slot_state)='queued',
           COALESCE(sender.id,task.id),
           CASE WHEN sender.id IS NOT NULL AND task.slot_state='cancelling' THEN 0
                WHEN task.slot_state='peer_waiting' THEN 2 ELSE 1 END, task.id
       ) AS position
FROM candidates task
LEFT JOIN peer_exchanges peer ON peer.recipient_task=task.id
LEFT JOIN candidates sender ON sender.id=peer.sender_task
  AND sender.environment_id=task.environment_id AND sender.project_id=task.project_id
  AND peer.phase<>'closed'
WHERE COALESCE(peer.kind,'')<>'consult';
CREATE VIEW IF NOT EXISTS project_route_slots AS
WITH queue AS (
  SELECT id,environment_id,project_id,action_id,
         FIRST_VALUE(id) OVER (PARTITION BY environment_id,project_id ORDER BY position) AS head_id
  FROM project_route_queue
)
SELECT q.id,q.environment_id,q.project_id,q.action_id,
       CASE WHEN q.id<>q.head_id AND (json_extract(owner.data,'$.workflowId') IS NULL OR
         json_extract(owner.data,'$.workflowId') IS NOT json_extract(t.data,'$.workflowId'))
       THEN q.head_id END AS blocker_id
FROM queue q JOIN tasks t ON t.id=q.id JOIN tasks owner ON owner.id=q.head_id;
CREATE VIEW IF NOT EXISTS task_project_slots AS
SELECT id,CASE WHEN count(blocker_id)=count(*) THEN min(blocker_id) END AS blocker_id
FROM project_route_slots GROUP BY id;
)SQL";
    if (sqlite3_exec(db,sql,nullptr,nullptr,nullptr) != SQLITE_OK) throw StorageError(db);
    install.commit();
}

bool Store::Impl::automationBeforeDispatch(Id task) const {
    return query("SELECT 1 FROM automation_before_dispatch WHERE id=?",{task}).next();
}

bool Store::Impl::projectRouteReady(Id task, std::string_view action) const {
    return query("SELECT 1 FROM project_route_slots WHERE id=? AND action_id=? AND blocker_id IS NULL",
                 {task,action}).next();
}
}
