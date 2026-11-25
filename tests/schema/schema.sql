VALUES
    ('index', 'automation_active_bot', 'automations', 'CREATE INDEX automation_active_bot ON automations(bot_id,id DESC) WHERE status=''active'''),
    ('index', 'automation_bot', 'automations', 'CREATE INDEX automation_bot ON automations(bot_id,id DESC)'),
    ('index', 'automation_due', 'automations', 'CREATE INDEX automation_due ON automations(next_run_at) WHERE status=''active'' AND next_run_at IS NOT NULL'),
    ('index', 'automation_source', 'automations', 'CREATE INDEX automation_source ON automations(json_extract(definition,''$.source''))
  WHERE status=''active'' AND kind=''trigger'''),
    ('index', 'channel_delivery_due', 'channel_deliveries', 'CREATE INDEX channel_delivery_due ON channel_deliveries(next_attempt) WHERE state IN (''pending'',''unknown'')'),
    ('index', 'channel_messages_scope', 'channel_messages', 'CREATE INDEX channel_messages_scope ON channel_messages(scope_key,message_id DESC)'),
    ('index', 'compactions_bot', 'conversation_compactions', 'CREATE INDEX compactions_bot ON conversation_compactions(bot_id,id DESC)'),
    ('index', 'conversation_active_tasks', 'tasks', 'CREATE INDEX conversation_active_tasks ON tasks(bot_id,id DESC)
  WHERE state NOT IN (''completed'',''cancelled'')'),
    ('index', 'conversation_model_repairs', 'conversation_model_failures', 'CREATE UNIQUE INDEX conversation_model_repairs ON conversation_model_failures(message_id) WHERE repair_attempt_id IS NOT NULL'),
    ('index', 'conversation_terminal_tasks', 'tasks', 'CREATE INDEX conversation_terminal_tasks ON tasks(bot_id,id DESC)
  WHERE state IN (''completed'',''cancelled'')'),
    ('index', 'conversation_turns_running', 'conversation_turns', 'CREATE INDEX conversation_turns_running ON conversation_turns(message_id) WHERE status=''running'''),
    ('index', 'fulfilled_secret', 'protected_inputs', 'CREATE UNIQUE INDEX fulfilled_secret ON protected_inputs(name) WHERE status=''fulfilled'''),
    ('index', 'git_events_repo', 'git_webhook_events', 'CREATE INDEX git_events_repo ON git_webhook_events(host,repo)'),
    ('index', 'git_pr_history', 'git_pr_operations', 'CREATE INDEX git_pr_history ON git_pr_operations(task_id,host,base_repo,base_ref,head_repo,head_ref)'),
    ('index', 'git_review_receipts_target', 'git_review_receipts', 'CREATE INDEX git_review_receipts_target ON git_review_receipts(task_id,host,repo,pr_number,reviewer,after_event)'),
    ('index', 'git_task_reads_by_task', 'git_task_reads', 'CREATE INDEX git_task_reads_by_task ON git_task_reads(task_id,id)'),
    ('index', 'inbox_decisions_by_bot', 'inbox_decisions', 'CREATE INDEX inbox_decisions_by_bot ON inbox_decisions(bot_id,id)'),
    ('index', 'journal_bot', 'journal', 'CREATE INDEX journal_bot ON journal(conversation_bot_id,id) WHERE conversation_bot_id IS NOT NULL'),
    ('index', 'journal_instruction_message', 'journal', 'CREATE INDEX journal_instruction_message ON journal(
  json_extract(data,''$.messageId''),task_id,json_extract(data,''$.step''))
  WHERE kind IN (''instruction_accepted'',''instruction_delivered'')'),
    ('index', 'journal_peer_message', 'journal', 'CREATE INDEX journal_peer_message ON journal(task_id,json_extract(data,''$.exchangeId''),id) WHERE kind=''bot_message'''),
    ('index', 'journal_pr_review_anchor', 'journal', 'CREATE UNIQUE INDEX journal_pr_review_anchor ON journal(task_id,json_extract(data,''$.itemId''))
    WHERE kind=''pr_review_requested'''),
    ('index', 'journal_session_changes', 'journal', 'CREATE INDEX journal_session_changes ON journal(id,task_id) WHERE kind IN (''run_started'',''report'')'),
    ('index', 'journal_task', 'journal', 'CREATE INDEX journal_task ON journal(task_id,id)'),
    ('index', 'journal_task_results', 'journal', 'CREATE INDEX journal_task_results ON journal(task_id,id DESC)
  WHERE kind IN (''completed'',''report'',''cancelled'')'),
    ('index', 'journal_wait_anchor', 'journal', 'CREATE INDEX journal_wait_anchor ON journal(task_id,json_extract(data,''$.id''),id)
  WHERE kind=''waiting'' AND conversation_bot_id IS NULL'),
    ('index', 'local_machine_open_request', 'local_machine_connections', 'CREATE UNIQUE INDEX local_machine_open_request
      ON local_machine_connections(bot_id,task_id,environment_id,thread_id,machine_id)
      WHERE status IN (''pending'',''approved'')'),
    ('index', 'memory_proposals_document', 'memory_proposals', 'CREATE INDEX memory_proposals_document ON memory_proposals(doc_key,id DESC)'),
    ('index', 'memory_revisions_document', 'memory_revisions', 'CREATE INDEX memory_revisions_document ON memory_revisions(doc_key,id DESC)'),
    ('index', 'outbox_task', 'outbox', 'CREATE INDEX outbox_task ON outbox(task_id)'),
    ('index', 'peer_sender_open', 'peer_exchanges', 'CREATE UNIQUE INDEX peer_sender_open ON peer_exchanges(sender_task) WHERE phase<>''closed'''),
    ('index', 'pending_pr_reconciliation', 'pr_review_items', 'CREATE UNIQUE INDEX pending_pr_reconciliation ON pr_review_items(operation_id) WHERE status=''pending'''),
    ('index', 'pending_secret', 'protected_inputs', 'CREATE UNIQUE INDEX pending_secret ON protected_inputs(bot,name) WHERE status=''pending'''),
    ('index', 'policy_denied', 'policy_approvals', 'CREATE INDEX policy_denied ON policy_approvals(bot,scope,action,target,status)'),
    ('index', 'policy_pending', 'policy_approvals', 'CREATE INDEX policy_pending ON policy_approvals(status,bot)'),
    ('index', 'pr_reviews_by_task', 'pr_review_items', 'CREATE INDEX pr_reviews_by_task ON pr_review_items(task_id,id)'),
    ('index', 'runs_task', 'runs', 'CREATE INDEX runs_task ON runs(task_id,action_id)'),
    ('index', 'session_invitations_inviter', 'session_invitations', 'CREATE INDEX session_invitations_inviter ON session_invitations(inviter_participation_id)'),
    ('index', 'session_message_interrupts_due', 'session_message_waits', 'CREATE INDEX session_message_interrupts_due ON session_message_waits(interrupt_at) WHERE interrupt_at IS NOT NULL'),
    ('index', 'session_message_missing_targets', 'session_message_waits', 'CREATE INDEX session_message_missing_targets ON session_message_waits(task_id,action_id) WHERE session_missing=1'),
    ('index', 'session_message_sends_by_message', 'session_message_sends', 'CREATE INDEX session_message_sends_by_message ON session_message_sends(message_id)'),
    ('index', 'session_message_sends_due', 'session_message_sends', 'CREATE INDEX session_message_sends_due ON session_message_sends(interrupt_at) WHERE interrupt_at IS NOT NULL'),
    ('index', 'session_message_waits_participation', 'session_message_waits', 'CREATE INDEX session_message_waits_participation ON session_message_waits(bot,environment,thread)'),
    ('index', 'session_queue_cancellations_due', 'session_queue_cancellations', 'CREATE INDEX session_queue_cancellations_due ON session_queue_cancellations(retry_at) WHERE state=''pending'''),
    ('index', 'sqlite_autoindex_automation_firings_1', 'automation_firings', NULL),
    ('index', 'sqlite_autoindex_automations_1', 'automations', NULL),
    ('index', 'sqlite_autoindex_bot_changes_1', 'bot_changes', NULL),
    ('index', 'sqlite_autoindex_bot_creation_intents_1', 'bot_creation_intents', NULL),
    ('index', 'sqlite_autoindex_bot_creation_intents_2', 'bot_creation_intents', NULL),
    ('index', 'sqlite_autoindex_bot_members_1', 'bot_members', NULL),
    ('index', 'sqlite_autoindex_bot_owners_1', 'bot_owners', NULL),
    ('index', 'sqlite_autoindex_bot_pauses_1', 'bot_pauses', NULL),
    ('index', 'sqlite_autoindex_bot_t3_projects_1', 'bot_t3_projects', NULL),
    ('index', 'sqlite_autoindex_channel_artifacts_1', 'channel_artifacts', NULL),
    ('index', 'sqlite_autoindex_channel_attachments_1', 'channel_attachments', NULL),
    ('index', 'sqlite_autoindex_channel_deliveries_1', 'channel_deliveries', NULL),
    ('index', 'sqlite_autoindex_channel_deliveries_2', 'channel_deliveries', NULL),
    ('index', 'sqlite_autoindex_channel_messages_1', 'channel_messages', NULL),
    ('index', 'sqlite_autoindex_channel_notices_1', 'channel_notices', NULL),
    ('index', 'sqlite_autoindex_conversation_expansion_cursors_1', 'conversation_expansion_cursors', NULL),
    ('index', 'sqlite_autoindex_conversation_model_failures_1', 'conversation_model_failures', NULL),
    ('index', 'sqlite_autoindex_conversation_steps_1', 'conversation_steps', NULL),
    ('index', 'sqlite_autoindex_conversation_turns_1', 'conversation_turns', NULL),
    ('index', 'sqlite_autoindex_conversation_turns_2', 'conversation_turns', NULL),
    ('index', 'sqlite_autoindex_deleted_bots_1', 'deleted_bots', NULL),
    ('index', 'sqlite_autoindex_environment_settings_1', 'environment_settings', NULL),
    ('index', 'sqlite_autoindex_events_1', 'events', NULL),
    ('index', 'sqlite_autoindex_external_mcp_allowed_bots_1', 'external_mcp_allowed_bots', NULL),
    ('index', 'sqlite_autoindex_external_mcp_connections_1', 'external_mcp_connections', NULL),
    ('index', 'sqlite_autoindex_external_mcp_connections_2', 'external_mcp_connections', NULL),
    ('index', 'sqlite_autoindex_external_mcp_receipts_1', 'external_mcp_receipts', NULL),
    ('index', 'sqlite_autoindex_external_mcp_receipts_2', 'external_mcp_receipts', NULL),
    ('index', 'sqlite_autoindex_git_mutations_1', 'git_mutations', NULL),
    ('index', 'sqlite_autoindex_git_mutations_2', 'git_mutations', NULL),
    ('index', 'sqlite_autoindex_git_pr_heads_1', 'git_pr_heads', NULL),
    ('index', 'sqlite_autoindex_git_pr_operations_1', 'git_pr_operations', NULL),
    ('index', 'sqlite_autoindex_git_pr_watch_origins_1', 'git_pr_watch_origins', NULL),
    ('index', 'sqlite_autoindex_git_pr_watches_1', 'git_pr_watches', NULL),
    ('index', 'sqlite_autoindex_git_review_receipts_1', 'git_review_receipts', NULL),
    ('index', 'sqlite_autoindex_git_task_reads_1', 'git_task_reads', NULL),
    ('index', 'sqlite_autoindex_git_webhook_deliveries_1', 'git_webhook_deliveries', NULL),
    ('index', 'sqlite_autoindex_git_webhook_events_1', 'git_webhook_events', NULL),
    ('index', 'sqlite_autoindex_git_webhook_events_2', 'git_webhook_events', NULL),
    ('index', 'sqlite_autoindex_inbox_decisions_1', 'inbox_decisions', NULL),
    ('index', 'sqlite_autoindex_memory_documents_1', 'memory_documents', NULL),
    ('index', 'sqlite_autoindex_memory_revisions_1', 'memory_revisions', NULL),
    ('index', 'sqlite_autoindex_message_attachments_1', 'message_attachments', NULL),
    ('index', 'sqlite_autoindex_messages_1', 'messages', NULL),
    ('index', 'sqlite_autoindex_messages_2', 'messages', NULL),
    ('index', 'sqlite_autoindex_model_budgets_1', 'model_budgets', NULL),
    ('index', 'sqlite_autoindex_outbox_1', 'outbox', NULL),
    ('index', 'sqlite_autoindex_peer_exchanges_1', 'peer_exchanges', NULL),
    ('index', 'sqlite_autoindex_peer_exchanges_2', 'peer_exchanges', NULL),
    ('index', 'sqlite_autoindex_policy_approvals_1', 'policy_approvals', NULL),
    ('index', 'sqlite_autoindex_policy_rules_1', 'policy_rules', NULL),
    ('index', 'sqlite_autoindex_pr_review_items_1', 'pr_review_items', NULL),
    ('index', 'sqlite_autoindex_protected_inputs_1', 'protected_inputs', NULL),
    ('index', 'sqlite_autoindex_runs_1', 'runs', NULL),
    ('index', 'sqlite_autoindex_runs_2', 'runs', NULL),
    ('index', 'sqlite_autoindex_secret_destinations_1', 'secret_destinations', NULL),
    ('index', 'sqlite_autoindex_session_asks_1', 'session_asks', NULL),
    ('index', 'sqlite_autoindex_session_asks_2', 'session_asks', NULL),
    ('index', 'sqlite_autoindex_session_invitations_1', 'session_invitations', NULL),
    ('index', 'sqlite_autoindex_session_invitations_2', 'session_invitations', NULL),
    ('index', 'sqlite_autoindex_session_message_sends_1', 'session_message_sends', NULL),
    ('index', 'sqlite_autoindex_session_message_waits_1', 'session_message_waits', NULL),
    ('index', 'sqlite_autoindex_session_participations_1', 'session_participations', NULL),
    ('index', 'sqlite_autoindex_session_queue_cancellations_1', 'session_queue_cancellations', NULL),
    ('index', 'sqlite_autoindex_session_read_refs_1', 'session_read_refs', NULL),
    ('index', 'sqlite_autoindex_session_read_refs_2', 'session_read_refs', NULL),
    ('index', 'sqlite_autoindex_t3_connected_environments_1', 't3_connected_environments', NULL),
    ('index', 'sqlite_autoindex_t3_events_1', 't3_events', NULL),
    ('index', 'sqlite_autoindex_task_revisions_1', 'task_revisions', NULL),
    ('index', 'sqlite_autoindex_task_run_destinations_1', 'task_run_destinations', NULL),
    ('index', 'sqlite_autoindex_workflow_dependencies_1', 'workflow_dependencies', NULL),
    ('index', 'sqlite_autoindex_workflow_stages_1', 'workflow_stages', NULL),
    ('index', 'sqlite_autoindex_workflow_stages_2', 'workflow_stages', NULL),
    ('index', 'tasks_bot', 'tasks', 'CREATE INDEX tasks_bot ON tasks(bot_id,id DESC)'),
    ('index', 'tasks_route', 'tasks', 'CREATE INDEX tasks_route ON tasks(environment_id,project_id,id)
  WHERE state NOT IN (''completed'',''cancelled'')'),
    ('index', 'unresolved_git_pr_target', 'git_pr_operations', 'CREATE UNIQUE INDEX unresolved_git_pr_target
 ON git_pr_operations(host,base_repo,base_ref,head_repo,head_ref) WHERE status IN (''pending'',''ambiguous'',''rejected'')'),
    ('index', 'waits_open', 'waits', 'CREATE UNIQUE INDEX waits_open ON waits(task_id) WHERE resolution IS NULL'),
    ('table', 'automation_firings', 'automation_firings', 'CREATE TABLE automation_firings(
  dedupe_key TEXT NOT NULL, bot_id TEXT NOT NULL, fired_at INTEGER NOT NULL, receipts TEXT NOT NULL,
  PRIMARY KEY(dedupe_key,bot_id))'),
    ('table', 'automations', 'automations', 'CREATE TABLE automations(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN (''schedule'',''reminder'',''trigger'')),
  definition TEXT NOT NULL, routing TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN (''active'',''inactive'',''canceled'',''delivered'',''failed'')),
  next_run_at INTEGER, created_by TEXT NOT NULL, created_at INTEGER NOT NULL, config_id TEXT, error TEXT,
  UNIQUE(bot_id,config_id))'),
    ('table', 'bot_changes', 'bot_changes', 'CREATE TABLE bot_changes(id INTEGER PRIMARY KEY,bot TEXT,operation TEXT,target TEXT,name TEXT,
 request_key TEXT UNIQUE NOT NULL,source_message_id INTEGER REFERENCES messages(id))'),
    ('table', 'bot_creation_intents', 'bot_creation_intents', 'CREATE TABLE bot_creation_intents(
 request_key TEXT PRIMARY KEY,target TEXT UNIQUE NOT NULL,person TEXT NOT NULL)'),
    ('table', 'bot_members', 'bot_members', 'CREATE TABLE bot_members(bot TEXT NOT NULL,person TEXT NOT NULL,PRIMARY KEY(bot,person))'),
    ('table', 'bot_owners', 'bot_owners', 'CREATE TABLE bot_owners(bot TEXT PRIMARY KEY,person TEXT NOT NULL)'),
    ('table', 'bot_pauses', 'bot_pauses', 'CREATE TABLE bot_pauses(bot TEXT PRIMARY KEY)'),
    ('table', 'bot_t3_projects', 'bot_t3_projects', 'CREATE TABLE bot_t3_projects(
  bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, project_id TEXT NOT NULL,
  PRIMARY KEY(bot_id,environment_id))'),
    ('table', 'channel_artifacts', 'channel_artifacts', 'CREATE TABLE channel_artifacts(
      id TEXT PRIMARY KEY REFERENCES channel_deliveries(id),
      title TEXT NOT NULL, html TEXT NOT NULL)'),
    ('table', 'channel_attachments', 'channel_attachments', 'CREATE TABLE channel_attachments(
      id INTEGER PRIMARY KEY, message_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      position INTEGER NOT NULL, name TEXT NOT NULL, mime_type TEXT NOT NULL, bytes BLOB NOT NULL,
      UNIQUE(message_id,position))'),
    ('table', 'channel_deliveries', 'channel_deliveries', 'CREATE TABLE channel_deliveries(
      id TEXT PRIMARY KEY, source_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      reply_id INTEGER UNIQUE REFERENCES journal(id),
      operation TEXT NOT NULL, payload TEXT NOT NULL,
      state TEXT NOT NULL DEFAULT ''pending'' CHECK(state IN (''pending'',''unknown'',''accepted'',''rejected'',''unsupported'')),
      external_id TEXT NOT NULL DEFAULT '''', next_attempt INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT '''')'),
    ('table', 'channel_messages', 'channel_messages', 'CREATE TABLE channel_messages(
      message_id INTEGER PRIMARY KEY REFERENCES messages(id),
      event_key TEXT NOT NULL UNIQUE, scope_key TEXT NOT NULL, origin TEXT NOT NULL,
      sender_id TEXT NOT NULL, person_id TEXT NOT NULL)'),
    ('table', 'channel_notices', 'channel_notices', 'CREATE TABLE channel_notices(
      source_id INTEGER NOT NULL REFERENCES channel_messages(message_id), reason TEXT NOT NULL,
      PRIMARY KEY(source_id,reason))'),
    ('table', 'conversation_compactions', 'conversation_compactions', 'CREATE TABLE conversation_compactions(
        id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, start_message_id INTEGER NOT NULL REFERENCES journal(id),
        end_message_id INTEGER NOT NULL REFERENCES journal(id), summary TEXT NOT NULL, original_count INTEGER NOT NULL,
        created_at INTEGER NOT NULL, created_by TEXT NOT NULL)'),
    ('table', 'conversation_expansion_cursors', 'conversation_expansion_cursors', 'CREATE TABLE conversation_expansion_cursors(
        id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, message_id INTEGER NOT NULL REFERENCES journal(id),
        byte_offset INTEGER NOT NULL, redaction_revision TEXT NOT NULL, end_message_id INTEGER NOT NULL,
        query_message_id INTEGER NOT NULL, query_step INTEGER NOT NULL,
        FOREIGN KEY(query_message_id,query_step) REFERENCES conversation_steps(message_id,step),
        UNIQUE(bot_id,message_id,byte_offset,redaction_revision,end_message_id,query_message_id,query_step))'),
    ('table', 'conversation_model_failures', 'conversation_model_failures', 'CREATE TABLE conversation_model_failures(
  attempt_id INTEGER PRIMARY KEY REFERENCES model_attempts(id),
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id),
  step INTEGER NOT NULL,
  reason TEXT NOT NULL,
  repair_attempt_id INTEGER UNIQUE REFERENCES model_attempts(id) ON DELETE SET NULL)'),
    ('table', 'conversation_retries', 'conversation_retries', 'CREATE TABLE conversation_retries(
  message_id INTEGER PRIMARY KEY REFERENCES conversation_turns(message_id),
  failures INTEGER NOT NULL CHECK(failures BETWEEN 1 AND 7),
  retry_at INTEGER NOT NULL)'),
    ('table', 'conversation_steps', 'conversation_steps', 'CREATE TABLE conversation_steps(
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id), step INTEGER NOT NULL,
  attempt_id INTEGER, decision TEXT CHECK(json_type(decision)=''array''), effect TEXT,
  result TEXT CHECK(json_type(result)=''array''),
  PRIMARY KEY(message_id,step))'),
    ('table', 'conversation_turns', 'conversation_turns', 'CREATE TABLE conversation_turns(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id),
  status TEXT NOT NULL CHECK(status IN (''running'',''paused'',''policy_waiting'',''done'')),
  pause_id INTEGER UNIQUE REFERENCES journal(id), reserved_attempt INTEGER,
  reply_id INTEGER UNIQUE REFERENCES journal(id))'),
    ('table', 'deleted_bots', 'deleted_bots', 'CREATE TABLE deleted_bots(bot TEXT PRIMARY KEY)'),
    ('table', 'environment_settings', 'environment_settings', 'CREATE TABLE environment_settings(bot TEXT,environment TEXT,timeout INTEGER,PRIMARY KEY(bot,environment))'),
    ('table', 'events', 'events', 'CREATE TABLE events(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  dedupe TEXT NOT NULL, consumed INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
  retry_at INTEGER NOT NULL DEFAULT 0, data TEXT NOT NULL, evidence TEXT NOT NULL DEFAULT ''[]'',
  reserved_attempt INTEGER REFERENCES model_attempts(id),
  UNIQUE(task_id,dedupe))'),
    ('table', 'external_mcp_allowed_bots', 'external_mcp_allowed_bots', 'CREATE TABLE external_mcp_allowed_bots(
 connection_id TEXT NOT NULL REFERENCES external_mcp_connections(id), bot_id TEXT NOT NULL,
 PRIMARY KEY(connection_id,bot_id)
)'),
    ('table', 'external_mcp_connections', 'external_mcp_connections', 'CREATE TABLE external_mcp_connections(
 id TEXT PRIMARY KEY, person_id TEXT NOT NULL, token_digest TEXT UNIQUE
)'),
    ('table', 'external_mcp_receipts', 'external_mcp_receipts', 'CREATE TABLE external_mcp_receipts(
 connection_id TEXT NOT NULL REFERENCES external_mcp_connections(id), request_id TEXT NOT NULL,
 request_digest TEXT NOT NULL, task_id INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
 PRIMARY KEY(connection_id,request_id)
)'),
    ('table', 'git_mutations', 'git_mutations', 'CREATE TABLE git_mutations(
 invocation TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id), dedupe TEXT, result TEXT,
 PRIMARY KEY(task_id,invocation), UNIQUE(task_id,dedupe))'),
    ('table', 'git_pr_heads', 'git_pr_heads', 'CREATE TABLE git_pr_heads(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL, event_cursor INTEGER NOT NULL,
 head_sha TEXT NOT NULL, synchronized INTEGER NOT NULL CHECK(synchronized IN (0,1)),
 PRIMARY KEY(host,repo,pr_number,event_cursor))'),
    ('table', 'git_pr_operations', 'git_pr_operations', 'CREATE TABLE git_pr_operations(
 operation_id TEXT PRIMARY KEY NOT NULL, host TEXT NOT NULL, base_repo TEXT NOT NULL,
 base_ref TEXT NOT NULL, head_repo TEXT NOT NULL, head_ref TEXT NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL, invocation TEXT NOT NULL,
 event_cursor INTEGER NOT NULL, status TEXT NOT NULL CHECK(status IN (''pending'',''created'',''ambiguous'',''failed'',''rejected'')),
 pr_number INTEGER, url TEXT)'),
    ('table', 'git_pr_watch_origins', 'git_pr_watch_origins', 'CREATE TABLE git_pr_watch_origins(
  host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL, event_cursor INTEGER NOT NULL,
  PRIMARY KEY(host,repo,pr_number),
  FOREIGN KEY(host,repo,pr_number) REFERENCES git_pr_watches(host,repo,pr_number) ON DELETE CASCADE)'),
    ('table', 'git_pr_watches', 'git_pr_watches', 'CREATE TABLE git_pr_watches(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL,
 url TEXT NOT NULL, event_cursor INTEGER NOT NULL,
 PRIMARY KEY(host,repo,pr_number))'),
    ('table', 'git_review_receipts', 'git_review_receipts', 'CREATE TABLE git_review_receipts(
  inbox_key TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id),
  host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
  reviewer TEXT NOT NULL, after_event INTEGER NOT NULL,
  PRIMARY KEY(inbox_key,reviewer))'),
    ('table', 'git_task_reads', 'git_task_reads', 'CREATE TABLE git_task_reads(
 id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL REFERENCES tasks(id),
 invocation TEXT NOT NULL, tool TEXT NOT NULL, target TEXT NOT NULL, result TEXT NOT NULL,
 UNIQUE(task_id,invocation))'),
    ('table', 'git_webhook_deliveries', 'git_webhook_deliveries', 'CREATE TABLE git_webhook_deliveries(
 event INTEGER NOT NULL REFERENCES git_webhook_events(id) ON DELETE CASCADE, pr_number INTEGER NOT NULL,
 PRIMARY KEY(event,pr_number))'),
    ('table', 'git_webhook_events', 'git_webhook_events', 'CREATE TABLE git_webhook_events(
 id INTEGER PRIMARY KEY, host TEXT NOT NULL, event_id TEXT NOT NULL, repo TEXT NOT NULL,
 data TEXT NOT NULL, payload_digest TEXT, UNIQUE(host,event_id), UNIQUE(host,payload_digest))'),
    ('table', 'inbox_decisions', 'inbox_decisions', 'CREATE TABLE inbox_decisions(
    id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, item_kind TEXT NOT NULL, item_id INTEGER NOT NULL,
    wait_id INTEGER NOT NULL, task_id INTEGER REFERENCES tasks(id),
    target_revision INTEGER NOT NULL, target_generation INTEGER NOT NULL,
    decided_by TEXT NOT NULL, decided_at INTEGER NOT NULL, decision TEXT NOT NULL,
    UNIQUE(bot_id,item_kind,item_id,wait_id))'),
    ('table', 'journal', 'journal', 'CREATE TABLE journal(
  id INTEGER PRIMARY KEY, task_id INTEGER REFERENCES tasks(id),
  time INTEGER NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL, conversation_bot_id TEXT,
  CHECK(task_id IS NOT NULL OR conversation_bot_id IS NOT NULL))'),
    ('table', 'local_machine_connections', 'local_machine_connections', 'CREATE TABLE local_machine_connections(
      id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id),
      environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, requested_run_id TEXT NOT NULL,
      machine_id TEXT NOT NULL, machine_owner TEXT NOT NULL, rules TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN (''pending'',''approved'',''declined'',''disconnected'')),
      decided_by TEXT, decided_at INTEGER)'),
    ('table', 'managed_web_pending', 'managed_web_pending', 'CREATE TABLE managed_web_pending(
 person TEXT PRIMARY KEY, verifier TEXT NOT NULL, expires_at INTEGER NOT NULL
) WITHOUT ROWID'),
    ('table', 'managed_web_users', 'managed_web_users', 'CREATE TABLE managed_web_users(
 person TEXT PRIMARY KEY, verifier TEXT NOT NULL,
 admin INTEGER NOT NULL CHECK(admin IN (0,1)), revoked INTEGER NOT NULL CHECK(revoked IN (0,1))
) WITHOUT ROWID'),
    ('table', 'memory_documents', 'memory_documents', 'CREATE TABLE memory_documents(
  doc_key TEXT PRIMARY KEY, scope_kind TEXT NOT NULL, owner_bot TEXT, doc_id TEXT NOT NULL,
  content TEXT NOT NULL, version INTEGER NOT NULL, updated_at INTEGER NOT NULL,
  CHECK((scope_kind=''private'' AND owner_bot IS NOT NULL) OR (scope_kind=''shared'' AND owner_bot IS NULL)))'),
    ('table', 'memory_proposals', 'memory_proposals', 'CREATE TABLE memory_proposals(
  id INTEGER PRIMARY KEY, doc_key TEXT NOT NULL REFERENCES memory_documents(doc_key),
  base_version INTEGER NOT NULL, proposed_content TEXT NOT NULL, rationale TEXT NOT NULL,
  author TEXT NOT NULL, status TEXT NOT NULL CHECK(status IN (''pending'',''applied'',''rejected'')),
  created_at INTEGER NOT NULL, decided_by TEXT, decided_at INTEGER,
  diff_summary TEXT NOT NULL)'),
    ('table', 'memory_revisions', 'memory_revisions', 'CREATE TABLE memory_revisions(
  id INTEGER PRIMARY KEY, doc_key TEXT NOT NULL REFERENCES memory_documents(doc_key),
  version INTEGER NOT NULL, base_version INTEGER NOT NULL, author TEXT NOT NULL,
  diff_summary TEXT NOT NULL, previous_content TEXT NOT NULL, new_content TEXT NOT NULL,
  reverted_from INTEGER REFERENCES memory_revisions(id), created_at INTEGER NOT NULL,
  UNIQUE(doc_key,version))'),
    ('table', 'message_attachments', 'message_attachments', 'CREATE TABLE message_attachments(
 message_id INTEGER NOT NULL REFERENCES messages(id), attachment_id TEXT NOT NULL,
 metadata TEXT NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(message_id,attachment_id))'),
    ('table', 'messages', 'messages', 'CREATE TABLE messages(
  id INTEGER PRIMARY KEY REFERENCES journal(id), bot_id TEXT NOT NULL,
  client_message_id TEXT NOT NULL, sequence INTEGER NOT NULL,
  reply_to INTEGER, text TEXT NOT NULL, routing TEXT NOT NULL,
  person_id TEXT NOT NULL, person_root INTEGER NOT NULL CHECK(person_root IN (0,1)),
  principal_source INTEGER NOT NULL CHECK(principal_source IN (0,1,2,3,4)),
  UNIQUE(bot_id,person_id,client_message_id), UNIQUE(bot_id,sequence))'),
    ('table', 'model_attempts', 'model_attempts', 'CREATE TABLE model_attempts(
  id INTEGER PRIMARY KEY, budget_id TEXT NOT NULL REFERENCES model_budgets(id),
  purpose TEXT NOT NULL, time INTEGER NOT NULL,
  message_id INTEGER REFERENCES messages(id), task_id INTEGER REFERENCES tasks(id),
  event_id INTEGER REFERENCES events(id), usage TEXT)'),
    ('table', 'model_budgets', 'model_budgets', 'CREATE TABLE model_budgets(
  id TEXT PRIMARY KEY, call_limit INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
  CHECK(attempts>=0 AND attempts<=call_limit))'),
    ('table', 'outbox', 'outbox', 'CREATE TABLE outbox(
  id TEXT PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  phase TEXT NOT NULL, auth_generation TEXT NOT NULL DEFAULT '''', data TEXT NOT NULL,
  tool TEXT NOT NULL, environment_id TEXT NOT NULL, project_id TEXT NOT NULL, revision_id INTEGER,
  session_participation_id INTEGER, stop_operation TEXT, instruction TEXT NOT NULL,
  instruction_deferred INTEGER NOT NULL DEFAULT 0 CHECK(instruction_deferred IN (0,1)),
  memory_version INTEGER, memory_section TEXT, memory_excerpt_bytes INTEGER,
  CHECK((memory_version IS NULL)=(memory_section IS NULL) AND (memory_version IS NULL)=(memory_excerpt_bytes IS NULL)))'),
    ('table', 'peer_exchanges', 'peer_exchanges', 'CREATE TABLE peer_exchanges(
  id TEXT PRIMARY KEY, kind TEXT NOT NULL, phase TEXT NOT NULL, last_message TEXT NOT NULL,
  sender_task INTEGER NOT NULL REFERENCES tasks(id),
  recipient_task INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
  sender_environment TEXT NOT NULL, sender_revision INTEGER)'),
    ('table', 'policy_approvals', 'policy_approvals', 'CREATE TABLE policy_approvals(
 id INTEGER PRIMARY KEY, operation TEXT NOT NULL UNIQUE, bot TEXT NOT NULL,
 action TEXT NOT NULL,target TEXT NOT NULL,fingerprint TEXT NOT NULL,preview TEXT NOT NULL,
 scope TEXT NOT NULL, requester TEXT NOT NULL, anchor INTEGER NOT NULL, turn INTEGER NOT NULL,
 task INTEGER NOT NULL, revision INTEGER NOT NULL,generation INTEGER NOT NULL,
 created_at INTEGER NOT NULL,expires_at INTEGER,mode_revision INTEGER NOT NULL,
 status TEXT NOT NULL DEFAULT ''pending'', resume TEXT,
 wait_id INTEGER NOT NULL DEFAULT 0, card_generation INTEGER NOT NULL, resume_state TEXT NOT NULL DEFAULT ''''
)'),
    ('table', 'policy_journal', 'policy_journal', 'CREATE TABLE policy_journal(id INTEGER PRIMARY KEY,time INTEGER,actor TEXT,bot TEXT,action TEXT,details TEXT)'),
    ('table', 'policy_rules', 'policy_rules', 'CREATE TABLE policy_rules(id INTEGER PRIMARY KEY,kind TEXT NOT NULL CHECK(kind IN (''allow'',''ask'',''never'')),
 action TEXT NOT NULL,target TEXT NOT NULL,created_at INTEGER NOT NULL,exact INTEGER NOT NULL,UNIQUE(kind,action,target))'),
    ('table', 'policy_settings', 'policy_settings', 'CREATE TABLE policy_settings(id INTEGER PRIMARY KEY CHECK(id=1),mode TEXT NOT NULL CHECK(mode IN (''full'',''supervised'')), revision INTEGER NOT NULL)'),
    ('table', 'pr_review_items', 'pr_review_items', 'CREATE TABLE pr_review_items(
    id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
    target_revision INTEGER NOT NULL,
    url TEXT NOT NULL, title TEXT NOT NULL, prompt TEXT NOT NULL,
    git_key TEXT, operation_id TEXT REFERENCES git_pr_operations(operation_id), candidates TEXT NOT NULL DEFAULT ''[]'',
    status TEXT NOT NULL DEFAULT ''pending'' CHECK(status IN (''pending'',''approved'',''changes_requested'',''linked'',''rejected'',''superseded'')),
    UNIQUE(task_id,git_key), CHECK((git_key IS NULL) <> (operation_id IS NULL)))'),
    ('table', 'protected_inputs', 'protected_inputs', 'CREATE TABLE protected_inputs(id INTEGER PRIMARY KEY,bot TEXT,name TEXT,file TEXT NOT NULL UNIQUE,
 status TEXT DEFAULT ''pending'',source_message_id INTEGER REFERENCES messages(id))'),
    ('table', 'runs', 'runs', 'CREATE TABLE runs(
  action_id TEXT PRIMARY KEY REFERENCES outbox(id), task_id INTEGER NOT NULL REFERENCES tasks(id),
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, run_id TEXT NOT NULL, link TEXT NOT NULL,
  UNIQUE(environment_id,thread_id,run_id))'),
    ('table', 'secret_destinations', 'secret_destinations', 'CREATE TABLE secret_destinations(destination TEXT PRIMARY KEY)'),
    ('table', 'session_asks', 'session_asks', 'CREATE TABLE session_asks(
 person_id TEXT NOT NULL, request_id TEXT NOT NULL,
 participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 request_digest TEXT NOT NULL, message_id INTEGER NOT NULL UNIQUE REFERENCES messages(id),
 PRIMARY KEY(person_id,request_id))'),
    ('table', 'session_invitations', 'session_invitations', 'CREATE TABLE session_invitations(
 source_message_id INTEGER NOT NULL, step INTEGER NOT NULL,
 inviter_participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 participation_id INTEGER NOT NULL REFERENCES session_participations(id),
 depth INTEGER NOT NULL CHECK(depth BETWEEN 1 AND 3),
 message_id INTEGER NOT NULL UNIQUE REFERENCES messages(id),
 PRIMARY KEY(source_message_id,step),
 FOREIGN KEY(source_message_id,step) REFERENCES conversation_steps(message_id,step))'),
    ('table', 'session_message_sends', 'session_message_sends', 'CREATE TABLE session_message_sends(
  action TEXT PRIMARY KEY,
  message_id INTEGER NOT NULL REFERENCES messages(id),
  environment TEXT NOT NULL, thread TEXT NOT NULL, gateway_environment TEXT NOT NULL,
  instruction TEXT NOT NULL, interrupt_at INTEGER, step INTEGER)'),
    ('table', 'session_message_waits', 'session_message_waits', 'CREATE TABLE session_message_waits(
  message_id INTEGER NOT NULL REFERENCES messages(id),
  bot TEXT NOT NULL, environment TEXT NOT NULL, thread TEXT NOT NULL, run TEXT NOT NULL, title TEXT NOT NULL,
  gateway_environment TEXT NOT NULL DEFAULT '''',
  notified INTEGER NOT NULL DEFAULT 0,
  task_id INTEGER REFERENCES tasks(id), action_id TEXT REFERENCES outbox(id),
  delivery TEXT NOT NULL DEFAULT ''started'', link TEXT NOT NULL DEFAULT '''',
  run_finished INTEGER NOT NULL DEFAULT 0, interrupt_at INTEGER, session_missing INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(message_id,environment,thread,run))'),
    ('table', 'session_participations', 'session_participations', 'CREATE TABLE session_participations(
 id INTEGER PRIMARY KEY,
 bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, person_id TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN (''active'',''removed'',''left'')),
 stopped_through INTEGER NOT NULL DEFAULT 0,
 UNIQUE(bot_id,environment_id,thread_id))'),
    ('table', 'session_queue_cancellations', 'session_queue_cancellations', 'CREATE TABLE session_queue_cancellations(
 participation_id INTEGER NOT NULL REFERENCES session_participations(id), run TEXT NOT NULL,
 stop_id INTEGER NOT NULL REFERENCES journal(id),
 state TEXT NOT NULL DEFAULT ''pending'' CHECK(state IN (''pending'',''cancelled'',''started'',''unavailable'')),
 retry_at INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(participation_id,run))'),
    ('table', 'session_read_refs', 'session_read_refs', 'CREATE TABLE session_read_refs(
  ref TEXT PRIMARY KEY, bot TEXT NOT NULL, kind INTEGER NOT NULL,
  environment TEXT NOT NULL, thread TEXT NOT NULL, payload TEXT NOT NULL,
  UNIQUE(bot,kind,environment,thread,payload))'),
    ('table', 'sqlite_sequence', 'sqlite_sequence', 'CREATE TABLE sqlite_sequence(name,seq)'),
    ('table', 't3_connected_environments', 't3_connected_environments', 'CREATE TABLE t3_connected_environments(environment_id TEXT PRIMARY KEY)'),
    ('table', 't3_connector', 't3_connector', 'CREATE TABLE t3_connector(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), id TEXT NOT NULL, person_id TEXT NOT NULL,
 token_digest TEXT NOT NULL, port INTEGER NOT NULL)'),
    ('table', 't3_events', 't3_events', 'CREATE TABLE t3_events(
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, item_type TEXT NOT NULL, item_id TEXT NOT NULL,
  PRIMARY KEY(environment_id,thread_id,item_type,item_id))'),
    ('table', 'task_revisions', 'task_revisions', 'CREATE TABLE task_revisions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  number INTEGER NOT NULL, intent TEXT NOT NULL, done_when TEXT NOT NULL,
  source_message_id INTEGER REFERENCES messages(id),
  UNIQUE(task_id,number))'),
    ('table', 'task_run_destinations', 'task_run_destinations', 'CREATE TABLE task_run_destinations(
  task_id INTEGER NOT NULL REFERENCES tasks(id), position INTEGER NOT NULL,
  name TEXT NOT NULL, environment_id TEXT NOT NULL, project_id TEXT NOT NULL,
  action_id TEXT, status TEXT NOT NULL, data TEXT NOT NULL,
  PRIMARY KEY(task_id,position),
  CHECK(action_id IS NOT NULL OR status=''queued''))'),
    ('table', 'tasks', 'tasks', 'CREATE TABLE tasks(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL,
  project_id TEXT NOT NULL, state TEXT NOT NULL, generation INTEGER NOT NULL,
  request TEXT NOT NULL, source_message_id INTEGER, current_revision INTEGER,
  replanning INTEGER NOT NULL CHECK(replanning IN (0,1)),
  pending_environment_id TEXT, pending_project_id TEXT, pending_gateway_environment_id TEXT,
  pending_pinned INTEGER CHECK(pending_pinned IN (0,1)),
  active_run TEXT, active_thread_id TEXT, active_run_id TEXT, last_run TEXT,
  action_id TEXT NOT NULL, workflow_id INTEGER, workflow_stage_id TEXT, workflow_artifact_version TEXT, trigger_data TEXT,
  origin_environment_id TEXT, origin_thread_id TEXT, origin_gateway_environment_id TEXT, origin_title TEXT,
  selected_environment_id TEXT, selected_thread_id TEXT, selected_gateway_environment_id TEXT, selected_project_id TEXT,
  run_outcome TEXT, data TEXT NOT NULL)'),
    ('table', 'waits', 'waits', 'CREATE TABLE waits(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  reason TEXT NOT NULL, data TEXT NOT NULL, resolution TEXT)'),
    ('table', 'web_identity_required', 'web_identity_required', 'CREATE TABLE web_identity_required(id INTEGER PRIMARY KEY CHECK(id=1))'),
    ('table', 'workflow_artifacts', 'workflow_artifacts', 'CREATE TABLE workflow_artifacts(
        report_id INTEGER PRIMARY KEY REFERENCES journal,workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,
        version TEXT NOT NULL,FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages)'),
    ('table', 'workflow_dependencies', 'workflow_dependencies', 'CREATE TABLE workflow_dependencies(
        workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,parent_id TEXT NOT NULL,report_id INTEGER REFERENCES journal,
        artifact_version TEXT NOT NULL DEFAULT '''',
        PRIMARY KEY(workflow_id,stage_id,parent_id),
        FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages,
        FOREIGN KEY(workflow_id,parent_id) REFERENCES workflow_stages)'),
    ('table', 'workflow_stages', 'workflow_stages', 'CREATE TABLE workflow_stages(workflow_id INTEGER NOT NULL REFERENCES workflows,
        stage_id TEXT NOT NULL,spec TEXT NOT NULL,task_id INTEGER UNIQUE REFERENCES tasks,result_id INTEGER REFERENCES journal,instruction TEXT,status TEXT NOT NULL DEFAULT ''pending'',
        input TEXT NOT NULL DEFAULT '''',output TEXT NOT NULL DEFAULT '''',deadline INTEGER NOT NULL DEFAULT 0, alerted INTEGER NOT NULL DEFAULT 0,
        confirmed_state TEXT NOT NULL DEFAULT '''',confirmed_at INTEGER NOT NULL DEFAULT 0,unreachable INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(workflow_id,stage_id))'),
    ('table', 'workflow_verdicts', 'workflow_verdicts', 'CREATE TABLE workflow_verdicts(
        report_id INTEGER PRIMARY KEY REFERENCES journal,workflow_id INTEGER NOT NULL,stage_id TEXT NOT NULL,
        version TEXT NOT NULL,verdict TEXT NOT NULL CHECK(verdict IN (''pass'',''fail'')),
        FOREIGN KEY(workflow_id,stage_id) REFERENCES workflow_stages)'),
    ('table', 'workflows', 'workflows', 'CREATE TABLE workflows(
        id INTEGER PRIMARY KEY,bot_id TEXT NOT NULL,routing TEXT NOT NULL,request TEXT NOT NULL,
        message_id INTEGER NOT NULL REFERENCES messages,stop_requested INTEGER NOT NULL DEFAULT 0)'),
    ('view', 'automation_before_dispatch', 'automation_before_dispatch', 'CREATE VIEW automation_before_dispatch AS
SELECT task.id FROM tasks task
WHERE task.project_id<>'''' AND task.trigger_data IS NOT NULL AND task.active_run IS NULL AND task.last_run IS NULL
  AND NOT EXISTS (SELECT 1 FROM task_run_destinations WHERE task_id=task.id)
  AND NOT EXISTS (SELECT 1 FROM outbox WHERE task_id=task.id)'),
    ('view', 'channel_posts', 'channel_posts', 'CREATE VIEW channel_posts AS
      SELECT d.rowid AS sequence,d.reply_id,d.external_id,d.payload,c.scope_key,m.bot_id
      FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id
      JOIN messages m ON m.id=c.message_id
      WHERE d.state=''accepted'' AND d.external_id!=''''
      AND d.operation IN (''reply'',''channel_send_attachment'',''publish_artifact'')'),
    ('view', 'channel_tasks', 'channel_tasks', 'CREATE VIEW channel_tasks AS
      SELECT t.id AS task_id,c.message_id,c.scope_key FROM tasks t JOIN channel_messages c
      ON c.message_id=t.source_message_id'),
    ('view', 'compactable_messages', 'compactable_messages', 'CREATE VIEW compactable_messages AS
        SELECT j.id,coalesce(j.conversation_bot_id,t.bot_id) bot_id,j.kind,j.task_id,j.data,
          CASE j.kind
            WHEN ''conversation_message'' THEN j.id
            WHEN ''conversation_reply'' THEN json_extract(j.data,''$.replyToMessageId'')
            ELSE t.source_message_id
          END source_message_id,
          CASE j.kind
            WHEN ''conversation_message'' THEN m.text
            WHEN ''report'' THEN json_extract(j.data,''$.report'')
            ELSE json_extract(j.data,''$.text'')
          END text
        FROM journal j LEFT JOIN messages m ON m.id=j.id LEFT JOIN tasks t ON t.id=j.task_id
        WHERE (j.conversation_bot_id IS NOT NULL AND j.kind IN (''conversation_message'',''conversation_reply''))
          OR (j.conversation_bot_id IS NULL AND (
            j.kind IN (''completed'',''cancelled'',''report'')
            OR (j.kind=''waiting'' AND json_extract(j.data,''$.reason'')=''user_question'')
            OR (j.kind=''user_answer'' AND json_extract(j.data,''$.conversation'') IS NOT 1)))'),
    ('view', 'conversation_input_status', 'conversation_input_status', 'CREATE VIEW conversation_input_status AS
SELECT t.message_id,
  CASE t.status WHEN ''paused'' THEN ''needs_you''
                WHEN ''policy_waiting'' THEN ''needs_you''
                WHEN ''done'' THEN coalesce(
                  (SELECT json_extract(data,''$.status'') FROM journal WHERE id=t.reply_id),''access_revoked'')
  END AS status
FROM conversation_turns t'),
    ('view', 'outdated_workflow_stages', 'outdated_workflow_stages', 'CREATE VIEW outdated_workflow_stages AS
        WITH RECURSIVE outdated(workflow_id,stage_id) AS (
          SELECT d.workflow_id,d.stage_id FROM workflow_dependencies d JOIN workflow_stages p
            ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id
            WHERE d.report_id IS NOT NULL AND (p.status<>''completed'' OR p.output<>d.artifact_version
              OR (p.result_id IS NOT d.report_id AND
                  (d.artifact_version='''' OR json_extract(p.spec,''$.role'') NOT IN (''implement'',''verify''))))
          UNION
          SELECT d.workflow_id,d.stage_id FROM workflow_dependencies d JOIN outdated p
            ON p.workflow_id=d.workflow_id AND p.stage_id=d.parent_id)
        SELECT * FROM outdated'),
    ('view', 'project_route_queue', 'project_route_queue', 'CREATE VIEW project_route_queue AS
WITH candidates AS (
  SELECT task.id,task.state AS slot_state,task.project_id,task.environment_id,NULL AS action_id
  FROM tasks task
  WHERE NOT EXISTS (SELECT 1 FROM task_run_destinations WHERE task_id=task.id)
    AND task.project_id<>''''
    AND task.state NOT IN (''completed'',''cancelled'')
    AND NOT (task.state=''waiting'' AND EXISTS (
      SELECT 1 FROM automation_before_dispatch undispatched
      JOIN waits wait ON wait.task_id=undispatched.id AND wait.resolution IS NULL
      WHERE undispatched.id=task.id AND wait.reason=''bot_notes_unavailable''
    ))
  UNION ALL
  SELECT task.id,CASE WHEN outbox.phase IN (''started'',''accepted'') THEN task.state ELSE ''queued'' END,
         route.project_id,route.environment_id,outbox.id
  FROM tasks task CROSS JOIN task_run_destinations route ON route.task_id=task.id
  LEFT JOIN outbox ON outbox.task_id=task.id AND outbox.id=route.action_id
  WHERE task.state NOT IN (''completed'',''cancelled'')
    AND route.status IN (''queued'',''running'',''confirmation_pending'')
)
SELECT task.id,task.environment_id,task.project_id,task.action_id,
       ROW_NUMBER() OVER (
         PARTITION BY task.environment_id,task.project_id
         ORDER BY COALESCE(sender.slot_state,task.slot_state)=''queued'',
           COALESCE(sender.id,task.id),
           CASE WHEN sender.id IS NOT NULL AND task.slot_state=''cancelling'' THEN 0
                WHEN task.slot_state=''peer_waiting'' THEN 2 ELSE 1 END, task.id
       ) AS position
FROM candidates task
LEFT JOIN peer_exchanges peer ON peer.recipient_task=task.id
LEFT JOIN candidates sender ON sender.id=peer.sender_task
  AND sender.environment_id=task.environment_id AND sender.project_id=task.project_id
  AND peer.phase<>''closed''
WHERE COALESCE(peer.kind,'''')<>''consult'''),
    ('view', 'project_route_slots', 'project_route_slots', 'CREATE VIEW project_route_slots AS
WITH queue AS (
  SELECT id,environment_id,project_id,action_id,
         FIRST_VALUE(id) OVER (PARTITION BY environment_id,project_id ORDER BY position) AS head_id
  FROM project_route_queue
)
SELECT q.id,q.environment_id,q.project_id,q.action_id,
       CASE WHEN q.id<>q.head_id AND (owner.workflow_id IS NULL OR owner.workflow_id IS NOT t.workflow_id)
       THEN q.head_id END AS blocker_id
FROM queue q JOIN tasks t ON t.id=q.id JOIN tasks owner ON owner.id=q.head_id'),
    ('view', 'task_project_slots', 'task_project_slots', 'CREATE VIEW task_project_slots AS
SELECT id,CASE WHEN count(blocker_id)=count(*) THEN min(blocker_id) END AS blocker_id
FROM project_route_slots GROUP BY id');
