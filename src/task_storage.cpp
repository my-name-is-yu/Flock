#include "task_storage.hpp"
#include "storage.hpp"
#include "record_decode.hpp"
#include "outbox_storage.hpp"

#include <algorithm>
#include <tuple>

namespace flockv0 {
namespace {
constexpr std::array states{
    std::pair{TaskState::queued, std::string_view{"queued"}},         std::pair{TaskState::deciding, std::string_view{"deciding"}},
    std::pair{TaskState::sending, std::string_view{"sending"}},       std::pair{TaskState::running, std::string_view{"running"}},
    std::pair{TaskState::waiting, std::string_view{"waiting"}},       std::pair{TaskState::peer_waiting, std::string_view{"peer_waiting"}},
    std::pair{TaskState::cancelling, std::string_view{"cancelling"}}, std::pair{TaskState::completed, std::string_view{"completed"}},
    std::pair{TaskState::cancelled, std::string_view{"cancelled"}},
};

PendingDestination decodeDestination(const Json& data, const RecordDecoder& row) {
    return {{row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "projectId"),
             row.get<std::string>(data, "gatewayEnvironmentId")},
            row.get<bool>(data, "pinned")};
}

Json destinationJson(const PendingDestination& destination) {
    const auto& [environment, project, gateway] = destination.route;
    return {{"environmentId", environment}, {"projectId", project}, {"gatewayEnvironmentId", gateway}, {"pinned", destination.pinned}};
}
}

std::string_view taskStateName(TaskState state) {
    return enumName(states, state);
}

bool terminal(const TaskData& task) {
    return task.state == TaskState::completed || task.state == TaskState::cancelled;
}

TaskState decodeTaskState(Id id, std::string_view state) {
    if (auto known = enumValue(states, state)) return *known;
    throw RecordDecodeError("tasks", std::to_string(id), "state", "unknown task state");
}

TaskData decodeTask(Id id, std::string_view state, Id generation, std::string_view text) {
    const auto rowId = std::to_string(id);
    const RecordDecoder row("tasks", rowId);
    auto data = row.parse(text);
    row.object(data);
    TaskData task;
    task.id = id;
    task.state = decodeTaskState(id, state);
    task.generation = generation;
    task.botId = row.get<std::string>(data, "botId");
    task.request = row.get<std::string>(data, "request");
    task.intent = row.get<std::string>(data, "intent");
    task.environmentId = row.get<std::string>(data, "environmentId");
    task.projectId = row.get<std::string>(data, "projectId");
    task.doneWhen = row.get<std::string>(data, "doneWhen");
    row.object(row.require(data, "config"), "config");
    task.config = std::move(data.at("config"));
    task.gatewayEnvironmentId = row.optional<std::string>(data, "gatewayEnvironmentId");
    task.currentRevision = row.optional<Id>(data, "currentRevision");
    if (const auto found = data.find("pendingDestination"); found != data.end())
        task.pendingDestination = decodeDestination(*found, row.nested("pendingDestination"));
    for (const auto* key : {"id", "state", "generation", "botId", "request", "intent", "environmentId", "projectId", "doneWhen", "config",
                            "gatewayEnvironmentId", "currentRevision", "pendingDestination", "revisions"})
        data.erase(key);
    task.rest = std::move(data);
    return task;
}

TaskData decodeTaskRow(const Statement& row, int col) {
    return decodeTask(row.number(col + 1), row.string(col + 2), row.number(col + 3), row.string(col));
}

Json encodeTask(const TaskData& task) {
    auto data = task.rest;
    data.update({{"botId", task.botId},
                 {"request", task.request},
                 {"intent", task.intent},
                 {"environmentId", task.environmentId},
                 {"projectId", task.projectId},
                 {"doneWhen", task.doneWhen},
                 {"config", task.config}});
    if (task.gatewayEnvironmentId) data["gatewayEnvironmentId"] = *task.gatewayEnvironmentId;
    if (task.pendingDestination) data["pendingDestination"] = destinationJson(*task.pendingDestination);
    if (task.currentRevision) data["currentRevision"] = *task.currentRevision;
    return data;
}

Json waitQuestionTarget(const Wait& wait) {
    const auto* question = std::get_if<ModelQuestionWait>(&wait.details);
    return {{"waitId", wait.id},
            {"eventId", wait.eventId},
            {"run", waitRunJson(wait.run)},
            {"requestId", question ? question->questionId : std::string{}}};
}

namespace {
Json taskWaitJson(const TaskData& task, const std::optional<Wait>& wait) {
    if (!wait) return nullptr;
    auto result = to_json(*wait);
    result["id"] = wait->id;
    if (wait->messageId) result["messageId"] = *wait->messageId;
    if (wait->allowance) result["allowance"] = *wait->allowance;
    if (wait->reason() == WaitReason::user_question && !task.rest.at("question").is_object()) result["target"] = waitQuestionTarget(*wait);
    return result;
}

Json blockerJson(const std::optional<TaskBlocker>& blocker) {
    if (!blocker) return nullptr;
    return {{"id", blocker->id}, {"botId", blocker->botId}, {"request", blocker->request}, {"state", taskStateName(blocker->state)}};
}
}

Json taskJson(const TaskView& view) {
    const auto& task = view.task;
    auto data = encodeTask(task);
    data["id"] = task.id;
    data["state"] = taskStateName(task.state);
    data["generation"] = task.generation;
    data["wait"] = taskWaitJson(task, view.wait);
    data["blockedBy"] = blockerJson(view.blockedBy);
    data["pendingAction"] = view.pendingAction;
    data["runs"] = view.runs;
    data["revisions"] = Json::array();
    for (const auto& revision : view.revisions)
        data["revisions"].push_back({{"id", revision.id},
                                     {"number", revision.number},
                                     {"intent", revision.intent},
                                     {"doneWhen", revision.doneWhen},
                                     {"sourceMessageId", revision.sourceMessageId.value_or(0)}});
    return data;
}

Json taskJson(const std::vector<TaskView>& tasks) {
    auto result = Json::array();
    for (const auto& view : tasks) result.push_back(taskJson(view));
    return result;
}

TaskData Store::Impl::load(Id id) const {
    auto row = query("SELECT data,id,state,generation FROM tasks WHERE id=?", {id});
    if (!row.next()) throw Conflict("Task does not exist");
    return decodeTaskRow(row);
}

void Store::Impl::save(TaskData& task) {
    const auto generation = task.generation + 1;
    exec("UPDATE tasks SET state=?,generation=?,data=?,environment_id=?,project_id=? WHERE id=?",
         {taskStateName(task.state), generation, encodeTask(task), task.environmentId, task.projectId, task.id});
    task.generation = generation;
}

namespace {
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
       task.data,task.id,task.state,task.generation,w.id,w.data
FROM page JOIN tasks task ON task.id=page.id
LEFT JOIN blockers ON blockers.id=page.id
LEFT JOIN waits w ON w.task_id=task.id AND w.resolution IS NULL
ORDER BY task.id DESC)SQL";
}

std::optional<TaskBlocker> taskBlocker(const Statement& row) {
    const auto id = row.number(0);
    if (!id) return std::nullopt;
    return TaskBlocker{id, row.string(1), row.string(2), decodeTaskState(id, row.string(3))};
}
}

std::optional<TaskBlocker> Store::Impl::queueBlocker(Id id) const {
    static const auto sql = "WITH requested(id) AS (VALUES(?)), " + queueBlockers + "SELECT owner_id,bot_id,request,state FROM blockers";
    auto row = query(sql.c_str(), {id});
    return row.next() ? taskBlocker(row) : std::nullopt;
}

TaskView Store::task(Id id) const {
    std::lock_guard lock(p_->mutex);
    return p_->taskDetails(id, true);
}

TaskView Store::Impl::taskDetails(Id id, bool includeBlocker) const {
    auto row = query("SELECT t.data,t.id,t.state,t.generation,w.id,w.data FROM tasks t"
                     " LEFT JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL WHERE t.id=?",
                     {id});
    if (!row.next()) throw Conflict("Task does not exist");
    TaskView view{.task = decodeTaskRow(row), .wait = projectedWait(id, row, 4)};
    if (includeBlocker && view.task.state == TaskState::queued) view.blockedBy = queueBlocker(id);
    addTaskDetails(view);
    return view;
}

void Store::Impl::addTaskDetails(TaskView& view) const {
    const auto& task = view.task;
    if (auto pending = outbox(task)) {
        pending->sessionMemory.reset();
        auto data = to_json(*pending);
        data["id"] = pending->command.actionId;
        data["phase"] = outboxPhaseName(pending->phase);
        view.pendingAction = std::move(data);
    }
    auto revisions = query("SELECT id,number,intent,done_when,source_message_id FROM task_revisions"
                           " WHERE task_id=? ORDER BY number",
                           {task.id});
    while (revisions.next()) {
        const auto source = revisions.number(4);
        view.revisions.push_back({revisions.number(0), revisions.number(1), revisions.string(2), revisions.string(3),
                                  source ? std::optional{source} : std::nullopt});
    }
    auto runs = query("SELECT runs.thread_id,runs.run_id,runs.link,runs.action_id,runs.environment_id,"
                      "json_extract(outbox.data,'$.revisionId'),json_extract(outbox.data,'$.projectId') FROM runs"
                      " JOIN outbox ON runs.action_id=outbox.id WHERE runs.task_id=? ORDER BY outbox.rowid",
                      {task.id});
    while (runs.next())
        view.runs.push_back({{"threadId", runs.string(0)},
                             {"runId", runs.string(1)},
                             {"link", runs.string(2)},
                             {"actionId", runs.string(3)},
                             {"environmentId", runs.string(4)},
                             {"revisionId", runs.number(5)},
                             {"projectId", runs.string(6)},
                             {"ownerBotId", task.botId}});
}

std::vector<TaskView> Store::tasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    static const auto sql = taskPageSql("(?='' OR bot_id=?) AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?");
    auto rows = p_->query(sql.c_str(), {bot, bot, before, before, std::clamp(limit, 1u, 200u)});
    std::vector<TaskView> result;
    while (rows.next()) {
        auto& view = result.emplace_back(decodeTaskRow(rows, 4), p_->projectedWait(rows.number(5), rows, 8), taskBlocker(rows));
        p_->addTaskDetails(view);
    }
    return result;
}

std::vector<Store::ActiveTask> Store::activeTasks(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    static const auto sql = taskPageSql("bot_id=? AND state NOT IN ('completed','cancelled')");
    auto rows = p_->query(sql.c_str(), {bot});
    std::vector<ActiveTask> result;
    while (rows.next()) result.push_back({decodeTaskRow(rows, 4), p_->projectedWait(rows.number(5), rows, 8), taskBlocker(rows)});
    return result;
}

std::map<std::string, std::vector<TaskPresence>> Store::activeTaskStatesByBot() const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT bot_id,state,coalesce(json_extract(data,'$.replanning'),0) AS replanning,min(id)
FROM tasks WHERE state NOT IN ('completed','cancelled')
GROUP BY bot_id,state,replanning)SQL");
    std::map<std::string, std::vector<TaskPresence>> result;
    while (rows.next()) result[rows.string(0)].push_back({decodeTaskState(rows.number(3), rows.string(1)), rows.number(2) != 0});
    return result;
}

TaskView Store::Impl::sessionTask(Id id) const {
    auto view = taskDetails(id, false);
    auto reports = query(R"SQL(
SELECT coalesce(nullif(json_extract(data,'$.run.environmentId'),''),json_extract(data,'$.environmentId')),
       json_extract(data,'$.run.threadId'),json_extract(data,'$.run.runId'),json_extract(data,'$.status'),
       json_extract(data,'$.sessionCreatedAt')
FROM journal WHERE task_id=? AND kind IN ('report','run_settled') ORDER BY id
)SQL",
                         {id});
    using SessionKey = std::pair<std::string, std::string>;
    using RunKey = std::tuple<std::string, std::string, std::string>;
    std::map<RunKey, std::string> statuses;
    std::map<SessionKey, std::string> createdAt;
    while (reports.next()) {
        const auto environment = reports.string(0);
        const auto thread = reports.string(1);
        auto status = reports.string(3);
        if (!status.empty()) statuses.insert_or_assign(RunKey{environment, thread, reports.string(2)}, std::move(status));
        auto timestamp = reports.string(4);
        if (!timestamp.empty()) createdAt.try_emplace(SessionKey{environment, thread}, std::move(timestamp));
    }
    for (auto& run : view.runs) {
        const SessionKey session{field(run, "environmentId"), field(run, "threadId")};
        const auto status = statuses.find({session.first, session.second, field(run, "runId")});
        if (status != statuses.end()) run["status"] = status->second;
        const auto timestamp = createdAt.find(session);
        if (timestamp != createdAt.end()) run["sessionCreatedAt"] = timestamp->second;
    }
    return view;
}

std::vector<TaskView> Store::sessionTasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query("SELECT id FROM tasks WHERE bot_id=? AND (?=0 OR id<?)"
                          " AND EXISTS (SELECT 1 FROM runs WHERE task_id=tasks.id)"
                          " ORDER BY id DESC LIMIT ?",
                          {bot, before, before, std::clamp(limit, 1u, 200u)});
    std::vector<TaskView> result;
    while (rows.next()) result.push_back(p_->sessionTask(rows.number(0)));
    return result;
}

std::pair<Id, std::vector<TaskView>> Store::sessionChangesSince(std::string_view bot, std::optional<Id> after) const {
    std::lock_guard lock(p_->mutex);
    auto newest = p_->query("SELECT coalesce(max(id),0) FROM journal WHERE kind IN ('run_started','report')");
    newest.next();
    const auto through = newest.number(0);
    std::vector<TaskView> tasks;
    if (after && *after < through) {
        auto rows = p_->query(R"SQL(
SELECT DISTINCT j.task_id FROM journal j CROSS JOIN tasks t ON t.id=j.task_id
WHERE j.kind IN ('run_started','report') AND j.id>? AND j.id<=? AND t.bot_id=?
)SQL",
                              {*after, through, bot});
        while (rows.next()) tasks.push_back(p_->sessionTask(rows.number(0)));
    }
    return {through, std::move(tasks)};
}
}
