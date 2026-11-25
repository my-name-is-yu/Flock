#pragma once

#include "flock/core.hpp"

namespace flockv0 {
class RecordDecoder;

void decodeTaskContext(TaskData&, const Json&, const RecordDecoder&);
void encodeTaskContext(Json&, const TaskData&);
Json taskRestartJson(const TaskRestart&);
Json taskTriggerJson(const TaskTrigger&);
Json sourceResultsJson(std::span<const OutboxSource>);
}
