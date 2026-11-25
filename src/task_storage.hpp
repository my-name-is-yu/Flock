#pragma once

#include "flock/core.hpp"

namespace flockv0 {
class Statement;

TaskState decodeTaskState(Id id, std::string_view);
TaskData decodeTask(Id id, std::string_view state, Id generation, std::string_view data);
TaskData decodeTaskRow(const Statement&, int col = 0);
Json encodeTask(const TaskData&);
Json waitQuestionTarget(const Wait&);
}
