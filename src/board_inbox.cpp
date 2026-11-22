#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
using enum InboxKind;
constexpr std::pair<InboxKind,std::string_view> kinds[] = {
    {user_question,"user_question"}, {extra_run,"extra_run"}, {deadline,"deadline"},
    {model_budget,"model_budget"}, {conversation_model_budget,"conversation_model_budget"},
    {unknown_delivery,"unknown_delivery"}, {grant_request,"grant_request"},
    {protected_input,"protected_input"}, {bot_change,"bot_change"}, {pr_review,"pr_review"}
};

std::optional<InboxKind> waitKind(std::string_view reason) {
    if (reason == "user_question") return user_question;
    if (reason == "budget") return extra_run;
    if (reason == "deadline") return deadline;
    if (reason == "model_budget") return model_budget;
    if (reason == "unknown_delivery") return unknown_delivery;
    return {};
}

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

Json publicWait(const Json& wait) {
    if (!wait.is_object()) return nullptr;
    const auto reason = field(wait,"reason");
    Json view{{"id",wait.at("id")},{"reason",reason}};
    if (reason == "unknown_delivery") view["text"] = "Delivery could not be confirmed. Check the Session before proceeding.";
    else if (waitKind(reason)) view["text"] = field(wait,"text");
    if (reason == "user_question") view["choices"] = wait.value("choices",Json::array());
    return view;
}

std::string taskStatus(const Json& task, const std::string& blocker) {
    if (terminal(task) || task.at("state") == "cancelling") return field(task,"state");
    if (task.value("replanning",false)) return "replanning";
    if (!blocker.empty()) return "blocked";
    if (task.at("state") == "waiting" && !waitKind(field(task.at("wait"),"reason"))) return "blocked";
    return field(task,"state");
}

void verifyVersion(const InboxTarget& item, const Json& task) {
    if (task.at("generation") != item.targetGeneration || task.value("replanning",false))
        throw RequestChanged{};
}

bool validDecision(InboxKind kind, std::string_view decision) {
    switch (kind) {
    case user_question: return decision == "answer";
    case extra_run: case model_budget: case conversation_model_budget: return decision == "approve";
    case deadline: return decision == "extend";
    case unknown_delivery: return decision == "reconcile";
    case grant_request: case bot_change: return decision == "approve" || decision == "deny";
    case protected_input: return decision == "supply";
    case pr_review: return decision == "approve" || decision == "request_changes";
    }
    return false;
}
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

void Store::Impl::migrateBoardInbox() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS pr_review_items(
    id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
    target_revision INTEGER NOT NULL, target_generation INTEGER NOT NULL,
    url TEXT NOT NULL, title TEXT NOT NULL, prompt TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','approved','changes_requested')));
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

Json Store::board(const Person& actor, std::string_view bot, std::string_view status) const {
    std::lock_guard lock(p_->mutex);
    Transaction snapshot(p_->db,Transaction::Mode::read);
    auto result = Json::array();
    for (const auto& id : p_->visibleBoardBots(actor,bot)) {
        auto rows = p_->query(R"SQL(
SELECT t.id,t.state,t.generation,t.data,w.id,w.data,
       (SELECT json_group_array(json_object('id',p.id,'url',p.url,'title',p.title,'status',p.status))
        FROM (SELECT id,url,title,status FROM pr_review_items WHERE task_id=t.id ORDER BY id DESC) p),
       (SELECT j.time FROM journal j WHERE j.task_id=t.id ORDER BY j.id DESC LIMIT 1)
FROM tasks t LEFT JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL
WHERE t.bot_id=? ORDER BY t.id DESC
)SQL",{id});
        const auto scrub = p_->redactor(id);
        while (rows.next()) {
            auto task = rows.json(3);
            task["state"] = rows.string(1);
            auto wait = rows.number(4) ? rows.json(5) : Json(nullptr);
            if (wait.is_object()) wait["id"] = rows.number(4);
            task["wait"] = std::move(wait);
            auto blocker = field(task,"blockerReason");
            if (blocker.empty() && task.at("state") == "queued" && !p_->queueBlocker(rows.number(0)).is_null())
                blocker = "Waiting for the project to become available";
            const auto state = taskStatus(task,blocker);
            const bool matches = status.empty() || status == state ||
                (status == "waiting" && (task.at("state") == "waiting" || task.at("state") == "peer_waiting") &&
                 !task.value("replanning",false)) ||
                (status == "active" && !terminal(task)) || (status == "terminal" && terminal(task));
            if (!matches) continue;
            const auto due = task.value("deadline",Id{0});
            auto links = rows.json(6);
            auto row = scrub.record({{"requestTitle",taskTitle(task)},{"request",task.at("request")},
                {"description",task.value("intent",task.at("request"))},{"doneWhen",task.at("doneWhen")},
                {"blockerReason",blocker},{"wait",publicWait(task.at("wait"))},{"prRefs",std::move(links)},
                {"status",state},{"state",task.at("state")},{"taskId",rows.number(0)},
                {"generation",rows.number(2)},{"revisionId",task.value("currentRevision",Id{0})},
                {"deadlineAt",due > 0 ? Json(due) : Json(nullptr)},{"updatedAt",rows.number(7)}});
            row["botId"] = id;
            row["botName"] = id;
            if (row["wait"].is_object()) row["wait"]["reason"] = field(task.at("wait"),"reason");
            result.push_back(std::move(row));
        }
    }
    std::sort(result.begin(),result.end(),[](const Json& a,const Json& b) { return a.at("taskId") > b.at("taskId"); });
    snapshot.commit();
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
        const auto add = [&](const InboxTarget& target, Json view) {
            view = scrub.record(std::move(view));
            view.update(key(target));
            view["botName"] = id;
            view["canDecide"] = inboxDecisionRole(target.itemKind) == Role::admin ? canAdmin : canDecide;
            result.push_back(std::move(view));
        };
        auto waits = p_->query(R"SQL(
SELECT t.id,t.generation,t.data,w.id,w.data
FROM tasks t JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL
WHERE t.bot_id=? AND t.state='waiting' AND NOT coalesce(json_extract(t.data,'$.replanning'),0)
ORDER BY w.id DESC
)SQL",{id});
        while (waits.next()) {
            const auto task = waits.json(2);
            auto wait = waits.json(4);
            const auto kind = waitKind(field(wait,"reason"));
            if (!kind) continue;
            const auto waitId = waits.number(3);
            wait["id"] = waitId;
            const auto view = publicWait(wait);
            add({id,*kind,waitId,waitId,waits.number(0),task.value("currentRevision",Id{0}),waits.number(1)},
                {{"requestTitle",taskTitle(task)},{"request",task.at("request")},{"prompt",view.at("text")},
                 {"choices",view.value("choices",Json::array())}});
        }
        auto turns = p_->query(R"SQL(
SELECT m.id,m.text,t.pause_id,j.data FROM conversation_turns t
JOIN messages m ON m.id=t.message_id JOIN journal j ON j.id=t.pause_id
WHERE m.bot_id=? AND t.status='paused' ORDER BY t.pause_id DESC
)SQL",{id});
        while (turns.next()) {
            add({id,conversation_model_budget,turns.number(0),turns.number(2),0,0,turns.number(2)},
                {{"requestTitle",title(turns.string(1))},{"request",turns.string(1)},{"prompt",field(turns.json(3),"text")}});
        }
        auto grants = p_->query("SELECT id,op,kind,resource FROM grant_requests WHERE bot=? AND status='pending' ORDER BY id DESC",{id});
        while (grants.next())
            add({id,grant_request,grants.number(0),0,0,0,0},
                {{"requestTitle","Resource access"},{"prompt",grants.string(1)+" "+grants.string(2)+" "+grants.string(3)}});
        auto inputs = p_->query(R"SQL(
SELECT id,name FROM protected_inputs p WHERE bot=? AND status='pending'
AND NOT EXISTS (SELECT 1 FROM protected_inputs filled WHERE filled.name=p.name AND filled.status='fulfilled')
ORDER BY id DESC
)SQL",{id});
        while (inputs.next())
            add({id,protected_input,inputs.number(0),0,0,0,0},
                {{"requestTitle","Protected input"},{"prompt","Supply a value for "+inputs.string(1)}});
        auto changes = p_->query("SELECT id,operation,target,name FROM bot_changes WHERE bot=? AND status='pending' ORDER BY id DESC",{id});
        while (changes.next())
            add({id,bot_change,changes.number(0),0,0,0,0},{{"requestTitle","Bot change"},
                {"prompt",(changes.string(1) == "create_bot" ? "Create " : "Rename ") + changes.string(2) + " as " + changes.string(3)}});
        auto reviews = p_->query(R"SQL(
SELECT p.id,p.task_id,p.target_revision,p.target_generation,p.url,p.title,p.prompt
FROM pr_review_items p JOIN tasks t ON t.id=p.task_id
WHERE t.bot_id=? AND p.status='pending' AND p.target_generation=t.generation
AND p.target_revision=coalesce(json_extract(t.data,'$.currentRevision'),0)
AND NOT coalesce(json_extract(t.data,'$.replanning'),0) ORDER BY p.id DESC
)SQL",{id});
        while (reviews.next())
            add({id,pr_review,reviews.number(0),0,reviews.number(1),reviews.number(2),reviews.number(3)},
                {{"requestTitle",reviews.string(5)},{"prompt",reviews.string(6)},{"prUrl",reviews.string(4)}});
    }
    snapshot.commit();
    return result;
}

void Store::Impl::recordInboxDecision(const Person& actor, const InboxTarget& target, std::string_view decision) {
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
    if (!access.permitsPerson(actor,item.botId,inboxDecisionRole(item.itemKind))) throw DecisionDenied{};
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
        auto paused = p_->query(R"SQL(
SELECT t.pause_id FROM conversation_turns t JOIN messages m ON m.id=t.message_id
WHERE t.status='paused' AND m.bot_id=? AND t.message_id=?
)SQL",{item.botId,item.itemId});
        if (!paused.next()) throw DecisionUnavailable{};
        if (item.taskId || item.targetRevision || item.waitId != paused.number(0) || item.targetGeneration != paused.number(0))
            throw RequestChanged{};
        p_->approveMessageModelCall(item.botId,item.waitId);
        p_->recordInboxDecision(actor,item,decision);
    } else {
        if (!p_->query("SELECT 1 FROM tasks WHERE id=? AND bot_id=?",{item.taskId,item.botId}).next())
            throw RequestChanged{};
        const auto task = p_->load(item.taskId);
        if (task.value("currentRevision",Id{0}) != item.targetRevision) throw RequestChanged{};
        if (kind == pr_review) {
            auto review = p_->query("SELECT target_revision,target_generation,status FROM pr_review_items WHERE id=? AND task_id=?",
                                    {item.itemId,item.taskId});
            if (!review.next()) throw RequestChanged{};
            if (review.string(2) != "pending") throw DecisionUnavailable{};
            if (item.waitId || review.number(0) != item.targetRevision || review.number(1) != item.targetGeneration)
                throw RequestChanged{};
            verifyVersion(item,task);
            p_->exec("UPDATE pr_review_items SET status=? WHERE id=?",
                     {decision == "approve" ? "approved" : "changes_requested",item.itemId});
        } else {
            auto open = p_->query("SELECT data,resolution IS NULL FROM waits WHERE id=? AND task_id=?",{item.waitId,item.taskId});
            if (item.itemId != item.waitId || !open.next() || waitKind(field(open.json(0),"reason")) != kind)
                throw RequestChanged{};
            if (!open.number(1)) throw DecisionUnavailable{};
            verifyVersion(item,task);
            const auto& wait = task.at("wait");
            if (!wait.is_object() || wait.at("id") != item.waitId || task.at("state") != "waiting")
                throw DecisionUnavailable{};
            auto args = Json::object();
            if (kind == user_question) args["text"] = textInput("text");
            if (kind == deadline) args["deadline"] = data.at("deadline");
            if (kind == unknown_delivery)
                for (const auto* name : {"threadId","runId","questionId"})
                    if (data.contains(name)) args[name] = textInput(name);
            p_->operate(item.taskId,item.waitId,decision,p_->redactor(item.botId).payload(args));
        }
        p_->recordInboxDecision(actor,item,decision);
    }
    transaction.commit();
    if (kind == bot_change && decision == "approve") {
        try { access.applyApprovedBotChange(actor,item.botId,item.itemId,apply); }
        catch (const Conflict&) { throw BotChangePending{}; }
    }
}

Id Store::requestPrReview(Id taskId, Id revision, Id generation, std::string_view url,
                          std::string_view title, std::string_view prompt) {
    if (!(url.starts_with("https://") || url.starts_with("http://")) ||
        url.find_first_of(" \t\r\n") != url.npos || url.size() > 2048 || title.empty() || title.size() > 500 ||
        prompt.empty() || prompt.size() > 8000)
        throw Conflict("A PR review needs a URL, title, and prompt");
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    const auto task = p_->load(taskId);
    if (task.value("currentRevision",Id{0}) != revision || task.at("generation") != generation ||
        task.value("replanning",false)) throw RequestChanged{};
    const auto scrub = p_->redactor(field(task,"botId"));
    scrub.requirePublic(url);
    p_->exec("INSERT INTO pr_review_items(task_id,target_revision,target_generation,url,title,prompt) VALUES(?,?,?,?,?,?)",
             {taskId,revision,generation,url,scrub.text(std::string(title)),scrub.text(std::string(prompt))});
    const auto id = p_->last();
    transaction.commit();
    return id;
}
}
