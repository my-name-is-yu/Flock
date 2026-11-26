#pragma once

#include <string_view>
#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {
class Policy;

class Database;
class SessionParticipationStorage;
class TaskRouting;
class T3;
struct Decision;
struct Person;

class BotSettingsStorage {
public:
    BotSettingsStorage(Database& database, Policy& permissions, SessionParticipationStorage& sessionParticipationStorage,
                       TaskRouting& taskRouting)
        : database(database), permissions(permissions), sessionParticipationStorage(sessionParticipationStorage), taskRouting(taskRouting) {
    }

    Json botSettingsTool(std::string_view bot, const Person& actor, Id sourceMessage, const Json& routing, const Decision&);
    Json readiness(std::string_view bot, const Json& routing, const Json& args, T3&);
    Seconds environmentTimeout(std::string_view bot, std::string_view environment, const Json& config) const;
private:
    Database& database;
    Policy& permissions;
    SessionParticipationStorage& sessionParticipationStorage;
    TaskRouting& taskRouting;
};
}
