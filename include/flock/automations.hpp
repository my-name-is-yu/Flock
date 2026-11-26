#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;
class AutomationStorage;
class BotLifecycleStorage;
class Database;
class Policy;
class SecretStorage;
class TaskOperations;
struct ConfiguredSchedule;
struct Intake;
struct Person;
struct ScheduleImportError;

class Automations {
public:
    explicit Automations(Store&);

    using CommitConfiguredSchedules = std::function<void(const std::vector<ConfiguredSchedule>&, const std::function<void()>&)>;
    using EditConfiguredSchedules = std::function<void(const CommitConfiguredSchedules&)>;
    Json createAutomation(const Person&, const Intake&, const Json& args, Seconds now);
    Json automations(const Person&, std::string_view bot, Id before = 0, bool activeOnly = false) const;
    Json activeConfiguredSchedules(std::string_view bot) const;
    Json cancelAutomation(const Person&, std::string_view bot, Id id);
    [[nodiscard]] std::vector<ScheduleImportError> syncSchedules(std::string_view bot, const std::vector<ConfiguredSchedule>&, Seconds now);
    [[nodiscard]] std::vector<ScheduleImportError> syncSchedules(std::string_view bot, Seconds now, const EditConfiguredSchedules&);
    void pruneAutomations(const std::vector<std::string>& configuredBots);
    void fireDueAutomations(Seconds now);
    Json receiveWebhook(const Person&, std::string_view source, std::string_view externalEventId, const Json& event, Seconds now);
private:
    Database& database_;
    AutomationStorage& automation_;
    const BotLifecycleStorage& lifecycle_;
    const SecretStorage& secrets_;
    TaskOperations& tasks_;
    const Policy& policy_;
};
}
