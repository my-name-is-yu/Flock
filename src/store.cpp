#include "storage.hpp"
#include "flock/session_settings.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

namespace flockv0 {
namespace {
constexpr Id applicationId = 0x464c4f43; // FLOC
constexpr Id schemaVersion = 9;
constexpr std::chrono::milliseconds storageBusyTimeout{4000};

[[noreturn]] void sqlError(sqlite3* db) { throw StorageError(db); }
}
StorageError::StorageError(sqlite3* db)
    : std::runtime_error(sqlite3_errmsg(db)), code_(sqlite3_errcode(db) & 0xff) {}

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
std::string sessionPrompt(const std::string& actionId, const std::string& instruction) {
    const auto marker = "\n\n[Flock action ID: " + actionId + "]";
    return instruction.ends_with(marker) ? instruction : instruction + marker;
}

std::string field(const Json& j, std::string_view key) {
    const auto found = j.find(key);
    return found != j.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
Id requiredInteger(const Json& data, std::string_view key, Id minimum) {
    const auto value = data.find(key);
    if (value == data.end() || !value->is_number_integer() ||
        (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()) ||
        value->get<Id>() < minimum)
        throw Conflict("Invalid " + std::string(key));
    return value->get<Id>();
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
    Json result{{"threadId", run.threadId}, {"runId", run.runId}, {"link", run.link}};
    if (!run.gatewayEnvironmentId.empty()) result["gatewayEnvironmentId"] = run.gatewayEnvironmentId;
    return result;
}
RunRef runRef(const Json& data) {
    return {field(data,"threadId"), field(data,"runId"), field(data,"link"), field(data,"gatewayEnvironmentId")};
}

Json questionTarget(const Json& task) {
    const auto& wait = task.at("wait");
    return {{"waitId",field(wait,"reason") == "user_question" ? wait.at("id") : Json(nullptr)},
        {"eventId",task.at("eventId")},{"run",task.at("activeRun")},{"requestId",field(task.at("question"),"requestId")}};
}

Store::Impl::Impl(std::filesystem::path file, MemorySeed seed, AutomationModelResolver model)
    : path(std::move(file)), memorySeed(std::move(seed)), automationModel(std::move(model)) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    try {
        if (rc != SQLITE_OK) sqlError(db);
        sqlite3_busy_timeout(db,storageBusyTimeout.count());
        const auto deadline = std::chrono::steady_clock::now() + storageBusyTimeout;
        for (;;) {
            try {
                openSchema();
                break;
            } catch (const StorageError& error) {
                // SQLite can skip the busy handler while a competing connection switches to WAL.
                if (!error.contended() || std::chrono::steady_clock::now() >= deadline) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    } catch (...) { sqlite3_close(db); throw; }
}

void Store::Impl::openSchema() {
    exec("PRAGMA foreign_keys=ON");
    exec("PRAGMA synchronous=FULL");
    const auto format = [&] {
        auto row = query("SELECT application_id,user_version FROM pragma_application_id(),pragma_user_version()");
        row.next();
        return std::pair{row.number(0),row.number(1)};
    };
    const std::pair current{applicationId,schemaVersion};
    if (format() != current) {
        Transaction initialize(db);
        const auto existing = format();
        if (existing != current) {
            if (existing != std::pair<Id,Id>{} || query("SELECT 1 FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*' LIMIT 1").next())
                throw Conflict("Unsupported Flock database schema. Use --home with a new state directory.");
            initializeSchema();
            exec(("PRAGMA application_id=" + std::to_string(applicationId)).c_str());
            exec(("PRAGMA user_version=" + std::to_string(schemaVersion)).c_str());
        }
        initialize.commit();
    }
    initializeProjectQueue();
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS git_review_receipts(
  inbox_key TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id),
  host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
  reviewer TEXT NOT NULL, after_event INTEGER NOT NULL,
  PRIMARY KEY(inbox_key,reviewer))
)SQL");
    exec("CREATE INDEX IF NOT EXISTS git_review_receipts_target"
         " ON git_review_receipts(task_id,host,repo,pr_number,reviewer,after_event)");
    exec("CREATE TABLE IF NOT EXISTS deleted_bots(bot TEXT PRIMARY KEY)");
    initializeExternalMcpSchema();
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS git_pr_watch_origins(
  host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL, event_cursor INTEGER NOT NULL,
  PRIMARY KEY(host,repo,pr_number),
  FOREIGN KEY(host,repo,pr_number) REFERENCES git_pr_watches(host,repo,pr_number) ON DELETE CASCADE)
)SQL");
    exec("PRAGMA journal_mode=WAL");
    // Open the WAL now so a read-only peer can use a freshly initialized database.
    exec("PRAGMA user_version");
}

void Store::Impl::initializeSchema() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS tasks(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL,
  project_id TEXT NOT NULL, state TEXT NOT NULL, generation INTEGER NOT NULL, data TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS tasks_route ON tasks(environment_id,project_id,id)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS tasks_bot ON tasks(bot_id,id DESC);
CREATE TABLE IF NOT EXISTS journal(
  id INTEGER PRIMARY KEY, task_id INTEGER REFERENCES tasks(id),
  time INTEGER NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL, conversation_bot_id TEXT,
  CHECK(task_id IS NOT NULL OR conversation_bot_id IS NOT NULL));
CREATE INDEX IF NOT EXISTS journal_task ON journal(task_id,id);
CREATE INDEX IF NOT EXISTS journal_wait_anchor ON journal(task_id,json_extract(data,'$.id'),id)
  WHERE kind='waiting' AND conversation_bot_id IS NULL;
CREATE TABLE IF NOT EXISTS events(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  dedupe TEXT NOT NULL, consumed INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
  retry_at INTEGER NOT NULL DEFAULT 0, data TEXT NOT NULL, evidence TEXT NOT NULL DEFAULT '[]',
  reserved_attempt INTEGER REFERENCES model_attempts(id),
  UNIQUE(task_id,dedupe));
CREATE TABLE IF NOT EXISTS t3_events(
  environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, item_type TEXT NOT NULL, item_id TEXT NOT NULL,
  PRIMARY KEY(environment_id,thread_id,item_type,item_id));
CREATE TABLE IF NOT EXISTS bot_t3_projects(
  bot_id TEXT NOT NULL, environment_id TEXT NOT NULL, project_id TEXT NOT NULL,
  PRIMARY KEY(bot_id,environment_id));
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
CREATE TABLE IF NOT EXISTS session_read_refs(
  ref TEXT PRIMARY KEY, bot TEXT NOT NULL, kind INTEGER NOT NULL,
  environment TEXT NOT NULL, thread TEXT NOT NULL, payload TEXT NOT NULL,
  UNIQUE(bot,kind,environment,thread,payload));
CREATE TABLE IF NOT EXISTS peer_exchanges(
  id TEXT PRIMARY KEY, kind TEXT NOT NULL, phase TEXT NOT NULL, last_message TEXT NOT NULL,
  sender_task INTEGER NOT NULL REFERENCES tasks(id),
  recipient_task INTEGER NOT NULL UNIQUE REFERENCES tasks(id),
  sender_environment TEXT NOT NULL, sender_revision INTEGER);
CREATE UNIQUE INDEX IF NOT EXISTS peer_sender_open ON peer_exchanges(sender_task) WHERE phase<>'closed';
CREATE VIEW IF NOT EXISTS project_execution_queue AS
WITH candidates AS (
  SELECT task.id,task.state,task.data,task.project_id,task.environment_id
  FROM tasks task
  WHERE task.project_id<>'' AND task.state NOT IN ('completed','cancelled')
)
SELECT task.id,task.environment_id,task.project_id,task.state,
       ROW_NUMBER() OVER (
         PARTITION BY task.environment_id,task.project_id
         ORDER BY COALESCE(sender.state,task.state)='queued',
           COALESCE(sender.id,task.id),
           CASE WHEN sender.id IS NOT NULL AND task.state='cancelling' THEN 0
                WHEN task.state='peer_waiting' THEN 2 ELSE 1 END, task.id
       ) AS position
FROM candidates task
LEFT JOIN peer_exchanges peer ON peer.recipient_task=task.id
LEFT JOIN candidates sender ON sender.id=peer.sender_task
  AND sender.environment_id=task.environment_id AND sender.project_id=task.project_id
  AND peer.phase<>'closed'
WHERE COALESCE(peer.kind,'')<>'consult';
CREATE VIEW IF NOT EXISTS project_slots AS
WITH queue AS (
  SELECT id,FIRST_VALUE(id) OVER (PARTITION BY environment_id,project_id ORDER BY position) AS head_id
  FROM project_execution_queue
)
SELECT q.id,CASE WHEN q.id<>q.head_id AND (json_extract(owner.data,'$.workflowId') IS NULL OR
             json_extract(owner.data,'$.workflowId') IS NOT json_extract(t.data,'$.workflowId')) THEN q.head_id END AS blocker_id
FROM queue q JOIN tasks t ON t.id=q.id JOIN tasks owner ON owner.id=q.head_id;
)SQL";
    if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) sqlError(db);
    initializePolicySchema();
    initializeConversationSchema();
    initializeParticipationSchema();
    initializeChannelSchema();
    initializeLocalMachineSchema();
    initializeModelBudgetSchema();
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS conversation_model_failures(
  attempt_id INTEGER PRIMARY KEY REFERENCES model_attempts(id),
  message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id),
  step INTEGER NOT NULL,
  reason TEXT NOT NULL,
  repair_attempt_id INTEGER UNIQUE REFERENCES model_attempts(id) ON DELETE SET NULL)
)SQL");
    exec("CREATE UNIQUE INDEX IF NOT EXISTS conversation_model_repairs"
         " ON conversation_model_failures(message_id) WHERE repair_attempt_id IS NOT NULL");
    initializeAttachmentsSchema();
    initializeCompactionSchema();
    initializeWorkflowSchema();
    initializeMemorySchema();
    initializeAutomationSchema();
    initializeGitSchema();
    initializeBoardInboxSchema();
    exec("CREATE INDEX IF NOT EXISTS journal_session_changes ON journal(id,task_id)"
         " WHERE kind IN ('run_started','report')");
    exec("CREATE INDEX IF NOT EXISTS journal_peer_message ON journal(task_id,json_extract(data,'$.exchangeId'),id)"
         " WHERE kind='bot_message'");
}
Store::Impl::~Impl() { sqlite3_close(db); }
void Store::Impl::exec(const char* sql, const Json& args) const { query(sql, args).next(); }
namespace {
Json waitQuestionTarget(const Json& wait) {
    return {{"waitId",wait.at("id")},{"eventId",wait.at("eventId")},
            {"run",wait.at("run")},{"requestId",field(wait,"questionId")}};
}

void attachWait(Json& task, Json wait, Id anchor) {
    if (anchor) wait["messageId"] = anchor;
    if (field(wait,"reason") == "user_question" && !task.at("question").is_object())
        wait["target"] = waitQuestionTarget(wait);
    task["wait"] = std::move(wait);
}

Json taskRow(const auto& store, const Statement& row, int col = 0) {
    auto task = row.json(col);
    const auto id = row.number(col+1);
    task["id"] = id;
    task["state"] = row.string(col+2);
    task["wait"] = nullptr;
    if (const auto waitId = row.number(col+3)) {
        auto wait = row.json(col+4);
        wait["id"] = waitId;
        attachWait(task,std::move(wait),store.waitAnchor(id,waitId));
    }
    return task;
}
}

Id Store::Impl::waitAnchor(Id task, Id wait) const {
    auto row = query("SELECT id FROM journal WHERE task_id=? AND kind='waiting' AND conversation_bot_id IS NULL"
                     " AND json_extract(data,'$.id')=? ORDER BY id DESC LIMIT 1",{task,wait});
    return row.next() ? row.number(0) : 0;
}

Json Store::Impl::load(Id id) const {
    auto row = query("SELECT t.data,t.id,t.state,w.id,w.data,t.generation FROM tasks t"
                     " LEFT JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL WHERE t.id=?",{id});
    if (!row.next()) throw Conflict("Task does not exist");
    auto task = taskRow(*this,row);
    task["generation"] = row.number(5);
    return task;
}
void Store::Impl::save(Json& task) {
    task["generation"] = task["generation"].get<Id>() + 1;
    exec("UPDATE tasks SET state=?,generation=?,data=json_remove(?,'$.wait'),environment_id=?,project_id=? WHERE id=?",
         {task["state"],task["generation"],task,task["environmentId"],task["projectId"],task["id"]});
}

namespace {
// CROSS JOIN visits the requested IDs first. An empty request set never opens the queue.
const std::string queueBlockers = R"SQL(
blockers AS (
  SELECT requested.id,owner.id AS owner_id,owner.bot_id,
         json_extract(owner.data,'$.request') AS request,owner.state
  FROM requested
  CROSS JOIN task_project_slots slot ON slot.id=requested.id
  JOIN tasks owner ON owner.id=slot.blocker_id
)
)SQL";

std::string taskPageSql(const char* filter) {
    return std::string("WITH page AS (SELECT id,state FROM tasks WHERE ") + filter +
        "), requested AS (SELECT id FROM page WHERE state='queued'), " + queueBlockers + R"SQL(
SELECT blockers.owner_id,blockers.bot_id,blockers.request,blockers.state,
       task.data,task.id,task.state,w.id,w.data,task.generation
FROM page JOIN tasks task ON task.id=page.id
LEFT JOIN blockers ON blockers.id=page.id
LEFT JOIN waits w ON w.task_id=task.id AND w.resolution IS NULL
ORDER BY task.id DESC)SQL";
}

Json blockerJson(const Statement& row) {
    if (!row.number(0)) return nullptr;
    return {{"id",row.number(0)},{"botId",row.string(1)},{"request",row.string(2)},{"state",row.string(3)}};
}

Json pageTask(const auto& store, const Statement& row) {
    auto task = taskRow(store,row,4);
    task["blockedBy"] = blockerJson(row);
    return task;
}
}

Json Store::Impl::queueBlocker(Id id) const {
    static const auto sql = "WITH requested(id) AS (VALUES(?)), " + queueBlockers +
        "SELECT owner_id,bot_id,request,state FROM blockers";
    auto row = query(sql.c_str(), {id});
    return row.next() ? blockerJson(row) : Json(nullptr);
}
Id Store::Impl::journal(Id id, std::string_view kind, const Json& data) {
    exec("INSERT INTO journal(task_id,time,kind,data) VALUES(?,?,?,?)", {id, unixTime(), kind, data});
    const auto entry = last();
    queueChannelTaskUpdate(id,entry,kind,data);
    return entry;
}
std::optional<PeerExchange> Store::Impl::peerExchange(Id id) const {
    auto rows = query("SELECT id,kind,phase,last_message,sender_task,recipient_task FROM peer_exchanges"
                      " WHERE phase<>'closed' AND (sender_task=? OR recipient_task=?)", {id,id});
    if (!rows.next()) return std::nullopt;
    return PeerExchange{rows.string(0),rows.string(1),rows.string(2),rows.string(3),rows.number(4),rows.number(5)};
}

bool Store::Impl::peerWorkRouteWouldCycle(Id sender, std::string_view environment, std::string_view project) const {
    return query(R"SQL(
WITH RECURSIVE source(env,project) AS (
  SELECT environment_id,project_id FROM tasks WHERE id=?1
), work_routes(source_env,source_project,current_env,current_project,pending_env,pending_project) AS (
  SELECT s.environment_id,s.project_id,r.environment_id,r.project_id,
         CASE WHEN r.state<>'cancelling' THEN json_extract(r.data,'$.pendingDestination.environmentId') END,
         json_extract(r.data,'$.pendingDestination.projectId')
  FROM peer_exchanges p JOIN tasks s ON s.id=p.sender_task JOIN tasks r ON r.id=p.recipient_task
  WHERE p.kind='work' AND p.phase<>'closed' AND p.sender_task<>?1
), dependencies(source_env,source_project,target_env,target_project) AS (
  SELECT source_env,source_project,current_env,current_project FROM work_routes
  WHERE current_project<>''
  UNION ALL
  SELECT source_env,source_project,pending_env,pending_project FROM work_routes
  WHERE pending_env IS NOT NULL AND pending_project<>''
), reachable(env,project) AS (
  SELECT ?2,?3 FROM source WHERE env<>?2 OR project<>?3
  UNION
  SELECT target_env,target_project FROM dependencies JOIN reachable
    ON source_env=env AND source_project=project
)
SELECT 1 FROM reachable JOIN source USING(env,project) LIMIT 1
)SQL",{sender,environment,project}).next();
}

bool Store::Impl::recipientRouteWouldCycle(Id task, std::string_view environment, std::string_view project) const {
    if (project.empty()) return false;
    const auto peer = peerExchange(task);
    return peer && peer->recipient == task && peer->kind == "work" &&
        peerWorkRouteWouldCycle(peer->sender,environment,project);
}

bool Store::Impl::replanReady(const Json& task) const {
    if (!task.value("replanning",false)) return false;
    std::lock_guard lock(mutex);
    const Id id = task.at("id");
    const auto peer = peerExchange(id);
    return !peer || peer->allowsReplan(id);
}

bool Store::Impl::waitIsActionable(const Json& task) const {
    return !replanReady(task);
}

void Store::Impl::peerMessage(const PeerExchange& exchange, const Json& message) {
    journal(exchange.sender,"bot_message",message);
    journal(exchange.recipient,"bot_message",message);
}

void Store::Impl::requestCancellation(Json& task, SessionStop stop) {
    const Id id = task["id"];
    const auto peer = peerExchange(id);
    const bool undispatched = task["state"] == "queued" || isUndispatchedNotesWait(task);
    if (!task.value("pendingInstructions",Json::array()).empty())
        task["cancelledInstructions"] = task.at("pendingInstructions");
    clearWait(task,"cancel");
    consumeEvent(task);
    journal(id,"cancel_requested",stop.participation
        ? Json{{"sessionParticipationId",stop.participation},{"sessionStopId",stop.cutoff}}
        : Json::object());
    if (undispatched && !peer) {
        for (const auto* key : {"replanning","pendingDestination","pendingInstructions"})
            task.erase(key);
        task["state"] = "cancelled";
        journal(id,"cancelled",Json::object());
    } else task["state"] = "cancelling";
    save(task);
    if (peer && peer->sender == id) {
        auto recipient = load(peer->recipient);
        if (stop.participation && peer->kind == "work") {
            // The Session exit owns this task's Run, while the peer still owns its own task and Run.
            exec("UPDATE peer_exchanges SET phase='closed' WHERE id=?",{peer->id});
            if (recipient["state"] == "peer_waiting") {
                event(recipient,"peer-closed/" + peer->id,{{"kind","intake"}});
                recipient["state"] = "deciding";
                save(recipient);
            }
        } else if (!terminal(recipient) && recipient["state"] != "cancelling") requestCancellation(recipient);
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
    releaseModelCall(takeReservedTaskCall(task.at("eventId")));
    exec("UPDATE events SET consumed=1 WHERE id=? AND task_id=? AND consumed=0", {task["eventId"], task["id"]});
}
void Store::Impl::wait(Json& task, std::string_view reason, Json data) {
    clearWait(task, "superseded");
    data["reason"] = reason;
    data["eventId"] = task.value("eventId", Id{});
    if (!data.contains("run")) data["run"] = task["activeRun"];
    if (data["run"].is_null() && reason == "unknown_delivery") {
        const auto action = outbox(task);
        if (action.is_object() && action.contains("candidate") && !field(action["candidate"],"link").empty())
            data["run"] = action["candidate"];
    }
    if (task.contains("actionId")) data["actionId"] = task["actionId"];
    exec("INSERT INTO waits(task_id,data) VALUES(?,?)", {task["id"], data});
    data["id"] = last();
    const auto anchor = journal(task["id"], "waiting", data);
    attachWait(task,std::move(data),anchor);
    task["state"] = "waiting";
}
void Store::Impl::clearWait(Json& task, std::string_view resolution) {
    const auto wait = task.find("wait");
    if (wait == task.end() || wait->is_null()) return;
    const Json data = {{"waitId", (*wait)["id"]}, {"resolution", resolution}};
    exec("UPDATE waits SET resolution=? WHERE id=? AND task_id=? AND resolution IS NULL",
         {resolution,(*wait)["id"],task["id"]});
    if (task.value("blockerWaitId",Id{0}) == wait->at("id")) {
        task.erase("blockerWaitId");
        task.erase("blockerReason");
    }
    task["wait"] = nullptr;
    journal(task["id"], "wait_closed", data);
}
Json Store::Impl::outbox(const Json& task, std::string_view actionId) const {
    const auto id = actionId.empty() ? field(task,"actionId") : std::string(actionId);
    if (id.empty()) return nullptr;
    auto row = query("SELECT data,phase,auth_generation FROM outbox WHERE id=? AND task_id=?",
                     {id, task["id"]});
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

std::string taskIntent(const Json& task) {
    return task.at("intent");
}

std::string effectiveDoneWhen(const Json& task) {
    return task.value("requestedDoneWhen",field(task,"doneWhen"));
}

Store::Store(std::filesystem::path db, MemorySeed seed, AutomationModelResolver automationModel)
    : p_(std::make_unique<Impl>(std::move(db),std::move(seed),std::move(automationModel))) {}
Store::~Store() = default;
const std::filesystem::path& Store::path() const { return p_->path; }

Id Store::Impl::dataVersion() const {
    auto version = query("PRAGMA data_version");
    return version.next() ? version.number(0) : 0;
}

std::pair<Id,Id> Store::Impl::changeStamp() const {
    std::lock_guard lock(mutex);
    return {sqlite3_total_changes64(db),dataVersion()};
}

std::pair<Id,Id> Store::changeStamp() const { return p_->changeStamp(); }

Store::ConversationChanges Store::conversationChangesSince(std::optional<Id> after, std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto newest = p_->query("SELECT coalesce(max(id),0) FROM journal");
    newest.next();
    ConversationChanges changes{newest.number(0),{}};
    if (!after || *after >= changes.through) return changes;
    auto rows = p_->query(R"SQL(
SELECT m.bot_id,m.id FROM journal j JOIN messages m ON m.id=CASE j.kind
  WHEN 'conversation_reply' THEN json_extract(j.data,'$.replyToMessageId') ELSE json_extract(j.data,'$.messageId') END
WHERE j.id>?1 AND j.id<=?2 AND (?3='' OR m.bot_id=?3)
  AND j.kind IN ('conversation_reply','instruction_accepted','instruction_delivered',
                 'conversation_budget_approved')
UNION
SELECT j.conversation_bot_id,json_extract(j.data,'$.waitMessageId') FROM journal j
WHERE j.id>?1 AND j.id<=?2 AND j.kind='conversation_budget_approved'
  AND (?3='' OR j.conversation_bot_id=?3)
ORDER BY 1,2
)SQL",{*after,changes.through,bot});
    while (rows.next()) changes.messages[rows.string(0)].push_back(rows.number(1));
    return changes;
}

std::optional<Store::BotViewStamp> Store::botViewStamp() const {
    std::lock_guard lock(p_->mutex);
    if (!p_->channels.empty() || p_->query("SELECT 1 FROM protected_inputs WHERE status='fulfilled' LIMIT 1").next())
        return std::nullopt;
    return BotViewStamp{changeStamp(),p_->gitCredentials.size()};
}

Id Store::submit(const Intake& item, const Person* requester) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    const auto id = p_->submitRequest(item,requester);
    transaction.commit();
    return id;
}

Id Store::Impl::submitRequest(const Intake& item, const Person* requester) {
    if (requester && !Policy(*this).allows(*requester,Action::bot_talk,item.botId)) throw DecisionDenied{};
    const auto id = submit(item);
    if (requester) {
        sharedRedactor().requirePublic(requester->id);
        exec("UPDATE tasks SET data=json_set(data,'$.requester',?) WHERE id=?",{requester->id,id});
    }
    return id;
}

Id Store::Impl::submit(const Intake& item, Id sourceMessageId, const Json& source,
                      TaskSource sourceKind, const Json& trigger) {
    const bool consultation = sourceKind == TaskSource::consultation;
    if (item.botId.empty() || item.request.find_first_not_of(" \r\n\t") == std::string::npos ||
        item.request.size() > 16000 || (!consultation && item.environmentId.empty()) ||
        !item.config.is_object())
        throw Conflict("Bot, nonempty request (up to 16,000 bytes), and environment are required");
    if (botDeleted(item.botId)) throw Conflict("Bot has been deleted");
    const auto scrub = redactor(item.botId);
    scrub.requirePublic({item.botId,item.environmentId,item.projectId,item.config});
    modelCallLimit(item.config);
    const auto request = scrub.text(item.request);
    Json data = {{"botId", item.botId}, {"request", request}, {"intent", request}, {"environmentId", item.environmentId},
        {"projectId", item.projectId}, {"config", item.config},
        {"createdAt", unixTime()}, {"doneWhen", ""}, {"remaining", 3},
        {"additionalUsed", 0}, {"eventId", 0}, {"actionId", ""}, {"deadline", 0},
        {"activeRun", nullptr}, {"lastRun", nullptr}, {"question", nullptr}};
    if (sourceMessageId) data["sourceMessageId"] = sourceMessageId;
    if (!source.is_null()) {
        data["sourceTaskId"] = source.at("taskId");
        data["sourceResultRefs"] = Json::array({source});
    }
    if (!trigger.is_null()) data["trigger"] = trigger;
    const auto state = consultation ? "deciding" : "queued";
    exec("INSERT INTO tasks(bot_id,environment_id,project_id,state,generation,data) VALUES(?,?,?,?,0,?)",
         {item.botId,item.environmentId,item.projectId,state,data});
    const auto id = last();
    if (sourceKind == TaskSource::request) {
        Json entry = {{"text",request},{"messageId",sourceMessageId},
                      {"source",sourceMessageId ? "conversation" : trigger.is_null() ? "manual" : "automation"}};
        if (!trigger.is_null()) entry["trigger"] = trigger;
        journal(id,"request",entry);
    }
    return id;
}

Json reconciliationData(const Json& task, Id waitId, Json target, const IntakeResolver& intake) {
    const auto& wait = task.at("wait");
    const auto& pending = task.at("pendingAction");
    if (!wait.is_object() || wait.at("id") != waitId || wait.at("reason") != "settings" ||
        (!pending.is_null() && pending.at("phase") != "ready")) return target;
    if (pending.is_object() && pending.at("tool") == "start_in_session") return target;

    const auto current = intake(task.at("botId").get_ref<const std::string&>(),
                                task.at("request").get_ref<const std::string&>());
    const auto environment = field(task,"environmentId");
    const auto destinations = current.config.value("conversationEnvironments",Json::object());
    const auto project = field(task,"projectId");
    const bool namedRoute = std::ranges::any_of(destinations,[&](const Json& alias) {
        return conversationEnvironmentId(alias) == environment &&
            (environment != current.environmentId || field(alias,"projectId") == project);
    });
    const bool settingsMatch = environment == current.environmentId
        ? (current.projectId.empty() || project == current.projectId)
        : current.environmentId.empty();
    if (!settingsMatch && !namedRoute)
        throw Conflict("The project route changed. Cancel this task and submit a new request.");
    target["session"] = current.config.value("session",Json::object());
    return target;
}

TaskTarget TaskTarget::from(const Json& task, Id waitId) {
    return {field(task,"botId"),task.at("id"),waitId,task.value("currentRevision",Id{0}),task.at("generation")};
}

void Store::operate(const Person& actor, const TaskTarget& target, std::string_view operation, const Json& data) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    p_->operate(actor,target,operation,data);
    transaction.commit();
}

void Store::Impl::operate(const Person& actor, const TaskTarget& target, std::string_view operation,
                          const Json& input, Id sourceMessage, std::optional<InboxKind> expectedKind) {
    const auto& [bot,id,waitId,revision,generation] = target;
    if (!Policy(*this).allows(actor,Action::bot_decide,bot)) throw DecisionDenied{};
    auto task = load(id);
    if (task.at("botId") != bot || task.value("currentRevision",Id{0}) != revision) throw RequestChanged{};
    const auto data = redactor(bot).payload(input);
    if (!data.is_object()) throw Conflict("Operation data must be an object");
    const auto& wait = task.at("wait");
    if (operation == "cancel") {
        if (task.at("generation") != generation) throw RequestChanged{};
        if (terminal(task) || task.at("state") == "cancelling" || (waitId && (wait.is_null() || wait.at("id") != waitId)))
            throw DecisionUnavailable{};
        requestCancellation(task);
        return;
    }
    if (!waitIsActionable(task)) throw RequestChanged{};
    if (wait.is_null() || wait.at("id") != waitId) {
        auto prior = query("SELECT resolution FROM waits WHERE id=? AND task_id=?",{waitId,id});
        if (!prior.next() || prior.string(0) == "request_changed") throw RequestChanged{};
        throw DecisionUnavailable{};
    }
    if (task.at("state") != "waiting") throw DecisionUnavailable{};
    const auto reason = field(wait,"reason");
    const auto deadlineRun = reason == "deadline" ? field(wait,"runActionId") : std::string{};
    const bool extendingVerificationDeadline = operation == "extend" && !deadlineRun.empty();
    if (task.at("generation") != generation && !extendingVerificationDeadline)
        throw RequestChanged{};
    const auto kind = inboxWaitKind(reason);
    if (expectedKind && kind != expectedKind) throw RequestChanged{};
    std::string next = "deciding";
    if (operation == "answer" && reason == "user_question") {
        const auto& owner = task["question"].is_object() ? task["question"] : wait;
        if (owner.at("target") != questionTarget(task))
            throw RequestChanged{};
        const auto answer = field(data,"text");
        if (answer.find_first_not_of(" \r\n\t") == std::string::npos || answer.size() > 8000)
            throw Conflict("A nonempty answer (up to 8,000 bytes) is required");
        auto answerKind = "user_answer";
        if (const auto projects = wait.find("projectChoices"); projects != wait.end()) {
            const auto choice = projects->find(answer);
            if (choice == projects->end()) throw Conflict("Choose one of the listed T3 projects");
            const auto environment = field(task,"environmentId");
            const auto& project = choice->get_ref<const std::string&>();
            if (recipientRouteWouldCycle(id,environment,project))
                throw Conflict("This project would make project queues wait for one another. Choose another project or move the task.");
            task["projectId"] = *choice;
            rememberT3Project(bot,environment,project);
            next = "queued";
            answerKind = "project_selected";
        } else {
            auto updated = currentEvent(task);
            updated["userAnswer"] = answer;
            updated["userQuestion"] = field(wait,"text");
            for (const auto* metadata : {"id","failures","retryAt","evidence","consumed"}) updated.erase(metadata);
            consumeEvent(task);
            event(task, "answer/" + std::to_string(waitId), updated);
        }
        Json entry = {{"text",answer},{"waitId",waitId},{"conversation",sourceMessage != 0}};
        if (sourceMessage) entry["messageId"] = sourceMessage;
        journal(id,answerKind,entry);
    } else if (operation == "approve" && reason == "budget") {
        task["remaining"] = task["remaining"].get<int>() + 1;
        journal(id, "budget_granted", {{"waitId",waitId},{"remaining",task["remaining"]}});
    } else if (operation == "approve" && reason == "model_budget") {
        const Id eventId = task.at("eventId");
        const auto attempt = reserveExtraModelCall(taskModelBudget(task),"task_decision",0,id,eventId);
        exec("UPDATE events SET reserved_attempt=? WHERE id=? AND task_id=?",{attempt,eventId,id});
        journal(id,"model_budget_granted",{{"waitId",waitId},{"attemptId",attempt}});
    } else if (operation == "retry" && (reason == "model_failure" || isUndispatchedNotesWait(task))) {
        if (reason == "bot_notes_unavailable") next = "queued";
        exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {task["eventId"]});
        task.erase("feedback");
        journal(id, "model_retry", {{"waitId",waitId}});
    } else if (operation == "extend" && reason == "deadline") {
        if (!data.contains("deadline") || !data["deadline"].is_number_integer() ||
            data["deadline"].get<Seconds>() <= unixTime()) throw Conflict("The new deadline must be in the future");
        auto& timed = deadlineRun.empty() ? task : destinationRun(task,deadlineRun);
        if (!deadlineRun.empty() && !destinationRunOutstanding(timed)) throw RequestChanged{};
        timed["deadline"] = data.at("deadline");
        next = "running";
        journal(id,"deadline_extended",{{"deadline",timed.at("deadline")},{"waitId",waitId}});
    } else if (operation == "reconcile" && (reason == "unknown_delivery" || reason == "auth" || reason == "settings")) {
        if (data.contains("session")) {
            const auto action = outbox(task);
            if (reason != "settings" || (!action.is_null() && action["phase"] != "ready"))
                throw Conflict("Session settings can only be corrected before an unsent action");
            task["config"]["session"] = normalizeSessionSettings(data["session"]);
        }
        if (field(task,"actionId").empty()) {
            if (!task["activeRun"].is_null()) next = "running";
            else if (field(task,"projectId").empty()) next = "queued";
        } else {
            const auto action = outbox(task);
            if (action.is_null()) throw Conflict("No delivery to reconcile");
            if (action.at("phase") == "ready" && action.at("tool") == "answer_session_question") {
                const auto differs = [&](std::string_view key, const std::string& saved) {
                    const auto hint = field(data,key);
                    return !hint.empty() && hint != saved;
                };
                if (differs("threadId",field(action.at("target"),"threadId")) ||
                    differs("runId",field(action.at("target"),"runId")) ||
                    differs("questionId",field(action,"questionId")))
                    throw Conflict("The supplied T3 question is outside the saved answer destination");
            }
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
    if (kind) recordInboxDecision(actor,{bot,*kind,waitId,waitId,id,revision,generation},operation);
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
    auto task = load(id);
    task["blockedBy"] = includeBlocker && task["state"] == "queued" ? queueBlocker(id) : Json(nullptr);
    addTaskDetails(task);
    return task;
}

Json Store::Impl::sessionTask(Id id) const {
    auto task = taskDetails(id,false);
    // A settled report identifies the assigned Run even when its result came from a later Run.
    auto reports = query(R"SQL(
SELECT coalesce(nullif(json_extract(data,'$.run.environmentId'),''),json_extract(data,'$.environmentId')),
       json_extract(data,'$.run.threadId'),json_extract(data,'$.run.runId'),json_extract(data,'$.status'),
       json_extract(data,'$.sessionCreatedAt')
FROM journal WHERE task_id=? AND kind IN ('report','run_settled') ORDER BY id
)SQL",{id});
    using SessionKey = std::pair<std::string,std::string>;
    using RunKey = std::tuple<std::string,std::string,std::string>;
    std::map<RunKey,std::string> statuses;
    std::map<SessionKey,std::string> createdAt;
    while (reports.next()) {
        const auto environment = reports.string(0);
        const auto thread = reports.string(1);
        auto status = reports.string(3);
        if (!status.empty())
            statuses.insert_or_assign(RunKey{environment,thread,reports.string(2)},std::move(status));
        auto timestamp = reports.string(4);
        if (!timestamp.empty()) createdAt.try_emplace(SessionKey{environment,thread},std::move(timestamp));
    }
    for (auto& run : task.at("runs")) {
        const SessionKey session{field(run,"environmentId"),field(run,"threadId")};
        const auto status = statuses.find({session.first,session.second,field(run,"runId")});
        if (status != statuses.end()) run["status"] = status->second;
        const auto timestamp = createdAt.find(session);
        if (timestamp != createdAt.end()) run["sessionCreatedAt"] = timestamp->second;
    }
    return task;
}

void Store::Impl::addTaskDetails(Json& result) const {
    const Id id = result.at("id");
    auto pending = outbox(result);
    if (pending.is_object()) {
        pending.erase("authGeneration");
        pending.erase("sessionMemory");
    }
    result["pendingAction"] = pending;
    auto revisions = query("SELECT id,number,intent,done_when,source_message_id FROM task_revisions"
                           " WHERE task_id=? ORDER BY number", {id});
    result["revisions"] = Json::array();
    while (revisions.next())
        result["revisions"].push_back({{"id",revisions.number(0)},{"number",revisions.number(1)},
            {"intent",revisions.string(2)},{"doneWhen",revisions.string(3)},
            {"sourceMessageId",revisions.number(4)}});
    auto runs = query("SELECT runs.thread_id,runs.run_id,runs.link,runs.action_id,runs.environment_id,"
                      "json_extract(outbox.data,'$.revisionId'),json_extract(outbox.data,'$.projectId') FROM runs"
                      " JOIN outbox ON runs.action_id=outbox.id WHERE runs.task_id=? ORDER BY outbox.rowid", {id});
    result["runs"] = Json::array();
    while (runs.next())
        result["runs"].push_back({{"threadId",runs.string(0)},{"runId",runs.string(1)},
                                  {"link",runs.string(2)},{"actionId",runs.string(3)},
                                  {"environmentId",runs.string(4)},{"revisionId",runs.number(5)},
                                  {"projectId",runs.string(6)},{"ownerBotId",result.at("botId")}});
}

Json Store::tasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    static const auto sql = taskPageSql("(?='' OR bot_id=?) AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?");
    auto rows = p_->query(sql.c_str(), {bot,bot,before,before,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) {
        auto task = pageTask(*p_,rows);
        task["generation"] = rows.number(9);
        p_->addTaskDetails(task);
        result.push_back(std::move(task));
    }
    return result;
}

Json Store::activeTasks(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    static const auto sql = taskPageSql("bot_id=? AND state NOT IN ('completed','cancelled')");
    auto rows = p_->query(sql.c_str(), {bot});
    Json result = Json::array();
    while (rows.next()) result.push_back(pageTask(*p_,rows));
    return result;
}

Json Store::activeTaskStatesByBot() const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT DISTINCT bot_id,state,coalesce(json_extract(data,'$.replanning'),0)
FROM tasks WHERE state NOT IN ('completed','cancelled'))SQL");
    auto result = Json::object();
    while (rows.next())
        result[rows.string(0)].push_back({{"state",rows.string(1)},{"replanning",rows.number(2) != 0}});
    return result;
}

Json Store::sessionTasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query("SELECT id FROM tasks WHERE bot_id=? AND (?=0 OR id<?)"
                          " AND EXISTS (SELECT 1 FROM runs WHERE task_id=tasks.id)"
                          " ORDER BY id DESC LIMIT ?", {bot,before,before,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(p_->sessionTask(rows.number(0)));
    return result;
}

std::pair<Id,Json> Store::sessionChangesSince(std::string_view bot, std::optional<Id> after) const {
    std::lock_guard lock(p_->mutex);
    auto newest = p_->query("SELECT coalesce(max(id),0) FROM journal WHERE kind IN ('run_started','report')");
    newest.next();
    const auto through = newest.number(0);
    auto tasks = Json::array();
    if (after && *after < through) {
        auto rows = p_->query(R"SQL(
SELECT DISTINCT j.task_id FROM journal j CROSS JOIN tasks t ON t.id=j.task_id
WHERE j.kind IN ('run_started','report') AND j.id>? AND j.id<=? AND t.bot_id=?
)SQL",{*after,through,bot});
        while (rows.next()) tasks.push_back(p_->sessionTask(rows.number(0)));
    }
    return {through,std::move(tasks)};
}

namespace {
Json journalRow(const Statement& row) {
    return {{"id",row.number(0)},{"taskId",row.number(1)},{"time",row.number(2)},
            {"kind",row.string(3)},{"data",row.json(4)}};
}
}
Json Store::history(Id taskId, Id after, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT j.id,j.task_id,j.time,j.kind,
  CASE WHEN input.message_id IS NULL THEN j.data
       ELSE json_patch(j.data,json_object('status',input.status)) END
FROM journal j LEFT JOIN conversation_input_status input ON input.message_id=j.id
WHERE j.task_id=? AND j.id>? ORDER BY j.id LIMIT ?
)SQL",{taskId,after,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(journalRow(rows));
    return result;
}

Json Store::conversation(std::string_view bot, Id before, unsigned limit, ConversationEvents events,
                         std::string_view viewer) const {
    if (before == std::numeric_limits<Id>::min()) return Json::array();
    const auto through = before ? before-1 : std::numeric_limits<Id>::max();
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
WITH visible AS NOT MATERIALIZED (
  SELECT id,task_id,conversation_bot_id FROM journal
  WHERE id<=?2
    AND (?3=0 OR kind IN ('conversation_reply','report','completed','pr_review_requested')
         OR (kind='waiting' AND json_extract(data,'$.reason')='user_question'))
    AND ((conversation_bot_id IS NOT NULL AND kind NOT IN ('git_webhook','conversation_budget_approved','session_participation_stop'))
      OR (conversation_bot_id IS NULL
        AND kind IN ('request','waiting','wait_closed','user_answer','report','completed','cancelled','bot_message','pr_review_requested')
        AND (kind!='request' OR json_extract(data,'$.source') IS NOT 'conversation')
        AND (kind!='user_answer' OR json_extract(data,'$.conversation') IS NOT 1)
        AND (kind!='bot_message' OR (json_extract(data,'$.initiator') IS NOT 'user'
          AND json_extract(data,'$.recipient') IS NOT 'user'))))
), bot_page AS (
  SELECT id FROM visible WHERE conversation_bot_id=?1 ORDER BY id DESC LIMIT ?4
), page_ids AS MATERIALIZED (
  -- A row past the page limit within one task cannot reach the combined page.
  SELECT candidate.value AS id FROM tasks t
  CROSS JOIN json_each((SELECT json_group_array(id) FROM (
    SELECT id FROM visible WHERE task_id=t.id AND conversation_bot_id IS NULL
    ORDER BY id DESC LIMIT ?4
  ))) candidate
  WHERE t.bot_id=?1
  UNION ALL SELECT id FROM bot_page
  ORDER BY id DESC LIMIT ?4
), page AS (
  SELECT j.id,j.task_id,j.time,j.kind,
    CASE WHEN j.kind='cancelled' AND t.id IS NOT NULL
         THEN json_set(j.data,'$.request',json_extract(t.data,'$.request')) ELSE j.data END AS data
  FROM page_ids ids JOIN journal j ON j.id=ids.id
  LEFT JOIN tasks t ON j.kind='cancelled' AND j.conversation_bot_id IS NULL AND t.id=j.task_id
)
SELECT page.id,page.task_id,page.time,page.kind,
  CASE WHEN page.kind='user_answer' AND w.id IS NOT NULL
       THEN json_set(page.data,'$.question',json_extract(w.data,'$.text'))
  WHEN m.id IS NULL THEN page.data
  ELSE json_patch(page.data,json_object('messageId',m.id,'text',m.text,'sequence',m.sequence,
            'replyToMessageId',coalesce(m.reply_to,0),'status',coalesce(input.status,'received'),
            'imageAttachmentCount',(SELECT count(*) FROM message_attachments a
              WHERE a.message_id=m.id AND json_extract(a.metadata,'$.kind')='image'),
            'deliveredToT3',json(CASE WHEN (
              WITH instructions AS (
                SELECT kind,task_id,json_extract(data,'$.step') AS step
                FROM journal WHERE kind IN ('instruction_accepted','instruction_delivered')
                -- Unary + prevents SQLite from coercing JSON values and lets it use the expression index.
                AND json_extract(data,'$.messageId')=+m.id
              )
              SELECT min(EXISTS (
                SELECT 1 FROM instructions d WHERE d.kind='instruction_delivered'
                AND d.task_id=i.task_id AND d.step=i.step
              )) FROM instructions i WHERE i.kind!='instruction_delivered'
            ) THEN 'true' ELSE 'false' END))) END,
  channel.message_id IS NOT NULL
FROM page LEFT JOIN messages m ON m.id=page.id
LEFT JOIN conversation_input_status input ON input.message_id=m.id
LEFT JOIN waits w ON page.kind='user_answer'
  AND w.id=json_extract(page.data,'$.waitId') AND w.task_id=page.task_id
LEFT JOIN channel_messages channel ON page.kind='conversation_message' AND channel.message_id=m.id
ORDER BY page.id DESC)SQL",
        {bot,through,events == ConversationEvents::incoming ? 1 : 0,std::clamp(limit,1u,200u)});
    Json result = Json::array();
    while (rows.next()) {
        auto event = journalRow(rows);
        if (rows.number(5))
            event["data"].update(p_->channelContext(rows.number(0),bot,nullptr,viewer));
        result.push_back(std::move(event));
    }
    std::reverse(result.begin(),result.end());
    return result;
}

Json Store::peerConversation(std::string_view bot, std::string_view peer, Id before, unsigned limit, Id from,
                             std::string_view viewer) const {
    std::lock_guard lock(p_->mutex);
    // The sending task keeps its original source message even as its revisions change.
    // Match the saved quote as well, so missing or unverified provenance falls back to a generic label.
    const auto sql = std::string(R"SQL(
SELECT j.id,j.task_id,j.time,j.kind,j.data,
  CASE WHEN ?5<>'' AND json_type(j.data,'$.forwardedMessage')='object' THEN EXISTS(
    SELECT 1 FROM messages m JOIN journal original ON original.id=m.id
    WHERE m.id=json_extract(sender.data,'$.sourceMessageId') AND m.bot_id=sender.bot_id
      AND original.kind='conversation_message' AND m.person_id=?5
      AND m.text=json_extract(j.data,'$.forwardedMessage.text')
      AND original.time=json_extract(j.data,'$.forwardedMessage.time')
  ) ELSE 0 END
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
JOIN journal j ON j.task_id=p.sender_task AND j.kind='bot_message'
  -- Unary + removes TEXT affinity so SQLite can use the exchange ID in the JSON expression index.
  AND json_extract(j.data,'$.exchangeId') = +p.id
WHERE ((sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1))
)SQL") + (from ? " AND j.id>=?3 ORDER BY j.id ASC LIMIT ?4"
               : " AND (?3=0 OR j.id<?3) ORDER BY j.id DESC LIMIT ?4");
    auto rows = p_->query(sql.c_str(),{bot,peer,from ? from : before,std::clamp(limit,1u,200u),viewer});
    Json result = Json::array();
    while (rows.next()) {
        auto event = journalRow(rows);
        if (rows.number(5)) event["forwardedFromViewer"] = true;
        result.push_back(std::move(event));
    }
    if (!from) std::reverse(result.begin(),result.end());
    return result;
}

Id Store::peerMessageAnchor(std::string_view bot, std::string_view peer, Id event) const {
    std::lock_guard lock(p_->mutex);
    // Each participant has a journal row. The shared DM displays the sender task's copy.
    auto row = p_->query(R"SQL(
SELECT j.id
FROM journal source
JOIN peer_exchanges p ON p.id=json_extract(source.data,'$.exchangeId')
JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
JOIN journal j ON j.task_id=p.sender_task AND j.kind='bot_message'
  AND json_extract(j.data,'$.exchangeId')=+p.id
  AND json_extract(j.data,'$.messageId')=json_extract(source.data,'$.messageId')
WHERE source.id=?3 AND source.kind='bot_message' AND source.task_id IN (p.sender_task,p.recipient_task)
  AND ((sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1))
LIMIT 1
)SQL",{bot,peer,event});
    return row.next() ? row.number(0) : 0;
}

std::vector<std::pair<std::string,std::string>> Store::peerExchangePairs() const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT DISTINCT sender.bot_id,recipient.bot_id
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
)SQL");
    std::vector<std::pair<std::string,std::string>> result;
    while (rows.next()) result.emplace_back(rows.string(0),rows.string(1));
    return result;
}

bool Store::hasPeerExchange(std::string_view bot, std::string_view peer) const {
    std::lock_guard lock(p_->mutex);
    return p_->query(R"SQL(
SELECT 1
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE (sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1)
LIMIT 1
)SQL",{bot,peer}).next();
}
} // namespace flockv0
