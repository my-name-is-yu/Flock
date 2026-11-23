#include "storage.hpp"

namespace flockv0 {
void Store::Impl::initializeProjectQueue() {
    constexpr auto installed = "SELECT 1 FROM sqlite_schema WHERE type='view' AND name='task_project_slots'";
    if (query(installed).next()) return;
    Transaction update(db);
    if (query(installed).next()) return;
    constexpr auto sql = R"SQL(
CREATE VIEW project_route_queue AS
WITH candidates AS (
  SELECT task.id,task.state,task.project_id,task.environment_id,NULL AS action_id
  FROM tasks task
  WHERE json_type(task.data,'$.runTargets') IS NULL AND task.project_id<>''
    AND task.state NOT IN ('completed','cancelled')
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
         ORDER BY COALESCE(sender.state,task.state)='queued',
           COALESCE(sender.id,task.id),
           CASE WHEN sender.id IS NOT NULL AND task.state='cancelling' THEN 0
                WHEN task.state='peer_waiting' THEN 2 ELSE 1 END, task.id
       ) AS position
FROM candidates task
LEFT JOIN peer_exchanges peer ON peer.recipient_task=task.id
LEFT JOIN candidates sender ON sender.id=peer.sender_task
  AND sender.environment_id=task.environment_id AND sender.project_id=task.project_id
  AND peer.phase<>'closed'
WHERE COALESCE(peer.kind,'')<>'consult';
CREATE VIEW project_route_slots AS
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
CREATE VIEW task_project_slots AS
SELECT id,CASE WHEN count(blocker_id)=count(*) THEN min(blocker_id) END AS blocker_id
FROM project_route_slots GROUP BY id;
)SQL";
    if (sqlite3_exec(db,sql,nullptr,nullptr,nullptr) != SQLITE_OK) throw StorageError(db);
    update.commit();
}

bool Store::Impl::projectRouteReady(Id task, std::string_view action) const {
    return query("SELECT 1 FROM project_route_slots WHERE id=? AND action_id=? AND blocker_id IS NULL",
                 {task,action}).next();
}
}
