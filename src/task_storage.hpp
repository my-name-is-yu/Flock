#pragma once

#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/session_ref.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace flockv0 {
struct TaskData;
std::string effectiveDoneWhen(const TaskData& task);
class RecordDecoder;

const std::string& taskProjection();
TaskState decodeTaskState(Id id, std::string_view);
TaskData decodeTaskPayload(TaskData, std::string_view data);
Json encodeTaskPayload(const TaskData&);
Json encodeTask(const TaskData&);
RunTarget decodeRunTarget(const Json&, const RecordDecoder&);
Json runTargetJson(const RunTarget&);
Json runTargetsJson(std::span<const RunTarget>);
Json destinationRunSummaryJson(const DestinationRun&);
Json destinationRunJson(const DestinationRun&);
DestinationRunStatus destinationRunStatusFromName(std::string);
RunOutcome decodeRunOutcome(std::string_view, const RecordDecoder&);
std::optional<std::string_view> runEvidenceStatusName(RunEvidence::Availability);
RunEvidence::Availability decodeReportAvailability(const Json&, const RecordDecoder&);
Json assignedRunJson(const AssignedRun&);
Json assignedRunJson(const std::optional<AssignedRun>&);
Json runStopJson(const RunStop&);
Json settledRunReportJson(const SettledRunReport&);
SessionOrigin decodeSessionOrigin(const Json&, const RecordDecoder&);
Json sessionOriginJson(const SessionOrigin&);
}
