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
class WaitStorage;

// Moves a task forward when its current event needs a step. Every step is deterministic: no model is called here.
class runtime::TaskTurn {
public:
    TaskTurn(Database& database, TaskEvents& taskEvents, TaskRouting& taskRouting, WaitStorage& waitStorage, TaskUpdates& updates,
             RunObserver& runs, TaskDecisions& decisions, TaskScheduler& scheduler, BotSetupStorage& botSetup, MemoryStorage& memoryStorage,
             const SoulLoader& soul, const IntakeResolver& intake, const std::function<Seconds()>& clock, std::stop_token stop);

    void decide(const TaskData& task);
    // Returns true when the task waits for access to its Bot profile or notes.
    bool resumeNotes(const TaskData& task, const Wait& wait);
private:
    std::optional<Json> launchDefaults(const TaskData& task) const;

    Database& database;
    TaskEvents& taskEvents;
    TaskRouting& taskRouting;
    WaitStorage& waitStorage;
    TaskUpdates& updates;
    RunObserver& runs;
    TaskDecisions& decisions;
    TaskScheduler& scheduler;
    BotSetupStorage& botSetup;
    MemoryStorage& memoryStorage;
    const SoulLoader& soul;
    const IntakeResolver& intake;
    const std::function<Seconds()>& clock;
    std::stop_token stop;
};
}
