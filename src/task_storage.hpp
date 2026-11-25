#pragma once

#include "flock/core.hpp"

namespace flockv0 {
class Statement;
class RecordDecoder;

TaskState decodeTaskState(Id id, std::string_view);
TaskData decodeTask(Id id, std::string_view state, Id generation, std::string_view data);
TaskData decodeTaskRow(const Statement&, int col = 0);
Json encodeTask(const TaskData&);
Json assignedRunJson(const AssignedRun&);
Json assignedRunJson(const std::optional<AssignedRun>&);
Json runStopJson(const RunStop&);
Json settledRunReportJson(const SettledRunReport&);
SessionOrigin decodeSessionOrigin(const Json&, const RecordDecoder&);
Json sessionOriginJson(const SessionOrigin&);
Json waitQuestionTarget(const Wait&);
}
