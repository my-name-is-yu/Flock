#include "runtime_run_reports.hpp"
#include "task_run_reports.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "database.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "journal_storage.hpp"
#include "task_events.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
runtime::RunReports::RunReports(Database& database, JournalStorage& journalStorage, TaskEvents& taskEvents, T3& t3, const Policy& policy,
                                TaskUpdates& updates, const std::function<Seconds()>& clock)
    : database(database), journalStorage(journalStorage), taskEvents(taskEvents), t3(t3), policy(policy), updates(updates), clock(clock) {}

void runtime::RunReports::writeRunReport(TaskData& task, const SettledRunReport& report) {
    const auto entry = runReportEntry(report);
    const auto& run = report.run.ref;
    if (task.state != TaskState::cancelling && !task.replanning) {
        taskEvents.event(task, "result/" + report.run.environmentId + "/" + run.threadId + "/" + run.runId, encodeRunReport(entry));
    }
    journalStorage.journal(task.id, entry);
}

RunEvidence runtime::RunReports::fetchRunEvidence(const TaskData& task, const std::string& environment, RunRef source,
                                                  std::string_view resultRunId, std::string_view status) {
    source.runId = resultRunId;
    RunEvidence evidence;
    try {
        evidence = t3.readRunEvidence(environment, task.config, source, status);
    } catch (const std::exception& error) {
        evidence.availability = RunEvidence::Availability::ReportUnavailable;
        evidence.detail = error.what();
    }
    const auto scrub = policy.secretRedactor(task.botId);
    evidence.report = scrub.text(evidence.report);
    evidence.detail = scrub.text(evidence.detail);
    if (scrub.text(evidence.itemId) != evidence.itemId) evidence.itemId.clear();
    return evidence;
}

void runtime::RunReports::recoverRunReport(const TaskData& task) {
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

bool runtime::RunReports::hasSettledRun(const TaskData& task) {
    return !task.activeRun && task.lastRun && hasRun(task.lastRun->ref);
}

std::optional<runtime::RunReports::JournalReport> runtime::RunReports::settledSessionReport(const TaskData& task) const {
    if (!task.lastRun || !hasRun(task.lastRun->ref)) return std::nullopt;
    const auto& run = task.lastRun->ref;
    auto report = database.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT 1", {task.id});
    if (!report.next()) return std::nullopt;
    auto data = decodeRunReport(report.number(0), report.json(1));
    if (data.run.threadId != run.threadId || data.run.runId != run.runId) return std::nullopt;
    return JournalReport{report.number(0), std::move(data)};
}
}
