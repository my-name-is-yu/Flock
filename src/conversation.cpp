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
    if (sqlite3_exec(db,R"SQL(
DROP TABLE IF EXISTS route_candidates;
DROP TABLE IF EXISTS route_decisions;
DROP TABLE IF EXISTS conversation_migrations;
CREATE INDEX IF NOT EXISTS journal_bot ON journal(conversation_bot_id,id) WHERE conversation_bot_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS journal_pending_turn ON journal(conversation_bot_id,id)
WHERE kind='conversation_message' AND json_extract(data,'$.status') IS NULL;
CREATE INDEX IF NOT EXISTS journal_task_results ON journal(task_id,id DESC)
  WHERE kind IN ('completed','report','cancelled');
CREATE INDEX IF NOT EXISTS journal_instruction_message ON journal(
  CAST(json_extract(data,'$.messageId') AS NUMERIC),task_id,coalesce(json_extract(data,'$.step'),0))
  WHERE kind IN ('instruction_accepted','instruction_steered','instruction_delivered');
CREATE INDEX IF NOT EXISTS conversation_active_tasks ON tasks(bot_id,id DESC)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS conversation_terminal_tasks ON tasks(bot_id,id DESC)
  WHERE state IN ('completed','cancelled');
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY REFERENCES journal(id), bot_id TEXT NOT NULL,
  client_message_id TEXT NOT NULL, sequence INTEGER NOT NULL,
  reply_to INTEGER, text TEXT NOT NULL, routing TEXT NOT NULL,
  UNIQUE(bot_id,client_message_id), UNIQUE(bot_id,sequence));
CREATE TABLE IF NOT EXISTS conversation_turns(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id),
  status TEXT NOT NULL CHECK(status IN ('running','paused','done')),
  pause_id INTEGER UNIQUE REFERENCES journal(id), reserved_attempt INTEGER,
  reply_id INTEGER UNIQUE REFERENCES journal(id));
CREATE TABLE IF NOT EXISTS conversation_steps(
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id), step INTEGER NOT NULL,
  attempt_id INTEGER, decision TEXT, effect TEXT, result TEXT,
  PRIMARY KEY(message_id,step));
CREATE TABLE IF NOT EXISTS task_revisions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  number INTEGER NOT NULL, intent TEXT NOT NULL, done_when TEXT NOT NULL,
  source_message_id INTEGER REFERENCES messages(id),
  UNIQUE(task_id,number));
)SQL",nullptr,nullptr,nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
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

// True while a turn of this Bot may still change its tasks, including one paused for a model call approval.
bool Store::Impl::conversationPending(std::string_view bot) const {
    return query(R"SQL(
SELECT 1 FROM journal WHERE conversation_bot_id=? AND kind='conversation_message'
AND json_extract(data,'$.status') IS NULL
UNION ALL
SELECT 1 FROM conversation_turns t JOIN messages m ON m.id=t.message_id WHERE m.bot_id=? AND t.status='paused'
LIMIT 1
)SQL",{bot,bot}).next();
}

std::optional<Json> Store::receipt(std::string_view bot, std::string_view clientId,
                                   std::string_view text, Id replyTo) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query("SELECT id,text,coalesce(reply_to,0) FROM messages WHERE bot_id=? AND client_message_id=?",
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
            const Json saved = {{"environmentId",routing.environmentId},{"projectId",routing.projectId},
                                {"config",routing.config}};
            p_->exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_message','{}',?)",
                     {unixTime(),routing.botId});
            const auto id = p_->last();
            p_->exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,?,? FROM messages WHERE bot_id=?
)SQL", {id,routing.botId,clientId,replyTo ? Json(replyTo) : Json(nullptr),text,saved,routing.botId});
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

}
