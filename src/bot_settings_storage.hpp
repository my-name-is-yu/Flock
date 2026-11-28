#pragma once

#include <string_view>
#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {
class Policy;
class SecretRequests;

class Database;
class TaskRouting;
class T3;
struct Decision;
struct BotTools;
namespace conversation {
struct Message;
}

class BotSettingsStorage {
public:
    BotSettingsStorage(Database& database, Policy& permissions, SecretRequests& secretRequests, TaskRouting& taskRouting)
        : database(database), permissions(permissions), secretRequests(secretRequests), taskRouting(taskRouting) {}

    Json botSettingsTool(std::string_view bot, Id sourceMessage, const BotTools&, const Decision&);
    // The message is only handed on to discover linked destinations; its Bot and routing are passed beside it so this
    // header does not need the conversation types.
    Json readiness(const conversation::Message&, std::string_view bot, const Json& routing, const Json& args, T3&);
private:
    Database& database;
    Policy& permissions;
    SecretRequests& secretRequests;
    TaskRouting& taskRouting;
};
}
