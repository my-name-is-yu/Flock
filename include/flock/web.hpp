#pragma once
#include "flock/bot_profile.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/types.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
class T3;
struct T3ConnectRequest {
    enum class Source { Local, Url, Saved };
    Source source;
    std::string value;
};

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
    T3* sessionT3 = nullptr;
    std::function<std::optional<Person>(std::string_view)> configuredPerson = {};
    // Optional local model status for installation roots.
    std::function<Json()> modelStatus = {};
    // Optional: starts ChatGPT sign-in in the background (opens the browser on this machine).
    std::function<bool(const Person&, bool reconsent)> modelLogin = {};
    // Optional: the models Bots can use, as [{slug, displayName}].
    std::function<Json()> modelCatalog = {};
    std::function<bool(const Person&, const T3ConnectRequest&)> t3Connect = {};
    std::function<Json(const std::string& environmentId)> t3Connection = {};
    std::function<Json(const Person&)> t3Setup = {};
    // A missing environment retries cleanup of a revoked provider registration.
    std::function<void(const std::optional<std::string>& environment)> t3Disconnect = {};
    std::function<bool(const Person&)> modelSwitch = {};
    // nullopt means a sign-in is in progress. The value indicates remote revocation.
    std::function<std::optional<bool>()> modelLogout = {};
    // Managed credentials may be enabled while the listener is running.
    std::function<bool()> identityRequired = {};
    std::function<Json(const Person&)> installationSettings = {};
    std::function<Json(const Person&, std::string_view section, const Json&)> changeInstallationSettings = {};
    std::function<Json(std::string_view botId)> botChannels = {};
    std::function<Json(std::string_view botId, const Json&)> changeBotChannel = {};
};
void configureChannelBridges(Store&, const std::filesystem::path&, WebCallbacks&);
void configureWebIdentities(Store&, const std::filesystem::path&, WebCallbacks&);
class WebServer {
public:
    virtual ~WebServer() = default;
    virtual unsigned port() const = 0;
};
std::unique_ptr<WebServer> startWeb(Store&, unsigned port, WebCallbacks, std::function<void(unsigned)> afterBind = {});
}
