#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {

// Routing and execution settings are saved at intake. Current instructions are loaded for each turn.
struct Intake {
    std::string botId, request, environmentId, projectId;
    Json config = Json::object();
};
using IntakeResolver = std::function<Intake(std::string_view botId, std::string_view request)>;

struct PeerBot {
    std::string name;
    Intake intake;
    std::string role = {};
};
using PeerResolver = std::function<std::vector<PeerBot>(std::string_view)>;
}
