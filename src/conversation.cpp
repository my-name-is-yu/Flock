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

void Store::setWebIdentityResolver(WebIdentityResolver resolver) {
    std::lock_guard lock(p_->mutex);
    p_->resolveWebIdentity = std::move(resolver);
}

void Store::Impl::initializeConversationSchema() {
    Transaction transaction(db);
    const bool initializeTurnState = !query("SELECT 1 FROM sqlite_schema WHERE type='view'"
                                           " AND name='conversation_input_status'").next();
    if (sqlite3_exec(db,R"SQL(
CREATE INDEX IF NOT EXISTS journal_bot ON journal(conversation_bot_id,id) WHERE conversation_bot_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS journal_task_results ON journal(task_id,id DESC)
  WHERE kind IN ('completed','report','cancelled');
CREATE INDEX IF NOT EXISTS journal_instruction_message ON journal(
  json_extract(data,'$.messageId'),task_id,coalesce(json_extract(data,'$.step'),0))
  WHERE kind IN ('instruction_accepted','instruction_steered','instruction_delivered');
CREATE INDEX IF NOT EXISTS conversation_active_tasks ON tasks(bot_id,id DESC)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS conversation_terminal_tasks ON tasks(bot_id,id DESC)
  WHERE state IN ('completed','cancelled');
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY REFERENCES journal(id), bot_id TEXT NOT NULL,
  client_message_id TEXT NOT NULL, sequence INTEGER NOT NULL,
  reply_to INTEGER, text TEXT NOT NULL, routing TEXT NOT NULL,
  person_id TEXT NOT NULL, person_root INTEGER NOT NULL CHECK(person_root IN (0,1)),
  principal_source INTEGER NOT NULL DEFAULT 0 CHECK(principal_source IN (0,1,2,3)),
  UNIQUE(bot_id,person_id,client_message_id), UNIQUE(bot_id,sequence));
CREATE TABLE IF NOT EXISTS conversation_turns(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id),
  status TEXT NOT NULL CHECK(status IN ('running','paused','policy_waiting','done')),
  pause_id INTEGER UNIQUE REFERENCES journal(id), reserved_attempt INTEGER,
  reply_id INTEGER UNIQUE REFERENCES journal(id));
CREATE INDEX IF NOT EXISTS conversation_turns_running ON conversation_turns(message_id) WHERE status='running';
CREATE VIEW IF NOT EXISTS conversation_input_status AS
SELECT t.message_id,
  CASE t.status WHEN 'paused' THEN 'needs_you'
                WHEN 'policy_waiting' THEN 'needs_you'
                WHEN 'done' THEN coalesce(
                  (SELECT json_extract(data,'$.status') FROM journal WHERE id=t.reply_id),'access_revoked')
  END AS status
FROM conversation_turns t;
CREATE TABLE IF NOT EXISTS conversation_retries(
  message_id INTEGER PRIMARY KEY REFERENCES conversation_turns(message_id),
  failures INTEGER NOT NULL CHECK(failures BETWEEN 1 AND 7),
  retry_at INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS conversation_steps(
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id), step INTEGER NOT NULL,
  attempt_id INTEGER, decision TEXT CHECK(json_type(decision)='array'), effect TEXT,
  result TEXT CHECK(json_type(result)='array'),
  PRIMARY KEY(message_id,step));
CREATE TABLE IF NOT EXISTS task_revisions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  number INTEGER NOT NULL, intent TEXT NOT NULL, done_when TEXT NOT NULL,
  source_message_id INTEGER REFERENCES messages(id),
  UNIQUE(task_id,number));
)SQL",nullptr,nullptr,nullptr) != SQLITE_OK)
        throw StorageError(db);
    if (!query("SELECT 1 FROM pragma_table_info('messages') WHERE name='principal_source'").next())
        exec("ALTER TABLE messages ADD COLUMN principal_source INTEGER NOT NULL DEFAULT 0 CHECK(principal_source IN (0,1,2,3))");
    if (initializeTurnState || query("SELECT 1 FROM sqlite_schema WHERE type='index' AND name='journal_pending_turn'").next()) {
        exec(R"SQL(
INSERT OR IGNORE INTO conversation_turns(message_id,status)
SELECT j.id,'running' FROM journal j JOIN messages m ON m.id=j.id
WHERE j.kind IN ('conversation_message','git_webhook') AND json_extract(j.data,'$.status') IS NULL
)SQL");
        exec("DROP INDEX IF EXISTS journal_pending_turn");
    }
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
    task.erase("blockerReason");
    task.erase("blockerWaitId");
    task["intent"] = intent;
    task["doneWhen"] = doneWhen;
    journal(task["id"],"task_revision",{{"revisionId",revision},{"sourceMessageId",source}});
    return revision;
}

bool Store::Impl::conversationPending(std::string_view bot) const {
    // Start with the running-turn index so long Bot histories do not add work.
    return query(R"SQL(
SELECT 1 FROM conversation_turns t CROSS JOIN messages m ON m.id=t.message_id JOIN journal j ON j.id=m.id
WHERE t.status IN ('running','policy_waiting') AND m.bot_id=? AND j.kind='conversation_message' LIMIT 1
)SQL",{bot}).next();
}

std::optional<Json> Store::Impl::messageReceipt(const Person& actor, std::string_view bot, std::string_view clientId,
                                               std::string_view text, Id replyTo, const Json& attachments) const {
    auto row = query("SELECT id,text,coalesce(reply_to,0) FROM messages WHERE bot_id=? AND person_id=? AND client_message_id=?",
                         {bot,actor.id,clientId});
    if (!row.next()) return std::nullopt;
    auto stored = query("SELECT payload FROM message_attachments WHERE message_id=? ORDER BY attachment_id",{row.number(0)});
    auto media = Json::array();
    while (stored.next()) media.push_back(stored.json(0));
    if (media != attachments) throw Conflict("This client message ID already has different attachments");
    return receiptRow(row,redactor(bot).text(std::string(text)),replyTo);
}

std::optional<Json> Store::receipt(const Person& actor, std::string_view bot, std::string_view clientId,
                                   std::string_view text, Id replyTo, const Json& attachments) const {
    std::lock_guard lock(p_->mutex);
    if (clientId.starts_with("channel:")) throw Conflict("Channel message IDs are reserved");
    if (!Policy(*p_).allows(actor,Action::bot_talk,bot)) throw DecisionDenied{};
    return p_->messageReceipt(actor,bot,clientId,text,replyTo,attachments);
}

Json Store::receive(const Person& actor, const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                    const Json& attachments) {
    return receive(actor,routing.botId,clientId,text,replyTo,conversation::PrincipalSource::trusted,
                   nullptr,[&] { return routing; },attachments);
}

Json Store::receiveWeb(const Person& actor, const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                       const Json& attachments) {
    return receive(actor,routing.botId,clientId,text,replyTo,conversation::PrincipalSource::web,
                   nullptr,[&] { return routing; },attachments);
}

Json Store::receive(const Person& actor, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                    conversation::PrincipalSource source, const VerifiedChannelMessage* channel,
                    const std::function<Intake()>& route, const Json& attachments) {
    if (!channel && clientId.starts_with("channel:")) throw Conflict("Channel message IDs are reserved");
    if (bot.empty() || !hasText(clientId) || clientId.size() > (channel ? 2048u : 256u) ||
        !hasText(text) || text.size() > messageSize || replyTo < 0)
        throw Conflict("Bot, client message ID and nonempty text are required");
    Json receipt;
    {
        std::lock_guard lock(p_->mutex);
        Transaction transaction(p_->db);
        if (!policy().allows(actor,Action::bot_decide,bot)) throw DecisionDenied{};
        const auto existing = p_->messageReceipt(actor,bot,clientId,text,replyTo,attachments);
        if (existing) receipt = *existing;
        else {
            const auto routing = route();
            if (routing.botId != bot || !routing.config.is_object()) throw Conflict("Invalid Bot conversation route");
            modelCallLimit(routing.config);
            const Json saved = {{"environmentId",routing.environmentId},{"projectId",routing.projectId},
                                {"config",routing.config}};
            const auto scrub = p_->redactor(routing.botId);
            scrub.requirePublic({routing.botId,actor.id,clientId,saved});
            p_->exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_message','{}',?)",
                     {unixTime(),routing.botId});
            const auto id = p_->last();
            p_->exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root,principal_source)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,?,?,?,?,? FROM messages WHERE bot_id=?
)SQL", {id,routing.botId,clientId,replyTo ? Json(replyTo) : Json(nullptr),scrub.text(std::string(text)),saved,
        actor.id,source == conversation::PrincipalSource::trusted && actor.adminRoot ? 1 : 0,
        static_cast<int>(source),routing.botId});
            p_->exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')",{id});
            p_->saveAttachments(id,attachments);
            receipt = {{"messageId",id},{"status","received"}};
        }
        if (channel) p_->saveChannelMessage(receipt.at("messageId"),bot,*channel,existing.has_value());
        transaction.commit();
    }
    return receipt;
}

std::vector<std::pair<std::string,Id>> Store::Impl::pendingConversations(unsigned limit, Seconds now) const {
    std::lock_guard lock(mutex);
    auto next = query(R"SQL(
WITH heads AS (
  SELECT m.bot_id AS bot,min(t.message_id) AS id
  FROM conversation_turns t CROSS JOIN messages m ON m.id=t.message_id
  WHERE t.status='running' GROUP BY m.bot_id
)
SELECT h.bot,h.id FROM heads h LEFT JOIN conversation_retries r ON r.message_id=h.id
WHERE r.retry_at IS NULL OR r.retry_at<=? ORDER BY h.id LIMIT ?
)SQL",{now,limit});
    std::vector<std::pair<std::string,Id>> result;
    while (next.next()) result.emplace_back(next.string(0),next.number(1));
    return result;
}

bool Store::Impl::isNextTurn(Id id) const {
    return query(R"SQL(
SELECT 1 WHERE ?1=(
  SELECT t.message_id FROM conversation_turns t CROSS JOIN messages m ON m.id=t.message_id
  WHERE t.status='running' AND t.message_id<=?1 AND m.bot_id=(SELECT bot_id FROM messages WHERE id=?1)
  ORDER BY t.message_id LIMIT 1)
)SQL",{id}).next();
}

void Store::Impl::deferConversation(Id id, Seconds now) {
    // Consecutive failures back off by 1, 2, 4, 8, 16, 32, then at most 60 seconds.
    exec(R"SQL(
INSERT INTO conversation_retries(message_id,failures,retry_at) VALUES(?,1,?+1)
ON CONFLICT(message_id) DO UPDATE SET
  failures=min(failures+1,7), retry_at=?+min(1 << failures,60)
)SQL",{id,now,now});
}

}
