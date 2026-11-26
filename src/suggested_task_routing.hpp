#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <span>

namespace flockv0 {
class SuggestionStorage;
class TaskRouting;
struct LinkedTaskDestinations;
struct SuggestedTask;
struct TaskData;

Json suggestedTaskRows(std::span<const SuggestedTask>);
namespace conversation {
struct Message;
}

std::size_t suggestTasks(SuggestionStorage&, TaskRouting&, const conversation::Message&, const Json& items, const LinkedTaskDestinations&,
                         const TaskData* completedTask = nullptr);
Json suggestionRouting(const TaskData&);
}
