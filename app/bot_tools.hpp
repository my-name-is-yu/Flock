#pragma once

#include "flock/workspace_tools.hpp"

namespace flockv0::app {
std::shared_ptr<const BotTools> loadBotTools(const std::filesystem::path& home);
Json readBotToolSettings(const std::filesystem::path& home);
std::shared_ptr<const BotTools> loadBotTools(const std::filesystem::path& home, const Json& settings);
}
