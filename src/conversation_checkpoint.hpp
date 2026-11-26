#pragma once

#include "model_calls.hpp"
#include "flock/json.hpp"
#include "flock/model_error.hpp"
#include "flock/types.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <variant>
#include <vector>

namespace flockv0 {
struct ConversationModelFailure {
    std::optional<Seconds> retryFrom;
    ModelErrorCode code = ModelErrorCode::inference_failed;
};

struct ConversationModelCheckpoint {
    Id attempt;
    int step;
    Json usage;
    std::variant<std::vector<ModelCall>, ConversationModelFailure> outcome;
};
}
