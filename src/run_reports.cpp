#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
void setReportEvidence(Json& data, const RunEvidence& evidence) {
    using Availability = RunEvidence::Availability;
    const bool missing = evidence.availability == Availability::ReportUnavailable;
    const auto report = missing ? std::string{} : bounded(evidence.report,8192);
    data["report"] = report;
    data["truncated"] = !missing && (evidence.truncated || report.size() < evidence.report.size());
    data["detail"] = bounded(evidence.detail,2048);
    data["itemId"] = missing ? std::string{} : evidence.itemId;
    if (!evidence.sessionCreatedAt.empty()) data["sessionCreatedAt"] = evidence.sessionCreatedAt;
    data.erase("evidenceStatus");
    if (missing) data["evidenceStatus"] = "report_unavailable";
    if (evidence.availability == Availability::DiagnosticUnavailable) data["evidenceStatus"] = "diagnostic_unavailable";
}

Json& destinationRun(Json& task, std::string_view action) {
    auto& runs = task.at("runResults");
    const auto entry = std::ranges::find_if(runs,[&](const Json& run) { return field(run,"actionId") == action; });
    if (entry == runs.end()) throw Conflict("The saved Run is outside this task");
    return *entry;
}

bool destinationRunOutstanding(const Json& run) {
    const auto status = field(run,"status");
    return status == "queued" || status == "running" || status == "confirmation_pending";
}

bool Store::Impl::advanceDestinationSessionMessageRun(Json& task, Json& run) {
    auto pending = query(R"SQL(
SELECT w.environment,w.thread,w.run,w.gateway_environment,w.delivery,w.link
FROM session_message_waits w
WHERE w.task_id=? AND w.action_id=?
  AND w.environment=? AND w.thread=? AND w.gateway_environment=?
  AND NOT EXISTS (
    SELECT 1 FROM journal j WHERE j.task_id=? AND j.kind='report'
      AND json_extract(j.data,'$.environmentId')=w.environment
      AND json_extract(j.data,'$.run.runId')=w.run
      AND json_extract(j.data,'$.run.threadId')=w.thread)
ORDER BY w.rowid LIMIT 1)SQL",
        {task.at("id"),field(run,"actionId"),field(run,"environmentId"),field(run,"threadId"),
         field(run,"gatewayEnvironmentId"),task.at("id")});
    if (!pending.next()) return false;
    const auto environment = pending.string(0);
    const auto thread = pending.string(1);
    const auto runId = pending.string(2);
    const auto gateway = pending.string(3);
    const auto delivery = pending.string(4);
    const auto link = pending.string(5);
    if (runId == field(run,"runId")) return false;
    exec("UPDATE runs SET thread_id=?,run_id=?,link=? WHERE action_id=? AND task_id=? AND environment_id=?",
         {thread,runId,link.empty() ? field(run,"link") : link,field(run,"actionId"),task.at("id"),environment});
    if (sqlite3_changes(db) != 1) return false;
    run["threadId"] = thread;
    run["runId"] = runId;
    run["gatewayEnvironmentId"] = gateway;
    if (!link.empty()) run["link"] = link;
    run["status"] = delivery == "queued" ? "queued" : "running";
    auto action = outbox(task,field(run,"actionId"));
    if (action.value("stopRequested",false)) {
        for (const auto* key : {"stopRequested","stopOperation","stopRunId","stopThreadId","stopPolicy","stopPolicyOrigin"})
            action.erase(key);
        exec("UPDATE outbox SET data=? WHERE id=?",{action,field(run,"actionId")});
    }
    for (const auto* key : {"commit","report","detail","evidenceStatus","sessionCreatedAt"}) run.erase(key);
    run.erase("retryAt");
    run.erase("stopRetryAt");
    run.erase("evidenceAttempts");
    return true;
}

bool Store::Impl::recordDestinationReport(Json& task, Json& run, const Observation& observed) {
    const auto ref = runRef(run);
    auto status = observed.status;
    if (status.empty()) status = "failed";
    auto detail = observed.detail;
    if (status == "completed" && task.contains("requestedCommit") &&
        observed.checkedCommit != field(task,"requestedCommit")) {
        status = "failed";
        if (detail.empty()) detail = "The Run did not verify the requested commit.";
    }
    Json data = {{"kind","result"},{"run",runJson(ref)},{"environmentId",run.at("environmentId")},
        {"projectId",run.at("projectId")},{"destination",run.at("name")},{"status",status},
        {"commit",observed.checkedCommit},
        {"revisionId",task.value("currentRevision",Json(nullptr))},{"resultRunId",observed.resultRunId.empty() ? ref.runId : observed.resultRunId}};
    if (task.contains("requestedCommit")) data["requestedCommit"] = task.at("requestedCommit");
    setReportEvidence(data,{observed.evidence,observed.report,observed.itemId,std::move(detail),
        observed.truncated,observed.sessionCreatedAt});
    for (const auto* key : {"status","commit","report","detail","evidenceStatus","sessionCreatedAt"})
        if (data.contains(key)) run[key] = data.at(key);
    run.erase("retryAt");
    run.erase("stopRetryAt");
    run.erase("evidenceAttempts");
    if (field(task.at("wait"),"runActionId") == field(run,"actionId")) {
        clearWait(task,"run_finished");
        task["state"] = "running";
    }
    auto priorReport = query("SELECT 1 FROM journal WHERE task_id=? AND kind='report' "
                             "AND json_extract(data,'$.environmentId')=? AND json_extract(data,'$.run.runId')=? "
                             "AND json_extract(data,'$.run.threadId')=? AND json_extract(data,'$.revisionId') IS ? LIMIT 1",
        {task.at("id"),run.at("environmentId"),ref.runId,ref.threadId,
         task.value("currentRevision",Json(nullptr))});
    if (!priorReport.next()) journal(task.at("id"),"report",data);
    if (advanceDestinationSessionMessageRun(task,run)) return false;
    auto pendingSends = query(R"SQL(
SELECT json_extract(effect,'$.actionId') FROM conversation_steps
WHERE json_extract(effect,'$.kind')='message_session'
  AND json_extract(effect,'$.taskId')=?
)SQL",{task.at("id")});
    bool waitingForSend = false;
    while (pendingSends.next()) {
        waitingForSend = true;
        const auto actionId = pendingSends.string(0);
        const auto destination = std::ranges::find_if(task.at("runResults"),[&](const Json& candidate) {
            return field(candidate,"actionId") == actionId;
        });
        if (destination == task.at("runResults").end()) continue;
        if (!destinationRunOutstanding(*destination)) {
            (*destination)["status"] = "confirmation_pending";
            (*destination)["detail"] = "A Session message may have been accepted by T3, but Flock has no receipt to identify its Run. The task remains held.";
            (*destination)["retryAt"] = policyClock() + 5;
        }
    }
    if (waitingForSend) return false;
    const auto& results = task.at("runResults");
    if (std::ranges::any_of(results,destinationRunOutstanding)) return false;
    if (task.at("state") == "cancelling") return true;
    const auto completed = std::ranges::count_if(results,[](const Json& result) { return result.at("status") == "completed"; });
    const auto count = static_cast<std::size_t>(completed);
    task["runOutcome"] = count == results.size() ? "all_completed" : count > 0 ? "partial_failure" : "failed";
    task["state"] = "completed";
    auto summary = std::to_string(count) + " of " + std::to_string(results.size()) + " T3 Runs completed";
    if (task.contains("requestedCommit")) summary += " with a verified commit";
    Json completion = {{"text",std::move(summary) + "."},{"runOutcome",task.at("runOutcome")},
        {"doneWhen",task.at("doneWhen")},{"revisionId",data.at("revisionId")}};
    if (task.contains("requestedCommit")) completion["requestedCommit"] = task.at("requestedCommit");
    journal(task.at("id"),"completed",std::move(completion));
    return true;
}
}
