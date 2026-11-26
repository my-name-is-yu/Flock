#pragma once

#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/soul_loader.hpp"
#include "flock/task.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace flockv0 {
class Database;
class MemoryStorage;
class SecretStorage;
class TaskEvents;

class runtime::SessionPrompts {
public:
    SessionPrompts(Database& database, SecretStorage& secretStorage, MemoryStorage& memoryStorage, TaskEvents& taskEvents,
                   const SoulLoader& soul)
        : database(database), secretStorage(secretStorage), memoryStorage(memoryStorage), taskEvents(taskEvents), soul(soul) {}

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
    std::string prepareStandingContext(Outbox& action, const TaskData& task, Seconds now);
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
    MemoryStorage& memoryStorage;
    TaskEvents& taskEvents;
    const SoulLoader& soul;
};
}
