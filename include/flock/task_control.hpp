#pragma once

#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"
#include <nlohmann/json.hpp>
#include <string_view>

namespace flockv0 {
class Store;
class Database;
class OutboxStorage;
class TaskOperations;
class TaskRecords;
class WaitStorage;
struct Person;
struct TaskTarget;

class TaskControl {
public:
    explicit TaskControl(Store&);

    // Operations: answer {text}, approve, retry, extend {deadline}, reconcile
    // {threadId?,runId?,questionId?,session?}, cancel. All except cancel require a matching waitId.
    void operate(const Person&, const TaskTarget&, std::string_view operation, const Json& data = Json::object());
    Json reconciliationData(Id task, Id waitId, Json target, const IntakeResolver&) const;
private:
    Database& database_;
    const OutboxStorage& outbox_;
    TaskOperations& operations_;
    const TaskRecords& records_;
    const WaitStorage& waits_;
};
}
