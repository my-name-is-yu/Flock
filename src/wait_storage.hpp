#pragma once

#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/wait.hpp"

#include <optional>
#include <string_view>
namespace flockv0 {
struct TaskData;
class RecordDecoder;
class Statement;

Json to_json(const Wait&);
Json encodeWaitData(const Wait&);
std::string_view storedWaitReason(const Wait&);
Wait decodeWait(Id id, std::string_view reason, std::string_view data);
Wait decodeWaitRow(const Statement&, int col = 0);
Wait decodeWaitingEntry(Id entry, const Json& data);
Json waitRunJson(const std::optional<WaitRun>&);
std::optional<WaitRun> decodeWaitRun(const Json&, const RecordDecoder&);
std::optional<WaitRun> taskWaitRun(const TaskData& task);
WaitUsage waitUsage(const Json&);
WorkBudgetTool workBudgetTool(std::string_view);
}
