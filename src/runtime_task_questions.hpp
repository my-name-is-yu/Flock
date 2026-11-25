#pragma once

#include "storage.hpp"

namespace flockv0 {
class Runtime::TaskQuestions {
public:
    explicit TaskQuestions(Store::Impl& db) : db(db) {}

    void closeQuestion(TaskData& task, std::string_view reason, std::string_view resolution = {});
    void suspendQuestion(TaskData& task);
    std::optional<DecisionCheckpoint> resumeQuestion(TaskData& task);
private:
    Store::Impl& db;
};
}
