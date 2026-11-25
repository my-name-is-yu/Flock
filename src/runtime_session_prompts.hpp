#pragma once

#include "storage.hpp"

namespace flockv0 {
class Runtime::SessionPrompts {
public:
    explicit SessionPrompts(Store::Impl& db) : db(db) {}

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

    Store::Impl& db;
};
}
