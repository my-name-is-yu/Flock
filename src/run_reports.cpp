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
    journal(task.at("id"),"report",data);
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
