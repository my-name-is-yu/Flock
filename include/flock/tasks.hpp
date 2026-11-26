#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
class Store;
class Database;
class OutboxStorage;
class TaskOperations;
class TaskRecords;
class WaitStorage;
struct Person;
struct TaskTarget;

class Tasks {
public:
    explicit Tasks(Store&);

    Id submit(const Intake&, const Person* requester = nullptr);
    // Operations: answer {text}, approve, retry, extend {deadline}, reconcile
    // {threadId?,runId?,questionId?,session?}, cancel. All except cancel require a matching waitId.
    void operate(const Person&, const TaskTarget&, std::string_view operation, const Json& data = Json::object());
    TaskView task(Id taskId) const;
    bool hasTask(Id taskId) const;
    // tasks: newest first, cursor is an exclusive id. history: oldest first, cursor is an exclusive id.
    std::vector<TaskView> tasks(std::string_view botId = {}, Id before = 0, unsigned limit = 50) const;
    struct ActiveTask {
        TaskData task;
        std::optional<Wait> wait;
        std::optional<TaskBlocker> blockedBy;
    };
    std::vector<ActiveTask> activeTasks(std::string_view botId) const;
    static Wait waitingEvent(const Json& event);
    Json reconciliationData(Id task, Id waitId, Json target, const IntakeResolver&) const;
    // Distinct active state and replanning combinations, grouped by Bot ID.
    std::map<std::string, std::vector<TaskPresence>> activeTaskStatesByBot() const;
    std::vector<TaskView> sessionTasks(std::string_view botId, Id before, unsigned limit) const;
    // Journal position and tasks with a Run start or settled report since that position.
    // On the initial read only the position is needed, before loading the first page.
    std::pair<Id, std::vector<TaskView>> sessionChangesSince(std::string_view botId, std::optional<Id> after) const;
    Json history(Id taskId, Id after = 0, unsigned limit = 50) const;
private:
    Database& database_;
    const OutboxStorage& outbox_;
    TaskOperations& operations_;
    const TaskRecords& records_;
    const WaitStorage& waits_;
};
}
