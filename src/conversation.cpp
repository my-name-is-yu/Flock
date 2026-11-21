#include "storage.hpp"
#include "conversation_storage.hpp"

namespace flockv0 {
using conversation_storage::hasText;
using conversation_storage::messageSize;

namespace {
Json receiptRow(const Statement& row, std::string_view text, Id replyTo, Id candidate) {
    if (row.string(1) != text || row.number(2) != replyTo || row.number(3) != candidate)
        throw Conflict("This client message ID already has different content");
    return {{"messageId",row.number(0)},{"status","received"}};
}
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
                                   std::string_view text, Id replyTo, Id candidate) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query(R"SQL(
SELECT id,text,reply_to,coalesce(json_extract(routing,'$.candidateTaskId'),0)
FROM messages WHERE bot_id=? AND client_message_id=?
)SQL",{bot,clientId});
    if (!row.next()) return std::nullopt;
    return receiptRow(row,text,replyTo,candidate);
}

Json Store::receive(const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                    Id candidate) {
    if (routing.botId.empty() || !hasText(clientId) || clientId.size() > 256 ||
        !hasText(text) || text.size() > messageSize || replyTo < 0 || !routing.config.is_object())
        throw Conflict("Bot, client message ID and nonempty text are required");
    if (candidate < 0 || (candidate && !replyTo))
        throw Conflict("A selected candidate must reply to its clarification");
    Json receipt;
    {
        std::lock_guard lock(p_->mutex);
        Transaction transaction(p_->db);
        const auto existing = this->receipt(routing.botId,clientId,text,replyTo,candidate);
        if (existing) receipt = *existing;
        else {
            modelCallLimit(routing.config);
            const auto now = unixTime();
            Json saved = {{"environmentId",routing.environmentId},{"projectId",routing.projectId},{"config",routing.config},
                          {"waitsAtIntake",p_->captureConversationWaits(routing.botId)}};
            if (candidate) saved["candidateTaskId"] = candidate;
            p_->exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_message','{}',?)",
                     {now,routing.botId});
            const auto id = p_->last();
            p_->exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,?,? FROM messages WHERE bot_id=?
)SQL", {id,routing.botId,clientId,replyTo ? Json(replyTo) : Json(nullptr),text,saved,
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
