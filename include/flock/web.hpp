#pragma once
#include "flock/core.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
struct AvatarImage {
    std::string contentType;
    std::string bytes;
};

struct WebCallbacks {
    // Array of objects with id, name, soul, cron, and optional avatar/role.
    std::function<Json()> bots;
    std::function<Intake(std::string_view botId, std::string_view request)> intake;
    // Optional Bot profile management callbacks.
    std::function<Json(const Json& profile, const std::optional<AvatarImage>& image)> createBot;
    std::function<Json(std::string_view botId, const Json& profile, const std::optional<AvatarImage>& image)> updateBot;
    std::function<std::optional<AvatarImage>(std::string_view botId)> botAvatar;
    std::function<std::vector<PeerBot>(std::string_view botId)> peers = {};
};
class WebServer {
public:
    virtual ~WebServer() = default;
    virtual unsigned port() const = 0;
};
std::unique_ptr<WebServer> startWeb(Store&, unsigned port, WebCallbacks);
}
