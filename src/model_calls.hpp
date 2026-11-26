#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class Database;
class Policy;
class BotLifecycleStorage;
class SecretStorage;
class Model;
class SecretRedactor;
struct ModelInput;

bool modelArgumentsContainProtected(const SecretRedactor&, std::string_view tool, const Json& args);

struct ModelCall : Decision {
    std::string rejection;
};

struct CheckedModelTurn {
    std::vector<ModelCall> calls;
    Json usage;
};

class ModelCalls {
public:
    ModelCalls(Database& database, BotLifecycleStorage& botLifecycle, const Policy& permissions, SecretStorage& secretStorage)
        : database(database), botLifecycle(botLifecycle), permissions(permissions), secretStorage(secretStorage) {}

    std::string messageModelScope(Id messageId) const;
    std::string taskModelScope(Id taskId) const;
    Id recordAttempt(std::string_view scope, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    void recordBotTool(std::string_view scope, Id messageId);
    Id takeReservedConversationCall(Id messageId);
    void releaseModelCall(Id attempt);
    SecretRedactor modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args, bool gitNotification) const;
    ModelCall checkModelCall(std::string_view bot, Decision, const std::vector<std::string>& offered, bool gitNotification);
    CheckedModelTurn callModel(std::string_view bot, Model&, const ModelInput&);
private:
    Database& database;
    BotLifecycleStorage& botLifecycle;
    const Policy& permissions;
    SecretStorage& secretStorage;
};
}
