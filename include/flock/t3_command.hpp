#pragma once

#include "flock/command.hpp"
#include "flock/json.hpp"
#include "flock/session_ref.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace flockv0 {
struct Command : CommandPayload {
    Json config = Json::object();
    std::string authGeneration{}, ownerBotId{};
    std::optional<SessionOrigin> sessionOrigin{};
};
}
