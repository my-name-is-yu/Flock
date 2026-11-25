#include "storage.hpp"
#include "journal_peer.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"

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
constexpr std::pair<InboxKind, std::string_view> kinds[] = {{user_question, "user_question"},
                                                            {extra_run, "extra_run"},
                                                            {deadline, "deadline"},
                                                            {model_budget, "model_budget"},
                                                            {conversation_model_budget, "conversation_model_budget"},
                                                            {unknown_delivery, "unknown_delivery"},
                                                            {protected_input, "protected_input"},
                                                            {pr_review, "pr_review"},
                                                            {pr_reconciliation, "pr_reconciliation"},
                                                            {policy_approval, "policy_approval"}};

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

Id messageAnchor(const auto& store, std::string_view bot, Id message) {
    if (message <= 0) return 0;
    auto row = store.query("SELECT id FROM journal WHERE id=? AND conversation_bot_id=?"
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
    case extra_run:
    case model_budget:
    case conversation_model_budget: return decision == "approve";
    case deadline: return decision == "extend";
    case unknown_delivery: return decision == "reconcile";
    case protected_input: return decision == "supply";
    case pr_reconciliation: return decision == "link_pr" || decision == "reject_pr";
    case policy_approval:
        return decision == "allow_once" || decision == "always_allow" || decision == "deny" || decision == "allow_next_time";
    case pr_review: return decision == "approve" || decision == "request_changes";
    }
    return false;
}
}

std::optional<InboxKind> inboxWaitKind(WaitReason reason) {
    if (reason == WaitReason::user_question) return user_question;
    if (reason == WaitReason::budget) return extra_run;
    if (reason == WaitReason::deadline) return deadline;
    if (reason == WaitReason::model_budget) return model_budget;
    if (reason == WaitReason::unknown_delivery) return unknown_delivery;
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

std::vector<std::string> Store::Impl::visibleBoardBots(const Person& actor, std::string_view bot) {
    const Policy policy(*this);
    if (!bot.empty()) return policy.allows(actor, Action::bot_view, bot) ? std::vector{std::string(bot)} : std::vector<std::string>{};
    auto rows = actor.adminRoot
                    ? query(R"SQL(
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
UNION SELECT bot FROM protected_inputs UNION SELECT bot FROM bot_changes UNION SELECT bot_id FROM inbox_decisions
)SQL")
                    : query("SELECT bot FROM bot_owners WHERE person=?1 UNION SELECT bot FROM bot_members WHERE person=?1 ORDER BY bot",
                            {actor.id});
    std::vector<std::string> result;
    while (rows.next()) {
        auto id = rows.string(0);
        if (policy.allows(actor, Action::bot_view, id)) result.push_back(std::move(id));
    }
    return result;
}

Statement Store::Impl::actionablePrReviews(std::string_view bot) const {
    static const auto sql = R"SQL(
SELECT p.id,p.url,p.title,p.prompt,p.operation_id IS NOT NULL,p.candidates,j.id,)SQL" +
                            taskProjection() + R"SQL(
FROM pr_review_items p JOIN tasks t ON t.id=p.task_id
LEFT JOIN journal j ON j.task_id=p.task_id AND j.kind='pr_review_requested'
AND j.pr_review_item_id=p.id AND p.operation_id IS NULL
WHERE t.bot_id=? AND p.status='pending' AND (p.operation_id IS NOT NULL OR (
    p.target_revision=coalesce(t.current_revision,0)
    AND NOT t.replanning)) ORDER BY p.id DESC
)SQL";
    return query(sql.c_str(), {bot});
}

Json Store::inbox(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    p_->settlePolicyApprovals(p_->policyClock());
    Transaction snapshot(p_->db, Transaction::Mode::read);
    const Policy policy(*p_);
    auto result = Json::array();
    const auto visible = p_->visibleBoardBots(actor, bot);
    auto approvalBots = p_->query("SELECT DISTINCT bot FROM policy_approvals WHERE status IN ('pending','expired')"
                                  " AND (?='' OR bot=?) AND (requester=? OR ?)",
                                  {bot, bot, actor.id, actor.adminRoot});
    auto withCards = visible;
    while (approvalBots.next())
        if (std::ranges::find(withCards, approvalBots.string(0)) == withCards.end()) withCards.push_back(approvalBots.string(0));
    for (const auto& id : withCards)
        for (auto& card : p_->policyApprovalCards(actor, id)) result.push_back(std::move(card));
    for (const auto& id : visible) {
        const auto scrub = p_->redactor(id);
        const bool canDecide = policy.allows(actor, Action::bot_decide, id);
        const auto anchorTime = [&](Id anchor) -> Id {
            if (anchor <= 0) return 0;
            auto row = p_->query("SELECT time FROM journal WHERE id=?", {anchor});
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
                messageAnchor(*p_, id, message), createdAt);
        };
        static const auto waitSql = "SELECT w.id,w.reason,w.data," + taskProjection() + R"SQL(
FROM tasks t JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL
WHERE t.bot_id=? AND t.state='waiting'
ORDER BY w.id DESC
)SQL";
        auto waits = p_->query(waitSql.c_str(), {id});
        while (waits.next()) {
            const auto task = p_->readTask(waits, 3);
            const auto wait = decodeWaitRow(waits);
            const auto waitId = wait.id;
            const auto reason = wait.reason();
            const auto kind = inboxWaitKind(reason);
            if (!kind || !p_->waitIsActionable(task)) continue;
            Json view{{"requestTitle", taskTitle(task)}, {"request", task.request}, {"prompt", wait.text}, {"choices", wait.choices()}};
            if (const auto allowance = p_->waitAllowance(task.id, reason); allowance) view["allowance"] = allowance;
            add({id, *kind, waitId, waitId, task.id, task.currentRevision.value_or(0), task.generation}, std::move(view),
                p_->waitAnchor(task.id, waitId));
        }
        auto turns = p_->query(R"SQL(
SELECT m.id,m.text,t.pause_id,j.data,j.kind='conversation_reply' AND j.conversation_bot_id=m.bot_id
-- Seek the populated pause IDs instead of visiting all messages for the Bot.
FROM conversation_turns t
CROSS JOIN messages m ON m.id=t.message_id CROSS JOIN journal j ON j.id=t.pause_id
WHERE t.pause_id IS NOT NULL AND t.status='paused' AND m.bot_id=? ORDER BY t.pause_id DESC
)SQL",
                               {id});
        while (turns.next()) {
            const auto message = turns.number(0);
            const auto request = turns.string(1);
            add({id, conversation_model_budget, message, turns.number(2), 0, 0, turns.number(2)},
                {{"requestTitle", title(request)},
                 {"request", request},
                 {"prompt", field(turns.json(3), "text")},
                 {"allowance", p_->messageModelBudget(message).initialLimit}},
                turns.number(4) ? turns.number(2) : 0);
        }
        auto inputs = p_->query(R"SQL(
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
        auto reviews = p_->actionablePrReviews(id);
        while (reviews.next()) {
            const auto task = p_->readTask(reviews, 7);
            const auto kind = reviews.number(4) ? pr_reconciliation : pr_review;
            add({id, kind, reviews.number(0), 0, task.id, task.currentRevision.value_or(0), task.generation},
                {{"requestTitle", taskTitle(task)},
                 {"request", task.request},
                 {"prompt", reviews.string(3)},
                 {"prTitle", reviews.string(2)},
                 {"prUrl", reviews.string(1)},
                 {"candidates", reviews.json(5)}},
                reviews.number(6));
        }
    }
    snapshot.commit();
    return result;
}

void Store::Impl::recordInboxDecision(const Person& actor, const InboxTarget& target, std::string_view decision) {
    redactor(target.botId).requirePublic(actor.id);
    const auto now = unixTime();
    exec(R"SQL(
INSERT INTO inbox_decisions(bot_id,item_kind,item_id,wait_id,task_id,target_revision,target_generation,
                            decided_by,decided_at,decision)
VALUES(?,?,?,?,?,?,?,?,?,?)
)SQL",
         {target.botId, inboxKindName(target.itemKind), target.itemId, target.waitId, target.taskId ? Json(target.taskId) : Json(nullptr),
          target.targetRevision, target.targetGeneration, actor.id, now, decision});
    if (target.taskId) {
        auto entry = key(target);
        entry.update(Json{{"decidedBy", actor.id}, {"decidedAt", now}, {"decision", decision}});
        journal(target.taskId, "inbox_decision", entry);
    }
}

Json Store::decisionHistory(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    Transaction snapshot(p_->db, Transaction::Mode::read);
    auto result = Json::array();
    for (const auto& id : p_->visibleBoardBots(actor, bot)) {
        // Join what was decided on, so History can say what each response was about.
        auto rows = p_->query(R"SQL(
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
    WHEN 'conversation_model_budget' THEN d.item_id
    WHEN 'protected_input' THEN p.source_message_id
    WHEN 'policy_approval' THEN a.anchor
END AND (d.item_kind<>'policy_approval' OR NOT EXISTS (SELECT 1 FROM channel_messages c WHERE c.message_id=m.id))
WHERE d.bot_id=? ORDER BY d.id DESC LIMIT 100
)SQL",
                              {id});
        const auto scrub = p_->redactor(id);
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

void Store::decideInbox(const Person& actor, const InboxTarget& item, std::string_view decision, const Json& data) {
    std::lock_guard lock(p_->mutex);
    p_->settlePolicyApprovals(p_->policyClock());
    Transaction transaction(p_->db);
    auto policy = this->policy();
    if (item.itemId <= 0 || !data.is_object() || !validDecision(item.itemKind, decision))
        throw std::invalid_argument("Invalid inbox decision");
    const auto textInput = [&](std::string_view fieldName) {
        const auto value = data.find(fieldName);
        if (value == data.end() || !value->is_string()) throw std::invalid_argument("Missing decision text");
        return value->get_ref<const std::string&>();
    };

    const auto kind = item.itemKind;
    if (kind == policy_approval) {
        p_->decidePolicyApproval(actor, item, decision);
    } else if (kind == protected_input) {
        if (item.waitId || item.taskId || item.targetRevision || item.targetGeneration) throw RequestChanged{};
        policy.fulfillSecret(actor, item.botId, item.itemId, textInput("value"));
    } else if (kind == conversation_model_budget) {
        if (!policy.allows(actor, Action::bot_decide, item.botId)) throw DecisionDenied{};
        auto paused = p_->query(R"SQL(
SELECT t.pause_id FROM conversation_turns t JOIN messages m ON m.id=t.message_id
WHERE t.status='paused' AND m.bot_id=? AND t.message_id=?
)SQL",
                                {item.botId, item.itemId});
        if (!paused.next()) throw DecisionUnavailable{};
        if (item.taskId || item.targetRevision || item.waitId != paused.number(0) || item.targetGeneration != paused.number(0))
            throw RequestChanged{};
        p_->approveMessageModelCall(actor, item.botId, item.itemId, item.waitId);
        p_->recordInboxDecision(actor, item, decision);
    } else if (kind == pr_review || kind == pr_reconciliation) {
        if (!policy.allows(actor, Action::bot_decide, item.botId)) throw DecisionDenied{};
        if (!p_->query("SELECT 1 FROM tasks WHERE id=? AND bot_id=?", {item.taskId, item.botId}).next()) throw RequestChanged{};
        const auto task = p_->load(item.taskId);
        auto review = p_->query("SELECT target_revision,status,operation_id IS NOT NULL"
                                " FROM pr_review_items WHERE id=? AND task_id=?",
                                {item.itemId, item.taskId});
        if (!review.next()) throw RequestChanged{};
        if (review.string(1) != "pending") throw DecisionUnavailable{};
        if (review.number(2) != (kind == pr_reconciliation) || item.waitId || task.currentRevision.value_or(0) != item.targetRevision ||
            task.generation != item.targetGeneration)
            throw RequestChanged{};
        if (kind == pr_review && (review.number(0) != item.targetRevision || task.replanning)) throw RequestChanged{};
        if (kind == pr_reconciliation)
            p_->decideGitPr(item, decision, data, task);
        else {
            p_->exec("UPDATE pr_review_items SET status=? WHERE id=?",
                     {decision == "approve" ? "approved" : "changes_requested", item.itemId});
            p_->exec("DELETE FROM git_review_receipts WHERE inbox_key=(SELECT git_key FROM pr_review_items WHERE id=?)", {item.itemId});
        }
        p_->recordInboxDecision(actor, item, decision);
    } else {
        if (item.itemId != item.waitId) throw RequestChanged{};
        auto args = Json::object();
        if (kind == user_question) args["text"] = textInput("text");
        if (kind == deadline) args["deadline"] = data.at("deadline");
        if (kind == unknown_delivery)
            for (const auto* name : {"threadId", "runId", "questionId"})
                if (data.contains(name)) args[name] = textInput(name);
        p_->operate(actor, {item.botId, item.taskId, item.waitId, item.targetRevision, item.targetGeneration}, decision, args, 0, kind);
    }
    transaction.commit();
}

void Store::Impl::insertPrReview(const TaskData& task, std::string_view url, std::string_view title, std::string_view prompt,
                                 std::string_view gitKey, std::string_view operationId, const Json& candidates) {
    if (!(url.starts_with("https://") || url.starts_with("http://")) || url.find_first_of(" \t\r\n") != url.npos ||
        url.find('\0') != url.npos || url.size() > 2048 || title.empty() || title.size() > 500 || prompt.empty() || prompt.size() > 8000)
        throw Conflict("A PR review needs a URL, title, and prompt");
    const auto scrub = sharedRedactor();
    scrub.requirePublic({url, candidates});
    const auto savedTitle = scrub.text(title);
    const auto savedPrompt = scrub.text(prompt);
    exec("INSERT INTO pr_review_items(task_id,target_revision,url,title,prompt,git_key,operation_id,candidates)"
         " VALUES(?,?,?,?,?,?,?,?)",
         {task.id, task.currentRevision.value_or(0), url, savedTitle, savedPrompt, gitKey.empty() ? Json(nullptr) : Json(gitKey),
          operationId.empty() ? Json(nullptr) : Json(operationId), candidates});
    if (operationId.empty()) journal(task.id, PrReviewRequestedEntry{last(), savedTitle, savedPrompt, std::string(url)});
}
}
