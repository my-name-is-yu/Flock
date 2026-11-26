#pragma once

#include "runtime_components.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"

#include <optional>
#include <string_view>

namespace flockv0 {
class Database;
class WaitStorage;

class runtime::TaskQuestions {
public:
    TaskQuestions(Database& database, WaitStorage& waits) : database(database), waits(waits) {}

    void closeQuestion(TaskData& task, std::string_view reason, std::string_view resolution = {});
    void suspendQuestion(TaskData& task);
    std::optional<DecisionCheckpoint> resumeQuestion(TaskData& task);
private:
    Database& database;
    WaitStorage& waits;
};
}
