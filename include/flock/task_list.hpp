#pragma once

#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
class Database;
class TaskRecords;
class WaitStorage;

class TaskList {
public:
    explicit TaskList(Store&);

    // Newest first. The cursor is an exclusive id.
    std::vector<TaskView> tasks(std::string_view botId = {}, Id before = 0, unsigned limit = 50) const;
    struct ActiveTask {
        TaskData task;
        std::optional<Wait> wait;
        std::optional<TaskBlocker> blockedBy;
    };
    std::vector<ActiveTask> activeTasks(std::string_view botId) const;
    // Distinct active state and replanning combinations, grouped by Bot ID.
    std::map<std::string, std::vector<TaskPresence>> activeTaskStatesByBot() const;
private:
    Database& database_;
    const TaskRecords& records_;
    const WaitStorage& waits_;
};
}
