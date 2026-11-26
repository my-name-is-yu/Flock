#pragma once

#include "flock/json.hpp"

#include <nlohmann/json.hpp>
#include <optional>

namespace flockv0 {
struct CompletedToolCall {
    Json value;
    bool consumeApproval = false;
};
using ToolCallResult = std::optional<CompletedToolCall>;
}
