#pragma once

#include "flock/json.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <string>
#include <string_view>

namespace flockv0 {
class Database;
class JournalStorage;
class SecretStorage;
class TaskEvents;
class TaskRecords;
class TaskOperations;
class TaskRouting;
struct Observation;
struct TaskData;

class WorkflowStorage {
public:
    WorkflowStorage(Database&, TaskRecords&, TaskEvents&, JournalStorage&, const SecretStorage&, Store::Impl&, TaskOperations&,
                    TaskRouting&);

    Json workflowTool(std::string_view tool, const Json&, std::string_view bot, Id message, int step, const Json& routing);
    Json workflowStatus(Id id, std::string_view bot, Seconds now) const;
    void advanceWorkflows(Seconds now);
    void appendStageRequirements(std::string& instruction, const TaskData& task) const;
    void stageObservation(const TaskData& task, const Observation&, Seconds now);
    bool settleStageReport(TaskData& task);
private:
    Id startWorkflow(Id message, const Json& args, std::string_view bot, const Json& routing);
    void materializeWorkflowStages();

    Database& database_;
    TaskRecords& taskRecords_;
    TaskEvents& taskEvents_;
    JournalStorage& journalStorage_;
    const SecretStorage& secretStorage_;
    Store::Impl& remaining_;
    TaskOperations& operations_;
    TaskRouting& routing_;
};
}
