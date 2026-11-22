#pragma once

#include "flock/workspace_tools.hpp"

namespace flockv0::app {
std::shared_ptr<const BotTools> loadBotTools(const std::filesystem::path& home);
}
