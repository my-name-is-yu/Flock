#include "flock/web.hpp"
#include "flock/channels.hpp"

#include <httplib.h>

#include <cstdlib>
#include <fstream>
#include <map>

namespace flockv0 {
namespace {
struct Workspace {
    unsigned port;
    std::string credential;
    std::map<std::string,ChannelSender,std::less<>> senders;
};
using Workspaces = std::map<std::string,Workspace,std::less<>>;

bool sameCredential(std::string_view supplied, std::string_view expected) {
    if (supplied.size() != expected.size()) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(supplied[i] ^ expected[i]);
    return difference == 0;
}

class BridgeTransport final : public ChannelTransport {
public:
    explicit BridgeTransport(std::shared_ptr<const Workspaces> workspaces) : workspaces_(std::move(workspaces)) {}
    ChannelDelivery send(std::string_view key, const ChannelOrigin& origin, std::string_view operation,
                         const Json& payload) override {
        return call("/deliver",key,origin,{{"operation",operation},{"payload",payload}});
    }
    ChannelDelivery reconcile(std::string_view key, const ChannelOrigin& origin) override {
        return call("/reconcile",key,origin,Json::object());
    }
private:
    ChannelDelivery call(const char* path, std::string_view key, const ChannelOrigin& origin, Json request) const {
        const auto workspace = workspaces_->find(origin.workspaceId);
        if (workspace == workspaces_->end()) return {ChannelDelivery::State::rejected,{}};
        request["idempotencyKey"] = key;
        request["origin"] = origin.json();
        httplib::Client client("127.0.0.1",static_cast<int>(workspace->second.port));
        client.set_connection_timeout(2);
        client.set_read_timeout(5);
        client.set_write_timeout(5);
        const auto response = client.Post(path,{{"Authorization",workspace->second.credential}},request.dump(),"application/json");
        if (!response || response->status != 200) return {};
        const auto result = Json::parse(response->body,nullptr,false);
        if (!result.is_object()) return {};
        const auto state = result.value("state","");
        if (state == "accepted") return {ChannelDelivery::State::accepted,result.value("externalMessageId","")};
        if (state == "not_sent") return {ChannelDelivery::State::notSent,{}};
        if (state == "rejected") return {ChannelDelivery::State::rejected,{}};
        return {};
    }

    std::shared_ptr<const Workspaces> workspaces_;
};

void connect(Store& store, ChannelKind kind, const Json& entries) {
    if (!entries.is_object()) throw Conflict("Channel workspaces must be an object");
    if (entries.empty()) return;
    auto workspaces = std::make_shared<Workspaces>();
    for (const auto& [id,settings] : entries.items()) {
        const auto port = settings.at("bridgePort").get<unsigned>();
        const auto key = settings.at("credentialEnv").get<std::string>();
        const auto secret = std::getenv(key.c_str());
        if (!port || port > 65535 || !secret || std::string_view(secret).size() < 32 ||
            std::string_view(secret).find_first_of(" \r\n\t") != std::string_view::npos)
            throw Conflict("A channel workspace needs a local bridge port and a credential of at least 32 characters");
        Workspace workspace{port,"Bearer " + std::string(secret),{}};
        const auto& links = settings.at("senders");
        if (!links.is_object()) throw Conflict("Channel senders must be an object");
        for (const auto& [sender,link] : links.items()) {
            ChannelSender identity{link.at("botId"),link.at("personId")};
            if (!validBotId(identity.botId) || identity.personId.empty()) throw Conflict("Invalid channel sender link");
            workspace.senders.emplace(sender,std::move(identity));
        }
        workspaces->emplace(id,std::move(workspace));
    }
    auto authenticate = [workspaces](std::string_view credential, std::string_view workspace, std::string_view sender)
        -> std::optional<ChannelSender> {
        const auto selected = workspaces->find(workspace);
        if (selected == workspaces->end() || !sameCredential(credential,selected->second.credential)) return std::nullopt;
        const auto identity = selected->second.senders.find(sender);
        if (identity == selected->second.senders.end()) return std::nullopt;
        return identity->second;
    };
    auto transport = std::make_shared<BridgeTransport>(workspaces);
    if (kind == ChannelKind::slack) store.connectChannel(std::make_shared<SlackConnector>(authenticate,transport));
    else store.connectChannel(std::make_shared<IMessageConnector>(authenticate,transport));
}
}

void configureChannelBridges(Store& store, const std::filesystem::path& file) {
    std::ifstream source(file);
    if (!source) throw Conflict("Cannot read channel bridge configuration");
    const auto config = Json::parse(source,nullptr,false);
    if (!config.is_object()) throw Conflict("Invalid channel bridge configuration");
    try {
        connect(store,ChannelKind::slack,config.value("slack",Json::object()));
        connect(store,ChannelKind::imessage,config.value("imessage",Json::object()));
        store.artifactBaseUrl(config.value("artifactBaseUrl",""));
        const auto machines = config.value("localMachines",Json::object());
        if (!machines.is_object()) throw Conflict("Local machines must be an object");
        for (const auto& [id,owner] : machines.items()) store.connectLocalMachine(id,hostLocalMachine(owner.get<std::string>()));
    } catch (const Json::exception&) { throw Conflict("Invalid channel bridge configuration"); }
}
}
