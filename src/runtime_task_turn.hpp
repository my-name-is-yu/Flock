#pragma once

#include "runtime_components.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "flock/intake.hpp"
#include "flock/soul_loader.hpp"

#include <optional>
#include <stop_token>

#include <nlohmann/json.hpp>

namespace flockv0 {
class BotSetupStorage;
class Database;
class MemoryStorage;
struct Wait;
class TaskEvents;
class TaskRouting;
class T3;
class WaitStorage;

// Moves a task forward when its current event needs a step. Every step is deterministic: no model is called here.
class runtime::TaskTurn {
public:
    TaskTurn(Database& database, TaskEvents& taskEvents, TaskRouting& taskRouting, WaitStorage& waitStorage, TaskUpdates& updates,
             RunObserver& runs, TaskDecisions& decisions, BotSetupStorage& botSetup, MemoryStorage& memoryStorage, const SoulLoader& soul,
             const IntakeResolver& intake, T3&, const std::function<Seconds()>& clock, std::stop_token stop, bool requireT3Connection);

    void decide(const TaskData& task);
    // Returns true when the task waits for its Bot's notes, retrying them once they can be read again.
    bool resumeNotes(const TaskData& task, const Wait& wait);
    std::optional<Json> launchDefaults(const TaskData& task) const;
private:
    bool resolveProject(const TaskData& task, const Json& event);

    Database& database;
    TaskEvents& taskEvents;
    TaskRouting& taskRouting;
    WaitStorage& waitStorage;
    TaskUpdates& updates;
    RunObserver& runs;
    TaskDecisions& decisions;
    BotSetupStorage& botSetup;
    MemoryStorage& memoryStorage;
    const SoulLoader& soul;
    const IntakeResolver& intake;
    T3& t3;
    const std::function<Seconds()>& clock;
    std::stop_token stop;
    bool requireT3Connection;
};
}
