#include "storage.hpp"
#include "conversation_storage.hpp"

namespace flockv0 {
using conversation_storage::hasText;
using conversation_storage::messageSize;

namespace {
Json receiptRow(const Statement& row, std::string_view text, Id replyTo) {
    if (row.string(1) != text || row.number(2) != replyTo)
        throw Conflict("This client message ID already has different content");
    return {{"messageId",row.number(0)},{"status","received"}};
}
}

void Store::Impl::migrateConversation() {
    Transaction transaction(db);
    bool journalHasBot = false, unshippedColumn = false;
    {
        auto columns = query("PRAGMA table_info(journal)");
        while (columns.next()) {
            journalHasBot |= columns.string(1) == "conversation_bot_id";
            unshippedColumn |= columns.string(1) == "bot_id";
        }
    }
    const auto schema = [&](const char* sql) {
        if (sqlite3_exec(db,sql,nullptr,nullptr,nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    };
    if (unshippedColumn) schema("ALTER TABLE journal RENAME COLUMN bot_id TO conversation_bot_id");
    else if (!journalHasBot) {
        schema(R"SQL(
ALTER TABLE journal RENAME TO legacy_journal;
CREATE TABLE journal(
  id INTEGER PRIMARY KEY, task_id INTEGER REFERENCES tasks(id),
  time INTEGER NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL, conversation_bot_id TEXT,
  CHECK(task_id IS NOT NULL OR conversation_bot_id IS NOT NULL));
INSERT INTO journal(id,task_id,time,kind,data) SELECT id,task_id,time,kind,data FROM legacy_journal;
DROP TABLE legacy_journal;
CREATE INDEX journal_task ON journal(task_id,id);
)SQL");
    }
    schema(R"SQL(
CREATE INDEX IF NOT EXISTS journal_bot ON journal(conversation_bot_id,id) WHERE conversation_bot_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS journal_pending_route ON journal(conversation_bot_id,id)
WHERE kind='conversation_message' AND json_extract(data,'$.status') IS NULL;
CREATE INDEX IF NOT EXISTS journal_task_results ON journal(task_id,id DESC)
  WHERE kind IN ('completed','report','cancelled');
CREATE INDEX IF NOT EXISTS conversation_active_tasks ON tasks(bot_id,id DESC)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS conversation_terminal_tasks ON tasks(bot_id,id DESC)
  WHERE state IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS conversation_waiting_tasks ON tasks(bot_id,id DESC)
  WHERE state='waiting' AND json_extract(data,'$.wait.reason') IN ('user_question','budget');
CREATE INDEX IF NOT EXISTS conversation_task_name ON tasks(bot_id,lower(json_extract(data,'$.request')),id DESC);
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY REFERENCES journal(id), bot_id TEXT NOT NULL,
  client_message_id TEXT NOT NULL, sequence INTEGER NOT NULL,
  reply_to INTEGER, text TEXT NOT NULL, routing TEXT NOT NULL,
  UNIQUE(bot_id,client_message_id), UNIQUE(bot_id,sequence));
CREATE TABLE IF NOT EXISTS route_decisions(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id),
  reply_id INTEGER NOT NULL UNIQUE REFERENCES journal(id),
  status TEXT NOT NULL, data TEXT NOT NULL, resolution_id INTEGER REFERENCES messages(id));
CREATE TABLE IF NOT EXISTS route_candidates(
  message_id INTEGER NOT NULL REFERENCES route_decisions(message_id),
  task_id INTEGER NOT NULL REFERENCES tasks(id),
  PRIMARY KEY(message_id,task_id));
CREATE INDEX IF NOT EXISTS route_candidates_task ON route_candidates(task_id,message_id);
CREATE TABLE IF NOT EXISTS task_revisions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  number INTEGER NOT NULL, intent TEXT NOT NULL, done_when TEXT NOT NULL,
  source_message_id INTEGER REFERENCES messages(id),
  UNIQUE(task_id,number));
CREATE TABLE IF NOT EXISTS conversation_migrations(name TEXT PRIMARY KEY);
)SQL");
    {
        auto applied = query("SELECT 1 FROM conversation_migrations WHERE name='legacy_revisions'");
        if (applied.next()) {
            transaction.commit();
            return;
        }
    }
    exec(R"SQL(
INSERT INTO task_revisions(task_id,number,intent,done_when)
SELECT id,1,json_extract(data,'$.request'),json_extract(data,'$.doneWhen')
FROM tasks WHERE length(json_extract(data,'$.doneWhen'))>0
AND NOT EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=tasks.id)
)SQL");
    exec(R"SQL(
UPDATE tasks SET data=json_set(data,'$.currentRevision',(
  SELECT id FROM task_revisions WHERE task_id=tasks.id AND number=1))
WHERE json_extract(data,'$.currentRevision') IS NULL
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=tasks.id)
)SQL");
    exec(R"SQL(
UPDATE outbox SET data=json_set(data,'$.revisionId',(
  SELECT id FROM task_revisions WHERE task_id=outbox.task_id AND number=1))
WHERE json_extract(data,'$.revisionId') IS NULL
AND json_extract(data,'$.tool') IN ('delegate','follow_up')
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=outbox.task_id)
)SQL");
    exec(R"SQL(
UPDATE events SET data=json_set(data,'$.revisionId',(
  SELECT id FROM task_revisions WHERE task_id=events.task_id AND number=1))
WHERE json_extract(data,'$.revisionId') IS NULL AND json_extract(data,'$.kind')='result'
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=events.task_id)
)SQL");
    exec(R"SQL(
UPDATE journal SET data=json_set(data,'$.revisionId',(
  SELECT id FROM task_revisions WHERE task_id=journal.task_id AND number=1))
WHERE kind IN ('report','completed') AND json_extract(data,'$.revisionId') IS NULL
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=journal.task_id)
)SQL");
    exec("INSERT INTO conversation_migrations(name) VALUES('legacy_revisions')");
    transaction.commit();
}

Id Store::Impl::addRevision(Json& task, std::string_view intent, std::string_view doneWhen, Id source) {
    if (!hasText(intent) || !hasText(doneWhen)) throw Conflict("Task revision needs intent and completion criteria");
    if (source) {
        auto row = query("SELECT 1 FROM messages WHERE id=? AND bot_id=?", {source,task["botId"]});
        if (!row.next()) throw Conflict("Task revision must come from this Bot's conversation");
    }
    exec(R"SQL(
INSERT INTO task_revisions(task_id,number,intent,done_when,source_message_id)
SELECT ?,coalesce(max(number),0)+1,?,?,? FROM task_revisions WHERE task_id=?
)SQL", {task["id"],intent,doneWhen,source ? Json(source) : Json(nullptr),task["id"]});
    const auto revision = last();
    task["currentRevision"] = revision;
    task["intent"] = intent;
    task["doneWhen"] = doneWhen;
    journal(task["id"],"task_revision",{{"revisionId",revision},{"sourceMessageId",source}});
    return revision;
}

bool Store::Impl::conversationPending(std::string_view bot) const {
    return query(R"SQL(
SELECT 1 FROM journal WHERE conversation_bot_id=? AND kind='conversation_message'
AND json_extract(data,'$.status') IS NULL LIMIT 1
)SQL",{bot}).next();
}

bool Store::Impl::completionHeld(const Json& task) const {
    auto row = query(R"SQL(
SELECT 1 FROM route_candidates c JOIN route_decisions r ON r.message_id=c.message_id
WHERE c.task_id=? AND r.status='awaiting_resolution'
AND json_extract(r.data,'$.operation') IN ('cancel_task','revise_task','destination')
)SQL",{task["id"]});
    return row.next();
}

std::optional<Json> Store::receipt(std::string_view bot, std::string_view clientId,
                                   std::string_view text, Id replyTo) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query("SELECT id,text,reply_to FROM messages WHERE bot_id=? AND client_message_id=?",
                         {bot,clientId});
    if (!row.next()) return std::nullopt;
    return receiptRow(row,text,replyTo);
}

Json Store::receive(const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo) {
    if (routing.botId.empty() || !hasText(clientId) || clientId.size() > 256 ||
        !hasText(text) || text.size() > messageSize || replyTo < 0 || !routing.config.is_object())
        throw Conflict("Bot, client message ID and nonempty text are required");
    Json receipt;
    {
        std::lock_guard lock(p_->mutex);
        Transaction transaction(p_->db);
        const auto existing = this->receipt(routing.botId,clientId,text,replyTo);
        if (existing) receipt = *existing;
        else {
            modelCallLimit(routing.config);
            const auto now = unixTime();
            p_->exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_message','{}',?)",
                     {now,routing.botId});
            const auto id = p_->last();
            p_->exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,?,? FROM messages WHERE bot_id=?
)SQL", {id,routing.botId,clientId,replyTo ? Json(replyTo) : Json(nullptr),text,
                Json{{"environmentId",routing.environmentId},{"projectId",routing.projectId},{"config",routing.config},
                     {"waitsAtIntake",p_->captureConversationWaits(routing.botId)}},
                routing.botId});
            receipt = {{"messageId",id},{"status","received"}};
        }
        transaction.commit();
    }
    return receipt;
}

std::vector<std::pair<std::string,Id>> Store::Impl::pendingConversations(unsigned limit) const {
    std::lock_guard lock(mutex);
    auto next = query(R"SQL(
SELECT j.conversation_bot_id,min(j.id) FROM journal j
WHERE j.kind='conversation_message' AND json_extract(j.data,'$.status') IS NULL
GROUP BY j.conversation_bot_id ORDER BY min(j.id) LIMIT ?
)SQL",{limit});
    std::vector<std::pair<std::string,Id>> result;
    while (next.next()) result.emplace_back(next.string(0),next.number(1));
    return result;
}

Json Store::clarifications(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT j.data FROM route_decisions r JOIN journal j ON j.id=r.reply_id
WHERE j.conversation_bot_id=? AND r.status IN ('awaiting_resolution','model_budget') ORDER BY j.id DESC
)SQL",{bot});
    auto result = Json::array();
    while (rows.next()) result.push_back(rows.json(0));
    return result;
}

void Store::approveMessageModelCall(std::string_view botId, Id waitMessageId) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    auto wait = p_->query(R"SQL(
SELECT r.message_id FROM route_decisions r JOIN messages m ON m.id=r.message_id
WHERE m.bot_id=? AND r.reply_id=? AND r.status='model_budget'
)SQL",{botId,waitMessageId});
    if (!wait.next()) throw Conflict("Waiting item is no longer current");
    const auto message = wait.number(0);
    p_->grantModelCall(p_->messageModelBudget(message));
    p_->exec("DELETE FROM route_decisions WHERE message_id=?",{message});
    p_->exec("UPDATE journal SET data='{}' WHERE id=?",{message});
    p_->exec("UPDATE journal SET data=json_set(data,'$.status','done') WHERE id=?",{waitMessageId});
    transaction.commit();
}
}
