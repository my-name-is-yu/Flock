#pragma once

#include "runtime_components.hpp"
#include "journal.hpp"
#include "flock/types.hpp"
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
class Database;
class JournalStorage;
class Policy;
class T3;
class TaskEvents;

class runtime::RunReports {
public:
    struct JournalReport {
        Id id;
        RunReportEntry data;
    };

    RunReports(Database&, JournalStorage&, TaskEvents&, T3&, const Policy&, TaskUpdates&, const std::function<Seconds()>& clock);
    static bool hasSettledRun(const TaskData&);
    void writeRunReport(TaskData&, const SettledRunReport&);
    void recoverRunReport(const TaskData&);
    std::optional<JournalReport> settledSessionReport(const TaskData&) const;
private:
    RunEvidence fetchRunEvidence(const TaskData&, const std::string& environment, RunRef source, std::string_view resultRunId,
                                 std::string_view status);

    Database& database;
    JournalStorage& journalStorage;
    TaskEvents& taskEvents;
    T3& t3;
    const Policy& policy;
    TaskUpdates& updates;
    const std::function<Seconds()>& clock;
};
}
