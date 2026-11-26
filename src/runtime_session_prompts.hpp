#pragma once

#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/runtime.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace flockv0 {
class Database;
class SecretStorage;
class TaskEvents;

class WorkflowStorage;

class Runtime::Components::SessionPrompts {
public:
    SessionPrompts(Database& database, SecretStorage& secretStorage, TaskEvents& taskEvents, WorkflowStorage& workflowStorage)
        : database(database), secretStorage(secretStorage), taskEvents(taskEvents), workflowStorage(workflowStorage) {}

    struct WorkPrompt {
        std::string instruction;
        std::vector<OutboxInstruction> pending;
        OutboxContext contextIds;
        std::vector<OutboxSource> resultReferences;
    };

    struct ContextLimit : Conflict {
        ContextLimit()
            : Conflict("The Session prompt exceeds T3's size limit. "
                       "Continue the existing Session if possible, or revise the task before starting a new Session.") {}
    };

    WorkPrompt workPrompt(const TaskData& task, const Json& event, std::size_t actionIdBytes);
    std::vector<OutboxInstruction> pendingInstructions(const TaskData& task, const Json& event);
    void prepareDelegation(const TaskData& task, Outbox& action);
    static std::string dispatchedWorkPrompt(const Outbox& action);
private:
    struct SessionContract {
        std::string intent, doneWhen;
        Id answersAfter;
    };

    SessionContract sessionContract(const TaskData& task);
    std::vector<OutboxSource> appendSessionDirections(std::string& instruction, const TaskData& task,
                                                      std::span<const OutboxInstruction> pending, Id answersAfter);
    WorkPrompt workPrompt(const TaskData& task, const Json& event, std::size_t actionIdBytes, const SessionContract* contract);

    Database& database;
    SecretStorage& secretStorage;
    TaskEvents& taskEvents;
    WorkflowStorage& workflowStorage;
};
}
