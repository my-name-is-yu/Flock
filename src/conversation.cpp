#include "storage.hpp"
#include "flock/limits.hpp"
#include "conversation_storage.hpp"

namespace flockv0 {
using conversation_storage::hasText;
using limits::messages::maxBodyBytes;

namespace {
bool reservedMessageId(std::string_view id) {
    return id.starts_with("channel:") || id.starts_with("session-wake:") || id.starts_with("session-ask:") ||
           id.starts_with("session-invite:");
}

Json receiptRow(const Statement& row, std::string_view text, Id replyTo) {
    if (row.string(1) != text || row.number(2) != replyTo) throw Conflict("This client message ID already has different content");
    return {{"messageId", row.number(0)}, {"status", "received"}};
}
}

void Store::setWebIdentityResolver(WebIdentityResolver resolver, bool liveInteractiveRoles) {
    std::lock_guard lock(p_->mutex);
    p_->resolveWebIdentity = std::move(resolver);
    p_->liveInteractiveRoles = liveInteractiveRoles;
}

Id Store::Impl::addRevision(TaskData& task, std::string_view intent, std::string_view doneWhen, Id source) {
    if (!hasText(intent) || !hasText(doneWhen)) throw Conflict("Task revision needs intent and completion criteria");
    if (source) {
        auto row = query("SELECT 1 FROM messages WHERE id=? AND bot_id=?", {source, task.botId});
        if (!row.next()) throw Conflict("Task revision must come from this Bot's conversation");
    }
    exec(R"SQL(
INSERT INTO task_revisions(task_id,number,intent,done_when,source_message_id)
SELECT ?,coalesce(max(number),0)+1,?,?,? FROM task_revisions WHERE task_id=?
)SQL",
         {task.id, intent, doneWhen, source ? Json(source) : Json(nullptr), task.id});
    const auto revision = last();
    task.currentRevision = revision;
    task.rest.erase("blockerReason");
    task.rest.erase("blockerWaitId");
    task.intent = intent;
    task.doneWhen = doneWhen;
    journal(task.id, "task_revision", {{"revisionId", revision}, {"sourceMessageId", source}});
    return revision;
}

bool Store::Impl::conversationPending(std::string_view bot) const {
    // Start with the running-turn index so long Bot histories do not add work.
    return query(R"SQL(
SELECT 1 FROM conversation_turns t CROSS JOIN messages m ON m.id=t.message_id JOIN journal j ON j.id=m.id
WHERE t.status='running' AND m.bot_id=? AND j.kind='conversation_message' LIMIT 1
)SQL",
                 {bot})
        .next();
}

std::optional<Json> Store::Impl::messageReceipt(const Person& actor, std::string_view bot, std::string_view clientId, std::string_view text,
                                                Id replyTo, const Json& attachments) const {
    auto row = query("SELECT id,text,coalesce(reply_to,0) FROM messages WHERE bot_id=? AND person_id=? AND client_message_id=?",
                     {bot, actor.id, clientId});
    if (!row.next()) return std::nullopt;
    auto stored = query("SELECT payload FROM message_attachments WHERE message_id=? ORDER BY attachment_id", {row.number(0)});
    auto media = Json::array();
    while (stored.next()) media.push_back(stored.json(0));
    if (media != attachments) throw Conflict("This client message ID already has different attachments");
    return receiptRow(row, redactor(bot).text(std::string(text)), replyTo);
}

std::optional<Json> Store::receipt(const Person& actor, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                                   const Json& attachments) const {
    std::lock_guard lock(p_->mutex);
    if (reservedMessageId(clientId)) throw Conflict("This client message ID is reserved");
    if (!Policy(*p_).allows(actor, Action::bot_talk, bot)) throw DecisionDenied{};
    return p_->messageReceipt(actor, bot, clientId, text, replyTo, attachments);
}

Json Store::receive(const Person& actor, const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                    const Json& attachments) {
    return receive(
        actor, routing.botId, clientId, text, replyTo, conversation::PrincipalSource::trusted, nullptr, [&] { return routing; },
        attachments);
}

Json Store::receiveWeb(const Person& actor, const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo,
                       const Json& attachments) {
    return receive(
        actor, routing.botId, clientId, text, replyTo, conversation::PrincipalSource::web, nullptr, [&] { return routing; }, attachments);
}

Json Store::receive(const Person& actor, std::string_view bot, std::string_view clientId, std::string_view text, Id replyTo,
                    conversation::PrincipalSource source, const VerifiedChannelMessage* channel, const std::function<Intake()>& route,
                    const Json& attachments) {
    if (!channel && reservedMessageId(clientId)) throw Conflict("This client message ID is reserved");
    if (bot.empty() || !hasText(clientId) || clientId.size() > (channel ? 2048u : 256u) || !hasText(text) || text.size() > maxBodyBytes ||
        replyTo < 0)
        throw Conflict("Bot, client message ID and nonempty text are required");
    Json receipt;
    {
        std::lock_guard lock(p_->mutex);
        Transaction transaction(p_->db);
        if (!policy().allows(actor, Action::bot_decide, bot)) throw DecisionDenied{};
        const auto existing = p_->messageReceipt(actor, bot, clientId, text, replyTo, attachments);
        if (existing)
            receipt = *existing;
        else {
            if (p_->botPaused(bot)) throw BotPaused{};
            const auto routing = route();
            if (routing.botId != bot) throw Conflict("Invalid Bot conversation route");
            const auto id = p_->enqueueConversation(actor, routing, source, clientId, text, replyTo);
            p_->saveAttachments(id, attachments);
            receipt = {{"messageId", id}, {"status", "received"}};
        }
        if (channel) p_->saveChannelMessage(receipt.at("messageId"), bot, *channel, existing.has_value());
        transaction.commit();
    }
    return receipt;
}

Id Store::Impl::enqueueConversation(const Person& actor, const Intake& routing, conversation::PrincipalSource source,
                                    std::string_view clientId, std::string_view text, Id replyTo, const SessionOrigin* origin,
                                    const Json& invitation) {
    if (botPaused(routing.botId)) throw BotPaused{};
    if (!routing.config.is_object()) throw Conflict("Invalid Bot conversation route");
    initialModelCallLimit(routing.config);
    Json saved{{"environmentId", routing.environmentId}, {"projectId", routing.projectId}, {"config", routing.config}};
    const auto scrub = redactor(routing.botId);
    if (origin) {
        saved["sessionOrigin"] = {
            {"environmentId", origin->target.environmentId}, {"threadId", origin->target.threadId}, {"title", scrub.text(origin->title)}};
    } else if (replyTo && (source == conversation::PrincipalSource::trusted || source == conversation::PrincipalSource::web)) {
        auto previous = query(R"SQL(
SELECT coalesce(json_extract(t.data,'$.sessionOrigin'),json_extract(m.routing,'$.sessionOrigin')),
       json_extract(m.routing,'$.sessionInvitationDepth') FROM journal j
LEFT JOIN conversation_turns turn ON turn.reply_id=j.id
LEFT JOIN tasks t ON t.id=j.task_id AND t.bot_id=?2
JOIN messages m ON m.id=coalesce(turn.message_id,json_extract(t.data,'$.sourceMessageId'),j.id)
WHERE j.id=?1 AND m.bot_id=?2 AND m.person_id=?3
  AND (json_type(t.data,'$.sessionOrigin')='object' OR json_type(m.routing,'$.sessionOrigin')='object')
)SQL",
                              {replyTo, routing.botId, actor.id});
        if (previous.next()) {
            auto inherited = previous.json(0);
            inherited["title"] = scrub.text(field(inherited, "title"));
            saved["sessionOrigin"] = std::move(inherited);
            if (const auto depth = previous.number(1)) saved["sessionInvitationDepth"] = depth;
        }
    }
    Json event = Json::object();
    if (!invitation.is_null()) {
        event["sessionInvitation"] = scrub.record(invitation);
        saved["sessionInvitation"] = event.at("sessionInvitation");
        saved["sessionInvitationDepth"] = invitation.at("depth");
    }
    scrub.requirePublic({routing.botId, actor.id, clientId, saved});
    exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_message',?,?)",
         {unixTime(), event, routing.botId});
    const auto id = last();
    exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root,principal_source)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,?,?,?,?,? FROM messages WHERE bot_id=?
)SQL",
         {id, routing.botId, clientId, replyTo ? Json(replyTo) : Json(nullptr), scrub.text(std::string(text)), saved, actor.id,
          source == conversation::PrincipalSource::trusted && actor.adminRoot ? 1 : 0, static_cast<int>(source), routing.botId});
    exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')", {id});
    return id;
}

std::vector<std::pair<std::string, Id>> Store::Impl::pendingConversations(unsigned limit, Seconds now) const {
    std::lock_guard lock(mutex);
    auto next = query(R"SQL(
WITH heads AS (
  SELECT m.bot_id AS bot,min(t.message_id) AS id
  FROM conversation_turns t CROSS JOIN messages m ON m.id=t.message_id
  WHERE t.status='running' GROUP BY m.bot_id
)
SELECT h.bot,h.id FROM heads h LEFT JOIN conversation_retries r ON r.message_id=h.id
WHERE r.retry_at IS NULL OR r.retry_at<=? ORDER BY h.id LIMIT ?
)SQL",
                      {now, limit});
    std::vector<std::pair<std::string, Id>> result;
    while (next.next()) result.emplace_back(next.string(0), next.number(1));
    return result;
}

bool Store::Impl::isNextTurn(Id id) const {
    return query(R"SQL(
SELECT 1 WHERE ?1=(
  SELECT t.message_id FROM conversation_turns t CROSS JOIN messages m ON m.id=t.message_id
  WHERE t.status='running' AND t.message_id<=?1 AND m.bot_id=(SELECT bot_id FROM messages WHERE id=?1)
  ORDER BY t.message_id LIMIT 1)
)SQL",
                 {id})
        .next();
}

void Store::Impl::deferConversation(Id id, Seconds now) {
    // Consecutive failures back off by 1, 2, 4, 8, 16, 32, then at most 60 seconds.
    exec(R"SQL(
INSERT INTO conversation_retries(message_id,failures,retry_at) VALUES(?,1,?+1)
ON CONFLICT(message_id) DO UPDATE SET
  failures=min(failures+1,7), retry_at=?+min(1 << failures,60)
)SQL",
         {id, now, now});
}

}
