#include "t3_connection_setup.hpp"
#include "settings_access.hpp"
#include "settings_system.hpp"
#include "t3_setup.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/t3_connection.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/types.hpp"
#include "flock/web.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace flockv0::app {
namespace {

struct LoginTarget {
    std::optional<std::string> environmentId;
    std::string url;
};

LoginTarget resolveTarget(const T3ConnectRequest& request, const std::filesystem::path& credentialDirectory,
                          const std::optional<T3ConnectionSettings>& previous) {
    using Source = T3ConnectRequest::Source;
    switch (request.source) {
    case Source::Local: {
        auto local = discoverLocalT3();
        if (!local) throw Conflict("Open T3 Code on this machine, or connect using a T3 connection URL.");
        return {std::move(local->environmentId), std::move(local->mcpUrl)};
    }
    case Source::Url: return {std::nullopt, request.value};
    case Source::Saved: {
        const auto& id = request.value;
        if (!previous || std::ranges::find(previous->environments, id) == previous->environments.end())
            throw Conflict("This T3 environment is no longer connected.");
        auto saved = savedT3Connection(credentialDirectory, id);
        if (!saved) throw Conflict("The saved credentials are unavailable. Connect using a T3 connection URL.");
        if (saved->loopbackHttp) {
            auto local = discoverLocalT3();
            if (local && local->environmentId == id) return {id, std::move(local->mcpUrl)};
        }
        return {id, std::move(saved->mcpUrl)};
    }
    }
    throw Conflict("Unknown T3 connection source");
}

}

T3SetupResult connectT3(const std::filesystem::path& home, const T3ConnectRequest& request, unsigned port,
                        const std::function<void(const std::string&)>& authorize, std::stop_token stop,
                        const std::optional<Person>& requester) {
    const T3SetupLock setup;
    Store store(home / "flock.sqlite3");
    SettingsAccess access(store, home, std::getenv("FLOCK_WEB_IDENTITIES_FILE"));
    const auto resolve = access.identities().resolvePerson;
    const auto verifyAccess = [&] {
        if (access.enabled() && !resolve(localOwner.id)) throw Conflict("Keep a Web identity for local-owner to connect T3.");
        if (requester && !access.canManageInstallation(*requester))
            throw Conflict("The account that started this connection can no longer manage Flock.");
    };
    verifyAccess();
    T3Connections connections(store);
    const auto previous = connections.settings(localOwner.id);
    const auto credentialDirectory = home / "credentials" / "t3";
    const auto target = resolveTarget(request, credentialDirectory, previous);
    const auto savedPort = previous ? previous->port : 0;
    const auto cliPort = [&] { return SettingsSystem(home, std::nullopt, std::nullopt).t3CliPort(port, savedPort); };
    const auto connectionPort = requester ? port : cliPort();
    const auto owns = [&](std::string_view token) { return connections.ownsBearer(token); };

    std::vector<std::string> providers;
    auto login = loginT3(
        credentialDirectory, target.environmentId, target.url, authorize, true,
        [&](const std::string& environmentId, const std::function<void()>& saveCredential) {
            const auto refreshProviders = t3ProvidersNeedRefresh(owns);
            providers = configureT3Providers(
                "http://127.0.0.1:" + std::to_string(connectionPort) + "/mcp/flock", [&](const InstallT3Provider& install) {
                    connections.connect(
                        environmentId, localOwner.id, connectionPort,
                        [&](std::string_view token) {
                            verifyAccess();
                            if (!requester && cliPort() != connectionPort)
                                throw Conflict("The Web port changed during T3 setup. Start the connection again.");
                            if (!token.empty()) install(token);
                            saveCredential();
                        },
                        refreshProviders);
                });
        },
        stop);
    return {std::move(login.environmentId), connectionPort, savedPort != connectionPort, std::move(providers)};
}

void disconnectT3(Store& store, const std::optional<std::string>& environment) {
    const T3SetupLock setup;
    T3Connections connections(store);
    const bool cleanup = environment ? connections.disconnect(*environment) : connections.providerCleanupPending();
    if (cleanup) {
        removeT3Providers([&](std::string_view token) { return connections.ownsBearer(token); });
        connections.providerRemoved();
    }
}

T3WebSetup::T3WebSetup(std::filesystem::path home) : home_(std::move(home)) {}

void T3WebSetup::configure(WebCallbacks& callbacks, Store& store, bool live) {
    callbacks.t3Disconnect = [&store](const std::optional<std::string>& environment) { disconnectT3(store, environment); };
    if (!live) return;

    callbacks.t3Setup = [this](const Person& actor) { return status(actor); };
    callbacks.t3Connect = [this](const Person& actor, const T3ConnectRequest& request) { return start(actor, request); };
    callbacks.t3Connection = [directory = home_ / "credentials" / "t3"](const std::string& environmentId) -> Json {
        const auto saved = savedT3Connection(directory, environmentId);
        if (!saved) return nullptr;
        return {{"label", saved->label},
                {"expiresAt", saved->expiresAt},
                {"expired", saved->expiresAt <= unixTime()},
                {"loopbackHttp", saved->loopbackHttp}};
    };
}

Json T3WebSetup::status(const Person& actor) {
    std::lock_guard lock(mutex_);
    const bool ownsAttempt = actor.id == requester_;
    return {{"running", running_},
            {"authorizationUrl", ownsAttempt ? authorizationUrl_ : std::string{}},
            {"error", ownsAttempt ? error_ : std::string{}}};
}

bool T3WebSetup::start(const Person& actor, const T3ConnectRequest& request) {
    if (request.source == T3ConnectRequest::Source::Url) validateT3McpUrl(request.value);
    const auto port = port_.load();
    if (!port) return false;
    std::lock_guard lock(mutex_);
    if (running_) return false;

    worker_ = std::jthread([this, port, request, actor](std::stop_token stop) {
        std::string error;
        try {
            connectT3(
                home_, request, port,
                [&](const std::string& url) {
                    std::lock_guard lock(mutex_);
                    authorizationUrl_ = url;
                },
                stop, actor);
        } catch (const Conflict& failure) {
            error = failure.what();
        } catch (const std::exception&) {
            error = "Could not complete the T3 connection.";
        }
        std::lock_guard lock(mutex_);
        running_ = false;
        authorizationUrl_.clear();
        error_ = std::move(error);
    });
    running_ = true;
    requester_ = actor.id;
    authorizationUrl_.clear();
    error_.clear();
    return true;
}

}
