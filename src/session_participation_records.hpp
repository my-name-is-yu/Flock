#pragma once

#include "flock/types.hpp"

#include <string_view>

namespace flockv0 {
class Database;
struct SessionTarget;
namespace conversation {
struct Message;
}

// Reads whether a Bot still takes part in a T3 Session.
class SessionParticipationRecords {
public:
    explicit SessionParticipationRecords(Database& database) : database(database) {}

    bool activeOrigin(const conversation::Message&) const;
    bool activeOrigin(std::string_view bot, std::string_view person, const SessionTarget& origin, std::string_view destinationEnvironment,
                      Id message) const;
    bool stopped(std::string_view bot, const SessionTarget&, Id message) const;
private:
    Database& database;
};
}
