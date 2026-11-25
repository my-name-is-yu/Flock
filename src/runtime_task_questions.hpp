#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "flock/store.hpp"
#include "flock/runtime.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"

#include <optional>
#include <string_view>

namespace flockv0 {
class Runtime::Components::TaskQuestions {
public:
    explicit TaskQuestions(Store::Impl& db) : db(db) {}

    void closeQuestion(TaskData& task, std::string_view reason, std::string_view resolution = {});
    void suspendQuestion(TaskData& task);
    std::optional<DecisionCheckpoint> resumeQuestion(TaskData& task);
private:
    Store::Impl& db;
};
}
