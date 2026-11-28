#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <span>
#include <string>
#include <string_view>

namespace flockv0 {
class Database;
class Policy;

// Queues text for delivery to the channel a conversation message came from.
class ChannelDeliveryQueue {
public:
    ChannelDeliveryQueue(Database& database, const Policy& permissions) : database(database), permissions(permissions) {}

    void reply(Id source, Id reply, std::string_view text, const Json& suggestions);
    void taskUpdate(Id task, Id entry, std::string_view text, std::span<const std::string> suggestions = {});
private:
    Database& database;
    const Policy& permissions;
};
}
