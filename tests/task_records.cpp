#include "../src/task_storage.hpp"

#include "flock/record_decode_error.hpp"

#include <array>
#include <iostream>
#include <limits>

using namespace flockv0;
using namespace std::literals;

namespace {
constexpr Id id = 47, generation = 12;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Json stored() {
    return {{"botId", "bot"},
            {"request", "Original request"},
            {"intent", "Revised request"},
            {"environmentId", "net"},
            {"projectId", "project"},
            {"doneWhen", "Provide results"},
            {"config", {{"model", "example"}, {"custom", Json::array({"one", "two"})}}},
            {"actionId", ""},
            {"eventId", 0},
            {"deadline", 0},
            {"activeRun", nullptr},
            {"lastRun", nullptr},
            {"feedback", "Run is ready"},
            {"question", nullptr}};
}

Json pendingDestination() {
    return {{"environmentId", "remote"}, {"projectId", "other"}, {"gatewayEnvironmentId", "other-gateway"}, {"pinned", true}};
}

void roundTrips() {
    using enum TaskState;
    constexpr std::array states{
        std::pair{queued, "queued"sv},         std::pair{deciding, "deciding"sv},   std::pair{sending, "sending"sv},
        std::pair{running, "running"sv},       std::pair{waiting, "waiting"sv},     std::pair{peer_waiting, "peer_waiting"sv},
        std::pair{cancelling, "cancelling"sv}, std::pair{completed, "completed"sv}, std::pair{cancelled, "cancelled"sv},
    };
    const auto base = stored();
    const auto intake = decodeTask(id, "queued", generation, base.dump());
    check(!intake.gatewayEnvironmentId && !intake.currentRevision && !intake.pendingDestination && encodeTask(intake) == base,
          "Task intake encoded optional fields that were absent");

    auto revised = base;
    revised.update({{"gatewayEnvironmentId", "gateway"}, {"currentRevision", 23}, {"pendingDestination", pendingDestination()}});
    for (const auto& [state, name] : states) {
        const auto task = decodeTask(id, name, generation, revised.dump());
        check(task.id == id && task.generation == generation && task.state == state && taskStateName(task.state) == name,
              "Task columns did not round-trip");
        check(task.botId == "bot" && task.request == "Original request" && task.intent == "Revised request" &&
                  task.environmentId == "net" && task.projectId == "project" && task.doneWhen == "Provide results" &&
                  task.config == base.at("config") && task.gatewayEnvironmentId == "gateway" && task.currentRevision == 23,
              "Task routing and revision fields did not decode");
        check(task.pendingDestination && task.pendingDestination->pinned && task.pendingDestination->route.environmentId == "remote" &&
                  task.pendingDestination->route.projectId == "other" &&
                  task.pendingDestination->route.gatewayEnvironmentId == "other-gateway",
              "Pending destination did not decode");
        check(task.rest == Json{{"feedback", "Run is ready"}, {"question", nullptr}}, "Typed fields leaked into TaskData::rest");
        check(encodeTask(task) == revised, "Task payload changed on round-trip");
    }

    auto storedWithColumns = revised;
    storedWithColumns.update({{"id", 10}, {"generation", 9}, {"state", "completed"}, {"revisions", Json::array()}});
    const auto task = decodeTask(id, "queued", generation, storedWithColumns.dump());
    check(task.id == id && task.generation == generation && task.state == queued && encodeTask(task) == revised,
          "Task data overrode a persisted column or retained a projection");

    const TaskView view{.task = task, .revisions = {{23, 2, "Revised request", "Provide results", 33}}};
    const auto projected = taskJson(view);
    check(
        projected.at("id") == id && projected.at("generation") == generation && projected.at("state") == "queued" &&
            projected.at("revisions") ==
                Json::array(
                    {{{"id", 23}, {"number", 2}, {"intent", "Revised request"}, {"doneWhen", "Provide results"}, {"sourceMessageId", 33}}}),
        "Task view changed the wire shape for columns or revisions");
}

Json storedExecution() {
    const Json run{{"threadId", "session"},  {"runId", "assigned"}, {"link", "t3://session"}, {"gatewayEnvironmentId", "gateway"},
                   {"environmentId", "net"}, {"ownerBotId", "bot"}, {"projectId", "project"}, {"revisionId", nullptr}};
    const Json report{{"kind", "result"},
                      {"run", run},
                      {"environmentId", "net"},
                      {"revisionId", nullptr},
                      {"resultRunId", "continuation"},
                      {"status", "completed"},
                      {"report", ""},
                      {"itemId", ""},
                      {"detail", "Unavailable"},
                      {"truncated", false},
                      {"sessionCreatedAt", "2026-11-25T00:00:00Z"},
                      {"evidenceStatus", "report_unavailable"}};
    auto data = stored();
    data.update(
        {{"actionId", "action"},
         {"eventId", 51},
         {"deadline", 100},
         {"lastRun", run},
         {"runStop", {{"intent", "cancel"}, {"run", nullptr}, {"reconcileAt", 80}}},
         {"replanning", true},
         {"queuedInT3", true},
         {"cancelledByBotPause", true},
         {"replanningDetail", "Waiting for revised work"},
         {"cancellationDetail", "Waiting for delivery"},
         {"cancelledInstructions",
          Json::array({{{"messageId", 60}, {"step", 1}, {"text", "Check output"}, {"revisionId", nullptr}, {"conflicting", false}}})},
         {"pendingRunReport", {{"report", report}, {"retryAt", 90}, {"attempts", 1}}},
         {"pendingStageReport", 71},
         {"sessionReconcileAt", 95},
         {"workflowReconcileAt", 96},
         {"selectedSession",
          {{"environmentId", "net"}, {"threadId", "session"}, {"projectId", "project"}, {"gatewayEnvironmentId", "gateway"}}},
         {"sessionOrigin", {{"environmentId", "parent"}, {"threadId", "parent-session"}, {"title", "Planning"}}},
         {"newSessionRequired", true},
         {"freshContextRequested", true},
         {"launchWarning", "Session lineage unavailable"}});
    return data;
}

void executionRoundTrips() {
    auto data = storedExecution();
    const auto task = decodeTask(id, "cancelling", generation, data.dump());
    check(task.actionId == "action" && task.eventId == 51 && task.deadline == 100 && !task.activeRun, "Execution anchors did not decode");
    const auto& run = task.lastRun.value();
    check(run.ref.runId == "assigned" && run.ref.gatewayEnvironmentId == "gateway" && run.environmentId == "net" &&
              run.assignment.ownerBotId == "bot" && !run.assignment.revisionId,
          "Settled Run assignment did not decode");
    const auto& stop = task.runStop.value();
    check(stop.intent == RunStopIntent::cancel && !stop.run && !stop.retryAt && stop.reconcileAt == 80,
          "Stop before Run recovery lost its delivery state");
    const auto& pending = task.pendingRunReport.value();
    check(pending.report.run == run && pending.report.resultRunId == "continuation" &&
              pending.report.evidence.availability == RunEvidence::Availability::ReportUnavailable && pending.attempts == 1 &&
              pending.retryAt == 90 && task.pendingStageReport == 71 && task.sessionReconcileAt == 95 && task.workflowReconcileAt == 96,
          "Report or delivery recovery did not decode");
    check(task.cancelledInstructions.size() == 1 && task.cancelledInstructions.front().messageId == 60 && task.replanning &&
              task.queuedInT3 && task.cancelledByBotPause && task.cancellationDetail == "Waiting for delivery" &&
              task.replanningDetail == "Waiting for revised work",
          "Stopping and cancellation details did not decode");
    check(task.selectedSession && task.selectedSession->target == SessionTarget{"net", "session", "gateway"} &&
              task.selectedSession->projectId == "project" && task.sessionOrigin &&
              task.sessionOrigin->target == SessionTarget{"parent", "parent-session", ""} && task.sessionOrigin->title == "Planning" &&
              task.newSessionRequired && task.freshContextRequested && task.launchWarning == "Session lineage unavailable",
          "Selected Session or Session origin did not decode");
    check(task.rest == Json{{"feedback", "Run is ready"}, {"question", nullptr}} && encodeTask(task) == data,
          "Typed execution data changed on round-trip");
    data["activeRun"] = data.at("lastRun");
    data["runStop"] = {{"intent", "replan"}, {"run", data.at("activeRun")}, {"retryAt", 94}};
    const auto stopping = decodeTask(id, "running", generation, data.dump());
    check(stopping.activeRun && stopping.runStop->run == stopping.activeRun && stopping.runStop->intent == RunStopIntent::replan &&
              encodeTask(stopping) == data,
          "Stop bound to an assigned Run changed on round-trip");
}

void rejects(std::string_view record, std::string_view key, std::string_view issue, std::string_view state = "queued") {
    try {
        decodeTask(id, state, generation, record);
    } catch (const RecordDecodeError& error) {
        const auto expected = "tasks row 47, key " + std::string(key) + ": ";
        const std::string_view message(error.what());
        check(message.starts_with(expected) && message.ends_with(issue), message);
        return;
    }
    throw std::runtime_error("Task decoder accepted invalid " + std::string(key));
}

void decodeErrors() {
    rejects("{", "data", "invalid JSON");
    rejects("[]", "data", "expected an object");
    rejects(stored().dump(), "state", "unknown task state", "invalid");
    for (const auto* key : {"botId", "request", "intent", "environmentId", "projectId", "doneWhen"}) {
        auto invalid = stored();
        invalid.erase(key);
        rejects(invalid.dump(), key, "missing required key");
        invalid[key] = 42;
        rejects(invalid.dump(), key, "expected a string");
    }
    auto invalid = stored();
    invalid["config"] = Json::array();
    rejects(invalid.dump(), "config", "expected an object");
    invalid = stored();
    invalid["gatewayEnvironmentId"] = false;
    rejects(invalid.dump(), "gatewayEnvironmentId", "expected a string");
    invalid = stored();
    invalid["currentRevision"] = 2.5;
    rejects(invalid.dump(), "currentRevision", "expected an integer");
    invalid["currentRevision"] = std::numeric_limits<Json::number_unsigned_t>::max();
    rejects(invalid.dump(), "currentRevision", "integer out of range");
    invalid = stored();
    invalid["pendingDestination"] = nullptr;
    rejects(invalid.dump(), "pendingDestination", "expected an object");
    invalid["pendingDestination"] = pendingDestination();
    invalid["pendingDestination"]["pinned"] = "true";
    rejects(invalid.dump(), "pendingDestination.pinned", "expected a boolean");
    invalid["pendingDestination"] = pendingDestination();
    invalid["pendingDestination"].erase("projectId");
    rejects(invalid.dump(), "pendingDestination.projectId", "missing required key");
}

void executionDecodeErrors() {
    const auto base = storedExecution();
    auto invalid = base;
    invalid["actionId"] = 42;
    rejects(invalid.dump(), "actionId", "expected a string");
    invalid = base;
    invalid["eventId"] = nullptr;
    rejects(invalid.dump(), "eventId", "expected an integer");
    invalid = base;
    invalid["deadline"] = "100";
    rejects(invalid.dump(), "deadline", "expected an integer");
    invalid = base;
    invalid["lastRun"].erase("revisionId");
    rejects(invalid.dump(), "lastRun.revisionId", "missing required key");
    invalid = base;
    invalid["runStop"]["intent"] = "pause";
    rejects(invalid.dump(), "runStop.intent", "unknown Run stop intent");
    invalid = base;
    invalid["pendingRunReport"]["report"]["resultRunId"] = 20;
    rejects(invalid.dump(), "pendingRunReport.report.resultRunId", "expected a string");
    invalid = base;
    invalid["pendingRunReport"]["report"]["revisionId"] = 20;
    rejects(invalid.dump(), "pendingRunReport.report.revisionId", "does not match the assigned Run");
    invalid = base;
    invalid["cancelledInstructions"][0]["conflicting"] = 20;
    rejects(invalid.dump(), "cancelledInstructions.0.conflicting", "expected a boolean");
    invalid = base;
    invalid["selectedSession"].erase("threadId");
    rejects(invalid.dump(), "selectedSession.threadId", "missing required key");
    invalid = base;
    invalid["sessionOrigin"]["title"] = 20;
    rejects(invalid.dump(), "sessionOrigin.title", "expected a string");
}
}

int main() {
    try {
        roundTrips();
        executionRoundTrips();
        decodeErrors();
        executionDecodeErrors();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
