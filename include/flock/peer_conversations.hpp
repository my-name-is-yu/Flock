#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
class Store;
class Database;

class PeerConversations {
public:
    explicit PeerConversations(Store&);

    // Same chronological page for either participant.
    Json peerConversation(std::string_view bot, std::string_view peer, Id before = 0, unsigned limit = 50, Id from = 0,
                          std::string_view viewer = {}) const;
    Id peerMessageAnchor(std::string_view bot, std::string_view peer, Id event) const;
    std::vector<std::pair<std::string, std::string>> peerExchangePairs() const;
    bool hasPeerExchange(std::string_view bot, std::string_view peer) const;
private:
    Database& database_;
};
}
