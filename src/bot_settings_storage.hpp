#pragma once

#include <string_view>
#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {
class Policy;
class SecretRequests;

class Database;
class SessionParticipationStorage;
class TaskRouting;
class T3;
struct Decision;

class BotSettingsStorage {
public:
    BotSettingsStorage(Database& database, Policy& permissions, SecretRequests& secretRequests,
                       SessionParticipationStorage& sessionParticipationStorage, TaskRouting& taskRouting)
        : database(database), permissions(permissions), secretRequests(secretRequests),
          sessionParticipationStorage(sessionParticipationStorage), taskRouting(taskRouting) {}

    Json botSettingsTool(std::string_view bot, Id sourceMessage, const Json& routing, const Decision&);
    Json readiness(std::string_view bot, const Json& routing, const Json& args, T3&);
private:
    Database& database;
    Policy& permissions;
    SecretRequests& secretRequests;
    SessionParticipationStorage& sessionParticipationStorage;
    TaskRouting& taskRouting;
};
}
