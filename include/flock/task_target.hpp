#pragma once

#include "flock/types.hpp"

#include <string>

namespace flockv0 {
struct TaskData;

struct TaskTarget {
    std::string botId;
    Id taskId, waitId, revision, generation;
    static TaskTarget from(const TaskData& task, Id waitId);
};
}
