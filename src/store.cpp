#include "storage.hpp"
#include "flock/limits.hpp"
#include "task_storage.hpp"
#include "task_interaction_storage.hpp"
#include "task_context_storage.hpp"
#include "outbox_storage.hpp"
#include "flock/session_settings.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <system_error>
#include <thread>

namespace flockv0 {
namespace {
constexpr int initialWorkAllowance = 3;
constexpr std::chrono::milliseconds storageBusyTimeout{4000};

[[noreturn]] void sqlError(sqlite3* db) {
    throw StorageError(db);
}
}
StorageError::StorageError(sqlite3* db) : std::runtime_error(sqlite3_errmsg(db)), code_(sqlite3_errcode(db) & 0xff) {}

Statement::Statement(sqlite3* db, const char* sql, const Json& values) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) sqlError(db);
    try {
        int i = 0;
        for (const auto& value : values) {
            ++i;
            int rc;
            if (value.is_null())
                rc = sqlite3_bind_null(stmt_, i);
            else if (value.is_number_integer())
                rc = sqlite3_bind_int64(stmt_, i, value.get<Id>());
            else {
                const auto encoded = value.is_string() ? value.get<std::string>() : value.dump();
                rc = sqlite3_bind_text(stmt_, i, encoded.data(), static_cast<int>(encoded.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) sqlError(db);
        }
    } catch (...) {
        sqlite3_finalize(stmt_);
        throw;
    }
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
        (value->is_number_unsigned() && value->get<std::uint64_t>() > std::numeric_limits<Id>::max()) || value->get<Id>() < minimum)
        throw Conflict("Invalid " + std::string(key));
    return value->get<Id>();
}
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
    return {field(data, "threadId"), field(data, "runId"), field(data, "link"), field(data, "gatewayEnvironmentId")};
}

Store::Impl::Impl(std::filesystem::path file, MemorySeed seed, AutomationModelResolver model)
    : path(std::move(file)), memorySeed(std::move(seed)), automationModel(std::move(model)) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    try {
        if (rc != SQLITE_OK) sqlError(db);
        sqlite3_busy_timeout(db, storageBusyTimeout.count());
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
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
}

Store::Impl::~Impl() {
    sqlite3_close(db);
}
void Store::Impl::exec(const char* sql, const Json& args) const {
    query(sql, args).next();
}
Id Store::Impl::waitAnchor(Id task, Id wait) const {
    auto row = query("SELECT id FROM journal WHERE task_id=? AND kind='waiting' AND conversation_bot_id IS NULL"
                     " AND json_extract(data,'$.id')=? ORDER BY id DESC LIMIT 1",
                     {task, wait});
    return row.next() ? row.number(0) : 0;
}

Id Store::Impl::waitAllowance(Id task, WaitReason reason) const {
    if (reason == WaitReason::model_budget) return taskModelBudget(task).initialLimit;
    if (reason == WaitReason::budget) return initialWorkAllowance;
    return 0;
}

Id Store::Impl::journal(Id id, std::string_view kind, const Json& data) {
    exec("INSERT INTO journal(task_id,time,kind,data) VALUES(?,?,?,?)", {id, unixTime(), kind, data});
    const auto entry = last();
    queueChannelTaskUpdate(id, entry, kind, data);
    return entry;
}
std::optional<PeerExchange> Store::Impl::peerExchange(Id id) const {
    auto rows = query("SELECT id,kind,phase,last_message,sender_task,recipient_task FROM peer_exchanges"
                      " WHERE phase<>'closed' AND (sender_task=? OR recipient_task=?)",
                      {id, id});
    if (!rows.next()) return std::nullopt;
    return PeerExchange{rows.string(0), rows.string(1), rows.string(2), rows.string(3), rows.number(4), rows.number(5)};
}

bool Store::Impl::peerWorkRouteWouldCycle(Id sender, std::string_view environment, std::string_view project) const {
    return query(R"SQL(
WITH RECURSIVE source(env,project) AS (
  SELECT environment_id,project_id FROM tasks WHERE id=?1
), work_routes(source_env,source_project,current_env,current_project,pending_env,pending_project) AS (
  SELECT s.environment_id,s.project_id,r.environment_id,r.project_id,
         CASE WHEN r.state<>'cancelling' THEN r.pending_environment_id END,r.pending_project_id
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
)SQL",
                 {sender, environment, project})
        .next();
}

bool Store::Impl::recipientRouteWouldCycle(Id task, std::string_view environment, std::string_view project) const {
    if (project.empty()) return false;
    const auto peer = peerExchange(task);
    return peer && peer->recipient == task && peer->kind == "work" && peerWorkRouteWouldCycle(peer->sender, environment, project);
}

bool Store::Impl::replanReady(const TaskData& task) const {
    if (!task.replanning) return false;
    std::lock_guard lock(mutex);
    const Id id = task.id;
    const auto peer = peerExchange(id);
    return !peer || peer->allowsReplan(id);
}

bool Store::Impl::waitIsActionable(const TaskData& task) const {
    return !replanReady(task);
}

void Store::Impl::peerMessage(const PeerExchange& exchange, const Json& message) {
    journal(exchange.sender, "bot_message", message);
    journal(exchange.recipient, "bot_message", message);
}

void Store::Impl::requestCancellation(TaskData& task, SessionStop stop) {
    const Id id = task.id;
    const auto peer = peerExchange(id);
    const bool undispatched = task.state == TaskState::queued || isUndispatchedNotesWait(task);
    if (!task.pendingInstructions.empty()) task.cancelledInstructions = task.pendingInstructions;
    clearWait(task, "cancel");
    consumeEvent(task);
    journal(id, "cancel_requested",
            stop.participation ? Json{{"sessionParticipationId", stop.participation}, {"sessionStopId", stop.cutoff}} : Json::object());
    if (undispatched && !peer) {
        task.pendingDestination.reset();
        task.replanning = false;
        task.pendingInstructions.clear();
        task.state = TaskState::cancelled;
        journal(id, "cancelled", Json::object());
    } else
        task.state = TaskState::cancelling;
    save(task);
    if (peer && peer->sender == id) {
        auto recipient = load(peer->recipient);
        if (stop.participation && peer->kind == "work") {
            // The Session exit owns this task's Run, while the peer still owns its own task and Run.
            exec("UPDATE peer_exchanges SET phase='closed' WHERE id=?", {peer->id});
            if (recipient.state == TaskState::peer_waiting) {
                event(recipient, "peer-closed/" + peer->id, {{"kind", "intake"}});
                recipient.state = TaskState::deciding;
                save(recipient);
            }
        } else if (!terminal(recipient) && recipient.state != TaskState::cancelling)
            requestCancellation(recipient);
    }
}

Id Store::Impl::event(TaskData& task, std::string_view key, Json data) {
    exec("INSERT OR IGNORE INTO events(task_id,dedupe,data) VALUES(?,?,?)", {task.id, key, data});
    if (!sqlite3_changes(db)) return 0;
    const auto id = last();
    task.eventId = id;
    return id;
}
Json Store::Impl::currentEvent(const TaskData& task) const {
    const auto id = task.eventId;
    auto row = query("SELECT data,failures,retry_at,evidence,consumed FROM events WHERE id=? AND task_id=?", {id, task.id});
    if (!row.next()) return nullptr;
    auto result = row.json(0);
    result["id"] = id;
    result["failures"] = row.number(1);
    result["retryAt"] = row.number(2);
    result["evidence"] = row.json(3);
    result["consumed"] = row.number(4) != 0;
    return result;
}
void Store::Impl::consumeEvent(const TaskData& task) {
    releaseModelCall(takeReservedTaskCall(task.eventId));
    exec("UPDATE events SET consumed=1 WHERE id=? AND task_id=? AND consumed=0", {task.eventId, task.id});
}
std::optional<Wait> Store::Impl::currentWait(Id task) const {
    auto row = query("SELECT id,data FROM waits WHERE task_id=? AND resolution IS NULL", {task});
    if (!row.next()) return std::nullopt;
    return from_json(row.number(0), row.string(1));
}

std::optional<Wait> Store::Impl::projectedWait(Id task, const Statement& row, int col) const {
    const auto id = row.number(col);
    if (!id) return std::nullopt;
    auto wait = from_json(id, row.string(col + 1));
    if (const auto anchor = waitAnchor(task, wait.id)) wait.messageId = anchor;
    if (const auto allowance = waitAllowance(task, wait.reason())) wait.allowance = allowance;
    return wait;
}

Id Store::Impl::wait(TaskData& task, WaitDetails details, std::string text, std::optional<WaitRun> run) {
    return wait(task, {.details = std::move(details), .text = std::move(text), .run = run ? std::move(run) : taskWaitRun(task)});
}

Id Store::Impl::wait(TaskData& task, Wait waiting) {
    clearWait(task, "superseded");
    waiting.eventId = task.eventId;
    if (!waiting.run && waiting.reason() == WaitReason::unknown_delivery) {
        const auto action = outbox(task);
        if (action && action->command.candidate && !action->command.candidate->link.empty())
            waiting.run = WaitRun{*action->command.candidate};
    }
    waiting.actionId = task.actionId;
    auto data = to_json(waiting);
    exec("INSERT INTO waits(task_id,data) VALUES(?,?)", {task.id, data});
    const auto id = last();
    data["id"] = id;
    if (waiting.allowance) data["allowance"] = *waiting.allowance;
    journal(task.id, "waiting", data);
    task.state = TaskState::waiting;
    return id;
}

void Store::Impl::clearWait(TaskData& task, std::string_view resolution) {
    auto row = query("UPDATE waits SET resolution=? WHERE task_id=? AND resolution IS NULL RETURNING id", {resolution, task.id});
    if (!row.next()) return;
    const auto id = row.number(0);
    row.next();
    if (task.blocker && task.blocker->waitId == id) task.blocker.reset();
    journal(task.id, "wait_closed", {{"waitId", id}, {"resolution", resolution}});
}
void Store::Impl::unlockQuestion(TaskData& task, std::string_view reason) {
    if (!task.question) return;
    journal(task.id, "session_question_closed", {{"requestId", task.question->target.requestId}, {"resolution", reason}});
    task.question.reset();
}

std::string effectiveDoneWhen(const TaskData& task) {
    return task.criteriaProposal ? task.criteriaProposal->doneWhen : task.doneWhen;
}

Store::Store(std::filesystem::path db, MemorySeed seed, AutomationModelResolver automationModel)
    : p_(std::make_unique<Impl>(std::move(db), std::move(seed), std::move(automationModel))) {}
Store::~Store() = default;
const std::filesystem::path& Store::path() const {
    return p_->path;
}

Id Store::Impl::dataVersion() const {
    auto version = query("PRAGMA data_version");
    return version.next() ? version.number(0) : 0;
}

std::pair<Id, Id> Store::Impl::changeStamp() const {
    std::lock_guard lock(mutex);
    return {sqlite3_total_changes64(db), dataVersion()};
}

std::pair<Id, Id> Store::changeStamp() const {
    return p_->changeStamp();
}

Store::ConversationChanges Store::conversationChangesSince(std::optional<Id> after, std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto newest = p_->query("SELECT coalesce(max(id),0) FROM journal");
    newest.next();
    ConversationChanges changes{newest.number(0), {}};
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
)SQL",
                          {*after, changes.through, bot});
    while (rows.next()) changes.messages[rows.string(0)].push_back(rows.number(1));
    return changes;
}

Store::NotificationEvents Store::notificationEventsSince(const Person& viewer, const std::vector<std::string>& bots,
                                                         std::optional<Id> after, const std::map<std::string, Id>& previousScope) const {
    NotificationEvents result{after.value_or(0), Json::array(), {}, false};
    std::lock_guard lock(p_->mutex);
    Transaction snapshot(p_->db, Transaction::Mode::read);
    const Policy policy(*p_);
    std::map<std::string, Id> established, newlyVisible;
    for (const auto& bot : bots) {
        if (!policy.allows(viewer, Action::bot_view, bot)) continue;
        const auto previous = previousScope.find(bot);
        if (after && previous != previousScope.end()) {
            result.scope.emplace(bot, previous->second);
            established.emplace(bot, std::max(*after, previous->second));
        } else {
            result.scope.emplace(bot, 0);
            newlyVisible.emplace(bot, result.through);
        }
    }
    constexpr Id pageSize = 100;
    static const std::string notificationEvents = R"SQL(
WITH visible_bots AS (SELECT key AS bot,value AS after FROM json_each(?1)),
events(id,bot,kind) AS (
SELECT j.id,b.bot,
       CASE WHEN json_extract(j.data,'$.route')='error' THEN 'failure' ELSE 'reply' END
FROM visible_bots b JOIN journal j ON j.conversation_bot_id=b.bot AND j.id>b.after
WHERE j.kind='conversation_reply' AND json_extract(j.data,'$.route') IS NOT 'model_budget'
UNION ALL
SELECT j.id,b.bot,'failure'
FROM visible_bots b JOIN tasks t ON t.bot_id=b.bot JOIN journal j ON j.task_id=t.id AND j.id>b.after
WHERE j.conversation_bot_id IS NULL AND (
  (j.kind='report' AND (
    json_extract(j.data,'$.status') IN ('failed','rolled_back')
    OR ((json_extract(j.data,'$.status')='interrupted' OR (
           json_extract(j.data,'$.status')='cancelled'
           AND json_extract(j.data,'$.stopRequested')=0
           AND length(json_extract(j.data,'$.run.runId'))>0
           AND length(json_extract(j.data,'$.run.threadId'))>0
        )) AND json_extract(j.data,'$.stopRequested') IS NOT 1
        AND NOT EXISTS (
          SELECT 1 FROM journal stop
          WHERE stop.task_id=j.task_id AND stop.id<j.id AND stop.kind='interrupt_requested'
            AND (json_extract(stop.data,'$.run.environmentId'),json_extract(stop.data,'$.run.threadId'),
                 json_extract(stop.data,'$.run.runId'),json_extract(stop.data,'$.run.gatewayEnvironmentId'))
            IS (json_extract(j.data,'$.environmentId'),json_extract(j.data,'$.run.threadId'),
                json_extract(j.data,'$.run.runId'),json_extract(j.data,'$.run.gatewayEnvironmentId'))
        ))
  ))
  OR (j.kind='waiting' AND json_extract(j.data,'$.reason')='model_failure')
  OR (j.kind='completed' AND json_extract(j.data,'$.workflowOutcome')='failed')
)
)
)SQL";
    // The global page cursor advances only over events delivered for already visible Bots.
    if (!newlyVisible.empty()) {
        static const auto baselines = notificationEvents + " SELECT bot,max(id) FROM events GROUP BY bot";
        auto rows = p_->query(baselines.c_str(), Json::array({newlyVisible}));
        while (rows.next()) {
            result.scope.at(rows.string(0)) = rows.number(1);
            if (!after) result.through = std::max(result.through, rows.number(1));
        }
    }
    static const auto page = notificationEvents + " SELECT id,bot,kind FROM events ORDER BY id LIMIT ?2";
    auto rows = p_->query(page.c_str(), Json::array({established, pageSize + 1}));
    while (rows.next()) {
        if (result.events.size() == pageSize) {
            result.more = true;
            break;
        }
        result.through = rows.number(0);
        result.events.push_back({{"id", result.through}, {"botId", rows.string(1)}, {"kind", rows.string(2)}});
    }
    return result;
}

std::optional<Store::BotViewStamp> Store::botViewStamp() const {
    std::lock_guard lock(p_->mutex);
    if (!p_->channels.empty() || p_->query("SELECT 1 FROM protected_inputs WHERE status='fulfilled' LIMIT 1").next()) return std::nullopt;
    return BotViewStamp{changeStamp(), p_->gitCredentials.size()};
}

Id Store::submit(const Intake& item, const Person* requester) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    const auto id = p_->submitRequest(item, requester);
    transaction.commit();
    return id;
}

Id Store::Impl::submitRequest(const Intake& item, const Person* requester) {
    if (!requester) return submit(item);
    if (!Policy(*this).allows(*requester, Action::bot_talk, item.botId)) throw DecisionDenied{};
    sharedRedactor().requirePublic(requester->id);
    return submit(item, 0, nullptr, TaskSource::request, {}, requester->id);
}

Id Store::Impl::submit(const Intake& item, Id sourceMessageId, const OutboxSource* source, TaskSource sourceKind,
                       std::optional<TaskTrigger> trigger, std::optional<std::string> requester) {
    const bool consultation = sourceKind == TaskSource::consultation;
    if (item.botId.empty() || item.request.find_first_not_of(" \r\n\t") == std::string::npos ||
        item.request.size() > limits::messages::maxBodyBytes || (!consultation && item.environmentId.empty()) || !item.config.is_object())
        throw Conflict("Bot, nonempty request (up to " + std::to_string(limits::messages::maxBodyBytes) +
                       " bytes), and environment are required");
    if (botDeleted(item.botId)) throw Conflict("Bot has been deleted");
    if (botPaused(item.botId)) throw BotPaused{};
    const auto scrub = redactor(item.botId);
    scrub.requirePublic({item.botId, item.environmentId, item.projectId, item.config});
    initialModelCallLimit(item.config);
    const auto request = scrub.text(item.request);
    TaskData task{.state = consultation ? TaskState::deciding : TaskState::queued,
                  .botId = item.botId,
                  .request = request,
                  .intent = request,
                  .environmentId = item.environmentId,
                  .projectId = item.projectId,
                  .doneWhen = {},
                  .createdAt = unixTime(),
                  .allowance = {.remaining = initialWorkAllowance},
                  .config = item.config};
    if (sourceMessageId) task.sourceMessageId = sourceMessageId;
    if (source) {
        task.sourceTaskId = source->taskId;
        task.sourceResultRefs.push_back(*source);
    }
    task.trigger = std::move(trigger);
    task.requester = std::move(requester);
    exec("INSERT INTO tasks(bot_id,environment_id,project_id,state,generation,data) VALUES(?,?,?,?,0,?)",
         {task.botId, task.environmentId, task.projectId, taskStateName(task.state), encodeTask(task)});
    const auto id = last();
    if (sourceKind == TaskSource::request) {
        Json entry = {{"text", request},
                      {"messageId", sourceMessageId},
                      {"source", sourceMessageId ? "conversation"
                                 : task.trigger  ? "automation"
                                                 : "manual"}};
        if (task.trigger) entry["trigger"] = taskTriggerJson(*task.trigger);
        journal(id, "request", entry);
    }
    return id;
}

Json Store::reconciliationData(Id id, Id waitId, Json target, const IntakeResolver& intake) const {
    TaskData task;
    {
        std::lock_guard lock(p_->mutex);
        Transaction transaction(p_->db, Transaction::Mode::read);
        const auto wait = p_->currentWait(id);
        if (!wait || wait->id != waitId || wait->reason() != WaitReason::settings) return target;
        task = p_->load(id);
        const auto pending = p_->outbox(task);
        if (pending && (pending->phase != OutboxPhase::ready || pending->command.tool == CommandTool::start_in_session)) return target;
        transaction.commit();
    }

    const auto current = intake(task.botId, task.request);
    const auto environment = task.environmentId;
    const auto destinations = current.config.value("conversationEnvironments", Json::object());
    const auto project = task.projectId;
    const bool namedRoute = std::ranges::any_of(destinations, [&](const Json& alias) {
        return conversationEnvironmentId(alias) == environment &&
               (environment != current.environmentId || field(alias, "projectId") == project);
    });
    const bool settingsMatch =
        environment == current.environmentId ? (current.projectId.empty() || project == current.projectId) : current.environmentId.empty();
    if (!settingsMatch && !namedRoute) throw Conflict("The project route changed. Cancel this task and submit a new request.");
    target["session"] = current.config.value("session", Json::object());
    return target;
}

TaskTarget TaskTarget::from(const TaskData& task, Id waitId) {
    return {task.botId, task.id, waitId, task.currentRevision.value_or(0), task.generation};
}

void Store::operate(const Person& actor, const TaskTarget& target, std::string_view operation, const Json& data) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    p_->operate(actor, target, operation, data);
    transaction.commit();
}

void Store::Impl::operate(const Person& actor, const TaskTarget& target, std::string_view operation, const Json& input, Id sourceMessage,
                          std::optional<InboxKind> expectedKind) {
    const auto& [bot, id, waitId, revision, generation] = target;
    if (!Policy(*this).allows(actor, Action::bot_decide, bot)) throw DecisionDenied{};
    if (operation != "cancel" && botPaused(bot)) throw BotPaused{};
    auto task = load(id);
    if (task.botId != bot || task.currentRevision.value_or(0) != revision) throw RequestChanged{};
    const auto data = redactor(bot).payload(input);
    if (!data.is_object()) throw Conflict("Operation data must be an object");
    const auto wait = currentWait(id);
    if (operation == "cancel") {
        if (task.generation != generation) throw RequestChanged{};
        if (terminal(task) || task.state == TaskState::cancelling || (waitId && (!wait || wait->id != waitId))) throw DecisionUnavailable{};
        requestCancellation(task);
        return;
    }
    if (!waitIsActionable(task)) throw RequestChanged{};
    if (!wait || wait->id != waitId) {
        auto prior = query("SELECT resolution FROM waits WHERE id=? AND task_id=?", {waitId, id});
        if (!prior.next() || prior.string(0) == "request_changed") throw RequestChanged{};
        throw DecisionUnavailable{};
    }
    if (task.state != TaskState::waiting) throw DecisionUnavailable{};
    const auto reason = wait->reason();
    const auto* verification = wait->verification();
    const bool extendingVerificationDeadline = operation == "extend" && verification;
    if (task.generation != generation && !extendingVerificationDeadline) throw RequestChanged{};
    const auto kind = inboxWaitKind(reason);
    if (expectedKind && kind != expectedKind) throw RequestChanged{};
    auto next = TaskState::deciding;
    if (operation == "answer" && reason == WaitReason::user_question) {
        const auto owner = task.question ? task.question->target : waitQuestionTarget(*wait);
        if (owner != questionTarget(task, waitId)) throw RequestChanged{};
        const auto answer = field(data, "text");
        if (answer.find_first_not_of(" \r\n\t") == std::string::npos || answer.size() > limits::messages::maxWaitAnswerBytes)
            throw Conflict("A nonempty answer (up to " + std::to_string(limits::messages::maxWaitAnswerBytes) + " bytes) is required");
        auto answerKind = "user_answer";
        if (const auto* question = std::get_if<ProjectQuestionWait>(&wait->details)) {
            const auto choice = question->projects.find(answer);
            if (choice == question->projects.end()) throw Conflict("Choose one of the listed T3 projects");
            const auto environment = task.environmentId;
            const auto& project = choice->second;
            if (recipientRouteWouldCycle(id, environment, project))
                throw Conflict("This project would make project queues wait for one another. Choose another project or move the task.");
            task.projectId = project;
            rememberT3Project(bot, environment, project);
            next = TaskState::queued;
            answerKind = "project_selected";
        } else {
            auto updated = currentEvent(task);
            updated["userAnswer"] = answer;
            updated["userQuestion"] = wait->text;
            for (const auto* metadata : {"id", "failures", "retryAt", "evidence", "consumed"}) updated.erase(metadata);
            consumeEvent(task);
            event(task, "answer/" + std::to_string(waitId), updated);
        }
        Json entry = {{"text", answer}, {"waitId", waitId}, {"conversation", sourceMessage != 0}};
        if (sourceMessage) entry["messageId"] = sourceMessage;
        journal(id, answerKind, entry);
    } else if (operation == "approve" && reason == WaitReason::budget) {
        task.allowance.remaining += initialWorkAllowance;
        journal(id, "budget_granted", {{"waitId", waitId}, {"remaining", task.allowance.remaining}});
    } else if (operation == "approve" && reason == WaitReason::model_budget) {
        const Id eventId = task.eventId;
        const auto attempt = approveModelBudget(actor, taskModelBudget(id), "task_decision", 0, id, eventId);
        exec("UPDATE events SET reserved_attempt=? WHERE id=? AND task_id=?", {attempt, eventId, id});
        journal(id, "model_budget_granted", {{"waitId", waitId}, {"attemptId", attempt}});
    } else if (operation == "retry" && (reason == WaitReason::model_failure || isUndispatchedNotesWait(task))) {
        if (reason == WaitReason::bot_notes_unavailable) next = TaskState::queued;
        if (const auto* failure = std::get_if<ModelFailureWait>(&wait->details);
            failure && failure->code == WaitFailureCode::unsupported_reasoning_effort && automationModel) {
            if (const auto planner = automationModel(bot)) applyPlannerModel(task.config, *planner);
        }
        exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {task.eventId});
        task.feedback.reset();
        journal(id, "model_retry", {{"waitId", waitId}});
    } else if (operation == "extend" && reason == WaitReason::deadline) {
        if (!data.contains("deadline") || !data["deadline"].is_number_integer() || data["deadline"].get<Seconds>() <= unixTime())
            throw Conflict("The new deadline must be in the future");
        const auto deadline = data.at("deadline").get<Seconds>();
        if (verification) {
            auto& run = destinationRun(task, verification->actionId);
            if (!run.outstanding()) throw RequestChanged{};
            run.deadline = deadline;
        } else {
            task.deadline = deadline;
        }
        next = TaskState::running;
        journal(id, "deadline_extended", {{"deadline", deadline}, {"waitId", waitId}});
    } else if (operation == "reconcile" &&
               (reason == WaitReason::unknown_delivery || reason == WaitReason::auth || reason == WaitReason::settings)) {
        if (data.contains("session")) {
            const auto action = outbox(task);
            if (reason != WaitReason::settings || (action && action->phase != OutboxPhase::ready))
                throw Conflict("Session settings can only be corrected before an unsent action");
            task.config["session"] = normalizeSessionSettings(data["session"]);
        }
        if (task.actionId.empty()) {
            if (task.activeRun)
                next = TaskState::running;
            else if (task.projectId.empty())
                next = TaskState::queued;
        } else {
            auto action = outbox(task);
            if (!action) throw Conflict("No delivery to reconcile");
            auto& command = action->command;
            if (action->phase == OutboxPhase::ready && command.tool == CommandTool::answer_session_question) {
                const auto differs = [&](std::string_view key, const std::string& saved) {
                    const auto hint = field(data, key);
                    return !hint.empty() && hint != saved;
                };
                if (differs("threadId", command.target.threadId) || differs("runId", command.target.runId) ||
                    differs("questionId", command.questionId))
                    throw Conflict("The supplied T3 question is outside the saved answer destination");
            }
            RunRef hint{field(data, "threadId"), field(data, "runId"), {}, {}};
            if (!hint.threadId.empty() || !hint.runId.empty())
                command.candidate = std::move(hint);
            else
                command.candidate.reset();
            command.candidateQuestionId = field(data, "questionId");
            saveOutbox(*action);
            next = TaskState::sending;
        }
        journal(id, "reconcile_requested", {{"waitId", waitId}});
    } else {
        throw Conflict("Operation is unavailable for this waiting item");
    }
    clearWait(task, operation);
    if (operation == "answer" && task.question) task.question->target = questionTarget(task);
    task.state = next;
    save(task);
    if (kind) recordInboxDecision(actor, {bot, *kind, waitId, waitId, id, revision, generation}, operation);
}

bool Store::hasTask(Id id) const {
    std::lock_guard lock(p_->mutex);
    return p_->query("SELECT 1 FROM tasks WHERE id=?", {id}).next();
}

namespace {
Json journalRow(const Statement& row) {
    return {{"id", row.number(0)}, {"taskId", row.number(1)}, {"time", row.number(2)}, {"kind", row.string(3)}, {"data", row.json(4)}};
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
)SQL",
                          {taskId, after, std::clamp(limit, 1u, 200u)});
    Json result = Json::array();
    while (rows.next()) result.push_back(journalRow(rows));
    return result;
}

Json Store::conversation(std::string_view bot, Id before, unsigned limit, ConversationEvents events, std::string_view viewer) const {
    if (before == std::numeric_limits<Id>::min()) return Json::array();
    const auto through = before ? before - 1 : std::numeric_limits<Id>::max();
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
WITH visible AS NOT MATERIALIZED (
  SELECT id,task_id,conversation_bot_id FROM journal
  WHERE id<=?2
    AND (?3=0 OR kind IN ('conversation_reply','report','completed','pr_review_requested')
         OR (kind='waiting' AND json_extract(data,'$.reason') IN ('user_question','model_failure')))
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
         THEN json_set(j.data,'$.request',t.request) ELSE j.data END AS data
  FROM page_ids ids JOIN journal j ON j.id=ids.id
  LEFT JOIN tasks t ON j.kind='cancelled' AND j.conversation_bot_id IS NULL AND t.id=j.task_id
)
SELECT page.id,page.task_id,page.time,page.kind,
  CASE WHEN m.id IS NULL THEN page.data
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
  channel.message_id IS NOT NULL,w.id,w.data
FROM page LEFT JOIN messages m ON m.id=page.id
LEFT JOIN conversation_input_status input ON input.message_id=m.id
LEFT JOIN waits w ON page.kind='user_answer'
  AND w.id=json_extract(page.data,'$.waitId') AND w.task_id=page.task_id
LEFT JOIN channel_messages channel ON page.kind='conversation_message' AND channel.message_id=m.id
ORDER BY page.id DESC)SQL",
                          {bot, through, events == ConversationEvents::incoming ? 1 : 0, std::clamp(limit, 1u, 200u)});
    Json result = Json::array();
    while (rows.next()) {
        auto event = journalRow(rows);
        if (event.at("kind") == "memory_remembered") p_->projectMemoryNotice(bot, event.at("data"));
        if (rows.number(5)) event["data"].update(p_->channelContext(rows.number(0), bot, nullptr, viewer));
        if (const auto waitId = rows.number(6)) event["data"]["question"] = from_json(waitId, rows.string(7)).text;
        result.push_back(std::move(event));
    }
    std::reverse(result.begin(), result.end());
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
    WHERE m.id=sender.source_message_id AND m.bot_id=sender.bot_id
      AND original.kind='conversation_message' AND m.person_id=?5
      AND m.text=json_extract(j.data,'$.forwardedMessage.text')
      AND original.time=json_extract(j.data,'$.forwardedMessage.time')
  ) ELSE 0 END
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
JOIN journal j ON j.task_id=p.sender_task AND j.kind='bot_message'
  -- Unary + removes TEXT affinity so SQLite can use the exchange ID in the JSON expression index.
  AND json_extract(j.data,'$.exchangeId') = +p.id
WHERE ((sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1))
)SQL") + (from ? " AND j.id>=?3 ORDER BY j.id ASC LIMIT ?4" : " AND (?3=0 OR j.id<?3) ORDER BY j.id DESC LIMIT ?4");
    auto rows = p_->query(sql.c_str(), {bot, peer, from ? from : before, std::clamp(limit, 1u, 200u), viewer});
    Json result = Json::array();
    while (rows.next()) {
        auto event = journalRow(rows);
        if (rows.number(5)) event["forwardedFromViewer"] = true;
        result.push_back(std::move(event));
    }
    if (!from) std::reverse(result.begin(), result.end());
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
)SQL",
                         {bot, peer, event});
    return row.next() ? row.number(0) : 0;
}

std::vector<std::pair<std::string, std::string>> Store::peerExchangePairs() const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT DISTINCT sender.bot_id,recipient.bot_id
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
)SQL");
    std::vector<std::pair<std::string, std::string>> result;
    while (rows.next()) result.emplace_back(rows.string(0), rows.string(1));
    return result;
}

bool Store::hasPeerExchange(std::string_view bot, std::string_view peer) const {
    std::lock_guard lock(p_->mutex);
    return p_
        ->query(R"SQL(
SELECT 1
FROM peer_exchanges p JOIN tasks sender ON sender.id=p.sender_task JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE (sender.bot_id=?1 AND recipient.bot_id=?2) OR (sender.bot_id=?2 AND recipient.bot_id=?1)
LIMIT 1
)SQL",
                {bot, peer})
        .next();
}
} // namespace flockv0
