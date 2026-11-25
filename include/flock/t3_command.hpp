#pragma once

#include "flock/command.hpp"
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/session_ref.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace flockv0 {
std::string sessionPrompt(const std::string& actionId, const std::string& instruction);

struct Command : CommandPayload {
    Json config = Json::object();
    std::string authGeneration{}, ownerBotId{};
    std::optional<SessionOrigin> sessionOrigin{};
};
struct Delivery {
    enum class Kind { Accepted, Unknown, Auth, Settings, QuestionGone, NotAccepted } kind = Kind::Unknown;
    RunRef run;
    std::string detail;
    bool definitelyUnsent = false;
    std::string delivery = {};
    std::string launchWarning = {};
};
}
