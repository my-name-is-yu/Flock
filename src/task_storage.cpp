#include "flock/task_history.hpp"
#include "flock/task_list.hpp"
#include "task_operations.hpp"
#include "conversation_storage.hpp"
#include "journal_storage.hpp"
#include <sqlite3.h>
#include "task_events.hpp"
#include "database.hpp"
#include "wait_storage.hpp"
#include "task_records.hpp"
#include "task_storage.hpp"
#include "task_context_storage.hpp"
#include "task_interaction_storage.hpp"
#include "journal.hpp"
#include "journal_task.hpp"
#include "record_decode.hpp"
#include "outbox_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"
#include "run_ref_storage.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
std::string effectiveDoneWhen(const TaskData& task) {
    return task.criteriaProposal ? task.criteriaProposal->doneWhen : task.doneWhen;
}

namespace {
constexpr std::array states{
    std::pair{TaskState::queued, std::string_view{"queued"}},
    std::pair{TaskState::deciding, std::string_view{"deciding"}},
    std::pair{TaskState::sending, std::string_view{"sending"}},
    std::pair{TaskState::running, std::string_view{"running"}},
    std::pair{TaskState::waiting, std::string_view{"waiting"}},
    std::pair{TaskState::peer_waiting, std::string_view{"peer_waiting"}},
    std::pair{TaskState::external_waiting, std::string_view{"external_waiting"}},
    std::pair{TaskState::cancelling, std::string_view{"cancelling"}},
    std::pair{TaskState::completed, std::string_view{"completed"}},
    std::pair{TaskState::cancelled, std::string_view{"cancelled"}},
};

constexpr std::array stopIntents{
    std::pair{RunStopIntent::cancel, std::string_view{"cancel"}},
    std::pair{RunStopIntent::replan, std::string_view{"replan"}},
};

constexpr std::array unavailableEvidence{
    std::pair{RunEvidence::Availability::ReportUnavailable, std::string_view{"report_unavailable"}},
    std::pair{RunEvidence::Availability::DiagnosticUnavailable, std::string_view{"diagnostic_unavailable"}},
};

constexpr std::array destinationPhases{
    std::pair{DestinationRunPhase::queued, std::string_view{"queued"}},
    std::pair{DestinationRunPhase::running, std::string_view{"running"}},
    std::pair{DestinationRunPhase::confirmation_pending, std::string_view{"confirmation_pending"}},
};

constexpr std::array runOutcomes{
    std::pair{RunOutcome::all_completed, std::string_view{"all_completed"}},
    std::pair{RunOutcome::partial_failure, std::string_view{"partial_failure"}},
    std::pair{RunOutcome::failed, std::string_view{"failed"}},
};
}

RunEvidence::Availability decodeReportAvailability(const Json& data, const RecordDecoder& row) {
    const auto status = row.optional<std::string>(data, "evidenceStatus");
    if (!status) return RunEvidence::Availability::Available;
    const auto availability = enumValue(unavailableEvidence, *status);
    if (!availability) row.fail("evidenceStatus", "unknown Run evidence status");
    return *availability;
}

namespace {
RunTarget decodeRunTarget(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "name"),
            {row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "projectId"),
             row.optional<std::string>(data, "gatewayEnvironmentId").value_or("")}};
}

DestinationRun decodeDestinationRun(const Json& data, const RecordDecoder& row) {
    auto [name, route] = decodeRunTarget(data, row);
    DestinationRun run{
        .actionId = row.get<std::string>(data, "actionId"), .name = std::move(name), .environmentId = std::move(route.environmentId)};
    if (data.contains("ownerBotId") || data.contains("revisionId"))
        run.assignment = decodeRunAssignment(data, row);
    else
        run.assignment.projectId = std::move(route.projectId);
    run.explicitDirectGateway = route.gatewayEnvironmentId.empty() && data.contains("gatewayEnvironmentId");
    run.ref = {row.optional<std::string>(data, "threadId").value_or(""), row.optional<std::string>(data, "runId").value_or(""),
               row.optional<std::string>(data, "link").value_or(""), std::move(route.gatewayEnvironmentId)};
    run.status = destinationRunStatusFromName(row.get<std::string>(data, "status"));
    run.retryAt = row.optional<Seconds>(data, "retryAt");
    run.stopRetryAt = row.optional<Seconds>(data, "stopRetryAt");
    run.evidenceAttempts = row.optional<unsigned>(data, "evidenceAttempts").value_or(0);
    run.detail = row.optional<std::string>(data, "detail");
    if (data.contains("commit") || data.contains("report") || data.contains("sessionCreatedAt") || data.contains("evidenceStatus"))
        run.report =
            DestinationReport{row.get<std::string>(data, "commit"), row.get<std::string>(data, "report"),
                              row.optional<std::string>(data, "sessionCreatedAt").value_or(""), decodeReportAvailability(data, row)};
    return run;
}

std::optional<AssignedRun> decodeNullableRun(const Json& data, const RecordDecoder& row, std::string_view key) {
    const auto& value = row.require(data, key);
    if (value.is_null()) return std::nullopt;
    return row.nested(key).decodeClosed(value, decodeAssignedRun);
}

RunStop decodeRunStop(const Json& data, const RecordDecoder& row) {
    const auto intent = enumValue(stopIntents, row.get<std::string>(data, "intent"));
    if (!intent) row.fail("intent", "unknown Run stop intent");
    return {*intent, decodeNullableRun(data, row, "run"), row.optional<Seconds>(data, "retryAt"),
            row.optional<Seconds>(data, "reconcileAt")};
}

PendingRunReport decodePendingRunReport(const Json& data, const RecordDecoder& row) {
    return {decodeSettledRunReport(row.require(data, "report"), row.nested("report")), row.get<Seconds>(data, "retryAt"),
            row.get<int>(data, "attempts")};
}

Json destinationJson(const PendingDestination& destination) {
    const auto& [environment, project, gateway] = destination.route;
    return {{"environmentId", environment}, {"projectId", project}, {"gatewayEnvironmentId", gateway}, {"pinned", destination.pinned}};
}

SessionTarget decodeSessionTarget(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "threadId"),
            row.optional<std::string>(data, "gatewayEnvironmentId").value_or("")};
}

Json sessionTargetJson(const SessionTarget& session) {
    Json data{{"environmentId", session.environmentId}, {"threadId", session.threadId}};
    if (!session.gatewayEnvironmentId.empty()) data["gatewayEnvironmentId"] = session.gatewayEnvironmentId;
    return data;
}
}

std::string_view taskStateName(TaskState state) {
    return enumName(states, state);
}

DestinationRunStatus destinationRunStatusFromName(std::string status) {
    if (const auto phase = enumValue(destinationPhases, status)) return *phase;
    return status;
}

std::string_view destinationRunStatusName(const DestinationRunStatus& status) {
    if (const auto* phase = std::get_if<DestinationRunPhase>(&status)) return enumName(destinationPhases, *phase);
    return std::get<std::string>(status);
}

std::string_view runOutcomeName(RunOutcome outcome) {
    return enumName(runOutcomes, outcome);
}

RunOutcome decodeRunOutcome(std::string_view value, const RecordDecoder& row) {
    if (const auto outcome = enumValue(runOutcomes, value)) return *outcome;
    row.fail("runOutcome", "unknown task Run outcome");
}

std::optional<std::string_view> runEvidenceStatusName(RunEvidence::Availability availability) {
    if (availability == RunEvidence::Availability::Available) return std::nullopt;
    return enumName(unavailableEvidence, availability);
}

Json runTargetJson(const RunTarget& target) {
    const auto& [environment, project, gateway] = target.route;
    Json data{{"name", target.name}, {"environmentId", environment}, {"projectId", project}};
    if (!gateway.empty() && gateway != environment) data["gatewayEnvironmentId"] = gateway;
    return data;
}

Json runTargetsJson(std::span<const RunTarget> targets) {
    auto result = Json::array();
    for (const auto& target : targets) result.push_back(runTargetJson(target));
    return result;
}

Json destinationRunSummaryJson(const DestinationRun& run) {
    Json data{{"name", run.name},
              {"environmentId", run.environmentId},
              {"projectId", run.assignment.projectId},
              {"status", destinationRunStatusName(run.status)}};
    if (!run.ref.threadId.empty()) {
        data["threadId"] = run.ref.threadId;
        data["link"] = run.ref.link;
    }
    if (!run.ref.runId.empty()) data["runId"] = run.ref.runId;
    if (const auto& report = run.report) {
        data["commit"] = report->commit;
        if (const auto status = runEvidenceStatusName(report->evidence)) data["evidenceStatus"] = *status;
    }
    return data;
}

Json destinationRunJson(const DestinationRun& run) {
    auto data = destinationRunSummaryJson(run);
    data["actionId"] = run.actionId;
    if (!run.ref.gatewayEnvironmentId.empty() || run.explicitDirectGateway) data["gatewayEnvironmentId"] = run.ref.gatewayEnvironmentId;
    if (!run.assignment.ownerBotId.empty()) encodeRunAssignment(data, run.assignment);
    if (run.retryAt) data["retryAt"] = *run.retryAt;
    if (run.stopRetryAt) data["stopRetryAt"] = *run.stopRetryAt;
    if (run.evidenceAttempts) data["evidenceAttempts"] = run.evidenceAttempts;
    if (const auto& report = run.report) {
        data["report"] = report->text;
        if (!report->sessionCreatedAt.empty()) data["sessionCreatedAt"] = report->sessionCreatedAt;
    }
    if (run.detail) data["detail"] = *run.detail;
    return data;
}

bool terminal(const TaskData& task) {
    return task.state == TaskState::completed || task.state == TaskState::cancelled;
}

TaskState decodeTaskState(Id id, std::string_view state) {
    if (auto known = enumValue(states, state)) return *known;
    throw RecordDecodeError("tasks", std::to_string(id), "state", "unknown task state");
}

Json assignedRunJson(const AssignedRun& run) {
    auto data = runJson(run.ref);
    data["environmentId"] = run.environmentId;
    encodeRunAssignment(data, run.assignment);
    return data;
}

Json assignedRunJson(const std::optional<AssignedRun>& run) {
    return run ? assignedRunJson(*run) : Json(nullptr);
}

Json runStopJson(const RunStop& stop) {
    Json data{{"intent", enumName(stopIntents, stop.intent)}, {"run", assignedRunJson(stop.run)}};
    if (stop.retryAt) data["retryAt"] = *stop.retryAt;
    if (stop.reconcileAt) data["reconcileAt"] = *stop.reconcileAt;
    return data;
}

Json settledRunReportJson(const SettledRunReport& report) {
    return encodeRunReport(runReportEntry(report));
}

SessionOrigin decodeSessionOrigin(const Json& data, const RecordDecoder& row) {
    return {decodeSessionTarget(data, row), row.get<std::string>(data, "title")};
}

Json sessionOriginJson(const SessionOrigin& origin) {
    auto data = sessionTargetJson(origin.target);
    data["title"] = origin.title;
    return data;
}

namespace {
void decodeTaskFields(TaskData& task, const Json& data, const RecordDecoder& row) {
    task.intent = row.get<std::string>(data, "intent");
    task.doneWhen = row.get<std::string>(data, "doneWhen");
    task.config = row.object(row.require(data, "config"), "config");
    task.gatewayEnvironmentId = row.optional<std::string>(data, "gatewayEnvironmentId");
    task.eventId = row.get<Id>(data, "eventId");
    if (const auto found = row.find(data, "runStop"); found != data.end())
        task.runStop = row.nested("runStop").decodeClosed(*found, decodeRunStop);
    task.queuedInT3 = row.optional<bool>(data, "queuedInT3").value_or(false);
    task.cancelledByBotPause = row.optional<bool>(data, "cancelledByBotPause").value_or(false);
    task.replanningDetail = row.optional<std::string>(data, "replanningDetail");
    task.cancellationDetail = row.optional<std::string>(data, "cancellationDetail");
    if (const auto found = row.find(data, "pendingInstructions"); found != data.end())
        task.pendingInstructions = taskInstructions(*found, task.id, "pendingInstructions");
    if (const auto found = row.find(data, "cancelledInstructions"); found != data.end())
        task.cancelledInstructions = taskInstructions(*found, task.id, "cancelledInstructions");
    if (const auto found = row.find(data, "pendingRunReport"); found != data.end())
        task.pendingRunReport = row.nested("pendingRunReport").decodeClosed(*found, decodePendingRunReport);
    task.t3CheckAt = row.optional<Seconds>(data, "t3CheckAt");
    task.newSessionRequired = row.optional<bool>(data, "newSessionRequired").value_or(false);
    task.freshContextRequested = row.optional<bool>(data, "freshContextRequested").value_or(false);
    task.launchWarning = row.optional<std::string>(data, "launchWarning");
    task.createdAt = row.get<Seconds>(data, "createdAt");
    auto criteria = row.optional<std::string>(data, "requestedDoneWhen");
    const auto criteriaSource = row.optional<Id>(data, "criteriaSourceMessageId");
    if (criteria)
        task.criteriaProposal = TaskCriteriaProposal{std::move(*criteria), criteriaSource};
    else if (criteriaSource)
        row.fail("criteriaSourceMessageId", "requires proposed completion criteria");
    task.requestedCommit = row.optional<std::string>(data, "requestedCommit");
    task.feedback = row.optional<std::string>(data, "feedback");
    task.waitingNote = row.optional<std::string>(data, "waitingNote");
    decodeTaskInteraction(task, data, row);
    decodeTaskContext(task, data, row);
}
}

TaskData decodeTaskPayload(TaskData task, std::string_view text) {
    const auto rowId = std::to_string(task.id);
    const RecordDecoder row("tasks", rowId);
    return row.decodeClosed(row.parse(text), [&](const Json& data, const RecordDecoder& fields) {
        decodeTaskFields(task, data, fields);
        return std::move(task);
    });
}

Json encodeTaskPayload(const TaskData& task) {
    Json data{{"intent", task.intent},
              {"doneWhen", task.doneWhen},
              {"config", task.config},
              {"eventId", task.eventId},
              {"createdAt", task.createdAt}};
    if (task.gatewayEnvironmentId) data["gatewayEnvironmentId"] = *task.gatewayEnvironmentId;
    if (task.runStop) data["runStop"] = runStopJson(*task.runStop);
    if (task.queuedInT3) data["queuedInT3"] = true;
    if (task.cancelledByBotPause) data["cancelledByBotPause"] = true;
    if (task.replanningDetail) data["replanningDetail"] = *task.replanningDetail;
    if (task.cancellationDetail) data["cancellationDetail"] = *task.cancellationDetail;
    if (!task.pendingInstructions.empty()) data["pendingInstructions"] = instructionsJson(task.pendingInstructions);
    if (!task.cancelledInstructions.empty()) data["cancelledInstructions"] = instructionsJson(task.cancelledInstructions);
    if (const auto& pending = task.pendingRunReport)
        data["pendingRunReport"] = {
            {"report", settledRunReportJson(pending->report)}, {"retryAt", pending->retryAt}, {"attempts", pending->attempts}};
    if (task.t3CheckAt) data["t3CheckAt"] = *task.t3CheckAt;
    if (task.newSessionRequired) data["newSessionRequired"] = true;
    if (task.freshContextRequested) data["freshContextRequested"] = true;
    if (task.launchWarning) data["launchWarning"] = *task.launchWarning;
    if (task.criteriaProposal) {
        data["requestedDoneWhen"] = task.criteriaProposal->doneWhen;
        if (task.criteriaProposal->sourceMessageId) data["criteriaSourceMessageId"] = *task.criteriaProposal->sourceMessageId;
    }
    if (task.requestedCommit) data["requestedCommit"] = *task.requestedCommit;
    if (task.feedback) data["feedback"] = *task.feedback;
    if (task.waitingNote) data["waitingNote"] = *task.waitingNote;
    encodeTaskInteraction(data, task);
    encodeTaskContext(data, task);
    return data;
}

Json encodeTask(const TaskData& task) {
    auto data = encodeTaskPayload(task);
    data.update({{"botId", task.botId},
                 {"environmentId", task.environmentId},
                 {"projectId", task.projectId},
                 {"request", task.request},
                 {"actionId", task.actionId},
                 {"activeRun", assignedRunJson(task.activeRun)},
                 {"lastRun", assignedRunJson(task.lastRun)}});
    if (task.currentRevision) data["currentRevision"] = *task.currentRevision;
    if (task.pendingDestination) data["pendingDestination"] = destinationJson(*task.pendingDestination);
    if (task.replanning) data["replanning"] = true;
    if (task.sourceMessageId) data["sourceMessageId"] = *task.sourceMessageId;
    if (task.trigger) data["trigger"] = taskTriggerJson(*task.trigger);
    if (const auto& selected = task.selectedSession) {
        auto session = sessionTargetJson(selected->target);
        session["projectId"] = selected->projectId;
        data["selectedSession"] = std::move(session);
    }
    if (task.sessionOrigin) data["sessionOrigin"] = sessionOriginJson(*task.sessionOrigin);
    if (const auto outcome = task.runOutcome()) data["runOutcome"] = runOutcomeName(*outcome);
    if (const auto* targets = std::get_if<RunTargets>(&task.destinations))
        data["runTargets"] = runTargetsJson(*targets);
    else if (const auto* results = std::get_if<RunResults>(&task.destinations)) {
        auto& runs = data["runResults"] = Json::array();
        for (const auto& run : results->runs) runs.push_back(destinationRunJson(run));
    }
    return data;
}

namespace {
Json taskWaitJson(const TaskData& task, const std::optional<Wait>& wait) {
    if (!wait) return nullptr;
    auto result = to_json(*wait);
    result["id"] = wait->id;
    if (wait->messageId) result["messageId"] = *wait->messageId;
    if (wait->reason() == WaitReason::user_question && !task.question) result["target"] = questionTargetJson(waitQuestionTarget(*wait));
    return result;
}

Json blockerJson(const std::optional<TaskBlocker>& blocker) {
    if (!blocker) return nullptr;
    return {{"id", blocker->id}, {"botId", blocker->botId}, {"request", blocker->request}, {"state", taskStateName(blocker->state)}};
}

Json pendingActionJson(const std::optional<OutboxAction>& action) {
    if (!action) return nullptr;
    auto data = outboxActionJson(*action);
    data["id"] = action->command.actionId;
    data["phase"] = outboxPhaseName(action->phase);
    return data;
}

Json taskRunJson(const TaskRunView& view) {
    auto data = assignedRunJson(view.run);
    data["actionId"] = view.actionId;
    data["revisionId"] = view.run.assignment.revisionId.value_or(0);
    if (view.status) data["status"] = *view.status;
    if (view.sessionCreatedAt) data["sessionCreatedAt"] = *view.sessionCreatedAt;
    return data;
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
    data["pendingAction"] = pendingActionJson(view.pendingAction);
    auto& runs = data["runs"] = Json::array();
    for (const auto& run : view.runs) runs.push_back(taskRunJson(run));
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

namespace {
constexpr std::string_view taskColumns =
    "bot_id,environment_id,project_id,state,generation,request,source_message_id,current_revision,replanning,"
    "pending_environment_id,pending_project_id,pending_gateway_environment_id,pending_pinned,"
    "active_run,active_thread_id,active_run_id,last_run,action_id,trigger_data,"
    "origin_environment_id,origin_thread_id,origin_gateway_environment_id,origin_title,"
    "selected_environment_id,selected_thread_id,selected_gateway_environment_id,selected_project_id,run_outcome,external_check_at,data";
constexpr std::string_view taskPlaceholders = "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

Json optionalId(std::optional<Id> value) {
    return value ? Json(*value) : Json(nullptr);
}

Json taskBindings(const TaskData& task, Id generation) {
    const auto& pending = task.pendingDestination;
    const auto& active = task.activeRun;
    const auto& origin = task.sessionOrigin;
    const auto& selected = task.selectedSession;
    const auto outcome = task.runOutcome();
    auto activeData = assignedRunJson(active);
    if (active) {
        activeData.erase("threadId");
        activeData.erase("runId");
    }
    return Json::array({task.botId,
                        task.environmentId,
                        task.projectId,
                        taskStateName(task.state),
                        generation,
                        task.request,
                        optionalId(task.sourceMessageId),
                        optionalId(task.currentRevision),
                        static_cast<int>(task.replanning),
                        pending ? Json(pending->route.environmentId) : Json(nullptr),
                        pending ? Json(pending->route.projectId) : Json(nullptr),
                        pending ? Json(pending->route.gatewayEnvironmentId) : Json(nullptr),
                        pending ? Json(static_cast<int>(pending->pinned)) : Json(nullptr),
                        std::move(activeData),
                        active ? Json(active->ref.threadId) : Json(nullptr),
                        active ? Json(active->ref.runId) : Json(nullptr),
                        assignedRunJson(task.lastRun),
                        task.actionId,
                        task.trigger ? taskTriggerJson(*task.trigger) : Json(nullptr),
                        origin ? Json(origin->target.environmentId) : Json(nullptr),
                        origin ? Json(origin->target.threadId) : Json(nullptr),
                        origin ? Json(origin->target.gatewayEnvironmentId) : Json(nullptr),
                        origin ? Json(origin->title) : Json(nullptr),
                        selected ? Json(selected->target.environmentId) : Json(nullptr),
                        selected ? Json(selected->target.threadId) : Json(nullptr),
                        selected ? Json(selected->target.gatewayEnvironmentId) : Json(nullptr),
                        selected ? Json(selected->projectId) : Json(nullptr),
                        outcome ? Json(runOutcomeName(*outcome)) : Json(nullptr),
                        optionalId(task.externalCheckAt),
                        encodeTaskPayload(task)});
}

Json destinationBindings(Id task, Id position, const RunTarget& target) {
    Json data = Json::object();
    if (!target.route.gatewayEnvironmentId.empty() && target.route.gatewayEnvironmentId != target.route.environmentId)
        data["gatewayEnvironmentId"] = target.route.gatewayEnvironmentId;
    return Json::array(
        {task, position, target.name, target.route.environmentId, target.route.projectId, nullptr, "queued", std::move(data)});
}

Json destinationBindings(Id task, Id position, const DestinationRun& run) {
    auto data = destinationRunJson(run);
    for (const auto* key : {"name", "environmentId", "projectId", "actionId", "status"}) data.erase(key);
    return Json::array({task, position, run.name, run.environmentId, run.assignment.projectId, run.actionId,
                        destinationRunStatusName(run.status), std::move(data)});
}
}

const std::string& taskProjection() {
    static const auto columns = [] {
        std::string sql = "t.id,t.";
        for (char character : taskColumns) {
            sql += character;
            if (character == ',') sql += "t.";
        }
        return sql;
    }();
    return columns;
}

TaskData TaskRecords::readTask(const Statement& row, int col) const {
    TaskData task;
    task.id = row.number(col++);
    task.botId = row.string(col++);
    task.environmentId = row.string(col++);
    task.projectId = row.string(col++);
    task.state = decodeTaskState(task.id, row.string(col++));
    task.generation = row.number(col++);
    task.request = row.string(col++);
    if (!row.null(col)) task.sourceMessageId = row.number(col);
    ++col;
    if (!row.null(col)) task.currentRevision = row.number(col);
    ++col;
    task.replanning = row.number(col++) != 0;
    if (!row.null(col))
        task.pendingDestination = PendingDestination{{row.string(col), row.string(col + 1), row.string(col + 2)}, row.number(col + 3) != 0};
    col += 4;
    const auto rowId = std::to_string(task.id);
    const RecordDecoder decoder("tasks", rowId);
    if (!row.null(col)) {
        const auto active = decoder.nested("activeRun");
        auto value = active.parse(row.string(col));
        active.object(value);
        value["threadId"] = row.string(col + 1);
        value["runId"] = row.string(col + 2);
        task.activeRun = active.decodeClosed(value, decodeAssignedRun);
    }
    col += 3;
    if (!row.null(col)) {
        const auto last = decoder.nested("lastRun");
        task.lastRun = last.decodeClosed(last.parse(row.string(col)), decodeAssignedRun);
    }
    ++col;
    task.actionId = row.string(col++);
    if (!row.null(col)) {
        const auto trigger = decoder.nested("trigger");
        task.trigger = trigger.decodeClosed(trigger.parse(row.string(col)), decodeTaskTrigger);
    }
    ++col;
    if (!row.null(col))
        task.sessionOrigin = SessionOrigin{{row.string(col), row.string(col + 1), row.string(col + 2)}, row.string(col + 3)};
    col += 4;
    if (!row.null(col))
        task.selectedSession = WorkSession{{row.string(col), row.string(col + 1), row.string(col + 2)}, row.string(col + 3)};
    col += 4;
    const auto outcome = row.null(col) ? std::nullopt : std::optional{row.string(col)};
    ++col;
    if (!row.null(col)) task.externalCheckAt = row.number(col);
    ++col;

    RunTargets targets;
    RunResults results;
    auto routes = database.query(R"SQL(
SELECT name,environment_id,project_id,action_id,status,data
FROM task_run_destinations WHERE task_id=? ORDER BY position
)SQL",
                                 {task.id});
    while (routes.next()) {
        const auto result = !routes.null(3);
        const auto key = (result ? "runResults." : "runTargets.") + std::to_string(results.runs.size() + targets.size());
        const auto fields = decoder.nested(key);
        auto route = fields.parse(routes.string(5));
        fields.object(route);
        route["name"] = routes.string(0);
        route["environmentId"] = routes.string(1);
        route["projectId"] = routes.string(2);
        if (result) {
            route["actionId"] = routes.string(3);
            route["status"] = routes.string(4);
            results.runs.push_back(fields.decodeClosed(route, decodeDestinationRun));
        } else {
            targets.push_back(fields.decodeClosed(route, decodeRunTarget));
        }
    }
    if (!targets.empty() && !results.runs.empty()) decoder.fail("runTargets", "cannot coexist with dispatched Runs");
    if (outcome) {
        if (results.runs.empty()) decoder.fail("runOutcome", "requires dispatched Runs");
        results.outcome = decodeRunOutcome(*outcome, decoder);
    }
    if (!targets.empty()) task.destinations = std::move(targets);
    if (!results.runs.empty()) task.destinations = std::move(results);
    return decodeTaskPayload(std::move(task), row.string(col));
}

TaskData TaskRecords::load(Id id) const {
    static const auto sql = "SELECT id," + std::string(taskColumns) + " FROM tasks WHERE id=?";
    auto row = database.query(sql.c_str(), {id});
    if (!row.next()) throw Conflict("Task does not exist");
    return readTask(row);
}

std::optional<std::string> TaskRecords::externalWaitingNote(const TaskData& task) const {
    if (task.state == TaskState::external_waiting) return task.waitingNote;
    if (task.state != TaskState::queued && task.state != TaskState::deciding) return std::nullopt;
    auto event = database.query("SELECT json_extract(data,'$.waitingNote') FROM events "
                                "WHERE id=? AND task_id=? AND consumed=0 AND json_extract(data,'$.kind')='external_check'",
                                {task.eventId, task.id});
    if (event.next()) return event.string(0);
    return std::nullopt;
}

Id TaskRecords::insertTask(const TaskData& task) {
    static const auto sql = "INSERT INTO tasks(" + std::string(taskColumns) + ") VALUES" + std::string(taskPlaceholders);
    database.exec(sql.c_str(), taskBindings(task, task.generation));
    return database.last();
}

void TaskRecords::save(TaskData& task) {
    if (task.state != TaskState::external_waiting) {
        task.waitingNote.reset();
        task.externalCheckAt.reset();
    }
    const auto generation = task.generation + 1;
    auto values = taskBindings(task, generation);
    values.push_back(task.id);
    static const auto update = "UPDATE tasks SET (" + std::string(taskColumns) + ")=" + std::string(taskPlaceholders) + " WHERE id=?";
    constexpr auto insertRoute = R"SQL(
INSERT INTO task_run_destinations(task_id,position,name,environment_id,project_id,action_id,status,data) VALUES(?,?,?,?,?,?,?,?)
)SQL";
    database.exec("SAVEPOINT save_task");
    try {
        database.exec(update.c_str(), values);
        database.exec("DELETE FROM task_run_destinations WHERE task_id=?", {task.id});
        Id position = 0;
        for (const auto& target : task.runTargets()) database.exec(insertRoute, destinationBindings(task.id, position++, target));
        for (const auto& run : task.runResults()) database.exec(insertRoute, destinationBindings(task.id, position++, run));
        database.exec("RELEASE save_task");
    } catch (...) {
        sqlite3_exec(database.db, "ROLLBACK TO save_task; RELEASE save_task", nullptr, nullptr, nullptr);
        throw;
    }
    task.generation = generation;
}

Id TaskRecords::complete(TaskData& task, const CompletionEntry& entry) {
    task.pendingInstructions.clear();
    task.pendingDecision.reset();
    task.newSessionRequired = false;
    task.state = TaskState::completed;
    const auto id = journalStorage.journal(task.id, entry);
    save(task);
    return id;
}

void requireInstructionAcknowledgments(const TaskData& task, const Json& args) {
    auto ids = Json::array();
    for (const auto& item : task.pendingInstructions) ids.push_back({{"messageId", item.messageId}, {"step", item.step}});
    if ((!ids.empty()) != args.contains("acknowledgedInstructions") || args.value("acknowledgedInstructions", Json::array()) != ids)
        throw Conflict("Acknowledge each pending instruction by messageId and step, oldest first");
}

void waitForExternalEvent(TaskData& task, const Json& args, Seconds now) {
    auto note = field(args, "note");
    if (note.size() > 240 || !conversation_storage::hasText(note) || note.find_first_of("\r\n") != std::string::npos)
        throw Conflict("Provide a one-line waiting note of up to 240 bytes");
    const auto delay = args.find("checkAgainAfterMinutes");
    if (delay == args.end() || !delay->is_number_integer() || *delay < minExternalCheckMinutes || *delay > maxExternalCheckMinutes)
        throw Conflict("Choose a re-check time between 5 and 1440 minutes");
    requireInstructionAcknowledgments(task, args);
    task.pendingInstructions.clear();
    task.pendingDecision.reset();
    task.state = TaskState::external_waiting;
    task.waitingNote = std::move(note);
    task.externalCheckAt = now + 60 * delay->get<int>();
}

namespace {
const std::string queueBlockers = R"SQL(
blockers AS (
  SELECT requested.id,owner.id AS owner_id,owner.bot_id,owner.request,owner.state
  FROM requested
  CROSS JOIN task_project_slots slot ON slot.id=requested.id
  JOIN tasks owner ON owner.id=slot.blocker_id
)
)SQL";

std::string taskPageSql(const char* filter) {
    return std::string("WITH page AS (SELECT id,state FROM tasks WHERE ") + filter +
           "), requested AS (SELECT id FROM page WHERE state='queued'), " + queueBlockers + R"SQL(
SELECT blockers.owner_id,blockers.bot_id,blockers.request,blockers.state,
       w.id,w.reason,w.data,)SQL" +
           taskProjection() + R"SQL(
FROM page JOIN tasks t ON t.id=page.id
LEFT JOIN blockers ON blockers.id=page.id
LEFT JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL
ORDER BY t.id DESC)SQL";
}

std::optional<TaskBlocker> taskBlocker(const Statement& row) {
    const auto id = row.number(0);
    if (!id) return std::nullopt;
    return TaskBlocker{id, row.string(1), row.string(2), decodeTaskState(id, row.string(3))};
}
}

std::optional<TaskBlocker> TaskRecords::queueBlocker(Id id) const {
    static const auto sql = "WITH requested(id) AS (VALUES(?)), " + queueBlockers + "SELECT owner_id,bot_id,request,state FROM blockers";
    auto row = database.query(sql.c_str(), {id});
    return row.next() ? taskBlocker(row) : std::nullopt;
}

TaskView TaskRecords::taskDetails(Id id, bool includeBlocker) const {
    static const auto sql = "SELECT w.id,w.reason,w.data," + taskProjection() +
                            " FROM tasks t LEFT JOIN waits w ON w.task_id=t.id AND w.resolution IS NULL WHERE t.id=?";
    auto row = database.query(sql.c_str(), {id});
    if (!row.next()) throw Conflict("Task does not exist");
    TaskView view{.task = readTask(row, 3), .wait = waitStorage.projectedWait(id, row, 0)};
    if (includeBlocker && view.task.state == TaskState::queued) view.blockedBy = queueBlocker(id);
    addTaskDetails(view);
    return view;
}

void TaskRecords::addTaskDetails(TaskView& view) const {
    const auto& task = view.task;
    if (auto pending = outboxStorage.outbox(task)) view.pendingAction.emplace(std::move(*pending));
    auto revisions = database.query("SELECT id,number,intent,done_when,source_message_id FROM task_revisions"
                                    " WHERE task_id=? ORDER BY number",
                                    {task.id});
    while (revisions.next()) {
        const auto source = revisions.number(4);
        view.revisions.push_back({revisions.number(0), revisions.number(1), revisions.string(2), revisions.string(3),
                                  source ? std::optional{source} : std::nullopt});
    }
    auto runs = database.query("SELECT runs.thread_id,runs.run_id,runs.link,runs.action_id,runs.environment_id,"
                               "outbox.revision_id,outbox.project_id FROM runs"
                               " JOIN outbox ON runs.action_id=outbox.id WHERE runs.task_id=? ORDER BY outbox.rowid",
                               {task.id});
    while (runs.next()) {
        const auto revision = runs.number(5);
        view.runs.push_back({.run = {{runs.string(0), runs.string(1), runs.string(2), {}},
                                     runs.string(4),
                                     {task.botId, runs.string(6), revision ? std::optional{revision} : std::nullopt}},
                             .actionId = runs.string(3)});
    }
}

std::vector<TaskView> TaskList::tasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(database_.mutex);
    static const auto sql = taskPageSql("(?='' OR bot_id=?) AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?");
    auto rows = database_.query(sql.c_str(), {bot, bot, before, before, std::clamp(limit, 1u, 200u)});
    std::vector<TaskView> result;
    while (rows.next()) {
        auto& view = result.emplace_back(records_.readTask(rows, 7), waits_.projectedWait(rows.number(7), rows, 4), taskBlocker(rows));
        records_.addTaskDetails(view);
    }
    return result;
}

std::vector<TaskList::ActiveTask> TaskList::activeTasks(std::string_view bot) const {
    std::lock_guard lock(database_.mutex);
    static const auto sql = taskPageSql("bot_id=? AND state NOT IN ('completed','cancelled')");
    auto rows = database_.query(sql.c_str(), {bot});
    std::vector<ActiveTask> result;
    while (rows.next()) result.push_back({records_.readTask(rows, 7), waits_.projectedWait(rows.number(7), rows, 4), taskBlocker(rows)});
    return result;
}

std::map<std::string, std::vector<TaskPresence>> TaskList::activeTaskStatesByBot() const {
    std::lock_guard lock(database_.mutex);
    auto rows = database_.query(R"SQL(
SELECT bot_id,state,replanning,min(id)
FROM tasks WHERE state NOT IN ('completed','cancelled')
GROUP BY bot_id,state,replanning)SQL");
    std::map<std::string, std::vector<TaskPresence>> result;
    while (rows.next()) result[rows.string(0)].push_back({decodeTaskState(rows.number(3), rows.string(1)), rows.number(2) != 0});
    return result;
}

TaskView TaskRecords::sessionTask(Id id) const {
    auto view = taskDetails(id, false);
    auto reports = database.query(R"SQL(
SELECT run_environment_id,run_thread_id,run_id,run_status,
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
    for (auto& runView : view.runs) {
        const auto& run = runView.run;
        const SessionKey session{run.environmentId, run.ref.threadId};
        const auto status = statuses.find({session.first, session.second, run.ref.runId});
        if (status != statuses.end()) runView.status = status->second;
        const auto timestamp = createdAt.find(session);
        if (timestamp != createdAt.end()) runView.sessionCreatedAt = timestamp->second;
    }
    return view;
}

std::vector<TaskView> TaskHistory::finishedTasks(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(database_.mutex);
    auto rows = database_.query("SELECT id FROM tasks WHERE bot_id=? AND (?=0 OR id<?)"
                                " AND state IN ('completed','cancelled') ORDER BY id DESC LIMIT ?",
                                {bot, before, before, std::clamp(limit, 1u, 200u)});
    std::vector<TaskView> result;
    while (rows.next()) result.push_back(records_.sessionTask(rows.number(0)));
    return result;
}

std::pair<Id, std::vector<TaskView>> TaskHistory::finishedTaskChangesSince(std::string_view bot, std::optional<Id> after) const {
    std::lock_guard lock(database_.mutex);
    auto newest = database_.query("SELECT coalesce(max(id),0) FROM journal WHERE kind IN ('completed','cancelled','peer_completed')");
    newest.next();
    const auto through = newest.number(0);
    std::vector<TaskView> tasks;
    if (after && *after < through) {
        auto rows = database_.query(R"SQL(
SELECT DISTINCT j.task_id FROM journal j CROSS JOIN tasks t ON t.id=j.task_id
WHERE j.kind IN ('completed','cancelled','peer_completed') AND j.id>? AND j.id<=? AND t.bot_id=?
)SQL",
                                    {*after, through, bot});
        while (rows.next()) tasks.push_back(records_.sessionTask(rows.number(0)));
    }
    return {through, std::move(tasks)};
}

Id TaskEvents::event(TaskData& task, std::string_view key, Json data) {
    database.exec("INSERT OR IGNORE INTO events(task_id,dedupe,data) VALUES(?,?,?)", {task.id, key, data});
    if (!sqlite3_changes(database.db)) return 0;
    const auto id = database.last();
    task.eventId = id;
    return id;
}

Json TaskEvents::currentEvent(const TaskData& task) const {
    const auto id = task.eventId;
    auto row = database.query("SELECT data,failures,retry_at,evidence,consumed FROM events WHERE id=? AND task_id=?", {id, task.id});
    if (!row.next()) return nullptr;
    auto result = row.json(0);
    result["id"] = id;
    result["failures"] = row.number(1);
    result["retryAt"] = row.number(2);
    result["evidence"] = row.json(3);
    result["consumed"] = row.number(4) != 0;
    return result;
}

void TaskEvents::consumeEvent(const TaskData& task) {
    database.exec("UPDATE events SET consumed=1 WHERE id=? AND task_id=? AND consumed=0", {task.eventId, task.id});
}

void TaskEvents::wakeExternalChecks(TaskRecords& records, Seconds now) {
    std::lock_guard lock(database.mutex);
    auto rows = database.query(R"SQL(
SELECT task.id FROM tasks task
WHERE task.external_check_at<=?1
  AND NOT EXISTS (
    SELECT 1 FROM messages m LEFT JOIN conversation_retries retry ON retry.message_id=m.id
    WHERE m.id=(
      SELECT min(turn.message_id) FROM conversation_turns turn JOIN messages head ON head.id=turn.message_id
      WHERE turn.status='running' AND head.bot_id=task.bot_id
    )
      AND json_extract(m.routing,'$.gitEvent.taskId')=task.id AND coalesce(retry.retry_at,0)<=?1
  )
  AND NOT EXISTS (
    SELECT 1 FROM policy_approvals a JOIN conversation_turns turn ON turn.message_id=a.turn
    WHERE a.bot=task.bot_id AND a.task=task.id AND a.generation=task.generation AND a.status IN ('pending','approved')
      AND turn.status IN ('policy_waiting','running')
      AND (a.status='approved' OR a.expires_at IS NULL OR a.expires_at>?1)
  )
ORDER BY task.external_check_at,task.id LIMIT 20
)SQL",
                               {now});
    std::vector<Id> due;
    while (rows.next()) due.push_back(rows.number(0));
    if (due.empty()) return;
    Transaction transaction(database.db);
    for (const auto id : due) {
        auto task = records.load(id);
        if (!task.externalCheckAt || *task.externalCheckAt > now) continue;
        event(task, "external-check/" + std::to_string(task.generation),
              {{"kind", "external_check"}, {"waitingNote", task.waitingNote.value()}});
        task.state = TaskState::queued;
        records.save(task);
    }
    transaction.commit();
}

Id TaskRecords::addRevision(TaskData& task, std::string_view intent, std::string_view doneWhen, Id source) {
    if (!conversation_storage::hasText(intent) || !conversation_storage::hasText(doneWhen))
        throw Conflict("Task revision needs intent and completion criteria");
    if (source) {
        auto row = database.query("SELECT 1 FROM messages WHERE id=? AND bot_id=?", {source, task.botId});
        if (!row.next()) throw Conflict("Task revision must come from this Bot's conversation");
    }
    database.exec(R"SQL(
INSERT INTO task_revisions(task_id,number,intent,done_when,source_message_id)
SELECT ?,coalesce(max(number),0)+1,?,?,? FROM task_revisions WHERE task_id=?
)SQL",
                  {task.id, intent, doneWhen, source ? Json(source) : Json(nullptr), task.id});
    const auto revision = database.last();
    task.currentRevision = revision;
    task.blocker.reset();
    task.intent = intent;
    task.doneWhen = doneWhen;
    journalStorage.journal(task.id, TaskRevisionEntry{revision, source});
    return revision;
}

}
