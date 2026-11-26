#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace flockv0 {
class Store;
class LocalMachine;
class LocalMachineStorage;
struct Person;
struct RunRef;

class LocalMachines {
public:
    explicit LocalMachines(Store&);

    void connectLocalMachine(std::string id, std::shared_ptr<LocalMachine>);
    Json localConnections(const Person&) const;
    void decideLocalConnection(const Person&, Id id, bool approve);
    void disconnectLocalConnection(const Person&, Id id);
    Json runLocalMachineOperation(std::string_view environment, const RunRef& authenticatedRun, std::string_view machineId,
                                  std::string_view operation, const std::filesystem::path& path);
private:
    LocalMachineStorage& machines_;
};
}
