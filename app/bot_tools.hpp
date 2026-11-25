#pragma once

#include "flock/json.hpp"

#include <filesystem>
#include <memory>

namespace flockv0 {
struct BotTools;
}

namespace flockv0::app {
std::shared_ptr<const BotTools> loadBotTools(const std::filesystem::path& home);
Json readBotToolSettings(const std::filesystem::path& home);
std::shared_ptr<const BotTools> loadBotTools(const std::filesystem::path& home, const Json& settings);
}
