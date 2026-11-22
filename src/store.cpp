#include "storage.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <system_error>

namespace flockv0 {
namespace {
[[noreturn]] void sqlError(sqlite3* db) { throw std::runtime_error(sqlite3_errmsg(db)); }
}
Statement::Statement(sqlite3* db, const char* sql, const Json& values) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) sqlError(db);
    try {
        int i = 0;
        for (const auto& value : values) {
            ++i;
            int rc;
            if (value.is_null()) rc = sqlite3_bind_null(stmt_, i);
            else if (value.is_number_integer()) rc = sqlite3_bind_int64(stmt_, i, value.get<Id>());
            else {
                const auto encoded = value.is_string() ? value.get<std::string>() : value.dump();
                rc = sqlite3_bind_text(stmt_, i, encoded.data(), static_cast<int>(encoded.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) sqlError(db);
        }
    } catch (...) { sqlite3_finalize(stmt_); throw; }
}
bool Statement::next() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    sqlError(sqlite3_db_handle(stmt_));
}
std::string Statement::string(int col) const {
    const auto* data = sqlite3_column_text(stmt_, col);
    return data ? std::string(reinterpret_cast<const char*>(data), sqlite3_column_bytes(stmt_, col)) : std::string{};
}
Seconds unixTime() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}
std::string bounded(std::string_view text, std::size_t bytes) {
    if (text.size() <= bytes) return std::string(text);
    while (bytes && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80) --bytes;
    return std::string(text.substr(0, bytes));
}
std::string field(const Json& j, std::string_view key) {
    const auto found = j.find(key);
    return found != j.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
bool terminal(const Json& j) { return j["state"] == "completed" || j["state"] == "cancelled"; }
std::string actionId() {
    std::random_device random;
    constexpr std::string_view hex = "0123456789abcdef";
    std::string id = "flock-";
    for (int i = 0; i < 16; ++i) {
        const auto octet = random() & 0xff;
        id += hex[octet >> 4];
        id += hex[octet & 15];
    }
    return id;
}
Json runJson(const RunRef& run) {
    return {{"threadId", run.threadId}, {"runId", run.runId}, {"link", run.link}};
}
RunRef runRef(const Json& data) {
    return {field(data,"threadId"), field(data,"runId"), field(data,"link")};
}

Json questionTarget(const Json& task) {
    const auto& wait = task.at("wait");
    return {{"waitId",field(wait,"reason") == "user_question" ? wait.at("id") : Json(nullptr)},
        {"eventId",task.at("eventId")},{"run",task.at("activeRun")},{"requestId",field(task.at("question"),"requestId")}};
}

Store::Impl::Impl(std::filesystem::path file) : path(std::move(file)) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    try {
        if (rc != SQLITE_OK) sqlError(db);
        sqlite3_busy_timeout(db, 4000);
        const char* schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
PRAGMA synchronous=FULL;
CREATE TABLE IF NOT EXISTS tasks(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL,
  project_id TEXT NOT NULL, schedule_id TEXT, scheduled_at INTEGER,
  state TEXT NOT NULL, generation INTEGER NOT NULL, data TEXT NOT NULL,
  UNIQUE(bot_id,schedule_id,scheduled_at));
CREATE INDEX IF NOT EXISTS tasks_route ON tasks(environment_id,project_id,id)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS tasks_bot ON tasks(bot_id,id DESC);
CREATE TABLE IF NOT EXISTS journal(
  id INTEGER PRIMARY KEY, task_id INTEGER REFERENCES tasks(id),
  time INTEGER NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL, conversation_bot_id TEXT,
  CHECK(task_id IS NOT NULL OR conversation_bot_id IS NOT NULL));
CREATE INDEX IF NOT EXISTS journal_task ON journal(task_id,id);
CREATE TABLE IF NOT EXISTS events(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  dedupe TEXT NOT NULL, consumed INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
  retry_at INTEGER NOT NULL DEFAULT 0, data TEXT NOT NULL, evidence TEXT NOT NULL DEFAULT '[]',
  UNIQUE(task_id,dedupe));
CREATE TABLE IF NOT EXISTS t3_events(
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, item_type TEXT NOT NULL, item_id TEXT NOT NULL,
  PRIMARY KEY(environment_id,thread_id,item_type,item_id));
CREATE TABLE IF NOT EXISTS waits(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  data TEXT NOT NULL, resolution TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS waits_open ON waits(task_id) WHERE resolution IS NULL;
CREATE TABLE IF NOT EXISTS outbox(
  id TEXT PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  phase TEXT NOT NULL, auth_generation TEXT NOT NULL DEFAULT '', data TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS runs(
  action_id TEXT PRIMARY KEY REFERENCES outbox(id), task_id INTEGER NOT NULL REFERENCES tasks(id),
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, run_id TEXT NOT NULL, link TEXT NOT NULL,
  UNIQUE(environment_id,thread_id,run_id));
CREATE INDEX IF NOT EXISTS runs_task ON runs(task_id,action_id);
CREATE TABLE IF NOT EXISTS peer_exchanges(
  id TEXT PRIMARY KEY, kind TEXT NOT NULL, phase TEXT NOT NULL, last_message TEXT NOT NULL,
  sender_task INTEGER NOT NULL REFERENCES tasks(id),
  recipient_task INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
  sender_environment TEXT NOT NULL, sender_revision INTEGER);
CREATE UNIQUE INDEX IF NOT EXISTS peer_sender_open ON peer_exchanges(sender_task) WHERE phase<>'closed';
CREATE VIEW IF NOT EXISTS project_execution_queue AS
WITH candidates AS (
  SELECT task.id,task.project_id,task.state,task.data,
         CASE WHEN json_extract(task.data,'$.environmentReleased')
              THEN COALESCE(json_extract(task.data,'$.pendingEnvironmentId'),task.environment_id)
              ELSE task.environment_id END AS environment_id
  FROM tasks task
  WHERE task.state NOT IN ('completed','cancelled')
)
SELECT task.id,task.environment_id,task.project_id,task.state,
       ROW_NUMBER() OVER (
         PARTITION BY task.environment_id,task.project_id
         ORDER BY (COALESCE(sender.state,task.state)='queued'
                   OR COALESCE(json_extract(task.data,'$.environmentReleased'),0)),
           COALESCE(sender.id,task.id),
           CASE WHEN sender.id IS NOT NULL AND task.state='cancelling' THEN 0
                WHEN task.state='peer_waiting' THEN 2 ELSE 1 END, task.id
       ) AS position
FROM candidates task
LEFT JOIN peer_exchanges peer ON peer.recipient_task=task.id
LEFT JOIN candidates sender ON sender.id=peer.sender_task AND sender.environment_id=task.environment_id
  AND sender.project_id=task.project_id AND peer.phase<>'closed'
WHERE COALESCE(peer.kind,'')<>'consult';
CREATE VIEW IF NOT EXISTS project_task_order AS
SELECT task.id, task.environment_id, task.project_id, task.state,
       ROW_NUMBER() OVER (
         PARTITION BY task.environment_id,task.project_id
         ORDER BY COALESCE(sender.id,task.id),
           CASE WHEN sender.id IS NOT NULL AND task.state='cancelling' THEN 0
                WHEN task.state='peer_waiting' THEN 2 ELSE 1 END, task.id
       ) AS position
FROM tasks task
LEFT JOIN peer_exchanges peer ON peer.recipient_task=task.id
LEFT JOIN tasks sender ON sender.id=peer.sender_task AND sender.environment_id=task.environment_id
  AND sender.project_id=task.project_id AND sender.state NOT IN ('completed','cancelled')
WHERE task.state NOT IN ('completed','cancelled') AND COALESCE(peer.kind,'')<>'consult';
)SQL";
        if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) sqlError(db);
        {
            Transaction transaction(db);
            if (!query("SELECT 1 FROM pragma_table_info('peer_exchanges') WHERE name='sender_environment'").next()) {
                // Older exchanges have no recorded sender route and cannot attest to the current one.
                exec("ALTER TABLE peer_exchanges ADD COLUMN sender_environment TEXT NOT NULL DEFAULT ''");
                exec("ALTER TABLE peer_exchanges ADD COLUMN sender_revision INTEGER");
            }
            transaction.commit();
        }
        migrateConversation();
        migrateModelBudget();
        exec("CREATE INDEX IF NOT EXISTS journal_peer_message ON journal(task_id,json_extract(data,'$.exchangeId'),id)"
             " WHERE kind='bot_message'");
    } catch (...) { sqlite3_close(db); throw; }
}
Store::Impl::~Impl() { sqlite3_close(db); }
void Store::Impl::exec(const char* sql, const Json& args) const { query(sql, args).next(); }
Json Store::Impl::load(Id id) const {
    auto row = query("SELECT data,state,generation FROM tasks WHERE id=?", {id});
    if (!row.next()) throw Conflict("Task does not exist");
    auto result = row.json(0);
    result["id"] = id;
    result["state"] = row.string(1);
    result["generation"] = row.number(2);
    return result;
}
void Store::Impl::save(Json& task) {
    task["generation"] = task["generation"].get<Id>() + 1;
    exec("UPDATE tasks SET state=?,generation=?,data=?,environment_id=? WHERE id=?",
         {task["state"], task["generation"], task, task["environmentId"], task["id"]});
}
Json Store::Impl::queueBlocker(Id id) const {
    auto row = query("SELECT head.id,owner.bot_id,json_extract(owner.data,'$.request'),head.state"
                     " FROM project_execution_queue head JOIN project_execution_queue task"
                     " USING(environment_id,project_id) JOIN tasks owner ON owner.id=head.id"
                     " WHERE task.id=? AND head.position=1 AND head.id<>task.id", {id});
    if (!row.next()) return nullptr;
    return {{"id",row.number(0)},{"botId",row.string(1)},{"request",row.string(2)},{"state",row.string(3)}};
}
Id Store::Impl::journal(Id id, std::string_view kind, const Json& data) {
    exec("INSERT INTO journal(task_id,time,kind,data) VALUES(?,?,?,?)", {id, unixTime(), kind, data});
    return last();
}
std::optional<PeerExchange> Store::Impl::peerExchange(Id id) const {
    auto rows = query("SELECT id,kind,phase,last_message,sender_task,recipient_task FROM peer_exchanges"
                      " WHERE phase<>'closed' AND sender_task IS NOT NULL AND (sender_task=? OR recipient_task=?)", {id,id});
    if (!rows.next()) return std::nullopt;
    return PeerExchange{rows.string(0),rows.string(1),rows.string(2),rows.string(3),rows.number(4),rows.number(5)};
}

void Store::Impl::peerMessage(const PeerExchange& exchange, const Json& message) {
    journal(exchange.sender,"bot_message",message);
    journal(exchange.recipient,"bot_message",message);
}

void Store::Impl::requestCancellation(Json& task) {
    const Id id = task["id"];
    const auto peer = peerExchange(id);
    if (!task.value("pendingInstructions",Json::array()).empty())
        task["cancelledInstructions"] = task.at("pendingInstructions");
    clearWait(task,"cancel");
    consumeEvent(task);
    journal(id,"cancel_requested",Json::object());
    if (task["state"] == "queued" && !peer) {
        for (const auto* key : {"replanning","pendingEnvironmentId","environmentReleased","pendingInstructions"})
            task.erase(key);
        task["state"] = "cancelled";
        journal(id,"cancelled",Json::object());
    } else task["state"] = "cancelling";
    save(task);
    if (peer && peer->sender == id) {
        auto recipient = load(peer->recipient);
        if (!terminal(recipient) && recipient["state"] != "cancelling") requestCancellation(recipient);
    }
}

Id Store::Impl::event(Json& task, std::string_view key, Json data) {
    exec("INSERT OR IGNORE INTO events(task_id,dedupe,data) VALUES(?,?,?)", {task["id"], key, data});
    if (!sqlite3_changes(db)) return 0;
    const auto id = last();
    task["eventId"] = id;
    return id;
}
Json Store::Impl::currentEvent(const Json& task) const {
    const auto id = task.value("eventId", Id{0});
    auto row = query("SELECT data,failures,retry_at,evidence,consumed FROM events WHERE id=? AND task_id=?",
                     {id, task["id"]});
    if (!row.next()) return nullptr;
    auto result = row.json(0);
    result["id"] = id;
    result["failures"] = row.number(1);
    result["retryAt"] = row.number(2);
    result["evidence"] = row.json(3);
    result["consumed"] = row.number(4) != 0;
    return result;
}
void Store::Impl::consumeEvent(const Json& task) {
    exec("UPDATE events SET consumed=1 WHERE id=? AND task_id=?", {task["eventId"], task["id"]});
}
void Store::Impl::wait(Json& task, std::string_view reason, Json data) {
    clearWait(task, "superseded");
    data["reason"] = reason;
    data["eventId"] = task.value("eventId", Id{});
    data["run"] = task["activeRun"];
    if (data["run"].is_null() && reason == "unknown_delivery") {
        const auto action = outbox(task);
        if (action.is_object() && action.contains("candidate") && !field(action["candidate"],"link").empty())
            data["run"] = action["candidate"];
    }
    if (task.contains("actionId")) data["actionId"] = task["actionId"];
    exec("INSERT INTO waits(task_id,data) VALUES(?,?)", {task["id"], data});
    data["id"] = last();
    data["messageId"] = journal(task["id"], "waiting", data);
    task["wait"] = data;
    task["state"] = "waiting";
}
void Store::Impl::clearWait(Json& task, std::string_view resolution) {
    const auto wait = task.find("wait");
    if (wait == task.end() || wait->is_null()) return;
    const Json data = {{"waitId", (*wait)["id"]}, {"resolution", resolution}};
    exec("UPDATE waits SET resolution=? WHERE id=? AND resolution IS NULL", {resolution, (*wait)["id"]});
    task["wait"] = nullptr;
    journal(task["id"], "wait_closed", data);
}
Json Store::Impl::outbox(const Json& task) const {
    if (field(task, "actionId").empty()) return nullptr;
    auto row = query("SELECT data,phase,auth_generation FROM outbox WHERE id=? AND task_id=?",
                     {task["actionId"], task["id"]});
    if (!row.next()) return nullptr;
    auto result = row.json(0);
    result["phase"] = row.string(1);
    result["authGeneration"] = row.string(2);
    return result;
}
void Store::Impl::unlockQuestion(Json& task, std::string_view reason) {
    if (task["question"].is_null()) return;
    journal(task["id"], "session_question_closed", {{"requestId", task["question"]["requestId"]}, {"resolution", reason}});
    task["question"] = nullptr;
}

Store::Store(std::filesystem::path db) : p_(std::make_unique<Impl>(std::move(db))) {}
Store::~Store() = default;
const std::filesystem::path& Store::path() const { return p_->path; }

Id Store::submit(const Intake& item) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    const auto id = p_->submit(item);
    transaction.commit();
    return id;
}

Id Store::Impl::submit(const Intake& item, std::string_view peerKind, Id sourceMessageId, const Json& source) {
    if (item.botId.empty() || item.request.find_first_not_of(" \r\n\t") == std::string::npos ||
        item.request.size() > 16000 || item.environmentId.empty() || item.projectId.empty() ||
        !item.config.is_object())
        throw Conflict("Bot, nonempty request (up to 16,000 bytes), environment and project are required");
    modelCallLimit(item.config);
    Json data = {{"botId", item.botId}, {"request", item.request}, {"intent", item.request}, {"environmentId", item.environmentId},
        {"projectId", item.projectId}, {"config", item.config}, {"scheduleId", item.scheduleId},
        {"scheduledAt", item.scheduledAt}, {"createdAt", unixTime()}, {"doneWhen", ""}, {"remaining", 3},
        {"additionalUsed", 0}, {"eventId", 0}, {"actionId", ""}, {"deadline", 0},
        {"activeRun", nullptr}, {"lastRun", nullptr}, {"question", nullptr}, {"wait", nullptr}};
    if (sourceMessageId) data["sourceMessageId"] = sourceMessageId;
    if (!source.is_null()) {
        data["sourceTaskId"] = source.at("taskId");
        data["sourceResultRefs"] = Json::array({source});
    }
    exec("INSERT OR IGNORE INTO tasks(bot_id,environment_id,project_id,schedule_id,scheduled_at,state,generation,data)"
             " VALUES(?,?,?,?,?,'queued',0,?)",
             {item.botId,item.environmentId,item.projectId,item.scheduleId.empty() ? Json(nullptr) : Json(item.scheduleId),
              item.scheduledAt,data});
    if (!sqlite3_changes(db)) {
        auto row = query("SELECT id FROM tasks WHERE bot_id=? AND schedule_id=? AND scheduled_at=?",
                             {item.botId,item.scheduleId,item.scheduledAt});
        if (!row.next()) throw Conflict("Scheduled task could not be saved");
        return row.number(0);
    }
    const auto id = last();
    if (peerKind.empty())
        journal(id, "request", {{"text",item.request},
            {"source",sourceMessageId ? "conversation" : (item.scheduleId.empty() ? "manual" : "cron")},
            {"scheduleId",item.scheduleId},{"scheduledAt",item.scheduledAt},{"messageId",sourceMessageId}});
    return id;
}

Id Store::Impl::submit(const Intake& item, Id sourceMessageId, const Json& source) {
    return submit(item,std::string_view{},sourceMessageId,source);
}

Id Store::Impl::submit(const Intake& item, Id sourceMessageId, const Json& source, bool peerTask) {
    return submit(item,peerTask ? std::string_view{"peer"} : std::string_view{},sourceMessageId,source);
}

void Store::operate(Id id, Id waitId, std::string_view operation, const Json& data) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    p_->operate(id,waitId,operation,data);
    transaction.commit();
}

void Store::Impl::operate(Id id, Id waitId, std::string_view operation, const Json& data, bool fromConversation) {
    if (!data.is_object()) throw Conflict("Operation data must be an object");
    auto task = load(id);
    if (terminal(task) || task["state"] == "cancelling") throw Conflict("Task is closed or cancelling");
    const auto wait = task["wait"];
    if (operation == "cancel") {
        if (waitId && (wait.is_null() || wait["id"] != waitId)) throw Conflict("Waiting item is no longer current");
        requestCancellation(task);
        return;
    }
    if (task["state"] != "waiting" || wait.is_null() || wait["id"] != waitId)
        throw Conflict("Waiting item is no longer current");
    const auto reason = field(wait,"reason");
    std::string next = "deciding";
    if (operation == "answer" && reason == "user_question") {
        const auto& owner = task["question"].is_object() ? task["question"] : wait;
        const Json savedTarget = {{"waitId",wait.at("id")},{"eventId",wait.at("eventId")},
            {"run",wait.at("run")},{"requestId",field(wait,"questionId")}};
        if (owner.value("target",savedTarget) != questionTarget(task))
            throw Conflict("The question is no longer current");
        const auto answer = field(data,"text");
        if (answer.find_first_not_of(" \r\n\t") == std::string::npos || answer.size() > 8000)
            throw Conflict("A nonempty answer (up to 8,000 bytes) is required");
        auto updated = currentEvent(task);
        updated["userAnswer"] = answer;
        updated["userQuestion"] = field(wait,"text");
        for (const auto* metadata : {"id","failures","retryAt","evidence","consumed"}) updated.erase(metadata);
        consumeEvent(task);
        event(task, "answer/" + std::to_string(waitId), updated);
        Json entry = {{"text",answer},{"waitId",waitId},{"conversation",fromConversation}};
        if (const auto message = data.find("messageId"); message != data.end()) entry["messageId"] = *message;
        journal(id, "user_answer", entry);
    } else if (operation == "approve" && reason == "budget") {
        task["remaining"] = task["remaining"].get<int>() + 1;
        journal(id, "budget_granted", {{"waitId",waitId},{"remaining",task["remaining"]}});
    } else if (operation == "approve" && reason == "model_budget") {
        grantModelCall(taskModelBudget(task));
        journal(id,"model_budget_granted",{{"waitId",waitId}});
    } else if (operation == "retry" && reason == "model_failure") {
        exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {task["eventId"]});
        task.erase("feedback");
        journal(id, "model_retry", {{"waitId",waitId}});
    } else if (operation == "extend" && reason == "deadline") {
        if (!data.contains("deadline") || !data["deadline"].is_number_integer() ||
            data["deadline"].get<Seconds>() <= unixTime()) throw Conflict("The new deadline must be in the future");
        task["deadline"] = data["deadline"];
        next = "running";
        journal(id, "deadline_extended", {{"deadline",task["deadline"]},{"waitId",waitId}});
    } else if (operation == "reconcile" && (reason == "unknown_delivery" || reason == "auth" || reason == "settings")) {
        if (data.contains("session")) {
            const auto action = outbox(task);
            const auto& session = data["session"];
            if (reason != "settings" || (!action.is_null() && action["phase"] != "ready") || !session.is_object())
                throw Conflict("Session settings can only be corrected before an unsent action");
            for (auto item = session.begin(); item != session.end(); ++item) {
                if ((item.key() == "workspaceStrategy" && item->is_object()) ||
                    ((item.key() == "model" || item.key() == "providerInstanceId" || item.key() == "runtimeMode") &&
                     item->is_string())) continue;
                throw Conflict("Invalid Session setting");
            }
            task["config"]["session"] = session;
        }
        if (field(task,"actionId").empty()) next = task["activeRun"].is_null() ? "deciding" : "running";
        else {
            const auto action = outbox(task);
            if (action.is_null()) throw Conflict("No delivery to reconcile");
            auto updated = action;
            updated.erase("phase"); updated.erase("authGeneration");
            updated["candidate"] = {{"threadId",field(data,"threadId")},{"runId",field(data,"runId")}};
            updated["candidateQuestionId"] = field(data,"questionId");
            exec("UPDATE outbox SET data=? WHERE id=?", {updated, task["actionId"]});
            next = "sending";
        }
        journal(id, "reconcile_requested", {{"waitId",waitId}});
    } else {
        throw Conflict("Operation is unavailable for this waiting item");
    }
    clearWait(task,operation);
    if (operation == "answer" && task["question"].is_object()) task["question"]["target"] = questionTarget(task);
    task["state"] = next;
    save(task);
}

Json Store::task(Id id) const {
    std::lock_guard lock(p_->mutex);
    return p_->taskDetails(id,true);
}

bool Store::hasTask(Id id) const {
    std::lock_guard lock(p_->mutex);
    return p_->query("SELECT 1 FROM tasks WHERE id=?", {id}).next();
}

Json Store::Impl::taskDetails(Id id, bool includeBlocker) const {
    auto result = load(id);
    result["blockedBy"] = nullptr;
    auto pending = outbox(result);
    if (pending.is_object()) pending.erase("authGeneration");
    result["pendingAction"] = pending;
    if (includeBlocker && result["state"] == "queued") {
        auto row = query("SELECT head.id,owner.bot_id,json_extract(owner.data,'$.request'),head.state"
                             " FROM project_execution_queue head JOIN project_execution_queue task"
                             " USING(environment_id,project_id) JOIN tasks owner ON owner.id=head.id"
                             " WHERE task.id=? AND head.position=1 AND head.id<>task.id",
                             {id});
        if (row.next()) result["blockedBy"] = {{"id",row.number(0)},{"botId",row.string(1)},
                                              {"request",row.string(2)},{"state",row.string(3)}};
    }
    auto revisions = query("SELECT id,number,intent,done_when,source_message_id FROM task_revisions"
                           " WHERE task_id=? ORDER BY number", {id});
    result["revisions"] = Json::array();
    while (revisions.next())
        result["revisions"].push_back({{"id",revisions.number(0)},{"number",revisions.number(1)},
            {"intent",revisions.string(2)},{"doneWhen",revisions.string(3)},
            {"sourceMessageId",revisions.number(4)}});
    auto runs = query("SELECT runs.thread_id,runs.run_id,runs.link,runs.action_id,runs.environment_id,"
                      "json_extract(outbox.data,'$.revisionId') FROM runs"
                      " JOIN outbox ON runs.action_id=outbox.id WHERE runs.task_id=? ORDER BY outbox.rowid", {id});
    result["runs"] = Json::array();
    while (runs.next())
        result["runs"].push_back({{"threadId",runs.string(0)},{"runId",runs.string(1)},
                                  {"link",runs.string(2)},{"actionId",runs.string(3)},
                                  {"environmentId",runs.string(4)},{"revisionId",runs.number(5)}});
    return result;
}

Json Store::tasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query("SELECT id FROM tasks WHERE (?='' OR bot_id=?) AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?",
                          {bot,bot,before,before,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(task(rows.number(0)));
    return result;
}

Json Store::activeTasks(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
WITH queue AS (
  SELECT id,FIRST_VALUE(id) OVER (PARTITION BY environment_id,project_id ORDER BY position) AS head_id
  FROM project_execution_queue
)
SELECT task.id,task.state,task.data,owner.id,owner.bot_id,owner.state,json_extract(owner.data,'$.request')
FROM tasks task
LEFT JOIN queue target ON task.state='queued' AND target.id=task.id
LEFT JOIN tasks owner ON owner.id=target.head_id AND owner.id<>task.id
WHERE task.bot_id=? AND task.state NOT IN ('completed','cancelled')
ORDER BY task.id DESC)SQL", {bot});
    Json result = Json::array();
    while (rows.next()) {
        auto task = rows.json(2);
        task["id"] = rows.number(0);
        task["state"] = rows.string(1);
        task["blockedBy"] = nullptr;
        if (rows.number(3)) task["blockedBy"] = {{"id",rows.number(3)},{"botId",rows.string(4)},
                                                {"state",rows.string(5)},{"request",rows.string(6)}};
        result.push_back(std::move(task));
    }
    return result;
}

Json Store::sessionTasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query("SELECT id FROM tasks WHERE bot_id=? AND (?=0 OR id<?)"
                          " AND EXISTS (SELECT 1 FROM runs WHERE task_id=tasks.id)"
                          " ORDER BY id DESC LIMIT ?", {bot,before,before,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(p_->taskDetails(rows.number(0),false));
    return result;
}

namespace {
Json journalRow(const Statement& row) {
    return {{"id",row.number(0)},{"taskId",row.number(1)},{"time",row.number(2)},
            {"kind",row.string(3)},{"data",row.json(4)}};
}
}
Json Store::history(Id taskId, Id after, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query("SELECT id,task_id,time,kind,data FROM journal WHERE task_id=? AND id>? ORDER BY id LIMIT ?",
                          {taskId,after,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(journalRow(rows));
    return result;
}

Json Store::latestTaskResult(Id taskId) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query(R"SQL(
SELECT j.id,j.task_id,j.time,j.kind,j.data FROM journal j JOIN tasks t ON t.id=j.task_id
WHERE j.task_id=? AND j.kind IN ('completed','report')
AND coalesce(json_extract(j.data,'$.revisionId'),0)=coalesce(json_extract(t.data,'$.currentRevision'),0)
ORDER BY j.id DESC LIMIT 1
)SQL",{taskId});
    return row.next() ? journalRow(row) : Json(nullptr);
}

Json Store::conversation(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT page.id,page.task_id,page.time,page.kind,
  CASE WHEN page.kind='user_answer' AND w.id IS NOT NULL
       THEN json_set(page.data,'$.question',json_extract(w.data,'$.text'))
  WHEN m.id IS NULL THEN page.data
  ELSE json_patch(page.data,json_object('messageId',m.id,'text',m.text,'sequence',m.sequence,
            'replyToMessageId',coalesce(m.reply_to,0),'status',
            coalesce(json_extract(page.data,'$.status'),'received'),
            'deliveredToT3',json(CASE WHEN EXISTS (
              SELECT 1 FROM journal accepted WHERE accepted.kind IN ('instruction_accepted','instruction_steered')
              AND json_extract(accepted.data,'$.messageId')=m.id
            ) AND NOT EXISTS (
              SELECT 1 FROM journal accepted WHERE accepted.kind='instruction_accepted'
              AND json_extract(accepted.data,'$.messageId')=m.id AND NOT EXISTS (
                SELECT 1 FROM journal delivery WHERE delivery.task_id=accepted.task_id
                AND delivery.kind='instruction_delivered' AND json_extract(delivery.data,'$.messageId')=m.id
                AND coalesce(json_extract(delivery.data,'$.step'),0)=coalesce(json_extract(accepted.data,'$.step'),0))
            ) THEN 'true' ELSE 'false' END))) END
FROM (
  SELECT j.id,j.task_id,j.time,j.kind,
    CASE WHEN j.kind='cancelled' THEN json_set(j.data,'$.request',json_extract(t.data,'$.request')) ELSE j.data END AS data,
    j.conversation_bot_id
  FROM journal j JOIN tasks t ON t.id=j.task_id WHERE t.bot_id=?
    AND j.conversation_bot_id IS NULL
    AND kind IN ('request','waiting','wait_closed','user_answer','report','completed','cancelled','bot_message')
    AND (kind!='request' OR json_extract(j.data,'$.source') IS NOT 'conversation')
    AND (kind!='user_answer' OR json_extract(j.data,'$.conversation') IS NOT 1)
    AND (kind!='bot_message' OR (json_extract(j.data,'$.initiator') IS NOT 'user'
      AND json_extract(j.data,'$.recipient') IS NOT 'user'))
  UNION ALL SELECT * FROM journal WHERE conversation_bot_id=?
) AS page LEFT JOIN messages m ON m.id=page.id
LEFT JOIN waits w ON page.kind='user_answer'
  AND w.id=json_extract(page.data,'$.waitId') AND w.task_id=page.task_id
WHERE (?=0 OR page.id<?) ORDER BY page.id DESC LIMIT ?)SQL",
        {bot,bot,before,before,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(journalRow(rows));
    std::reverse(result.begin(),result.end());
    return result;
}

Json Store::peerConversation(std::string_view bot, std::string_view peer, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT j.id,j.task_id,j.time,j.kind,j.data
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
JOIN journal j ON j.task_id=p.sender_task AND j.kind='bot_message' AND json_extract(j.data,'$.exchangeId')=p.id
WHERE ((sender.bot_id=? AND recipient.bot_id=?) OR (sender.bot_id=? AND recipient.bot_id=?))
  AND (?=0 OR j.id<?)
ORDER BY j.id DESC LIMIT ?
)SQL",{bot,peer,peer,bot,before,before,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(journalRow(rows));
    std::reverse(result.begin(),result.end());
    return result;
}

std::vector<std::string> Store::peerPartners(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT DISTINCT CASE WHEN sender.bot_id=? THEN recipient.bot_id ELSE sender.bot_id END
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE sender.bot_id=? OR recipient.bot_id=?
)SQL",{bot,bot,bot});
    std::vector<std::string> result;
    while (rows.next()) result.push_back(rows.string(0));
    return result;
}
} // namespace flockv0
