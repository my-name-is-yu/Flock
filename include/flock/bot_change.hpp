#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
struct BotAvatarChoice {
    std::string kind, preset, color;
};

struct BotChange {
    std::string target, name, requestKey;
    std::optional<BotAvatarChoice> avatar = {};
};

using ApplyBotChange = std::function<void(const BotChange&)>;
using ReconcileBotCreation = std::function<bool(const BotChange&)>;
using PublishOwnedBot = std::function<void(std::string_view, const std::function<void()>&)>;

struct BotCreationUncertain : std::exception {};
}
