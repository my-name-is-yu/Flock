#include "task_run_reports.hpp"
#include "outbox_storage.hpp"
#include "journal_storage.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "journal_task.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/outbox.hpp"
#include "database.hpp"
#include "policy_approvals.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace flockv0 {
RunEvidence boundedReportEvidence(const RunEvidence& evidence) {
    const bool missing = evidence.availability == RunEvidence::Availability::ReportUnavailable;
    auto report = missing ? std::string{} : bounded(evidence.report, limits::session_evidence::maxReportBytes);
    const bool truncated = !missing && (evidence.truncated || report.size() < evidence.report.size());
    return {evidence.availability,          std::move(report), missing ? std::string{} : evidence.itemId,
            bounded(evidence.detail, 2048), truncated,         evidence.sessionCreatedAt};
}

void setReportEvidence(Json& data, const RunEvidence& evidence) {
    data["report"] = evidence.report;
    data["truncated"] = evidence.truncated;
    data["detail"] = evidence.detail;
    data["itemId"] = evidence.itemId;
    if (!evidence.sessionCreatedAt.empty()) data["sessionCreatedAt"] = evidence.sessionCreatedAt;
    data.erase("evidenceStatus");
    if (const auto status = runEvidenceStatusName(evidence.availability)) data["evidenceStatus"] = *status;
}

DestinationRun& destinationRun(TaskData& task, std::string_view action) {
    if (auto* results = std::get_if<RunResults>(&task.destinations)) {
        const auto entry = std::ranges::find(results->runs, action, &DestinationRun::actionId);
        if (entry != results->runs.end()) return *entry;
    }
    throw Conflict("The saved Run is outside this task");
}

bool destinationStopRequested(const std::optional<Outbox>& action, const RunRef& run) {
    if (!action || !action->stop) return false;
    const auto& requested = action->stop->run;
    return !requested || (requested->runId == run.runId && requested->threadId == run.threadId);
}

bool TaskRunReports::advanceDestinationSessionMessageRun(TaskData& task, DestinationRun& run) {
    auto pending = database.query(R"SQL(
SELECT w.run,w.delivery,w.link
FROM session_message_waits w
WHERE w.task_id=? AND w.action_id=?
  AND w.environment=? AND w.thread=? AND w.gateway_environment=?
  AND NOT EXISTS (
    SELECT 1 FROM journal j WHERE j.task_id=? AND j.kind='report'
      AND j.run_environment_id=w.environment AND j.run_id=w.run AND j.run_thread_id=w.thread)
ORDER BY w.rowid LIMIT 1)SQL",
                                  {task.id, run.actionId, run.environmentId, run.ref.threadId, run.ref.gatewayEnvironmentId, task.id});
    if (!pending.next()) return false;
    const auto runId = pending.string(0);
    const auto delivery = pending.string(1);
    const auto link = pending.string(2);
    if (runId == run.ref.runId) return false;
    database.exec("UPDATE runs SET thread_id=?,run_id=?,link=? WHERE action_id=? AND task_id=? AND environment_id=?",
                  {run.ref.threadId, runId, link.empty() ? run.ref.link : link, run.actionId, task.id, run.environmentId});
    if (sqlite3_changes(database.db) != 1) return false;
    run.ref.runId = runId;
    run.explicitDirectGateway = run.ref.gatewayEnvironmentId.empty();
    if (!link.empty()) run.ref.link = link;
    run.status = delivery == "queued" ? DestinationRunPhase::queued : DestinationRunPhase::running;
    auto action = outboxStorage.outbox(task, run.actionId);
    if (action && action->stop) {
        action->stop.reset();
        outboxStorage.saveOutbox(*action);
    }
    run.report.reset();
    run.detail.reset();
    run.retryAt.reset();
    run.stopRetryAt.reset();
    run.evidenceAttempts = 0;
    return true;
}

bool TaskRunReports::recordDestinationReport(TaskData& task, DestinationRun& run, const Observation& observed) {
    const auto& ref = run.ref;
    auto status = observed.status;
    if (status.empty()) status = "failed";
    auto detail = observed.detail;
    if (status == "completed" && task.requestedCommit && observed.checkedCommit != *task.requestedCommit) {
        status = "failed";
        if (detail.empty()) detail = "The Run did not verify the requested commit.";
    }
    std::optional<bool> stopRequested;
    if (status == "interrupted" || status == "cancelled") {
        const auto action = outboxStorage.outbox(task, run.actionId);
        stopRequested = task.state == TaskState::cancelling || destinationStopRequested(action, ref);
    }
    const RunReportEntry entry{
        ref,
        run.environmentId,
        observed.resultRunId.empty() ? ref.runId : observed.resultRunId,
        status,
        task.currentRevision,
        boundedReportEvidence(
            {observed.evidence, observed.report, observed.itemId, std::move(detail), observed.truncated, observed.sessionCreatedAt}),
        stopRequested,
        DestinationRunReportDetails{run.name, run.assignment.projectId, observed.checkedCommit, task.requestedCommit}};
    auto evidence = entry.evidence;
    const auto revision = task.currentRevision ? Json(*task.currentRevision) : Json(nullptr);
    run.status = destinationRunStatusFromName(std::move(status));
    if (run.report) {
        if (evidence.availability == RunEvidence::Availability::Available) evidence.availability = run.report->evidence;
        if (evidence.sessionCreatedAt.empty()) evidence.sessionCreatedAt = std::move(run.report->sessionCreatedAt);
    }
    run.report =
        DestinationReport{observed.checkedCommit, std::move(evidence.report), std::move(evidence.sessionCreatedAt), evidence.availability};
    run.detail = std::move(evidence.detail);
    run.retryAt.reset();
    run.stopRetryAt.reset();
    run.evidenceAttempts = 0;
    auto priorReport = database.query("SELECT 1 FROM journal WHERE task_id=? AND kind='report'"
                                      " AND run_environment_id=? AND run_thread_id=? AND run_id=? AND revision_id IS ? LIMIT 1",
                                      {task.id, run.environmentId, ref.threadId, ref.runId, revision});
    if (!priorReport.next()) journalStorage.journal(task.id, entry);
    if (advanceDestinationSessionMessageRun(task, run)) return false;
    auto pendingSends = database.query(R"SQL(
SELECT json_extract(effect,'$.actionId') FROM conversation_steps
WHERE json_extract(effect,'$.kind')='message_session'
  AND json_extract(effect,'$.taskId')=?
)SQL",
                                       {task.id});
    auto& results = std::get<RunResults>(task.destinations);
    auto& runs = results.runs;
    bool waitingForSend = false;
    while (pendingSends.next()) {
        waitingForSend = true;
        const auto destination = std::ranges::find(runs, pendingSends.string(0), &DestinationRun::actionId);
        if (destination == runs.end()) continue;
        if (!destination->outstanding()) {
            destination->status = DestinationRunPhase::confirmation_pending;
            destination->detail =
                "A Session message may have been accepted by T3, but Flock has no receipt to identify its Run. The task remains held.";
            destination->retryAt = policyApprovals.now() + 5;
        }
    }
    if (waitingForSend) return false;
    if (std::ranges::any_of(runs, &DestinationRun::outstanding)) return false;
    if (task.state == TaskState::cancelling) return true;
    const auto completed =
        std::ranges::count(runs, "completed", [](const DestinationRun& result) { return destinationRunStatusName(result.status); });
    const auto count = static_cast<std::size_t>(completed);
    const auto outcome = count == runs.size() ? RunOutcome::all_completed : count > 0 ? RunOutcome::partial_failure : RunOutcome::failed;
    results.outcome = outcome;
    task.state = TaskState::completed;
    auto summary = std::to_string(count) + " of " + std::to_string(runs.size()) + " T3 Runs completed";
    if (task.requestedCommit) summary += " with a verified commit";
    journalStorage.journal(task.id, CompletionEntry{std::move(summary) + ".", task.currentRevision,
                                                    FanoutCompletion{task.doneWhen, outcome, task.requestedCommit}});
    return true;
}
}
