#pragma once

#include "flock/core.hpp"

namespace flockv0 {
class RecordDecoder;

const std::string& taskProjection();
TaskState decodeTaskState(Id id, std::string_view);
TaskData decodeTaskPayload(TaskData, std::string_view data);
Json encodeTaskPayload(const TaskData&);
Json encodeTask(const TaskData&);
Json runTargetJson(const RunTarget&);
Json runTargetsJson(std::span<const RunTarget>);
Json destinationRunSummaryJson(const DestinationRun&);
Json destinationRunJson(const DestinationRun&);
DestinationRunStatus destinationRunStatusFromName(std::string);
std::optional<std::string_view> runEvidenceStatusName(RunEvidence::Availability);
RunEvidence::Availability decodeReportAvailability(const Json&, const RecordDecoder&);
Json assignedRunJson(const AssignedRun&);
Json assignedRunJson(const std::optional<AssignedRun>&);
Json runStopJson(const RunStop&);
Json settledRunReportJson(const SettledRunReport&);
SessionOrigin decodeSessionOrigin(const Json&, const RecordDecoder&);
Json sessionOriginJson(const SessionOrigin&);
}
