#pragma once

#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/task.hpp"

#include <compare>
#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace flockv0 {
class Database;
class T3;
class TaskEvents;
class TaskRouting;

class runtime::TaskScheduler {
public:
    struct Work {
        Id taskId = 0;
        std::optional<std::string> destinationAction;
        auto operator<=>(const Work&) const = default;
    };

    TaskScheduler(Database& database, TaskEvents& taskEvents, TaskRouting& taskRouting, T3& t3, TaskUpdates& updates,
                  OutboxDispatch& dispatch, bool requireT3Connection, std::stop_token shutdown);
    void activate(TaskData task);
    std::vector<Work> nextBatch(std::size_t slots, const std::function<bool(const Work&)>& isActive);
private:
    std::vector<Work> eligible();

    bool selectProject(TaskData& task);

    Database& database;
    TaskEvents& taskEvents;
    TaskRouting& taskRouting;
    T3& t3;
    TaskUpdates& updates;
    OutboxDispatch& dispatch;
    bool requireT3Connection;
    std::stop_token shutdown;
    Work lastScheduled;
};
}
