#pragma once

#include "flock/channels.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class ConversationMessages;

class ChannelConnectors;
class ChannelNotices;
class Database;
class Policy;
struct Decision;
struct ModelImage;
struct Person;
struct Resource;

struct AdmittedChannelMessage {
    std::string botId;
    Id replyTo = 0;
    std::string text;
    VerifiedChannelMessage message;
};

class PolicyApprovals;
class ChannelStorage {
public:
    ChannelStorage(Database& database, const std::filesystem::path& path, const Policy& permissions,
                   ConversationMessages& conversationMessages, PolicyApprovals& policyApprovals, ChannelConnectors& connectors,
                   ChannelNotices& notices)
        : database(database), path(path), permissions(permissions), conversationMessages(conversationMessages),
          policyApprovals(policyApprovals), connectors(connectors), notices(notices) {}

    void connectChannel(ChannelConnector);
    void artifactBaseUrl(std::string url);
    // Checks that the bridge and the sender may reach the Bot, and decodes what they sent.
    AdmittedChannelMessage admitChannelMessage(ChannelKind, std::string_view credential, const Json& event);
    std::optional<ChannelResource> artifact(std::string_view id) const;
    std::optional<ChannelResource> attachment(const Person&, Id id) const;
    bool hasConnectors() const;
    void saveChannelMessage(Id, std::string_view bot, const VerifiedChannelMessage&, bool replay);
    Json channelContext(Id, std::string_view bot, std::vector<ModelImage>* images = nullptr, std::string_view viewer = {}) const;
    Json channelTool(Id, std::string_view bot, const Decision&);
    std::optional<Resource> channelToolPolicy(Id, std::string_view tool);
    bool channelDeliveryDue() const;
    void deliverChannels(std::stop_token = {});
    void dropPausedBotDeliveries(std::string_view bot);
    Json channelToolStatus(Id source, std::string_view bot, Json, std::vector<ModelImage>* images = nullptr) const;
private:
    std::optional<ChannelResource> channelAttachment(Id source, Id attachment, std::string_view bot) const;

    Database& database;
    const std::filesystem::path& path;
    const Policy& permissions;
    ConversationMessages& conversationMessages;
    PolicyApprovals& policyApprovals;
    ChannelConnectors& connectors;
    ChannelNotices& notices;
    std::string artifactOrigin;
    std::mutex deliveryMutex;
};
}
