#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"

#include <functional>
#include <string>

namespace flockv0 {
class OutboxStorage;
class WorkflowStorage;

class Runtime::Components::RunObserver {
public:
    RunObserver(Store::Impl& db, OutboxStorage& outboxStorage, WorkflowStorage& workflowStorage, T3& t3, TaskUpdates& updates,
                TaskQuestions& questions, RunReports& reports, PeerExchanges& peerExchanges, const std::function<Seconds()>& clock,
                std::stop_token shutdown)
        : db(db), outboxStorage(outboxStorage), workflowStorage(workflowStorage), t3(t3), updates(updates), questions(questions),
          reports(reports), peerExchanges(peerExchanges), clock(clock), shutdown(shutdown) {}

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

    Store::Impl& db;
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
