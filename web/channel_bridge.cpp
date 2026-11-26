#include "flock/local_machines.hpp"
#include "flock/channel_bridge_settings.hpp"
#include "flock/channels.hpp"
#include "flock/json.hpp"
#include "flock/policy.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <httplib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
namespace {
struct Workspace {
    unsigned port;
    std::string credential;
    std::map<std::string, ChannelSender, std::less<>> senders;
};
using Workspaces = std::map<std::string, Workspace, std::less<>>;

bool sameCredential(std::string_view supplied, std::string_view expected) {
    if (supplied.size() != expected.size()) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) difference |= static_cast<unsigned char>(supplied[i] ^ expected[i]);
    return difference == 0;
}

std::string bridgeCredential(const std::string& secret, std::vector<std::string>& used) {
    if (secret.size() < 32 || secret.size() > 4096 || !std::ranges::all_of(secret, [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Conflict("Bridge credentials must have at least 32 characters without whitespace");
    std::string authorization = "Bearer " + secret;
    if (std::ranges::find(used, authorization) != used.end()) throw Conflict("Bridge credentials must be distinct");
    used.push_back(authorization);
    return authorization;
}

class BridgeTransport final : public ChannelTransport {
public:
    explicit BridgeTransport(std::shared_ptr<const Workspaces> workspaces) : workspaces_(std::move(workspaces)) {}
    ChannelDelivery send(std::string_view key, const ChannelOrigin& origin, std::string_view operation, const Json& payload) override {
        return call("/deliver", key, origin, {{"operation", operation}, {"payload", payload}});
    }
    ChannelDelivery reconcile(std::string_view key, const ChannelOrigin& origin) override {
        return call("/reconcile", key, origin, Json::object());
    }
private:
    ChannelDelivery call(const char* path, std::string_view key, const ChannelOrigin& origin, Json request) const {
        const auto workspace = workspaces_->find(origin.workspaceId);
        if (workspace == workspaces_->end()) return {ChannelDelivery::State::rejected, {}};
        request["idempotencyKey"] = key;
        request["origin"] = origin.json();
        httplib::Client client("127.0.0.1", static_cast<int>(workspace->second.port));
        client.set_connection_timeout(2);
        client.set_read_timeout(5);
        client.set_write_timeout(5);
        const auto response = client.Post(path, {{"Authorization", workspace->second.credential}}, request.dump(), "application/json");
        if (!response || response->status != 200) return {};
        const auto result = Json::parse(response->body, nullptr, false);
        if (!result.is_object()) return {};
        const auto state = result.value("state", "");
        if (state == "accepted") return {ChannelDelivery::State::accepted, result.value("externalMessageId", "")};
        if (state == "not_sent") return {ChannelDelivery::State::notSent, {}};
        if (state == "rejected") return {ChannelDelivery::State::rejected, {}};
        return {};
    }

    std::shared_ptr<const Workspaces> workspaces_;
};

std::optional<ChannelConnector> connector(ChannelKind kind, const Json& entries, std::vector<std::string>& credentials) {
    if (!entries.is_object()) throw Conflict("Channel workspaces must be an object");
    if (entries.empty()) return std::nullopt;
    auto workspaces = std::make_shared<Workspaces>();
    for (const auto& [id, settings] : entries.items()) {
        const auto& configuredPort = settings.at("bridgePort");
        const auto port = configuredPort.is_number_integer() ? configuredPort.get<std::uint64_t>() : 0;
        if (!port || port > 65535) throw Conflict("A channel workspace needs a valid local bridge port");
        if (id.empty() || id.size() > 128 || id.find('\0') != id.npos) throw Conflict("Invalid channel workspace ID");
        Workspace workspace{static_cast<unsigned>(port), bridgeCredential(settings.at("credential"), credentials), {}};
        const auto& links = settings.at("senders");
        if (!links.is_object()) throw Conflict("Channel senders must be an object");
        for (const auto& [sender, link] : links.items()) {
            ChannelSender identity{link.at("botId"), link.at("personId")};
            if (sender.empty() || sender.size() > 200 || sender.find('\0') != sender.npos || !validBotId(identity.botId) ||
                identity.personId.empty())
                throw Conflict("Invalid channel sender link");
            workspace.senders.emplace(sender, std::move(identity));
        }
        workspaces->emplace(id, std::move(workspace));
    }
    auto authenticate = [workspaces](std::string_view credential, std::string_view workspace) {
        const auto selected = workspaces->find(workspace);
        return selected != workspaces->end() && sameCredential(credential, selected->second.credential);
    };
    auto resolveSender = [workspaces](std::string_view workspace, std::string_view sender) -> std::optional<ChannelSender> {
        const auto selected = workspaces->find(workspace);
        if (selected == workspaces->end()) return std::nullopt;
        const auto identity = selected->second.senders.find(sender);
        if (identity == selected->second.senders.end()) return std::nullopt;
        return identity->second;
    };
    return ChannelConnector{kind, channelCapabilities(kind), std::move(authenticate), std::move(resolveSender),
                            std::make_shared<BridgeTransport>(workspaces)};
}

struct ChannelSetup {
    std::optional<ChannelConnector> slack, imessage;
    std::string artifactBaseUrl, sessionAuthorization;
    std::map<std::string, std::string> machines;
};

ChannelSetup channelSetup(const Json& config) {
    if (!config.is_object()) throw Conflict("Invalid channel bridge configuration");
    try {
        std::vector<std::string> credentials;
        ChannelSetup result;
        result.slack = connector(ChannelKind::slack, config.value("slack", Json::object()), credentials);
        result.imessage = connector(ChannelKind::imessage, config.value("imessage", Json::object()), credentials);
        result.artifactBaseUrl = normalizeArtifactBaseUrl(config.value("artifactBaseUrl", ""));
        const auto machines = config.value("localMachines", Json::object());
        if (!machines.is_object()) throw Conflict("Invalid local machine list");
        for (const auto& [id, owner] : machines.items()) {
            const auto person = owner.get<std::string>();
            if (id.empty() || id.size() > 128 || id.find('\0') != id.npos || person.empty()) throw Conflict("Invalid local machine");
            result.machines.emplace(id, person);
        }
        const auto sessionCredential = config.value("sessionCredential", "");
        if (!sessionCredential.empty()) result.sessionAuthorization = bridgeCredential(sessionCredential, credentials);
        return result;
    } catch (const Json::exception&) {
        throw Conflict("Invalid channel bridge configuration");
    }
}

std::string externalCredential(const Json& name) {
    const auto key = name.get<std::string>();
    const auto value = std::getenv(key.c_str());
    if (!value || !*value) throw Conflict("A bridge credential is missing");
    return value;
}
}

Json readChannelBridgeSettings(const std::filesystem::path& file) {
    std::ifstream source(file);
    if (!source) throw Conflict("Cannot read channel bridge configuration");
    auto config = Json::parse(source, nullptr, false);
    if (!config.is_object()) throw Conflict("Invalid channel bridge configuration");
    try {
        for (const auto* kind : {"slack", "imessage"}) {
            auto entries = config.value(kind, Json::object());
            if (!entries.is_object()) throw Conflict("Channel workspaces must be an object");
            for (auto& settings : entries) {
                settings["credential"] = externalCredential(settings.at("credentialEnv"));
                settings.erase("credentialEnv");
            }
            config[kind] = std::move(entries);
        }
        if (config.contains("sessionBridgeCredentialEnv")) {
            config["sessionCredential"] = externalCredential(config.at("sessionBridgeCredentialEnv"));
            config.erase("sessionBridgeCredentialEnv");
        }
        return config;
    } catch (const Json::exception&) {
        throw Conflict("Invalid channel bridge configuration");
    }
}

void validateChannelBridgeSettings(const Json& config) {
    (void)channelSetup(config);
}

void configureManagedChannelBridges(Store& store, const Json& config, WebCallbacks& callbacks) {
    auto setup = channelSetup(config);
    if (setup.slack) Channels(store).connectChannel(std::move(*setup.slack));
    if (setup.imessage) Channels(store).connectChannel(std::move(*setup.imessage));
    Channels(store).artifactBaseUrl(std::move(setup.artifactBaseUrl));
    for (auto& [id, person] : setup.machines) LocalMachines(store).connectLocalMachine(id, hostLocalMachine(std::move(person)));
    if (!setup.sessionAuthorization.empty()) {
        callbacks.authenticateSessionBridge = [credential = std::move(setup.sessionAuthorization)](std::string_view supplied) {
            return sameCredential(supplied, credential);
        };
    }
}

void configureChannelBridges(Store& store, const std::filesystem::path& file, WebCallbacks& callbacks) {
    configureManagedChannelBridges(store, readChannelBridgeSettings(file), callbacks);
}
}
