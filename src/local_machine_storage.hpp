#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Database;
class LocalMachine;
class Policy;
class SecretStorage;
class TaskRecords;
struct Decision;
struct Person;
struct RunRef;
struct WebIdentityState;

class LocalMachineStorage {
public:
    LocalMachineStorage(Database& database, const Policy& permissions, const SecretStorage& secretStorage,
                        const WebIdentityState& webIdentity, const TaskRecords& taskRecords)
        : database(database), permissions(permissions), secretStorage(secretStorage), webIdentity(webIdentity), taskRecords(taskRecords) {}

    void setRootPeople(std::vector<std::string> ids);
    void setWebOrigin(std::string origin);
    void connectLocalMachine(std::string id, std::shared_ptr<LocalMachine>);
    Json localConnections(const Person&) const;
    void decideLocalConnection(const Person&, Id id, bool approve);
    void disconnectLocalConnection(const Person&, Id id);
    Json runLocalMachineOperation(std::string_view environment, const RunRef& authenticatedRun, std::string_view machineId,
                                  std::string_view operation, const std::filesystem::path& path);
    bool hasMachines() const;
    Json availableMachines(std::string_view bot) const;
    Json localMachineTool(std::string_view bot, std::string_view scope, const Decision&);
private:
    bool permitsLocalMachineOwner(std::string_view owner, std::string_view bot) const;

    Database& database;
    const Policy& permissions;
    const SecretStorage& secretStorage;
    const WebIdentityState& webIdentity;
    const TaskRecords& taskRecords;
    std::map<std::string, std::shared_ptr<LocalMachine>, std::less<>> localMachines;
    std::vector<std::string> rootPeople;
    std::string webOrigin;
};
}
