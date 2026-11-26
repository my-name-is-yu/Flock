#pragma once

#include <optional>
#include <string_view>
#include "flock/json.hpp"
#include "flock/outbox.hpp"
#include "flock/run_ref.hpp"

namespace flockv0 {

class Database;
class PolicyApprovals;
class JournalStorage;
class OutboxStorage;
struct TaskData;
struct DestinationRun;
struct Observation;
struct RunEvidence;

DestinationRun& destinationRun(TaskData& task, std::string_view action);
bool destinationStopRequested(const std::optional<Outbox>& action, const RunRef&);
RunEvidence boundedReportEvidence(const RunEvidence&);
void setReportEvidence(Json&, const RunEvidence&);

class TaskRunReports {
public:
    TaskRunReports(Database& database, PolicyApprovals& policyApprovals, JournalStorage& journalStorage, OutboxStorage& outboxStorage)
        : database(database), policyApprovals(policyApprovals), journalStorage(journalStorage), outboxStorage(outboxStorage) {}

    bool advanceDestinationSessionMessageRun(TaskData&, DestinationRun&);
    bool recordDestinationReport(TaskData& task, DestinationRun& run, const Observation&);
private:
    Database& database;
    PolicyApprovals& policyApprovals;
    JournalStorage& journalStorage;
    OutboxStorage& outboxStorage;
};
}
