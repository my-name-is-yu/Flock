#include "flock/intake.hpp"
#include "flock/task_intake.hpp"
#include "../src/task_storage.hpp"
#include "../src/journal.hpp"
#include "../src/journal_task.hpp"
#include "../src/outbox_storage.hpp"
#include "../src/database.hpp"

#include "flock/tasks.hpp"
#include "flock/record_decode_error.hpp"
#include "flock/json.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <array>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unistd.h>

using namespace flockv0;
using namespace std::literals;

namespace {
constexpr Id id = 47, generation = 12;

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct SavedTask {
    struct Directory {
        std::filesystem::path path;
        Directory() {
            char pattern[] = "/tmp/flock-task-record-XXXXXX";
            if (const auto* created = mkdtemp(pattern))
                path = created;
            else
                throw std::runtime_error("Cannot create task test directory");
        }
        ~Directory() { std::filesystem::remove_all(path); }
    } directory;
    Store store{directory.path / "state.db"};
    Id task = TaskIntake(store).submit({.botId = "bot", .request = "Original request", .environmentId = "net", .projectId = "project"});

    void exec(const char* sql, Json args = Json::array()) {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open(store.path().c_str(), &raw);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        check(opened == SQLITE_OK, "Could not open task fixture database");
        Statement(db.get(), sql, args).next();
    }

    TaskData load() { return Tasks(store).task(task).task; }
};

Json stored() {
    return {{"intent", "Revised request"},
            {"doneWhen", "Provide results"},
            {"config", {{"model", "example"}, {"custom", Json::array({"one", "two"})}}},
            {"eventId", 0},
            {"createdAt", 1700000000},
            {"feedback", "Run is ready"},
            {"question", nullptr}};
}

void roundTrips() {
    using enum TaskState;
    constexpr std::array states{
        std::pair{queued, "queued"sv},         std::pair{deciding, "deciding"sv},   std::pair{sending, "sending"sv},
        std::pair{running, "running"sv},       std::pair{waiting, "waiting"sv},     std::pair{peer_waiting, "peer_waiting"sv},
        std::pair{cancelling, "cancelling"sv}, std::pair{completed, "completed"sv}, std::pair{cancelled, "cancelled"sv},
    };
    SavedTask saved;
    const auto intake = saved.load();
    check(intake.botId == "bot" && intake.request == "Original request" && intake.environmentId == "net" && intake.projectId == "project" &&
              !intake.currentRevision && !intake.pendingDestination,
          "Task intake columns did not decode");

    auto revised = stored();
    revised["gatewayEnvironmentId"] = "gateway";
    for (const auto& [state, name] : states) {
        saved.exec(R"SQL(
UPDATE tasks SET generation=?,state=?,data=?,current_revision=23,pending_environment_id='remote',
    pending_project_id='other',pending_gateway_environment_id='other-gateway',pending_pinned=1 WHERE id=?
)SQL",
                   {generation, name, revised, saved.task});
        const auto task = saved.load();
        check(task.id == saved.task && task.generation == generation && task.state == state && taskStateName(task.state) == name,
              "Task identity columns did not round-trip");
        check(task.intent == "Revised request" && task.doneWhen == "Provide results" && task.gatewayEnvironmentId == "gateway" &&
                  task.currentRevision == 23 && task.pendingDestination && task.pendingDestination->pinned &&
                  task.pendingDestination->route.environmentId == "remote" && task.pendingDestination->route.projectId == "other" &&
                  task.pendingDestination->route.gatewayEnvironmentId == "other-gateway",
              "Task routing and revision did not decode");
        check(encodeTaskPayload(task) == revised, "Task payload changed on round-trip");
    }

    const auto projected = taskJson(TaskView{.task = intake, .revisions = {{23, 2, "Revised request", "Provide results", 33}}});
    check(
        projected.at("id") == saved.task && projected.at("generation") == 0 && projected.at("state") == "queued" &&
            projected.at("revisions") ==
                Json::array(
                    {{{"id", 23}, {"number", 2}, {"intent", "Revised request"}, {"doneWhen", "Provide results"}, {"sourceMessageId", 33}}}),
        "Task view changed the wire shape for columns or revisions");
}

Json assigned() {
    return {{"threadId", "session"},  {"runId", "assigned"}, {"link", "t3://session"}, {"gatewayEnvironmentId", "gateway"},
            {"environmentId", "net"}, {"ownerBotId", "bot"}, {"projectId", "project"}, {"revisionId", nullptr}};
}

Json storedExecution() {
    const auto run = assigned();
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
        {{"eventId", 51},
         {"runStop", {{"intent", "cancel"}, {"run", nullptr}, {"reconcileAt", 80}}},
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
         {"newSessionRequired", true},
         {"freshContextRequested", true},
         {"launchWarning", "Session lineage unavailable"}});
    return data;
}

void executionRoundTrips() {
    auto data = storedExecution();
    SavedTask saved;
    saved.exec(R"SQL(
UPDATE tasks SET state='cancelling',data=?,action_id='action',last_run=?,replanning=1,selected_environment_id='net',
    selected_thread_id='session',selected_gateway_environment_id='gateway',selected_project_id='project',
    origin_environment_id='parent',origin_thread_id='parent-session',origin_title='Planning' WHERE id=?
)SQL",
               {data, assigned(), saved.task});
    const auto task = saved.load();
    check(task.actionId == "action" && task.eventId == 51 && !task.activeRun, "Execution anchors did not decode");
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
    check(encodeTaskPayload(task) == data, "Reconciliation payload changed on round-trip");
    auto active = assigned();
    active.erase("threadId");
    active.erase("runId");
    data["runStop"] = {{"intent", "replan"}, {"run", assigned()}, {"retryAt", 94}};
    saved.exec(R"SQL(
UPDATE tasks SET state='running',data=?,active_run=?,active_thread_id='session',active_run_id='assigned' WHERE id=?
)SQL",
               {data, active, saved.task});
    const auto stopping = saved.load();
    check(stopping.activeRun && stopping.runStop->run == stopping.activeRun && stopping.runStop->intent == RunStopIntent::replan &&
              encodeTaskPayload(stopping) == data,
          "Stop bound to an assigned Run changed on round-trip");
}

void destinationRoundTrips() {
    SavedTask saved;
    saved.exec(R"SQL(
INSERT INTO task_run_destinations(task_id,position,name,environment_id,project_id,status,data)
VALUES(?,0,'Mini','mini','repo','queued','{}')
)SQL",
               {saved.task});
    const auto queued = saved.load();
    check(queued.runTargets().size() == 1 && queued.runTargets().front().route.projectId == "repo" && queued.runResults().empty(),
          "Queued destination did not retain its route");

    const Json progress{
        {"gatewayEnvironmentId", "local"}, {"threadId", "mini-thread"},        {"link", ""}, {"retryAt", 90}, {"stopRetryAt", 95},
        {"evidenceAttempts", 1},           {"detail", "Waiting for a receipt"}};
    const Json report{{"threadId", "local-thread"},
                      {"runId", "local-run"},
                      {"link", "t3://local"},
                      {"gatewayEnvironmentId", ""},
                      {"ownerBotId", "bot"},
                      {"revisionId", nullptr},
                      {"commit", ""},
                      {"report", ""},
                      {"evidenceStatus", "report_unavailable"}};
    saved.exec(R"SQL(
UPDATE task_run_destinations SET action_id='send-mini',status='confirmation_pending',data=? WHERE task_id=? AND position=0
)SQL",
               {progress, saved.task});
    saved.exec(R"SQL(
INSERT INTO task_run_destinations(task_id,position,name,environment_id,project_id,action_id,status,data)
VALUES(?,1,'Local','local','other','send-local','rolled_back',?)
)SQL",
               {saved.task, report});
    const auto running = saved.load();
    const auto& result = running.runResults().front();
    check(running.runTargets().empty() && running.runResults().size() == 2 && result.inPhase(DestinationRunPhase::confirmation_pending) &&
              result.ref.threadId == "mini-thread" && result.retryAt == 90 && result.stopRetryAt == 95 && result.evidenceAttempts == 1 &&
              !running.runResults().back().outstanding(),
          "Destination progress and order changed on round-trip");

    saved.exec("UPDATE tasks SET run_outcome='partial_failure' WHERE id=?", {saved.task});
    const auto wire = taskJson(Tasks(saved.store).task(saved.task));
    auto settled = report;
    settled.update(
        {{"name", "Local"}, {"environmentId", "local"}, {"projectId", "other"}, {"actionId", "send-local"}, {"status", "rolled_back"}});
    check(wire.at("runResults").at(1) == settled && wire.at("runOutcome") == "partial_failure",
          "Destination aggregate changed its public shape");
}

void rejects(std::string_view record, std::string_view key, std::string_view issue) {
    try {
        decodeTaskPayload(TaskData{.id = id}, record);
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
    for (const auto* key : {"intent", "doneWhen"}) {
        auto invalid = stored();
        invalid.erase(key);
        rejects(invalid.dump(), key, "missing required key");
        invalid[key] = 42;
        rejects(invalid.dump(), key, "expected a string");
    }
    for (const auto* key : {"typo", "id", "state", "generation", "request", "actionId", "activeRun", "lastRun", "pendingDestination",
                            "runTargets", "runResults", "currentRevision", "replanning", "sessionOrigin", "selectedSession",
                            "sourceMessageId", "workflowId", "trigger", "runOutcome"}) {
        auto invalid = stored();
        invalid[key] = nullptr;
        rejects(invalid.dump(), key, "unknown key");
    }
    auto invalid = stored();
    invalid["criteriaSourceMessageId"] = 3;
    rejects(invalid.dump(), "criteriaSourceMessageId", "requires proposed completion criteria");
    invalid = stored();
    invalid["config"] = Json::array();
    rejects(invalid.dump(), "config", "expected an object");
    invalid = stored();
    invalid["gatewayEnvironmentId"] = false;
    rejects(invalid.dump(), "gatewayEnvironmentId", "expected a string");
}

void executionDecodeErrors() {
    const auto base = storedExecution();
    auto invalid = base;
    invalid["eventId"] = nullptr;
    rejects(invalid.dump(), "eventId", "expected an integer");
    invalid = base;
    invalid["pendingRunReport"]["report"]["run"].erase("revisionId");
    rejects(invalid.dump(), "pendingRunReport.report.run.revisionId", "missing required key");
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
}

void journalRunReportRoundTrips() {
    const auto assigned = storedExecution().at("pendingRunReport").at("report");
    const auto wholeRun = decodeRunReport(id, assigned);
    check(encodeRunReport(wholeRun) == assigned && reportSourceRun(wholeRun).assignment && !wholeRun.stopRequested,
          "Assigned Run report changed its journal representation");
    auto destination = assigned;
    destination.update({{"destination", "Build"},
                        {"projectId", "project"},
                        {"commit", "checked"},
                        {"requestedCommit", "requested"},
                        {"stopRequested", false},
                        {"status", "cancelled"},
                        {"revisionId", 5}});
    for (const auto* key : {"environmentId", "ownerBotId", "projectId", "revisionId"}) destination["run"].erase(key);
    const auto fanOut = decodeRunReport(id, destination);
    check(encodeRunReport(fanOut) == destination && !reportSourceRun(fanOut).assignment && fanOut.stopRequested == false,
          "Destination Run report lost its shape or explicit stop flag");
    destination["run"]["unexpected"] = "ignored before typing";
    try {
        (void)decodeRunReport(id, destination);
    } catch (const RecordDecodeError& error) {
        const std::string_view message = error.what();
        check(message.find("run.unexpected") != message.npos, "Run report error did not identify the malformed field");
        return;
    }
    throw std::runtime_error("Run report accepted an unknown Run field");
}

void acceptedInstructionRoundTrips() {
    Json data = storedExecution().at("cancelledInstructions").at(0);
    data["source"] = {{"taskId", 12}, {"messageId", 31}, {"run", {{"threadId", "thread"}, {"runId", "run"}}}};
    const auto instruction = decodeAcceptedInstruction(id, data);
    check(instruction.source && instruction.source->run && instruction.source->run->runId == "run" && instructionJson(instruction) == data,
          "Accepted instruction lost its source or changed its journal representation");

    data["step"] = "1";
    try {
        (void)decodeAcceptedInstruction(id, data);
    } catch (const RecordDecodeError& error) {
        const std::string_view message = error.what();
        check(message.find("step") != message.npos && message.find("expected an integer") != message.npos,
              "Accepted instruction error did not identify the malformed step");
        return;
    }
    throw std::runtime_error("Accepted instruction accepted a noninteger step");
}

void completionAndFeedbackRoundTrips() {
    const Json assignedRun = storedExecution().at("pendingRunReport").at("report").at("run");
    const Json workflow{{"text", "Stage failed"}, {"revisionId", 7}, {"source", assignedRun}, {"workflowOutcome", "failed"}};
    const auto stage = decodeCompletion(id, workflow);
    check(stage.source() && stage.source()->assignment && encodeCompletion(stage) == workflow,
          "Workflow completion lost its assigned Run or its outcome");

    const Json fanout{{"text", "One of two finished"},
                      {"revisionId", nullptr},
                      {"doneWhen", "Compare results"},
                      {"runOutcome", "partial_failure"},
                      {"requestedCommit", "checked"}};
    const auto runs = decodeCompletion(id, fanout);
    check(std::get<FanoutCompletion>(runs.details).outcome == RunOutcome::partial_failure && encodeCompletion(runs) == fanout,
          "Fan-out completion changed its outcome or requested commit");

    const Json dependency{{"taskId", 70},    {"exchangeId", "exchange"}, {"messageId", "result"},
                          {"botId", "peer"}, {"requestKind", "work"},    {"status", "completed"}};
    const Json evidence{
        {"environmentId", "env"}, {"threadId", "thread"}, {"runId", "run"}, {"kind", "peer_report"}, {"messageId", "result"}};
    Json peer{{"text", "Done"},
              {"revisionId", nullptr},
              {"doneWhen", "Compare results"},
              {"reason", "Peer verified"},
              {"dependency", dependency},
              {"evidenceRefs", Json::array({evidence})}};
    check(encodeCompletion(decodeCompletion(id, peer)) == peer, "Peer completion changed its dependencies or evidence");

    Json failure{{"eventId", 38}, {"tool", "follow_up"}, {"args", nullptr}, {"result", {{"ok", false}, {"error", "Unavailable"}}}};
    check(encodeToolError(decodeToolError(id, failure)) == failure, "Feedback lost null tool arguments");
    failure.erase("args");
    failure["argumentsExcerpt"] = "[bounded excerpt]";
    failure["result"]["delivery"] = "unknown";
    check(encodeToolError(decodeToolError(id, failure)) == failure, "Feedback lost the excerpt or unknown delivery status");

    const Json decision{{"eventId", 38},
                        {"revisionId", 7},
                        {"actionId", "send"},
                        {"tool", "follow_up"},
                        {"reason", "Continue"},
                        {"args", {{"instruction", "Compare results"}}},
                        {"evidence", Json::array()},
                        {"usage", {{"total_tokens", 19}}}};
    check(encodeJournalDecision(decodeJournalDecision(id, decision)) == decision, "Decision changed its recorded usage or arguments");

    peer["evidenceRefs"][0].erase("messageId");
    try {
        (void)decodeCompletion(id, peer);
    } catch (const RecordDecodeError& error) {
        check(std::string_view(error.what()).find("evidenceRefs.0.messageId") != std::string_view::npos,
              "Malformed peer evidence did not identify its missing message anchor");
        return;
    }
    throw std::runtime_error("Peer report completion accepted evidence with no message anchor");
}

void questionAndCheckpointRoundTrip() {
    const Json decision{{"generation", generation},
                        {"eventId", 15},
                        {"revisionId", nullptr},
                        {"tool", "answer_session_question"},
                        {"args", {{"answers", {{"branch", "main"}}}}},
                        {"usage", {{"total_tokens", 19}}},
                        {"attemptId", 25}};
    auto data = stored();
    data["question"] = {
        {"requestId", "request"},
        {"fields", Json::array({{{"id", "branch"},
                                 {"question", "Which branch?"},
                                 {"header", "Revision"},
                                 {"required", true},
                                 {"multiple", false},
                                 {"freeform", false},
                                 {"options", Json::array({{{"label", "Main"}, {"value", "main"}, {"description", "Default"}}})}}})},
        {"target", {{"eventId", 15}, {"requestId", "request"}, {"run", nullptr}, {"waitId", 20}}},
        {"suspendedWaitId", 20},
        {"suspendedDecision", decision}};
    data["pendingDecision"] = decision;
    data["blockerReason"] = "Waiting for a branch";
    data["blockerWaitId"] = 20;
    const auto task = decodeTaskPayload(TaskData{.id = id, .generation = generation, .state = TaskState::waiting}, data.dump());
    check(task.question && task.question->fields.at(0).options.at(0).value == "main" && task.question->target.waitId == 20 &&
              task.question->suspendedWaitId == 20 && task.question->suspendedDecision &&
              task.question->suspendedDecision->decision.attemptId == 25 && task.pendingDecision && !task.pendingDecision->revisionId &&
              task.pendingDecision->decision.tool == "answer_session_question" && task.blocker && task.blocker->waitId == 20 &&
              encodeTaskPayload(task) == data,
          "Question, suspended checkpoint, and blocker changed on round-trip");

    data["question"]["suspendedDecision"]["attempt"] = 25;
    rejects(data.dump(), "question.suspendedDecision.attempt", "unknown key");
}
}

int main() {
    try {
        roundTrips();
        executionRoundTrips();
        destinationRoundTrips();
        decodeErrors();
        executionDecodeErrors();
        journalRunReportRoundTrips();
        acceptedInstructionRoundTrips();
        completionAndFeedbackRoundTrips();
        questionAndCheckpointRoundTrip();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
