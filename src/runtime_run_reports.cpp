#include "runtime_run_reports.hpp"

#include "runtime_session_history.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "task_storage.hpp"

namespace flockv0 {
Runtime::RunReports::RunReports(Store::Impl& db, T3& t3, const Policy& policy, TaskUpdates& updates, SessionHistory& history,
                                const std::function<Seconds()>& clock)
    : db(db), t3(t3), policy(policy), updates(updates), history(history), clock(clock) {}

void Runtime::RunReports::writeRunReport(TaskData& task, const SettledRunReport& report) {
    const auto entry = runReportEntry(report);
    const auto& run = report.run.ref;
    if (task.state != TaskState::cancelling && !task.replanning) {
        db.event(task, "result/" + report.run.environmentId + "/" + run.threadId + "/" + run.runId, encodeRunReport(entry));
    }
    const auto reportId = db.journal(task.id, entry);
    if (task.workflow) {
        task.pendingStageReport = reportId;
        db.settleStageReport(task);
    }
}

RunEvidence Runtime::RunReports::fetchRunEvidence(const TaskData& task, const std::string& environment, RunRef source,
                                                  std::string_view resultRunId, std::string_view status) {
    source.runId = resultRunId;
    RunEvidence evidence;
    try {
        evidence = t3.readRunEvidence(environment, task.config, source, status);
    } catch (const std::exception& error) {
        evidence.availability = RunEvidence::Availability::ReportUnavailable;
        evidence.detail = error.what();
    }
    const auto scrub = db.redactor(task.botId);
    evidence.report = scrub.text(evidence.report);
    evidence.detail = scrub.text(evidence.detail);
    if (scrub.text(evidence.itemId) != evidence.itemId) evidence.itemId.clear();
    return evidence;
}

void Runtime::RunReports::recoverRunReport(const TaskData& task) {
    const auto& pending = task.pendingRunReport.value();
    if (clock() < pending.retryAt) return;
    auto report = pending.report;
    auto evidence = fetchRunEvidence(task, report.run.environmentId, report.run.ref, report.resultRunId, report.status);
    if (evidence.sessionCreatedAt.empty()) evidence.sessionCreatedAt = std::move(report.evidence.sessionCreatedAt);
    report.evidence = std::move(evidence);
    updates.change(task, [&](TaskData& current) {
        if (report.evidence.availability == RunEvidence::Availability::ReportUnavailable && pending.attempts == 0) {
            current.pendingRunReport = PendingRunReport{std::move(report), clock() + 10, 1};
        } else {
            current.pendingRunReport.reset();
            writeRunReport(current, report);
        }
        return true;
    });
}

bool Runtime::RunReports::hasSettledRun(const TaskData& task) {
    return !task.activeRun && task.lastRun && hasRun(task.lastRun->ref);
}

std::optional<Runtime::RunReports::JournalReport> Runtime::RunReports::settledSessionReport(const TaskData& task) const {
    if (!task.lastRun || !hasRun(task.lastRun->ref)) return std::nullopt;
    const auto& run = task.lastRun->ref;
    auto report = db.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT 1", {task.id});
    if (!report.next()) return std::nullopt;
    auto data = decodeRunReport(report.number(0), report.json(1));
    if (data.run.threadId != run.threadId || data.run.runId != run.runId) return std::nullopt;
    return JournalReport{report.number(0), std::move(data)};
}

bool Runtime::RunReports::sessionReportUnavailable(const TaskData& task) const {
    const auto report = settledSessionReport(task);
    return report && report->data.evidence.availability == RunEvidence::Availability::ReportUnavailable;
}

std::optional<Runtime::RunReports::JournalReport> Runtime::RunReports::reportRetry(const TaskData& task, const Json& event) const {
    if (!hasSettledRun(task) || task.replanning || task.newSessionRequired || task.lastRun->assignment.revisionId != task.currentRevision)
        return std::nullopt;
    auto retry = settledSessionReport(task);
    if (!retry) return std::nullopt;
    const auto& report = retry->data;
    if (report.evidence.availability != RunEvidence::Availability::ReportUnavailable || !report.matches(*task.lastRun) ||
        report.environmentId != task.environmentId || report.resultRunId.empty())
        return std::nullopt;
    if (event.at("kind") == "result") {
        const auto matches = [&](std::string_view key, const Json& value) {
            const auto saved = event.find(key);
            return saved != event.end() && *saved == value;
        };
        if (!matches("environmentId", report.environmentId) || !matches("run", assignedRunJson(*task.lastRun)) ||
            !matches("resultRunId", report.resultRunId) || !matches("status", report.status) ||
            !matches("revisionId", revisionJson(report.revisionId)))
            return std::nullopt;
    }
    auto settled = db.query(R"SQL(
SELECT id,data FROM journal WHERE task_id=? AND kind='run_settled'
AND run_environment_id=? AND run_thread_id=? AND run_id=? AND result_run_id=? AND run_status=?
AND revision_id IS ?
)SQL",
                            {task.id, report.environmentId, report.run.threadId, report.run.runId, report.resultRunId, report.status,
                             revisionJson(report.revisionId)});
    while (settled.next())
        if (decodeRunReport(settled.number(0), settled.json(1)).matches(*task.lastRun)) return retry;
    return std::nullopt;
}

void Runtime::RunReports::retryRunReport(const TaskData& task, const Json& event, const TaskDecision& decision) {
    const auto retry = [&] {
        std::lock_guard lock(db.mutex);
        return reportRetry(task, event);
    }();
    const auto& args = decision.args;
    if (!retry || args.contains("itemId") || args.contains("cursor") || args.contains("offset") ||
        field(args, "threadId") != retry->data.run.threadId || field(args, "runId") != retry->data.resultRunId)
        throw Conflict("Choose the exact settled result Run to retry its terminal report");
    const auto& report = retry->data;
    const auto& env = report.environmentId;
    const auto& thread = report.run.threadId;
    if (!policy.allows(task.botId, Resource::sessionRead(env, thread))) throw Conflict("Session read access required");
    const auto evidence = fetchRunEvidence(task, env, report.run, report.resultRunId, report.status);
    const bool available = evidence.availability != RunEvidence::Availability::ReportUnavailable;
    updates.change(task, [&](TaskData& current) {
        const auto latest = db.currentEvent(current);
        if (latest.is_null() || latest.at("consumed") == true || current.state != TaskState::deciding) return false;
        const auto target = reportRetry(current, latest);
        if (!target || target->id != retry->id || target->data != report) return false;
        if (available) {
            const auto boundedEvidence = boundedReportEvidence(evidence);
            auto updated = target->data;
            updated.evidence = boundedEvidence;
            if (updated.evidence.sessionCreatedAt.empty()) updated.evidence.sessionCreatedAt = report.evidence.sessionCreatedAt;
            db.exec("UPDATE journal SET data=? WHERE id=?", {encodeRunReport(updated), target->id});
            if (latest.at("kind") == "result") {
                auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=?", {latest.at("id"), current.id});
                if (!row.next()) throw Conflict("The settled result event is no longer available");
                auto data = row.json(0);
                setReportEvidence(data, boundedEvidence);
                db.exec("UPDATE events SET data=? WHERE id=? AND task_id=?", {data, latest.at("id"), current.id});
            }
        }
        history.recordSessionRead(current, latest,
                                  {{"environmentId", env},
                                   {"threadId", thread},
                                   {"runId", target->data.resultRunId},
                                   {"view", "report"},
                                   {"reportAvailable", available},
                                   {"text", bounded(evidence.detail, 2048)}},
                                  decision.usage);
        return true;
    });
}
}
