#pragma once

#include "flock/person.hpp"
#include "flock/types.hpp"

#include <string_view>

namespace flockv0 {
class Database;
class Policy;
struct WebIdentityState;
namespace conversation {
struct Message;
}

// Reads conversation messages and resolves who a message may act as now.
class ConversationMessages {
public:
    ConversationMessages(Database& database, const Policy& permissions, const WebIdentityState& webIdentity)
        : database(database), permissions(permissions), webIdentity(webIdentity) {}

    conversation::Message load(Id messageId) const;
    Person actor(const conversation::Message&) const;
    Person sessionActor(std::string_view person) const;
private:
    Database& database;
    const Policy& permissions;
    const WebIdentityState& webIdentity;
};
}
