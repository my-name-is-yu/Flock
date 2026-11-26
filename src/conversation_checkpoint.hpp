#pragma once

#include "model_calls.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <variant>
#include <vector>

namespace flockv0 {
enum class ModelErrorCode;

struct ConversationModelFailure {
    std::optional<Seconds> retryFrom;
    ModelErrorCode code;
};

struct ConversationModelCheckpoint {
    Id attempt;
    int step;
    Json usage;
    std::variant<std::vector<ModelCall>, ConversationModelFailure> outcome;
};
}
