#pragma once

#include <functional>
#include <string>

namespace flockv0 {
struct BotChange {
    std::string operation, target, name, requestKey;
};

using ApplyBotChange = std::function<void(const BotChange&)>;
using ReconcileBotCreation = std::function<bool(const BotChange&)>;
}
