#pragma once
#include "flock/policy.hpp"
#include "flock/bot_profile.hpp"
#include "flock/session_handoff.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
struct WebCallbacks {
    // Array of objects with id, name, soul, cron, and optional avatar/role.
    std::function<Json()> bots;
    IntakeResolver intake;
    // Optional Bot profile management callbacks.
    std::function<Json(const Person& creator, const Json& profile, const std::optional<AvatarImage>& image)> createBot = {};
    std::function<Json(std::string_view botId, const Json& profile, const std::optional<AvatarImage>& image)> updateBot = {};
    std::function<std::optional<AvatarImage>(std::string_view botId)> botAvatar = {};
    std::function<std::vector<PeerBot>(std::string_view botId)> peers = {};
    std::function<Person(std::string_view authorization)> authenticate = {};
    std::function<Seconds()> clock = unixTime;
    std::function<bool(std::string_view authorization)> authenticateSessionBridge = {};
    std::string githubWebhookSecret = {};
    // Required for queued messages with a custom authenticator. Resolve by person ID without credentials.
    WebIdentityResolver resolveIdentity = {};
    std::shared_ptr<SessionHandoff> sessionHandoffs = {};
    SessionHandoff::People configuredPerson = {};
};
void configureChannelBridges(Store&, const std::filesystem::path&, WebCallbacks&);
void configureWebIdentities(Store&, const std::filesystem::path&, WebCallbacks&);
class WebServer {
public:
    virtual ~WebServer() = default;
    virtual unsigned port() const = 0;
};
std::unique_ptr<WebServer> startWeb(Store&, unsigned port, WebCallbacks);
}
