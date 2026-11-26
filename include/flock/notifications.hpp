#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <cstddef>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace flockv0 {
class Store;
class Database;
class SecretStorage;
struct Person;
class ChannelStorage;
class Policy;

class Notifications {
public:
    explicit Notifications(Store&);

    struct BotViewStamp {
        std::pair<Id, Id> database;
        std::size_t gitCredentialCount;
        bool operator==(const BotViewStamp&) const = default;
    };
    // No reusable version when the view can depend on live channel permissions or vault files.
    std::optional<BotViewStamp> botViewStamp() const;
    struct NotificationEvents {
        Id through;
        Json events;
        std::map<std::string, Id> scope;
        bool more;
    };
    NotificationEvents notificationEventsSince(const Person&, const std::vector<std::string>& bots, std::optional<Id> after,
                                               const std::map<std::string, Id>& previousScope) const;
private:
    Database& database_;
    const SecretStorage& secrets_;
    const ChannelStorage& channels_;
    const Policy& permissions_;
};
}
