#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace flockv0 {
inline constexpr int minExternalCheckMinutes = 5;
inline constexpr int maxExternalCheckMinutes = 1440;

struct Decision {
    std::string tool;
    nlohmann::json args = nlohmann::json::object();
    std::string reason;
};

bool recordsTaskDecisionReason(std::string_view tool);
bool validDecisionReason(std::string_view reason);
}
