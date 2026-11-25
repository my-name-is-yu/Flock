#pragma once

#include "flock/task_context.hpp"
#include "flock/outbox_instruction.hpp"
#include "flock/json.hpp"

#include <span>

namespace flockv0 {
struct TaskData;
class RecordDecoder;

void decodeTaskContext(TaskData&, const Json&, const RecordDecoder&);
void encodeTaskContext(Json&, const TaskData&);
Json taskRestartJson(const TaskRestart&);
TaskTrigger decodeTaskTrigger(const Json&, const RecordDecoder&);
Json taskTriggerJson(const TaskTrigger&);
Json sourceResultsJson(std::span<const OutboxSource>);
}
