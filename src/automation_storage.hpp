#pragma once

#include "flock/json.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <string_view>
#include <vector>

namespace flockv0 {
class BotLifecycleStorage;
class Database;
class SecretRedactor;
class SecretStorage;
struct ConfiguredSchedule;
struct Decision;
struct Intake;
struct Person;
struct ScheduleImportError;

class TaskOperations;
class AutomationStorage {
public:
    AutomationStorage(Database&, const SecretStorage&, const BotLifecycleStorage&, Store::Impl&, Store::AutomationModelResolver,
                      TaskOperations&);

    void retireAutomations(const Json& botIds);
    Json automationTool(std::string_view bot, const Person&, const Json& routing, const Decision&, Seconds now);
    Json createAutomation(const Intake&, const Json&, std::string_view author, Seconds now);
    std::vector<ScheduleImportError> stageConfiguredSchedules(std::string_view bot, const std::vector<ConfiguredSchedule>&, Seconds now);
    Json automations(std::string_view bot, Id before, bool activeOnly = false) const;
    Json cancelAutomation(std::string_view bot, Id id);
    void fireDueAutomations(Seconds now);
    void skipDueAutomations(std::string_view bot, Seconds now);
    Json automationRoute(std::string_view bot, Json routing) const;
    void refreshPlannerModel(std::string_view bot, Json& config) const;
private:
    void syncSchedule(const ConfiguredSchedule&, Seconds now, const SecretRedactor&);
    std::vector<Id> dueAutomations(Seconds now, std::string_view bot = {}) const;
    void processDueAutomation(Id, Seconds now, bool skip = false);

    Database& database_;
    const SecretStorage& secretStorage_;
    const BotLifecycleStorage& lifecycle_;
    Store::Impl& remaining_;
    const Store::AutomationModelResolver model_;
    TaskOperations& taskOperations;
};
}
