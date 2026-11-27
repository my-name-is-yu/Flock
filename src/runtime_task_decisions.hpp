#pragma once

#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"

#include <nlohmann/json.hpp>

namespace flockv0 {
class ConversationStorage;
class Database;
class TaskEvents;
class TaskRecords;
class WaitStorage;

// Flock never asks a model what a task does next. A task runs the request it was started with, and the conversation that
// started it decides what follows. Without a conversation the same closure rule applies at once.
class runtime::TaskDecisions {
public:
    TaskDecisions(Database& database, ConversationStorage& conversationTurns, TaskEvents& taskEvents, TaskRecords& taskRecords,
                  WaitStorage& waitStorage, TaskUpdates& updates, OutboxDispatch& dispatch);

    void launch(const TaskData& task, const Json& event, const Json* launchDefaults);
    void handle(const TaskData& task, const Json& event, const Json* launchDefaults);
    bool answerSessionQuestion(const TaskData& task, const Json& event);
    bool continueFreshSession(const TaskData& task, const Json* launchDefaults = nullptr);
private:
    static void applyLaunchDefaults(TaskData& task, const Json& defaults);

    Database& database;
    ConversationStorage& conversationTurns;
    TaskEvents& taskEvents;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    TaskUpdates& updates;
    OutboxDispatch& dispatch;
};
}
