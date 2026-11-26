#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

namespace flockv0 {

class ChannelStorage;
class Database;
class Policy;
class BotLifecycleStorage;
class JournalStorage;
class SecretStorage;
class TaskRecords;
class WaitStorage;
class Model;
class SecretRedactor;
struct ModelInput;
struct Person;
namespace conversation {
struct Message;
}

struct ModelBudget {
    std::string id;
    Id initialLimit;
};

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
    ModelCalls(Database& database, BotLifecycleStorage& botLifecycle, const Policy& permissions, JournalStorage& journalStorage,
               SecretStorage& secretStorage, TaskRecords& taskRecords, WaitStorage& waitStorage, ChannelStorage& channelStorage)
        : database(database), botLifecycle(botLifecycle), permissions(permissions), journalStorage(journalStorage),
          secretStorage(secretStorage), taskRecords(taskRecords), waitStorage(waitStorage), channelStorage(channelStorage) {}

    void approveMessageModelCall(const Person&, std::string_view bot, Id message, Id pause);
    ModelBudget messageModelBudget(Id messageId) const;
    ModelBudget taskModelBudget(Id taskId) const;
    Id reserveModelCall(const ModelBudget&, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    Id takeReservedConversationCall(Id messageId);
    Id takeReservedTaskCall(Id event);
    void releaseModelCall(Id attempt);
    Id approveModelBudget(const Person&, const ModelBudget&, std::string_view purpose, Id messageId, Id taskId, Id eventId);
    void resumeSharedModelBudgetWaits(const Person&, std::string_view budget, Id approvedMessage, Id approvedTask);
    std::vector<Id> taskWaitsCoveredByConversationApproval(std::string_view bot, const ModelBudget&) const;
    void resumeMessageModelCall(std::string_view bot, Id message, Id pause, Id attempt);
    SecretRedactor modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args, bool gitNotification) const;
    ModelCall checkModelCall(std::string_view bot, Decision, const std::vector<std::string>& offered, bool gitNotification);
    CheckedModelTurn callModel(std::string_view bot, Model&, const ModelInput&);
    void pauseTurn(const conversation::Message&);
private:
    Database& database;
    BotLifecycleStorage& botLifecycle;
    const Policy& permissions;
    JournalStorage& journalStorage;
    SecretStorage& secretStorage;
    TaskRecords& taskRecords;
    WaitStorage& waitStorage;
    ChannelStorage& channelStorage;
};
}
