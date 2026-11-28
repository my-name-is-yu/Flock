#include "policy_internal.hpp"
#include "flock/decision_errors.hpp"
#include "inbox_storage.hpp"
#include "task_operations.hpp"
#include "task_storage.hpp"
#include "flock/board_inbox.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "wait_storage.hpp"
#include "flock/policy.hpp"
#include "policy_approvals.hpp"
#include "secret_storage.hpp"
#include "task_records.hpp"
#include "flock/task_target.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
using enum InboxKind;
constexpr std::pair<InboxKind, std::string_view> kinds[] = {
    {user_question, "user_question"}, {protected_input, "protected_input"}, {policy_approval, "policy_approval"}};

Json key(const InboxTarget& item) {
    return {{"botId", item.botId},
            {"itemKind", inboxKindName(item.itemKind)},
            {"itemId", item.itemId},
            {"waitId", item.waitId},
            {"taskId", item.taskId},
            {"targetRevision", item.targetRevision},
            {"targetGeneration", item.targetGeneration}};
}

std::string title(std::string_view request) {
    return bounded(request.substr(0, request.find_first_of("\r\n")), 160);
}

std::string taskTitle(const TaskData& task) {
    const auto intent = task.intent;
    return title(intent.empty() ? task.request : intent);
}

Id messageAnchor(const Database& database, std::string_view bot, Id message) {
    if (message <= 0) return 0;
    auto row = database.query("SELECT id FROM journal WHERE id=? AND conversation_bot_id=?"
                              " AND kind='conversation_message'",
                              {message, bot});
    return row.next() ? row.number(0) : 0;
}

void addAnchor(Json& view, Id anchor) {
    if (anchor > 0) view["conversationEventId"] = anchor;
}

bool validDecision(InboxKind kind, std::string_view decision) {
    switch (kind) {
    case user_question: return decision == "answer";
    case protected_input: return decision == "supply";
    case policy_approval:
        return decision == "allow_once" || decision == "always_allow" || decision == "deny" || decision == "allow_next_time";
    }
    return false;
}
}

std::optional<InboxKind> inboxWaitKind(WaitReason reason) {
    if (reason == WaitReason::user_question) return user_question;
    return {};
}

std::string_view inboxKindName(InboxKind kind) {
    for (const auto& [value, name] : kinds)
        if (kind == value) return name;
    throw std::invalid_argument("Unknown inbox item kind");
}

InboxKind parseInboxKind(std::string_view name) {
    for (const auto& [value, label] : kinds)
        if (name == label) return value;
    throw std::invalid_argument("Unknown inbox item kind");
}

std::vector<std::string> InboxStorage::visibleBoardBots(const Person& actor, std::string_view bot) const {
    const auto& policy = permissions;
    if (!bot.empty()) return policy.allows(actor, Action::bot_view, bot) ? std::vector{std::string(bot)} : std::vector<std::string>{};
    auto rows =
        actor.adminRoot
            ? database.query(R"SQL(
-- Seek the next Bot ID in each index, regardless of how much history belongs to it.
WITH RECURSIVE task_bots(bot) AS (
  SELECT min(bot_id) FROM tasks
  UNION ALL SELECT (SELECT min(bot_id) FROM tasks WHERE bot_id>task_bots.bot) FROM task_bots WHERE bot IS NOT NULL
), message_bots(bot) AS (
  SELECT min(bot_id) FROM messages
  UNION ALL SELECT (SELECT min(bot_id) FROM messages WHERE bot_id>message_bots.bot) FROM message_bots WHERE bot IS NOT NULL
)
SELECT bot FROM task_bots WHERE bot IS NOT NULL UNION SELECT bot FROM message_bots WHERE bot IS NOT NULL
UNION SELECT bot FROM bot_owners UNION SELECT bot FROM bot_members
UNION SELECT bot FROM protected_inputs UNION SELECT bot FROM bot_creations UNION SELECT bot_id FROM inbox_decisions
)SQL")
            : database.query("SELECT bot FROM bot_owners WHERE person=?1 UNION SELECT bot FROM bot_members WHERE person=?1 ORDER BY bot",
                             {actor.id});
    std::vector<std::string> result;
    while (rows.next()) {
        auto id = rows.string(0);
        if (policy.allows(actor, Action::bot_view, id)) result.push_back(std::move(id));
    }
    return result;
}

Json InboxStorage::inbox(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(database.mutex);
    policyApprovals.settlePolicyApprovals(policyApprovals.now());
    Transaction snapshot(database.db, Transaction::Mode::read);
    const auto& policy = permissions;
    auto result = Json::array();
    const auto visible = visibleBoardBots(actor, bot);
    auto approvalBots = database.query("SELECT DISTINCT bot FROM policy_approvals WHERE status IN ('pending','expired')"
                                       " AND (?='' OR bot=?) AND (requester=? OR ?)",
                                       {bot, bot, actor.id, actor.adminRoot});
    auto withCards = visible;
    while (approvalBots.next())
        if (std::ranges::find(withCards, approvalBots.string(0)) == withCards.end()) withCards.push_back(approvalBots.string(0));
    for (const auto& id : withCards)
        for (auto& card : policyApprovals.policyApprovalCards(actor, id)) result.push_back(std::move(card));
    for (const auto& id : visible) {
        const auto scrub = secretStorage.redactor(id);
        const bool canDecide = policy.allows(actor, Action::bot_decide, id);
        const auto anchorTime = [&](Id anchor) -> Id {
            if (anchor <= 0) return 0;
            auto row = database.query("SELECT time FROM journal WHERE id=?", {anchor});
            return row.next() ? row.number(0) : 0;
        };
        const auto add = [&](const InboxTarget& target, Json view, Id anchor = 0, Id createdAt = 0) {
            view = scrub.record(std::move(view));
            view.update(key(target));
            view["botName"] = id;
            view["canDecide"] = canDecide;
            addAnchor(view, anchor);
            if (const auto ts = createdAt > 0 ? createdAt : anchorTime(anchor); ts > 0) view["createdAt"] = ts;
            result.push_back(std::move(view));
        };
        const auto addProtectedInput = [&](InboxKind kind, Id requestId, std::string_view label, const std::string& request,
                                           std::string prompt, Id message, Id createdAt = 0) {
            add({id, kind, requestId, 0, 0, 0, 0},
                {{"requestTitle", request.empty() ? std::string(label) : title(request)},
                 {"request", request},
                 {"prompt", std::move(prompt)}},
                messageAnchor(database, id, message), createdAt);
        };
        static const auto waitSql = "SELECT w.id,w.reason,w.data," + taskProjection() + R"SQL(
FROM tasks t JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL
WHERE t.bot_id=? AND t.state='waiting'
ORDER BY w.id DESC
)SQL";
        auto waits = database.query(waitSql.c_str(), {id});
        while (waits.next()) {
            const auto task = taskRecords.readTask(waits, 3);
            const auto wait = decodeWaitRow(waits);
            const auto waitId = wait.id;
            const auto reason = wait.reason();
            const auto kind = inboxWaitKind(reason);
            if (!kind || !waitStorage.waitIsActionable(task)) continue;
            Json view{{"requestTitle", taskTitle(task)}, {"request", task.request}, {"prompt", wait.text}, {"choices", wait.choices()}};
            add({id, *kind, waitId, waitId, task.id, task.currentRevision.value_or(0), task.generation}, std::move(view),
                waitStorage.waitAnchor(task.id, waitId));
        }
        auto inputs = database.query(R"SQL(
SELECT p.id,p.name,m.text,m.id FROM protected_inputs p
LEFT JOIN messages m ON m.id=p.source_message_id AND m.bot_id=p.bot
WHERE p.bot=? AND p.status='pending'
AND NOT EXISTS (SELECT 1 FROM protected_inputs filled WHERE filled.name=p.name AND filled.status='fulfilled')
ORDER BY p.id DESC
)SQL",
                                     {id});
        while (inputs.next())
            addProtectedInput(protected_input, inputs.number(0), "Protected input", inputs.string(2),
                              "Supply a value for " + inputs.string(1), inputs.number(3));
    }
    snapshot.commit();
    return result;
}

void InboxDecisions::record(const Person& actor, const InboxTarget& target, std::string_view decision) {
    secretStorage.redactor(target.botId).requirePublic(actor.id);
    const auto now = unixTime();
    database.exec(R"SQL(
INSERT INTO inbox_decisions(bot_id,item_kind,item_id,wait_id,task_id,target_revision,target_generation,
                            decided_by,decided_at,decision)
VALUES(?,?,?,?,?,?,?,?,?,?)
)SQL",
                  {target.botId, inboxKindName(target.itemKind), target.itemId, target.waitId,
                   target.taskId ? Json(target.taskId) : Json(nullptr), target.targetRevision, target.targetGeneration, actor.id, now,
                   decision});
    if (target.taskId) {
        auto entry = key(target);
        entry.update(Json{{"decidedBy", actor.id}, {"decidedAt", now}, {"decision", decision}});
        database.writeJournal(target.taskId, "inbox_decision", entry);
    }
}

Json InboxStorage::decisionHistory(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(database.mutex);
    Transaction snapshot(database.db, Transaction::Mode::read);
    auto result = Json::array();
    for (const auto& id : visibleBoardBots(actor, bot)) {
        // Join what was decided on, so History can say what each response was about.
        auto rows = database.query(R"SQL(
SELECT d.id,d.item_kind,d.item_id,d.wait_id,coalesce(d.task_id,0),d.target_revision,d.target_generation,
       d.decided_by,d.decided_at,d.decision,
       coalesce(nullif(r.intent,''),nullif(t.request,''),m.text,''),
       coalesce(a.action,''),coalesce(a.target,'')
FROM inbox_decisions d
LEFT JOIN tasks t ON t.id=d.task_id AND t.bot_id=d.bot_id
LEFT JOIN task_revisions r ON r.id=d.target_revision AND r.task_id=t.id
LEFT JOIN policy_approvals a ON d.item_kind='policy_approval' AND a.id=d.item_id AND a.bot=d.bot_id
LEFT JOIN protected_inputs p ON d.item_kind='protected_input' AND p.id=d.item_id AND p.bot=d.bot_id
LEFT JOIN messages m ON m.bot_id=d.bot_id AND m.id=CASE d.item_kind
    WHEN 'protected_input' THEN p.source_message_id
    WHEN 'policy_approval' THEN a.anchor
END AND (d.item_kind<>'policy_approval' OR NOT EXISTS (SELECT 1 FROM channel_messages c WHERE c.message_id=m.id))
WHERE d.bot_id=? ORDER BY d.id DESC LIMIT 100
)SQL",
                                   {id});
        const auto scrub = secretStorage.redactor(id);
        while (rows.next()) {
            auto row =
                key({id, parseInboxKind(rows.string(1)), rows.number(2), rows.number(3), rows.number(4), rows.number(5), rows.number(6)});
            const auto decidedBy = rows.string(7);
            row.update(Json{{"id", rows.number(0)},
                            {"decidedBy", scrub.text(decidedBy)},
                            {"decidedByYou", decidedBy == actor.id},
                            {"decidedAt", rows.number(8)},
                            {"decision", rows.string(9)},
                            {"botName", id},
                            {"requestTitle", title(scrub.text(rows.string(10)))},
                            {"action", rows.string(11)},
                            {"target", scrub.text(rows.string(12))}});
            result.push_back(std::move(row));
        }
    }
    std::sort(result.begin(), result.end(), [](const Json& a, const Json& b) { return a.at("id") > b.at("id"); });
    if (result.size() > 100) result.erase(result.begin() + 100, result.end());
    snapshot.commit();
    return result;
}

void InboxStorage::decideInbox(const Person& actor, const InboxTarget& item, std::string_view decision, const Json& data) {
    std::lock_guard lock(database.mutex);
    policyApprovals.settlePolicyApprovals(policyApprovals.now());
    Transaction transaction(database.db);
    auto& policy = permissions;
    if (item.itemId <= 0 || !data.is_object() || !validDecision(item.itemKind, decision))
        throw std::invalid_argument("Invalid inbox decision");
    const auto textInput = [&](std::string_view fieldName) {
        const auto value = data.find(fieldName);
        if (value == data.end() || !value->is_string()) throw std::invalid_argument("Missing decision text");
        return value->get_ref<const std::string&>();
    };

    const auto kind = item.itemKind;
    if (kind == policy_approval) {
        policyApprovals.decidePolicyApproval(actor, item, decision);
    } else if (kind == protected_input) {
        if (item.waitId || item.taskId || item.targetRevision || item.targetGeneration) throw RequestChanged{};
        policy_storage::fulfillSecret(database, policy, secretStorage, inboxDecisions, actor, item.botId, item.itemId, textInput("value"));
    } else {
        if (item.itemId != item.waitId) throw RequestChanged{};
        taskOperations.operate(actor, {item.botId, item.taskId, item.waitId, item.targetRevision, item.targetGeneration}, decision,
                               {{"text", textInput("text")}}, 0, kind);
    }
    transaction.commit();
}

Json Inbox::inbox(const Person& actor, std::string_view bot) const {
    return inbox_.inbox(actor, bot);
}

Json Inbox::decisionHistory(const Person& actor, std::string_view bot) const {
    return inbox_.decisionHistory(actor, bot);
}

void Inbox::decideInbox(const Person& actor, const InboxTarget& item, std::string_view decision, const Json& data) {
    inbox_.decideInbox(actor, item, decision, data);
}
}
