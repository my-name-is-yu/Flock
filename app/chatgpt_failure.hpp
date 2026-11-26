#pragma once

#include "flock/model_error.hpp"

#include <string>
#include <string_view>

namespace flockv0::app::chatgpt {
struct Failure : ModelError {
    Failure(std::string message, bool transient = false, std::string providerCode = {}, Json usage = Json::object(),
            ModelErrorCode code = ModelErrorCode::inference_failed);
    std::string providerCode;
};

Failure remoteFailure(std::string_view site, long status, const Json&, std::string_view requestId = {}, Json usage = Json::object());
}
