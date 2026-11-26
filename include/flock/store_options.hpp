#pragma once

#include "flock/types.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
using MemorySeed = std::function<std::string(std::string_view)>;

struct PlannerModel {
    std::string model, provider;
    std::optional<std::string> reasoningEffort;
    std::optional<Id> modelCallLimit = std::nullopt;
};

using AutomationModelResolver = std::function<std::optional<PlannerModel>(std::string_view botId)>;

struct StoreOptions {
    MemorySeed memorySeed = {};
    AutomationModelResolver automationModel = {};
};
}
