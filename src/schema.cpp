#include "storage.hpp"

namespace flockv0 {
namespace {
constexpr Id applicationId = 0x464c4f43;
constexpr Id schemaVersion = 12;

void applySchema(sqlite3* db) {
    constexpr auto schema = R"SQL(
CREATE TABLE tasks(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL,
  project_id TEXT NOT NULL, state TEXT NOT NULL, generation INTEGER NOT NULL, data TEXT NOT NULL);
CREATE INDEX tasks_route ON tasks(environment_id,project_id,id)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX tasks_bot ON tasks(bot_id,id DESC);
CREATE TABLE journal(
  id INTEGER PRIMARY KEY, task_id INTEGER REFERENCES tasks(id),
  time INTEGER NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL, conversation_bot_id TEXT,
  CHECK(task_id IS NOT NULL OR conversation_bot_id IS NOT NULL));
CREATE INDEX journal_task ON journal(task_id,id);
CREATE INDEX journal_wait_anchor ON journal(task_id,json_extract(data,'$.id'),id)
  WHERE kind='waiting' AND conversation_bot_id IS NULL;
CREATE TABLE events(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  dedupe TEXT NOT NULL, consumed INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
  retry_at INTEGER NOT NULL DEFAULT 0, data TEXT NOT NULL, evidence TEXT NOT NULL DEFAULT '[]',
  reserved_attempt INTEGER REFERENCES model_attempts(id),
  UNIQUE(task_id,dedupe));
CREATE TABLE t3_events(
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, item_type TEXT NOT NULL, item_id TEXT NOT NULL,
  PRIMARY KEY(environment_id,thread_id,item_type,item_id));
CREATE TABLE bot_t3_projects(
  bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, project_id TEXT NOT NULL,
  PRIMARY KEY(bot_id,environment_id));
CREATE TABLE waits(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  data TEXT NOT NULL, resolution TEXT);
CREATE UNIQUE INDEX waits_open ON waits(task_id) WHERE resolution IS NULL;
CREATE TABLE outbox(
  id TEXT PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  phase TEXT NOT NULL, auth_generation TEXT NOT NULL DEFAULT '', data TEXT NOT NULL);
CREATE TABLE runs(
  action_id TEXT PRIMARY KEY REFERENCES outbox(id), task_id INTEGER NOT NULL REFERENCES tasks(id),
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, run_id TEXT NOT NULL, link TEXT NOT NULL,
  UNIQUE(environment_id,thread_id,run_id));
CREATE INDEX runs_task ON runs(task_id,action_id);
CREATE TABLE session_read_refs(
  ref TEXT PRIMARY KEY, bot TEXT NOT NULL, kind INTEGER NOT NULL,
  environment TEXT NOT NULL, thread TEXT NOT NULL, payload TEXT NOT NULL,
  UNIQUE(bot,kind,environment,thread,payload));
CREATE TABLE peer_exchanges(
  id TEXT PRIMARY KEY, kind TEXT NOT NULL, phase TEXT NOT NULL, last_message TEXT NOT NULL,
  sender_task INTEGER NOT NULL REFERENCES tasks(id),
  recipient_task INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
  sender_environment TEXT NOT NULL, sender_revision INTEGER);
CREATE UNIQUE INDEX peer_sender_open ON peer_exchanges(sender_task) WHERE phase<>'closed';
CREATE INDEX journal_session_changes ON journal(id,task_id) WHERE kind IN ('run_started','report');
CREATE INDEX journal_peer_message ON journal(task_id,json_extract(data,'$.exchangeId'),id) WHERE kind='bot_message';

CREATE TABLE web_identity_required(id INTEGER PRIMARY KEY CHECK(id=1));
CREATE TABLE policy_settings(id INTEGER PRIMARY KEY CHECK(id=1),mode TEXT NOT NULL CHECK(mode IN ('full','supervised')), revision INTEGER NOT NULL);
INSERT INTO policy_settings VALUES(1,'full',0);
CREATE TABLE policy_rules(id INTEGER PRIMARY KEY,kind TEXT NOT NULL CHECK(kind IN ('allow','ask','never')),
 action TEXT NOT NULL,target TEXT NOT NULL,created_at INTEGER NOT NULL,exact INTEGER NOT NULL,UNIQUE(kind,action,target));
CREATE TABLE policy_approvals(
 id INTEGER PRIMARY KEY, operation TEXT NOT NULL UNIQUE, bot TEXT NOT NULL,
 action TEXT NOT NULL,target TEXT NOT NULL,fingerprint TEXT NOT NULL,preview TEXT NOT NULL,
 scope TEXT NOT NULL, requester TEXT NOT NULL, anchor INTEGER NOT NULL, turn INTEGER NOT NULL,
 task INTEGER NOT NULL, revision INTEGER NOT NULL,generation INTEGER NOT NULL,
 created_at INTEGER NOT NULL,expires_at INTEGER,mode_revision INTEGER NOT NULL,
 status TEXT NOT NULL DEFAULT 'pending', resume TEXT,
 wait_id INTEGER NOT NULL DEFAULT 0, card_generation INTEGER NOT NULL, resume_state TEXT NOT NULL DEFAULT ''
);
CREATE INDEX policy_pending ON policy_approvals(status,bot);
CREATE INDEX policy_denied ON policy_approvals(bot,scope,action,target,status);
CREATE TABLE secret_destinations(destination TEXT PRIMARY KEY);
CREATE TABLE bot_owners(bot TEXT PRIMARY KEY,person TEXT NOT NULL);
CREATE TABLE bot_members(bot TEXT NOT NULL,person TEXT NOT NULL,PRIMARY KEY(bot,person));
CREATE TABLE policy_journal(id INTEGER PRIMARY KEY,time INTEGER,actor TEXT,bot TEXT,action TEXT,details TEXT);
CREATE TABLE protected_inputs(id INTEGER PRIMARY KEY,bot TEXT,name TEXT,file TEXT NOT NULL UNIQUE,
 status TEXT DEFAULT 'pending',source_message_id INTEGER REFERENCES messages(id));
CREATE UNIQUE INDEX pending_secret ON protected_inputs(bot,name) WHERE status='pending';
CREATE UNIQUE INDEX fulfilled_secret ON protected_inputs(name) WHERE status='fulfilled';
CREATE TABLE bot_changes(id INTEGER PRIMARY KEY,bot TEXT,operation TEXT,target TEXT,name TEXT,
 request_key TEXT UNIQUE NOT NULL,source_message_id INTEGER REFERENCES messages(id));
CREATE TABLE bot_creation_intents(
 request_key TEXT PRIMARY KEY,target TEXT UNIQUE NOT NULL,person TEXT NOT NULL);
CREATE TABLE environment_settings(bot TEXT,environment TEXT,timeout INTEGER,PRIMARY KEY(bot,environment));

CREATE INDEX journal_bot ON journal(conversation_bot_id,id) WHERE conversation_bot_id IS NOT NULL;
CREATE INDEX journal_task_results ON journal(task_id,id DESC)
  WHERE kind IN ('completed','report','cancelled');
CREATE INDEX journal_instruction_message ON journal(
  json_extract(data,'$.messageId'),task_id,json_extract(data,'$.step'))
  WHERE kind IN ('instruction_accepted','instruction_delivered');
CREATE INDEX conversation_active_tasks ON tasks(bot_id,id DESC)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX conversation_terminal_tasks ON tasks(bot_id,id DESC)
  WHERE state IN ('completed','cancelled');
CREATE TABLE messages(
  id INTEGER PRIMARY KEY REFERENCES journal(id), bot_id TEXT NOT NULL,
  client_message_id TEXT NOT NULL, sequence INTEGER NOT NULL,
  reply_to INTEGER, text TEXT NOT NULL, routing TEXT NOT NULL,
  person_id TEXT NOT NULL, person_root INTEGER NOT NULL CHECK(person_root IN (0,1)),
  principal_source INTEGER NOT NULL CHECK(principal_source IN (0,1,2,3,4)),
  UNIQUE(bot_id,person_id,client_message_id), UNIQUE(bot_id,sequence));
CREATE TABLE conversation_turns(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id),
  status TEXT NOT NULL CHECK(status IN ('running','paused','policy_waiting','done')),
  pause_id INTEGER UNIQUE REFERENCES journal(id), reserved_attempt INTEGER,
  reply_id INTEGER UNIQUE REFERENCES journal(id));
CREATE INDEX conversation_turns_running ON conversation_turns(message_id) WHERE status='running';
CREATE VIEW conversation_input_status AS
SELECT t.message_id,
  CASE t.status WHEN 'paused' THEN 'needs_you'
                WHEN 'policy_waiting' THEN 'needs_you'
                WHEN 'done' THEN coalesce(
                  (SELECT json_extract(data,'$.status') FROM journal WHERE id=t.reply_id),'access_revoked')
  END AS status
FROM conversation_turns t;
CREATE TABLE conversation_retries(
  message_id INTEGER PRIMARY KEY REFERENCES conversation_turns(message_id),
  failures INTEGER NOT NULL CHECK(failures BETWEEN 1 AND 7),
  retry_at INTEGER NOT NULL);
CREATE TABLE conversation_steps(
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id), step INTEGER NOT NULL,
  attempt_id INTEGER, decision TEXT CHECK(json_type(decision)='array'), effect TEXT,
  result TEXT CHECK(json_type(result)='array'),
  PRIMARY KEY(message_id,step));
CREATE TABLE session_message_waits(
  message_id INTEGER NOT NULL REFERENCES messages(id),
  bot TEXT NOT NULL, environment TEXT NOT NULL, thread TEXT NOT NULL, run TEXT NOT NULL, title TEXT NOT NULL,
  gateway_environment TEXT NOT NULL DEFAULT '',
  notified INTEGER NOT NULL DEFAULT 0,
  task_id INTEGER REFERENCES tasks(id), action_id TEXT REFERENCES outbox(id),
  delivery TEXT NOT NULL DEFAULT 'started', link TEXT NOT NULL DEFAULT '',
  run_finished INTEGER NOT NULL DEFAULT 0, interrupt_at INTEGER, session_missing INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(message_id,environment,thread,run));
CREATE TABLE task_revisions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  number INTEGER NOT NULL, intent TEXT NOT NULL, done_when TEXT NOT NULL,
  source_message_id INTEGER REFERENCES messages(id),
  UNIQUE(task_id,number));
CREATE INDEX session_message_interrupts_due ON session_message_waits(interrupt_at) WHERE interrupt_at IS NOT NULL;
CREATE INDEX session_message_missing_targets ON session_message_waits(task_id,action_id) WHERE session_missing=1;
CREATE TABLE conversation_model_failures(
  attempt_id INTEGER PRIMARY KEY REFERENCES model_attempts(id),
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id),
  step INTEGER NOT NULL,
  reason TEXT NOT NULL,
  repair_attempt_id INTEGER UNIQUE REFERENCES model_attempts(id) ON DELETE SET NULL);
CREATE UNIQUE INDEX conversation_model_repairs ON conversation_model_failures(message_id) WHERE repair_attempt_id IS NOT NULL;

CREATE TABLE session_participations(
 id INTEGER PRIMARY KEY,
 bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, person_id TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('active','removed','left')),
 stopped_through INTEGER NOT NULL DEFAULT 0,
 UNIQUE(bot_id,environment_id,thread_id));
CREATE TABLE session_asks(
 person_id TEXT NOT NULL, request_id TEXT NOT NULL,
 participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 request_digest TEXT NOT NULL, message_id INTEGER NOT NULL UNIQUE REFERENCES messages(id),
 PRIMARY KEY(person_id,request_id));
CREATE TABLE session_invitations(
 source_message_id INTEGER NOT NULL, step INTEGER NOT NULL,
 inviter_participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 depth INTEGER NOT NULL CHECK(depth BETWEEN 1 AND 3),
 message_id INTEGER NOT NULL UNIQUE REFERENCES messages(id),
 PRIMARY KEY(source_message_id,step),
 FOREIGN KEY(source_message_id,step) REFERENCES conversation_steps(message_id,step));
CREATE INDEX session_invitations_inviter ON session_invitations(inviter_participation_id);
CREATE INDEX session_message_waits_participation ON session_message_waits(bot,environment,thread);
CREATE TABLE session_queue_cancellations(
 participation_id INTEGER NOT NULL REFERENCES session_participations(id), run TEXT NOT NULL,
 stop_id INTEGER NOT NULL REFERENCES journal(id),
 state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','cancelled','started','unavailable')),
 retry_at INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(participation_id,run));
CREATE INDEX session_queue_cancellations_due ON session_queue_cancellations(retry_at) WHERE state='pending';
CREATE TABLE t3_connector(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), id TEXT NOT NULL, person_id TEXT NOT NULL,
 token_digest TEXT NOT NULL, port INTEGER NOT NULL);
CREATE TABLE t3_connected_environments(environment_id TEXT PRIMARY KEY);

CREATE TABLE channel_messages(
      message_id INTEGER PRIMARY KEY REFERENCES messages(id),
      event_key TEXT NOT NULL UNIQUE, scope_key TEXT NOT NULL, origin TEXT NOT NULL,
      sender_id TEXT NOT NULL, person_id TEXT NOT NULL);
CREATE INDEX channel_messages_scope ON channel_messages(scope_key,message_id DESC);
CREATE VIEW channel_tasks AS
      SELECT t.id AS task_id,c.message_id,c.scope_key FROM tasks t JOIN channel_messages c
      ON c.message_id=json_extract(t.data,'$.sourceMessageId');
CREATE TABLE channel_attachments(
      id INTEGER PRIMARY KEY, message_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      position INTEGER NOT NULL, name TEXT NOT NULL, mime_type TEXT NOT NULL, bytes BLOB NOT NULL,
      UNIQUE(message_id,position));
CREATE TABLE channel_deliveries(
      id TEXT PRIMARY KEY, source_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      reply_id INTEGER UNIQUE REFERENCES journal(id),
      operation TEXT NOT NULL, payload TEXT NOT NULL,
      state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','unknown','accepted','rejected','unsupported')),
      external_id TEXT NOT NULL DEFAULT '', next_attempt INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT '');
CREATE INDEX channel_delivery_due ON channel_deliveries(next_attempt) WHERE state IN ('pending','unknown');
CREATE VIEW channel_posts AS
      SELECT d.rowid AS sequence,d.reply_id,d.external_id,d.payload,c.scope_key,m.bot_id
      FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id
      JOIN messages m ON m.id=c.message_id
      WHERE d.state='accepted' AND d.external_id!=''
      AND d.operation IN ('reply','channel_send_attachment','publish_artifact');
CREATE TABLE channel_notices(
      source_id INTEGER NOT NULL REFERENCES channel_messages(message_id), reason TEXT NOT NULL,
      PRIMARY KEY(source_id,reason));
CREATE TABLE channel_artifacts(
      id TEXT PRIMARY KEY REFERENCES channel_deliveries(id),
      title TEXT NOT NULL, html TEXT NOT NULL);

CREATE TABLE local_machine_connections(
      id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id),
      environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, requested_run_id TEXT NOT NULL,
      machine_id TEXT NOT NULL, machine_owner TEXT NOT NULL, rules TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN ('pending','approved','declined','disconnected')),
      decided_by TEXT, decided_at INTEGER);
CREATE UNIQUE INDEX local_machine_open_request
      ON local_machine_connections(bot_id,task_id,environment_id,thread_id,machine_id)
      WHERE status IN ('pending','approved');

CREATE TABLE model_budgets(
  id TEXT PRIMARY KEY, call_limit INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
  CHECK(attempts>=0 AND attempts<=call_limit));
CREATE TABLE model_attempts(
  id INTEGER PRIMARY KEY, budget_id TEXT NOT NULL REFERENCES model_budgets(id),
  purpose TEXT NOT NULL, time INTEGER NOT NULL,
  message_id INTEGER REFERENCES messages(id), task_id INTEGER REFERENCES tasks(id),
  event_id INTEGER REFERENCES events(id), usage TEXT);

CREATE TABLE message_attachments(
 message_id INTEGER NOT NULL REFERENCES messages(id), attachment_id TEXT NOT NULL,
 metadata TEXT NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(message_id,attachment_id));

CREATE TABLE conversation_compactions(
        id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, start_message_id INTEGER NOT NULL REFERENCES journal(id),
        end_message_id INTEGER NOT NULL REFERENCES journal(id), summary TEXT NOT NULL, original_count INTEGER NOT NULL,
        created_at INTEGER NOT NULL, created_by TEXT NOT NULL);
CREATE INDEX compactions_bot ON conversation_compactions(bot_id,id DESC);
CREATE TABLE conversation_expansion_cursors(
        id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, message_id INTEGER NOT NULL REFERENCES journal(id),
        byte_offset INTEGER NOT NULL, redaction_revision TEXT NOT NULL, end_message_id INTEGER NOT NULL,
        query_message_id INTEGER NOT NULL, query_step INTEGER NOT NULL,
        FOREIGN KEY(query_message_id,query_step) REFERENCES conversation_steps(message_id,step),
        UNIQUE(bot_id,message_id,byte_offset,redaction_revision,end_message_id,query_message_id,query_step));
CREATE VIEW compactable_messages AS
        SELECT j.id,coalesce(j.conversation_bot_id,t.bot_id) bot_id,j.kind,j.task_id,j.data,
          CASE j.kind
            WHEN 'conversation_message' THEN j.id
            WHEN 'conversation_reply' THEN json_extract(j.data,'$.replyToMessageId')
            ELSE json_extract(t.data,'$.sourceMessageId')
          END source_message_id,
          CASE j.kind
            WHEN 'conversation_message' THEN m.text
            WHEN 'report' THEN json_extract(j.data,'$.report')
            ELSE json_extract(j.data,'$.text')
          END text
        FROM journal j LEFT JOIN messages m ON m.id=j.id LEFT JOIN tasks t ON t.id=j.task_id
        WHERE (j.conversation_bot_id IS NOT NULL AND j.kind IN ('conversation_message','conversation_reply'))
          OR (j.conversation_bot_id IS NULL AND (
            j.kind IN ('completed','cancelled','report')
            OR (j.kind='waiting' AND json_extract(j.data,'$.reason')='user_question')
            OR (j.kind='user_answer' AND json_extract(j.data,'$.conversation') IS NOT 1)));

CREATE TABLE workflows(
        id INTEGER PRIMARY KEY,bot_id TEXT NOT NULL,routing TEXT NOT NULL,request TEXT NOT NULL,
        message_id INTEGER NOT NULL REFERENCES messages,stop_requested INTEGER NOT NULL DEFAULT 0);
CREATE TABLE workflow_stages(workflow_id INTEGER NOT NULL REFERENCES workflows,
        stage_id TEXT NOT NULL,spec TEXT NOT NULL,task_id INTEGER UNIQUE REFERENCES tasks,result_id INTEGER REFERENCES journal,instruction TEXT,status TEXT NOT NULL DEFAULT 'pending',
        input TEXT NOT NULL DEFAULT '',output TEXT NOT NULL DEFAULT '',deadline INTEGER NOT NULL DEFAULT 0, alerted INTEGER NOT NULL DEFAULT 0,
        confirmed_state TEXT NOT NULL DEFAULT '',confirmed_at INTEGER NOT NULL DEFAULT 0,unreachable INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(workflow_id,stage_id));
CREATE TABLE workflow_dependencies(
        workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,parent_id TEXT NOT NULL,report_id INTEGER REFERENCES journal,
        artifact_version TEXT NOT NULL DEFAULT '',
        PRIMARY KEY(workflow_id,stage_id,parent_id),
        FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages,
        FOREIGN KEY(workflow_id,parent_id) REFERENCES workflow_stages);
CREATE VIEW outdated_workflow_stages AS
        WITH RECURSIVE outdated(workflow_id,stage_id) AS (
          SELECT d.workflow_id,d.stage_id FROM workflow_dependencies d JOIN workflow_stages p
            ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id
            WHERE d.report_id IS NOT NULL AND (p.status<>'completed' OR p.output<>d.artifact_version
              OR (p.result_id IS NOT d.report_id AND
                  (d.artifact_version='' OR json_extract(p.spec,'$.role') NOT IN ('implement','verify'))))
          UNION
          SELECT d.workflow_id,d.stage_id FROM workflow_dependencies d JOIN outdated p
            ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id)
        SELECT * FROM outdated;
CREATE TABLE workflow_artifacts(
        report_id INTEGER PRIMARY KEY REFERENCES journal,workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,
        version TEXT NOT NULL,FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages);
CREATE TABLE workflow_verdicts(
        report_id INTEGER PRIMARY KEY REFERENCES journal,workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,
        version TEXT NOT NULL,verdict TEXT NOT NULL CHECK(verdict IN ('pass','fail')),
        FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages);

CREATE TABLE memory_documents(
  doc_key TEXT PRIMARY KEY, scope_kind TEXT NOT NULL, owner_bot TEXT, doc_id TEXT NOT NULL,
  content TEXT NOT NULL, version INTEGER NOT NULL, updated_at INTEGER NOT NULL,
  CHECK((scope_kind='private' AND owner_bot IS NOT NULL) OR (scope_kind='shared' AND owner_bot IS NULL)));
CREATE TABLE memory_revisions(
  id INTEGER PRIMARY KEY, doc_key TEXT NOT NULL REFERENCES memory_documents(doc_key),
  version INTEGER NOT NULL, base_version INTEGER NOT NULL, author TEXT NOT NULL,
  diff_summary TEXT NOT NULL, previous_content TEXT NOT NULL, new_content TEXT NOT NULL,
  reverted_from INTEGER REFERENCES memory_revisions(id), created_at INTEGER NOT NULL,
  UNIQUE(doc_key,version));
CREATE INDEX memory_revisions_document ON memory_revisions(doc_key,id DESC);
CREATE TABLE memory_proposals(
  id INTEGER PRIMARY KEY, doc_key TEXT NOT NULL REFERENCES memory_documents(doc_key),
  base_version INTEGER NOT NULL, proposed_content TEXT NOT NULL, rationale TEXT NOT NULL,
  author TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('pending','applied','rejected')),
  created_at INTEGER NOT NULL, decided_by TEXT, decided_at INTEGER,
  diff_summary TEXT NOT NULL);
CREATE INDEX memory_proposals_document ON memory_proposals(doc_key,id DESC);

CREATE TABLE automations(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('schedule','reminder','trigger')),
  definition TEXT NOT NULL, routing TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN ('active','inactive','canceled','delivered','failed')),
  next_run_at INTEGER, created_by TEXT NOT NULL, created_at INTEGER NOT NULL, config_id TEXT, error TEXT,
  UNIQUE(bot_id,config_id));
CREATE INDEX automation_due ON automations(next_run_at) WHERE status='active' AND next_run_at IS NOT NULL;
CREATE INDEX automation_bot ON automations(bot_id,id DESC);
CREATE INDEX automation_active_bot ON automations(bot_id,id DESC) WHERE status='active';
CREATE INDEX automation_source ON automations(json_extract(definition,'$.source'))
  WHERE status='active' AND kind='trigger';
CREATE TABLE automation_firings(
  dedupe_key TEXT NOT NULL, bot_id TEXT NOT NULL, fired_at INTEGER NOT NULL, receipts TEXT NOT NULL,
  PRIMARY KEY(dedupe_key,bot_id));

CREATE TABLE git_pr_operations(
 operation_id TEXT PRIMARY KEY NOT NULL, host TEXT NOT NULL, base_repo TEXT NOT NULL,
 base_ref TEXT NOT NULL, head_repo TEXT NOT NULL, head_ref TEXT NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL, invocation TEXT NOT NULL,
 event_cursor INTEGER NOT NULL, status TEXT NOT NULL CHECK(status IN ('pending','created','ambiguous','failed','rejected')),
 pr_number INTEGER, url TEXT);
CREATE UNIQUE INDEX unresolved_git_pr_target
 ON git_pr_operations(host,base_repo,base_ref,head_repo,head_ref) WHERE status IN ('pending','ambiguous','rejected');
CREATE INDEX git_pr_history ON git_pr_operations(task_id,host,base_repo,base_ref,head_repo,head_ref);
CREATE TABLE git_pr_watches(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL,
 url TEXT NOT NULL, event_cursor INTEGER NOT NULL,
 PRIMARY KEY(host,repo,pr_number));
CREATE TABLE git_pr_heads(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL, event_cursor INTEGER NOT NULL,
 head_sha TEXT NOT NULL, synchronized INTEGER NOT NULL CHECK(synchronized IN (0,1)),
 PRIMARY KEY(host,repo,pr_number,event_cursor));
CREATE TABLE git_mutations(
 invocation TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id), dedupe TEXT, result TEXT,
 PRIMARY KEY(task_id,invocation), UNIQUE(task_id,dedupe));
CREATE TABLE git_task_reads(
 id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL REFERENCES tasks(id),
 invocation TEXT NOT NULL, tool TEXT NOT NULL, target TEXT NOT NULL, result TEXT NOT NULL,
 UNIQUE(task_id,invocation));
CREATE INDEX git_task_reads_by_task ON git_task_reads(task_id,id);
CREATE TABLE git_webhook_events(
 id INTEGER PRIMARY KEY, host TEXT NOT NULL, event_id TEXT NOT NULL, repo TEXT NOT NULL,
 data TEXT NOT NULL, payload_digest TEXT, UNIQUE(host,event_id), UNIQUE(host,payload_digest));
CREATE TABLE git_webhook_deliveries(
 event INTEGER NOT NULL REFERENCES git_webhook_events(id) ON DELETE CASCADE, pr_number INTEGER NOT NULL,
 PRIMARY KEY(event,pr_number));
CREATE INDEX git_events_repo ON git_webhook_events(host,repo);

CREATE TABLE pr_review_items(
    id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
    target_revision INTEGER NOT NULL,
    url TEXT NOT NULL, title TEXT NOT NULL, prompt TEXT NOT NULL,
    git_key TEXT, operation_id TEXT REFERENCES git_pr_operations(operation_id), candidates TEXT NOT NULL DEFAULT '[]',
    status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','changes_requested','linked','rejected','superseded')),
    UNIQUE(task_id,git_key), CHECK((git_key IS NULL) <> (operation_id IS NULL)));
CREATE UNIQUE INDEX pending_pr_reconciliation ON pr_review_items(operation_id) WHERE status='pending';
CREATE INDEX pr_reviews_by_task ON pr_review_items(task_id,id);
CREATE UNIQUE INDEX journal_pr_review_anchor ON journal(task_id,json_extract(data,'$.itemId'))
    WHERE kind='pr_review_requested';
CREATE TABLE inbox_decisions(
    id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, item_kind TEXT NOT NULL, item_id INTEGER NOT NULL,
    wait_id INTEGER NOT NULL, task_id INTEGER REFERENCES tasks(id),
    target_revision INTEGER NOT NULL, target_generation INTEGER NOT NULL,
    decided_by TEXT NOT NULL, decided_at INTEGER NOT NULL, decision TEXT NOT NULL,
    UNIQUE(bot_id,item_kind,item_id,wait_id));
CREATE INDEX inbox_decisions_by_bot ON inbox_decisions(bot_id,id);

CREATE VIEW automation_before_dispatch AS
SELECT task.id FROM tasks task
WHERE task.project_id<>'' AND json_type(task.data,'$.trigger')='object'
  AND json_extract(task.data,'$.activeRun') IS NULL AND json_extract(task.data,'$.lastRun') IS NULL
  AND json_type(task.data,'$.runTargets') IS NULL AND json_type(task.data,'$.runResults') IS NULL
  AND NOT EXISTS (SELECT 1 FROM outbox WHERE task_id=task.id);
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

CREATE TABLE session_message_sends(
  action TEXT PRIMARY KEY,
  message_id INTEGER NOT NULL REFERENCES messages(id),
  environment TEXT NOT NULL, thread TEXT NOT NULL, gateway_environment TEXT NOT NULL,
  instruction TEXT NOT NULL, interrupt_at INTEGER, step INTEGER);
CREATE INDEX session_message_sends_by_message ON session_message_sends(message_id);
CREATE INDEX session_message_sends_due ON session_message_sends(interrupt_at) WHERE interrupt_at IS NOT NULL;
CREATE TABLE bot_pauses(bot TEXT PRIMARY KEY);

CREATE TABLE git_review_receipts(
  inbox_key TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id),
  host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
  reviewer TEXT NOT NULL, after_event INTEGER NOT NULL,
  PRIMARY KEY(inbox_key,reviewer));
CREATE INDEX git_review_receipts_target ON git_review_receipts(task_id,host,repo,pr_number,reviewer,after_event);
CREATE TABLE git_pr_watch_origins(
  host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL, event_cursor INTEGER NOT NULL,
  PRIMARY KEY(host,repo,pr_number),
  FOREIGN KEY(host,repo,pr_number) REFERENCES git_pr_watches(host,repo,pr_number) ON DELETE CASCADE);

CREATE TABLE deleted_bots(bot TEXT PRIMARY KEY);

CREATE TABLE external_mcp_connections(
 id TEXT PRIMARY KEY, person_id TEXT NOT NULL, token_digest TEXT UNIQUE
);
CREATE TABLE external_mcp_allowed_bots(
 connection_id TEXT NOT NULL REFERENCES external_mcp_connections(id), bot_id TEXT NOT NULL,
 PRIMARY KEY(connection_id,bot_id)
);
CREATE TABLE external_mcp_receipts(
 connection_id TEXT NOT NULL REFERENCES external_mcp_connections(id), request_id TEXT NOT NULL,
 request_digest TEXT NOT NULL, task_id INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
 PRIMARY KEY(connection_id,request_id)
);

CREATE TABLE managed_web_users(
 person TEXT PRIMARY KEY, verifier TEXT NOT NULL,
 admin INTEGER NOT NULL CHECK(admin IN (0,1)), revoked INTEGER NOT NULL CHECK(revoked IN (0,1))
) WITHOUT ROWID;
CREATE TABLE managed_web_pending(
 person TEXT PRIMARY KEY, verifier TEXT NOT NULL, expires_at INTEGER NOT NULL
) WITHOUT ROWID;
)SQL";
    if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) throw StorageError(db);
}
}

void Store::Impl::openSchema() {
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA synchronous=FULL");
    const auto format = [&] {
        auto row = query("SELECT application_id,user_version FROM pragma_application_id(),pragma_user_version()");
        row.next();
        return std::pair{row.number(0), row.number(1)};
    };
    constexpr std::pair current{applicationId, schemaVersion};
    if (format() != current) {
        Transaction initialize(db);
        const auto existing = format();
        if (existing != current) {
            if (existing != std::pair<Id, Id>{} || query("SELECT 1 FROM sqlite_schema LIMIT 1").next())
                throw Conflict("Cannot open this database (schema version " + std::to_string(existing.second) +
                               "). This build requires a Flock database at schema version " + std::to_string(schemaVersion) +
                               ". Use --home with a new state directory.");
            applySchema(db);
            exec(("PRAGMA application_id=" + std::to_string(applicationId)).c_str());
            exec(("PRAGMA user_version=" + std::to_string(schemaVersion)).c_str());
        }
        initialize.commit();
    }
    exec("PRAGMA journal_mode=WAL");
    // Open the WAL now so a read-only peer can use a freshly initialized database.
    exec("PRAGMA user_version");
}
}
