#pragma once

#include "flock/channels.hpp"
#include "flock/types.hpp"

#include <map>
#include <string_view>

namespace flockv0 {
class ChannelDeliveryQueue;
class Database;
class JournalStorage;
class Policy;
struct WebIdentityState;

// The connected channel bridges, and whether a sender may still reach a Bot through one.
class ChannelConnectors {
public:
    ChannelConnectors(Database& database, const Policy& permissions, const WebIdentityState& webIdentity)
        : database(database), permissions(permissions), webIdentity(webIdentity) {}

    void connect(ChannelConnector);
    const ChannelConnector* find(ChannelKind) const;
    const ChannelConnector& at(ChannelKind kind) const { return connectors.at(kind); }
    bool empty() const { return connectors.empty(); }
    bool permits(std::string_view bot, const ChannelOrigin&, std::string_view sender, std::string_view person,
                 std::string_view operation) const;
    bool messageAuthorized(Id message, std::string_view bot) const;
private:
    Database& database;
    const Policy& permissions;
    const WebIdentityState& webIdentity;
    std::map<ChannelKind, ChannelConnector> connectors;
};

// Tells the channel a message came from why Flock did not act on it.
class ChannelNotices {
public:
    ChannelNotices(Database& database, const ChannelConnectors& connectors, const Policy& permissions, JournalStorage& journalStorage,
                   ChannelDeliveryQueue& channelDeliveries)
        : database(database), connectors(connectors), permissions(permissions), journalStorage(journalStorage),
          channelDeliveries(channelDeliveries) {}

    void limitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies);
    void approvalWaiting(Id source);
private:
    void post(Id source, std::string_view bot, std::string_view reason, bool supportsReplies, std::string_view followUp = {});

    Database& database;
    const ChannelConnectors& connectors;
    const Policy& permissions;
    JournalStorage& journalStorage;
    ChannelDeliveryQueue& channelDeliveries;
};
}
