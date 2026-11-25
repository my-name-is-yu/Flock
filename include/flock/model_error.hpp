#pragma once

#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace flockv0 {
enum class ModelErrorCode {
    inference_failed,
    authentication_required,
    unsupported_reasoning_effort,
    image_rejected,
    invalid_tool_count,
    invalid_tool_output,
    unavailable_tool,
    invalid_tool_arguments
};
struct ModelError : std::runtime_error {
    ModelError(std::string message, bool retryable, Json measurements = Json::object(),
               ModelErrorCode failureCode = ModelErrorCode::inference_failed)
        : std::runtime_error(std::move(message)), transient(retryable), usage(std::move(measurements)), code(failureCode) {}
    ModelError(std::string message, ModelErrorCode failureCode) : ModelError(std::move(message), false, Json::object(), failureCode) {}
    bool transient;
    Json usage;
    ModelErrorCode code;
};
}
