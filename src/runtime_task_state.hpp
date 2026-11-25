#pragma once

#include "flock/limits.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

namespace flockv0 {
inline bool present(std::string_view value, std::size_t limit = limits::session_prompt::maxDecisionTextBytes) {
    return value.size() <= limit && value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}

inline bool hasRun(const RunRef& r) {
    return !r.threadId.empty() && !r.runId.empty();
}

inline Json revisionJson(std::optional<Id> revision) {
    return revision ? Json(*revision) : Json(nullptr);
}

inline bool needsDoneWhen(const TaskData& task) {
    return !task.currentRevision && (!task.criteriaProposal || task.criteriaProposal->doneWhen.empty());
}
}
