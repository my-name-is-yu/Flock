#pragma once

#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"

#include <functional>
#include <stop_token>
#include <string>

namespace flockv0 {
struct Observation;

class Database;
class JournalStorage;
class PeerStorage;
class SecretStorage;
class T3;
class TaskEvents;
class TaskRecords;
class WaitStorage;

class OutboxStorage;
class WorkflowStorage;

class runtime::RunObserver {
public:
    RunObserver(Database& database, JournalStorage& journalStorage, PeerStorage& peerStorage, SecretStorage& secretStorage,
                TaskEvents& taskEvents, TaskRecords& taskRecords, WaitStorage& waitStorage, OutboxStorage& outboxStorage,
                WorkflowStorage& workflowStorage, T3& t3, TaskUpdates& updates, TaskQuestions& questions, RunReports& reports,
                PeerExchanges& peerExchanges, const std::function<Seconds()>& clock, std::stop_token shutdown)
        : database(database), journalStorage(journalStorage), peerStorage(peerStorage), secretStorage(secretStorage),
          taskEvents(taskEvents), taskRecords(taskRecords), waitStorage(waitStorage), outboxStorage(outboxStorage),
          workflowStorage(workflowStorage), t3(t3), updates(updates), questions(questions), reports(reports), peerExchanges(peerExchanges),
          clock(clock), shutdown(shutdown) {}

    bool shouldStop(const TaskData& task);
    Observation observeRun(const std::string& bot, const std::string& environment, const Json& config, const RunRef& run);
    Observation observe(const TaskData& task);
    bool ingest(const TaskData& snapshot, const Observation& observed);
    bool questionStillOpen(const TaskData& task);
    void interruptRun(const TaskData& task);
    void cancelDone(TaskData& task);
    void finishReplan(const TaskData& task);
private:
    void stopNote(const TaskData& snapshot, const std::string& detail);

    Database& database;
    JournalStorage& journalStorage;
    PeerStorage& peerStorage;
    SecretStorage& secretStorage;
    TaskEvents& taskEvents;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    OutboxStorage& outboxStorage;
    WorkflowStorage& workflowStorage;
    T3& t3;
    TaskUpdates& updates;
    TaskQuestions& questions;
    RunReports& reports;
    PeerExchanges& peerExchanges;
    const std::function<Seconds()>& clock;
    std::stop_token shutdown;
};
}
