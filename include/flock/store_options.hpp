#pragma once

#include "flock/types.hpp"
#include "flock/bot_setup.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
using MemorySeed = std::function<std::string(std::string_view)>;

struct PlannerModel {
    std::string model, provider;
    std::optional<std::string> reasoningEffort;
};

using AutomationModelResolver = std::function<std::optional<PlannerModel>(std::string_view botId)>;

struct StoreOptions {
    MemorySeed memorySeed = {};
    AutomationModelResolver automationModel = {};
    SaveBotSetup saveBotSetup = {};
    std::function<Json()> botSetupCatalog = {};
};
}
