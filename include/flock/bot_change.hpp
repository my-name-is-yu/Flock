#pragma once

#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace flockv0 {
struct BotChange {
    std::string operation, target, name, requestKey;
};

using ApplyBotChange = std::function<void(const BotChange&)>;
using ReconcileBotCreation = std::function<bool(const BotChange&)>;
using PublishOwnedBot = std::function<void(std::string_view, const std::function<void()>&)>;

struct BotCreationUncertain : std::exception {};
}
