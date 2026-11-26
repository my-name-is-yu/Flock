#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
class Store;
class Database;
class TaskRecords;
struct TaskView;
struct Wait;

class TaskHistory {
public:
    explicit TaskHistory(Store&);

    static Wait waitingEvent(const Json& event);
    std::vector<TaskView> sessionTasks(std::string_view botId, Id before, unsigned limit) const;
    // Journal position and tasks with a Run start or settled report since that position.
    // On the initial read only the position is needed, before loading the first page.
    std::pair<Id, std::vector<TaskView>> sessionChangesSince(std::string_view botId, std::optional<Id> after) const;
    // Oldest first. The cursor is an exclusive id.
    Json history(Id taskId, Id after = 0, unsigned limit = 50) const;
private:
    Database& database_;
    const TaskRecords& records_;
};
}
