#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "journal.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
class Runtime::Components::RunReports {
public:
    struct JournalReport {
        Id id;
        RunReportEntry data;
    };

    RunReports(Store::Impl&, T3&, const Policy&, TaskUpdates&, SessionHistory&, const std::function<Seconds()>& clock);
    static bool hasSettledRun(const TaskData&);
    void writeRunReport(TaskData&, const SettledRunReport&);
    void recoverRunReport(const TaskData&);
    std::optional<JournalReport> settledSessionReport(const TaskData&) const;
    bool sessionReportUnavailable(const TaskData&) const;
    std::optional<JournalReport> reportRetry(const TaskData&, const Json& event) const;
    void retryRunReport(const TaskData&, const Json& event, const TaskDecision&);
private:
    RunEvidence fetchRunEvidence(const TaskData&, const std::string& environment, RunRef source, std::string_view resultRunId,
                                 std::string_view status);

    Store::Impl& db;
    T3& t3;
    const Policy& policy;
    TaskUpdates& updates;
    SessionHistory& history;
    const std::function<Seconds()>& clock;
};
}
