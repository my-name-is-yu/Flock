#pragma once

#include <optional>
#include <string_view>
#include "flock/json.hpp"
#include "flock/outbox.hpp"
#include "flock/run_ref.hpp"
#include "flock/store.hpp"

namespace flockv0 {

class Database;
class JournalStorage;
class OutboxStorage;
class WaitStorage;
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
    TaskRunReports(Database& database, Store::Impl& remaining, JournalStorage& journalStorage, OutboxStorage& outboxStorage,
                   WaitStorage& waitStorage)
        : database(database), remaining(remaining), journalStorage(journalStorage), outboxStorage(outboxStorage), waitStorage(waitStorage) {
    }

    bool advanceDestinationSessionMessageRun(TaskData&, DestinationRun&);
    bool recordDestinationReport(TaskData& task, DestinationRun& run, const Observation&);
private:
    Database& database;
    Store::Impl& remaining;
    JournalStorage& journalStorage;
    OutboxStorage& outboxStorage;
    WaitStorage& waitStorage;
};
}
