#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
using enum InboxKind;
constexpr std::pair<InboxKind,std::string_view> kinds[] = {
    {user_question,"user_question"}, {extra_run,"extra_run"}, {deadline,"deadline"},
    {model_budget,"model_budget"}, {conversation_model_budget,"conversation_model_budget"},
    {unknown_delivery,"unknown_delivery"}, {grant_request,"grant_request"},
    {protected_input,"protected_input"}, {bot_change,"bot_change"}, {pr_review,"pr_review"},
    {pr_reconciliation,"pr_reconciliation"}
};

Json key(const InboxTarget& item) {
    return {{"botId",item.botId},{"itemKind",inboxKindName(item.itemKind)},{"itemId",item.itemId},
            {"waitId",item.waitId},{"taskId",item.taskId},{"targetRevision",item.targetRevision},
            {"targetGeneration",item.targetGeneration}};
}

std::string title(std::string_view request) {
    return bounded(request.substr(0,request.find_first_of("\r\n")),160);
}

std::string taskTitle(const Json& task) {
    const auto intent = field(task,"intent");
    return title(intent.empty() ? field(task,"request") : intent);
}

Id storedId(const Json& data, const char* name) {
    const auto value = data.find(name);
    return value != data.end() && value->is_number_integer() ? value->get<Id>() : 0;
}

Id messageAnchor(const auto& store, std::string_view bot, Id message) {
    if (message <= 0) return 0;
    auto row = store.query("SELECT id FROM journal WHERE id=? AND conversation_bot_id=?"
                           " AND kind='conversation_message'",{message,bot});
    return row.next() ? row.number(0) : 0;
}

Id waitAnchor(const auto& store, Id task, Id wait, const Json& data) {
    if (const auto message = storedId(data,"messageId"); message > 0) {
        auto row = store.query(R"SQL(
SELECT id FROM journal WHERE id=? AND task_id=? AND conversation_bot_id IS NULL
AND kind='waiting' AND json_extract(data,'$.id')=?
)SQL",{message,task,wait});
        if (row.next()) return row.number(0);
    }
    // Older tasks may keep only the wait ID. Limit the lookup to recent entries for this task.
    auto row = store.query(R"SQL(
SELECT id FROM (
    SELECT id,kind,data,conversation_bot_id FROM journal WHERE task_id=? ORDER BY id DESC LIMIT 128
)
WHERE conversation_bot_id IS NULL AND kind='waiting' AND json_extract(data,'$.id')=?
ORDER BY id DESC LIMIT 1
)SQL",{task,wait});
    return row.next() ? row.number(0) : 0;
}

void addAnchor(Json& view, Id anchor) {
    if (anchor > 0) view["conversationEventId"] = anchor;
}

Json publicWait(const Json& wait) {
    if (!wait.is_object()) return nullptr;
    const auto reason = field(wait,"reason");
    Json view{{"id",wait.at("id")},{"reason",reason},{"text",field(wait,"text")}};
    if (reason == "user_question") view["choices"] = wait.value("choices",Json::array());
    return view;
}

bool validDecision(InboxKind kind, std::string_view decision) {
    switch (kind) {
    case user_question: return decision == "answer";
    case extra_run: case model_budget: case conversation_model_budget: return decision == "approve";
    case deadline: return decision == "extend";
    case unknown_delivery: return decision == "reconcile";
    case grant_request: case bot_change: return decision == "approve" || decision == "deny";
    case protected_input: return decision == "supply";
    case pr_reconciliation: return decision == "link_pr" || decision == "reject_pr";
    case pr_review: return decision == "approve" || decision == "request_changes";
    }
    return false;
}
}

std::optional<InboxKind> inboxWaitKind(std::string_view reason) {
    if (reason == "user_question") return user_question;
    if (reason == "budget") return extra_run;
    if (reason == "deadline") return deadline;
    if (reason == "model_budget") return model_budget;
    if (reason == "unknown_delivery") return unknown_delivery;
    return {};
}

std::string_view inboxKindName(InboxKind kind) {
    for (const auto& [value,name] : kinds) if (kind == value) return name;
    throw std::invalid_argument("Unknown inbox item kind");
}

InboxKind parseInboxKind(std::string_view name) {
    for (const auto& [value,label] : kinds) if (name == label) return value;
    throw std::invalid_argument("Unknown inbox item kind");
}

Role inboxDecisionRole(InboxKind kind) {
    return kind == grant_request || kind == protected_input || kind == bot_change ? Role::admin : Role::decide;
}

void Store::Impl::initializeBoardInboxSchema() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS pr_review_items(
    id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
    target_revision INTEGER NOT NULL, target_generation INTEGER NOT NULL,
    url TEXT NOT NULL, title TEXT NOT NULL, prompt TEXT NOT NULL,
    git_key TEXT, operation_id TEXT REFERENCES git_pr_operations(operation_id), candidates TEXT NOT NULL DEFAULT '[]',
    status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','changes_requested','linked','rejected','superseded')),
    UNIQUE(task_id,git_key));
CREATE UNIQUE INDEX IF NOT EXISTS pending_pr_reconciliation ON pr_review_items(operation_id) WHERE status='pending';
CREATE INDEX IF NOT EXISTS pr_reviews_by_task ON pr_review_items(task_id,id);
CREATE TABLE IF NOT EXISTS inbox_decisions(
    id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, item_kind TEXT NOT NULL, item_id INTEGER NOT NULL,
    wait_id INTEGER NOT NULL, task_id INTEGER REFERENCES tasks(id),
    target_revision INTEGER NOT NULL, target_generation INTEGER NOT NULL,
    decided_by TEXT NOT NULL, decided_at INTEGER NOT NULL, decision TEXT NOT NULL,
    UNIQUE(bot_id,item_kind,item_id,wait_id));
CREATE INDEX IF NOT EXISTS inbox_decisions_by_bot ON inbox_decisions(bot_id,id);
)SQL";
    if (sqlite3_exec(db,schema,nullptr,nullptr,nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

std::vector<std::string> Store::Impl::visibleBoardBots(const Person& actor, std::string_view bot) {
    const Access access(*this);
    if (!bot.empty()) return access.permitsPerson(actor,bot,Role::view) ? std::vector{std::string(bot)} : std::vector<std::string>{};
    auto rows = actor.adminRoot ? query(R"SQL(
SELECT bot_id FROM tasks UNION SELECT bot_id FROM messages UNION SELECT bot FROM grant_requests
UNION SELECT bot FROM protected_inputs UNION SELECT bot FROM bot_changes UNION SELECT bot_id FROM inbox_decisions
)SQL") : query("SELECT bot FROM person_bot_grants WHERE person=? ORDER BY bot",{actor.id});
    std::vector<std::string> result;
    while (rows.next()) {
        auto id = rows.string(0);
        if (access.permitsPerson(actor,id,Role::view)) result.push_back(std::move(id));
    }
    return result;
}

Json Store::inbox(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    Transaction snapshot(p_->db,Transaction::Mode::read);
    const Access access(*p_);
    auto result = Json::array();
    for (const auto& id : p_->visibleBoardBots(actor,bot)) {
        const auto scrub = p_->redactor(id);
        const bool canDecide = access.permitsPerson(actor,id,Role::decide);
        const bool canAdmin = access.permitsPerson(actor,id,Role::admin);
        const auto add = [&](const InboxTarget& target, Json view, Id anchor = 0) {
            view = scrub.record(std::move(view));
            view.update(key(target));
            view["botName"] = id;
            view["canDecide"] = inboxDecisionRole(target.itemKind) == Role::admin ? canAdmin : canDecide;
            addAnchor(view,anchor);
            result.push_back(std::move(view));
        };
        const auto addAccess = [&](InboxKind kind, Id requestId, std::string_view label, const std::string& request,
                                   std::string prompt, Id message) {
            add({id,kind,requestId,0,0,0,0},{{"requestTitle",request.empty() ? std::string(label) : title(request)},
                {"request",request},{"prompt",std::move(prompt)}},messageAnchor(*p_,id,message));
        };
        auto waits = p_->query(R"SQL(
SELECT t.id,t.generation,t.data,w.id,w.data
FROM tasks t JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL
WHERE t.bot_id=? AND t.state='waiting'
ORDER BY w.id DESC
)SQL",{id});
        while (waits.next()) {
            const auto task = waits.json(2);
            auto wait = waits.json(4);
            const auto kind = inboxWaitKind(field(wait,"reason"));
            if (!kind || !p_->waitIsActionable(task)) continue;
            const auto waitId = waits.number(3);
            wait["id"] = waitId;
            const auto view = publicWait(wait);
            add({id,*kind,waitId,waitId,waits.number(0),task.value("currentRevision",Id{0}),waits.number(1)},
                {{"requestTitle",taskTitle(task)},{"request",task.at("request")},{"prompt",view.at("text")},
                 {"choices",view.value("choices",Json::array())}},
                waitAnchor(*p_,waits.number(0),waitId,task.value("wait",Json(nullptr))));
        }
        auto turns = p_->query(R"SQL(
SELECT m.id,m.text,t.pause_id,j.data,j.kind='conversation_reply' AND j.conversation_bot_id=m.bot_id
FROM conversation_turns t
JOIN messages m ON m.id=t.message_id JOIN journal j ON j.id=t.pause_id
WHERE m.bot_id=? AND t.status='paused' ORDER BY t.pause_id DESC
)SQL",{id});
        while (turns.next()) {
            add({id,conversation_model_budget,turns.number(0),turns.number(2),0,0,turns.number(2)},
                {{"requestTitle",title(turns.string(1))},{"request",turns.string(1)},{"prompt",field(turns.json(3),"text")}},
                turns.number(4) ? turns.number(2) : 0);
        }
        auto grants = p_->query(R"SQL(
SELECT g.id,g.op,g.kind,g.resource,m.text,m.id FROM grant_requests g
LEFT JOIN messages m ON m.id=g.source_message_id AND m.bot_id=g.bot
WHERE g.bot=? AND g.status='pending' ORDER BY g.id DESC
)SQL",{id});
        while (grants.next())
            addAccess(grant_request,grants.number(0),"Resource access",grants.string(4),
                      grants.string(1)+" "+grants.string(2)+" "+grants.string(3),grants.number(5));
        auto inputs = p_->query(R"SQL(
SELECT p.id,p.name,m.text,m.id FROM protected_inputs p
LEFT JOIN messages m ON m.id=p.source_message_id AND m.bot_id=p.bot
WHERE p.bot=? AND p.status='pending'
AND NOT EXISTS (SELECT 1 FROM protected_inputs filled WHERE filled.name=p.name AND filled.status='fulfilled')
ORDER BY p.id DESC
)SQL",{id});
        while (inputs.next())
            addAccess(protected_input,inputs.number(0),"Protected input",inputs.string(2),"Supply a value for "+inputs.string(1),inputs.number(3));
        auto changes = p_->query(R"SQL(
SELECT c.id,c.operation,c.target,c.name,m.text,m.id FROM bot_changes c
LEFT JOIN messages m ON m.id=c.source_message_id AND m.bot_id=c.bot
WHERE c.bot=? AND c.status='pending' ORDER BY c.id DESC
)SQL",{id});
        while (changes.next())
            addAccess(bot_change,changes.number(0),"Bot change",changes.string(4),
                      (changes.string(1) == "create_bot" ? "Create " : "Rename ") + changes.string(2) + " as " + changes.string(3),
                      changes.number(5));
        auto reviews = p_->query(R"SQL(
SELECT p.id,p.task_id,p.target_revision,p.target_generation,p.url,p.title,p.prompt,t.data,p.operation_id,p.candidates
FROM pr_review_items p JOIN tasks t ON t.id=p.task_id
WHERE t.bot_id=? AND p.status='pending' AND p.target_generation=t.generation
AND p.target_revision=coalesce(json_extract(t.data,'$.currentRevision'),0)
AND (p.operation_id IS NOT NULL OR NOT coalesce(json_extract(t.data,'$.replanning'),0)) ORDER BY p.id DESC
)SQL",{id});
        while (reviews.next()) {
            const auto task = reviews.json(7);
            const auto kind = reviews.string(8).empty() ? pr_review : pr_reconciliation;
            add({id,kind,reviews.number(0),0,reviews.number(1),reviews.number(2),reviews.number(3)},
                {{"requestTitle",taskTitle(task)},{"request",task.at("request")},{"prompt",reviews.string(6)},
                 {"prTitle",reviews.string(5)},{"prUrl",reviews.string(4)},{"candidates",reviews.json(9)}});
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
)SQL",{target.botId,inboxKindName(target.itemKind),target.itemId,target.waitId,
       target.taskId ? Json(target.taskId) : Json(nullptr),target.targetRevision,target.targetGeneration,
       actor.id,now,decision});
    if (target.taskId) {
        auto entry = key(target);
        entry.update(Json{{"decidedBy",actor.id},{"decidedAt",now},{"decision",decision}});
        journal(target.taskId,"inbox_decision",entry);
    }
}

Json Store::decisionHistory(const Person& actor, std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    Transaction snapshot(p_->db,Transaction::Mode::read);
    auto result = Json::array();
    for (const auto& id : p_->visibleBoardBots(actor,bot)) {
        auto rows = p_->query(R"SQL(
SELECT id,item_kind,item_id,wait_id,coalesce(task_id,0),target_revision,target_generation,
       decided_by,decided_at,decision
FROM inbox_decisions WHERE bot_id=? ORDER BY id DESC LIMIT 100
)SQL",{id});
        const auto scrub = p_->redactor(id);
        while (rows.next()) {
            auto row = key({id,parseInboxKind(rows.string(1)),rows.number(2),rows.number(3),rows.number(4),
                            rows.number(5),rows.number(6)});
            row.update(Json{{"id",rows.number(0)},{"decidedBy",scrub.text(rows.string(7))},{"decidedAt",rows.number(8)},
                            {"decision",rows.string(9)},{"botName",id}});
            result.push_back(std::move(row));
        }
    }
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b) { return a.at("id") > b.at("id"); });
    if (result.size() > 100) result.erase(result.begin()+100,result.end());
    snapshot.commit();
    return result;
}

void Store::decideInbox(const Person& actor, const InboxTarget& item, std::string_view decision,
                        const Json& data, const ApplyBotChange& apply) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    auto access = this->access();
    if (item.itemId <= 0 || !data.is_object() || !validDecision(item.itemKind,decision))
        throw std::invalid_argument("Invalid inbox decision");
    const auto textInput = [&](std::string_view fieldName) {
        const auto value = data.find(fieldName);
        if (value == data.end() || !value->is_string())
            throw std::invalid_argument("Missing decision text");
        return value->get_ref<const std::string&>();
    };

    const auto kind = item.itemKind;
    if (kind == grant_request || kind == protected_input || kind == bot_change) {
        if (item.waitId || item.taskId || item.targetRevision || item.targetGeneration) throw RequestChanged{};
        if (kind == grant_request) access.decideGrant(actor,item.botId,item.itemId,decision == "approve");
        else if (kind == protected_input) access.fulfillSecret(actor,item.botId,item.itemId,textInput("value"));
        else {
            const auto [change,status] = access.authorizedBotChange(actor,item.botId,item.itemId,decision == "approve",apply);
            if (status != "pending") throw DecisionUnavailable{};
            access.decideBotChange(actor,item.botId,item.itemId,decision == "approve",change);
        }
    } else if (kind == conversation_model_budget) {
        if (!access.permitsPerson(actor,item.botId,Role::decide)) throw DecisionDenied{};
        auto paused = p_->query(R"SQL(
SELECT t.pause_id FROM conversation_turns t JOIN messages m ON m.id=t.message_id
WHERE t.status='paused' AND m.bot_id=? AND t.message_id=?
)SQL",{item.botId,item.itemId});
        if (!paused.next()) throw DecisionUnavailable{};
        if (item.taskId || item.targetRevision || item.waitId != paused.number(0) || item.targetGeneration != paused.number(0))
            throw RequestChanged{};
        p_->approveMessageModelCall(item.itemId,item.waitId);
        p_->recordInboxDecision(actor,item,decision);
    } else if (kind == pr_review || kind == pr_reconciliation) {
        if (!access.permitsPerson(actor,item.botId,Role::decide)) throw DecisionDenied{};
        if (!p_->query("SELECT 1 FROM tasks WHERE id=? AND bot_id=?",{item.taskId,item.botId}).next())
            throw RequestChanged{};
        const auto task = p_->load(item.taskId);
        auto review = p_->query("SELECT target_revision,target_generation,status,operation_id"
                                " FROM pr_review_items WHERE id=? AND task_id=?",
                                {item.itemId,item.taskId});
        if (!review.next()) throw RequestChanged{};
        if (review.string(2) != "pending") throw DecisionUnavailable{};
        if (review.string(3).empty() != (kind == pr_review)) throw RequestChanged{};
        if (item.waitId || review.number(0) != item.targetRevision || review.number(1) != item.targetGeneration ||
            task.value("currentRevision",Id{0}) != item.targetRevision || task.at("generation") != item.targetGeneration ||
            (kind == pr_review && task.value("replanning",false))) throw RequestChanged{};
        if (kind == pr_reconciliation) p_->decideGitPr(item,decision,data,task);
        else p_->exec("UPDATE pr_review_items SET status=? WHERE id=?",
                      {decision == "approve" ? "approved" : "changes_requested",item.itemId});
        p_->recordInboxDecision(actor,item,decision);
    } else {
        if (item.itemId != item.waitId) throw RequestChanged{};
        auto args = Json::object();
        if (kind == user_question) args["text"] = textInput("text");
        if (kind == deadline) args["deadline"] = data.at("deadline");
        if (kind == unknown_delivery)
            for (const auto* name : {"threadId","runId","questionId"})
                if (data.contains(name)) args[name] = textInput(name);
        p_->operate(actor,{item.botId,item.taskId,item.waitId,item.targetRevision,item.targetGeneration},decision,args,0,kind);
    }
    transaction.commit();
    if (kind == bot_change && decision == "approve") {
        try { access.applyApprovedBotChange(actor,item.botId,item.itemId,apply); }
        catch (const Conflict&) { throw BotChangePending{}; }
    }
}

Id Store::requestPrReview(Id taskId, Id revision, Id generation, std::string_view url,
                          std::string_view title, std::string_view prompt) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    const auto task = p_->load(taskId);
    if (task.value("currentRevision",Id{0}) != revision || task.at("generation") != generation ||
        task.value("replanning",false)) throw RequestChanged{};
    const auto id = p_->insertPrReview(task,url,title,prompt);
    transaction.commit();
    return id;
}

Id Store::Impl::insertPrReview(const Json& task, std::string_view url, std::string_view title, std::string_view prompt,
                               std::string_view gitKey, std::string_view operationId, const Json& candidates) {
    if (!(url.starts_with("https://") || url.starts_with("http://")) ||
        url.find_first_of(" \t\r\n") != url.npos || url.find('\0') != url.npos || url.size() > 2048 || title.empty() || title.size() > 500 ||
        prompt.empty() || prompt.size() > 8000)
        throw Conflict("A PR review needs a URL, title, and prompt");
    const auto scrub = gitKey.empty() && operationId.empty() ? redactor(field(task,"botId")) : sharedRedactor();
    scrub.requirePublic({url,candidates});
    exec("INSERT INTO pr_review_items(task_id,target_revision,target_generation,url,title,prompt,git_key,operation_id,candidates)"
         " VALUES(?,?,?,?,?,?,?,?,?)",
         {task.at("id"),task.value("currentRevision",Id{0}),task.at("generation"),url,scrub.text(title),scrub.text(prompt),
          gitKey.empty() ? Json(nullptr) : Json(gitKey),operationId.empty() ? Json(nullptr) : Json(operationId),candidates});
    return last();
}
}
