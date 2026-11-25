#pragma once

#include "flock/core.hpp"

namespace flockv0 {
Json to_json(const Wait&);
Wait from_json(Id id, std::string_view data);
Wait decodeWaitingEntry(Id entry, const Json& data);
Json waitRunJson(const std::optional<WaitRun>&);
std::optional<WaitRun> taskWaitRun(const Json& task);
WaitUsage waitUsage(const Json&);
WorkBudgetTool workBudgetTool(std::string_view);
}
