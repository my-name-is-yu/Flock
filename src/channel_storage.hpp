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
class ConversationStorage;
class ConversationMessages;

class ChannelDeliveryQueue;
class Database;
class JournalStorage;
class Policy;
struct Decision;
struct ModelImage;
struct Person;
struct Resource;
struct WebIdentityState;

class PolicyApprovals;
class ChannelStorage {
public:
    ChannelStorage(Database& database, const std::filesystem::path& path, const Policy& permissions, const WebIdentityState& webIdentity,
                   JournalStorage& journalStorage, ChannelDeliveryQueue& channelDeliveries, ConversationStorage& conversationTurns,
                   ConversationMessages& conversationMessages, PolicyApprovals& policyApprovals)
        : database(database), path(path), permissions(permissions), webIdentity(webIdentity), journalStorage(journalStorage),
          channelDeliveries(channelDeliveries), conversationTurns(conversationTurns), conversationMessages(conversationMessages),
          policyApprovals(policyApprovals) {}

    void connectChannel(ChannelConnector);
    void artifactBaseUrl(std::string url);
    Json receiveChannel(ChannelKind, std::string_view credential, const Json& event, const IntakeResolver& routing);
    std::optional<ChannelResource> artifact(std::string_view id) const;
    std::optional<ChannelResource> attachment(const Person&, Id id) const;
    bool hasConnectors() const;
    void saveChannelMessage(Id, std::string_view bot, const VerifiedChannelMessage&, bool replay);
    bool channelMessageAuthorized(Id, std::string_view bot) const;
    Json channelContext(Id, std::string_view bot, std::vector<ModelImage>* images = nullptr, std::string_view viewer = {}) const;
    Json channelTool(Id, std::string_view bot, const Decision&);
    void channelApprovalNotice(Id source);
    std::optional<Resource> channelToolPolicy(Id, std::string_view tool);
    bool channelDeliveryDue() const;
    void deliverChannels(std::stop_token = {});
    void dropPausedBotDeliveries(std::string_view bot);
    Json channelToolStatus(Id source, std::string_view bot, Json, std::vector<ModelImage>* images = nullptr) const;
private:
    std::optional<ChannelResource> channelAttachment(Id source, Id attachment, std::string_view bot) const;
    void channelNotice(Id source, std::string_view bot, std::string_view reason, bool supportsReplies, std::string_view followUp = {});
    void channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies);
    bool permitsChannel(std::string_view bot, const ChannelOrigin&, std::string_view sender, std::string_view person,
                        std::string_view operation) const;

    Database& database;
    const std::filesystem::path& path;
    const Policy& permissions;
    const WebIdentityState& webIdentity;
    JournalStorage& journalStorage;
    ChannelDeliveryQueue& channelDeliveries;
    ConversationStorage& conversationTurns;
    ConversationMessages& conversationMessages;
    PolicyApprovals& policyApprovals;
    std::map<ChannelKind, ChannelConnector> connectors;
    std::string artifactOrigin;
    std::mutex deliveryMutex;
};
}
