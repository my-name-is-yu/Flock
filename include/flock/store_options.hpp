#pragma once

#include "flock/types.hpp"
#include "flock/bot_setup.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
using MemorySeed = std::function<std::string(std::string_view)>;

struct StoreOptions {
    MemorySeed memorySeed = {};
    SaveBotSetup saveBotSetup = {};
    std::function<Json()> botSetupCatalog = {};
};
}
