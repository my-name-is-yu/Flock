#pragma once

#include "flock/json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class SecretRedactor;

struct LearnedMemory {
    std::string content;
    std::vector<std::string> added, removed;
};

Json safeTaskLearnings(const Json&, const SecretRedactor&);
LearnedMemory mergeTaskLearnings(std::string_view memory, const Json&, std::string_view runId, const SecretRedactor&);
}
