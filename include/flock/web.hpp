#pragma once
#include "flock/core.hpp"

namespace flockv0 {
struct WebCallbacks {
    // Array of objects with id, name, soul, cron, and optional avatar.
    std::function<Json()> bots;
    std::function<Intake(std::string_view botId, std::string_view request)> intake;
};
class WebServer {
public:
    virtual ~WebServer() = default;
    virtual unsigned port() const = 0;
};
std::unique_ptr<WebServer> startWeb(Store&, unsigned port, WebCallbacks);
}
