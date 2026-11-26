#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <functional>
#include <string_view>

namespace flockv0 {
class Store;
struct Person;

inline constexpr std::string_view botSetupTool = "update_bot_setup";
inline constexpr std::string_view botSetupFields[] = {
    "name",  "soul", "model", "reasoningEffort", "environmentId", "projectId", "sessionModel", "workspaceStrategy", "fixPullRequests",
    "avatar"};

struct BotSetupEdit {
    const Json& patch;
    std::string_view change;
    std::function<void(const Json& before, const Json& after, const Json& dependencies)> record = {};
    const Json* expected = nullptr;
};

using SaveBotSetup = std::function<void(Store&, std::string_view, const BotSetupEdit&, const Json& catalog)>;

void undoBotSetup(Store&, const Person&, std::string_view bot, Id event);
}
