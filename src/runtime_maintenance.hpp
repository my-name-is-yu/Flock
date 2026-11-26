#pragma once

#include "runtime_components.hpp"
#include "database.hpp"
#include "flock/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0 {
class Database;

class AutomationStorage;
class WorkflowStorage;
class PolicyApprovals;

class runtime::Maintenance {
    using ErrorReporter = std::function<void(std::string_view)>;
public:
    Maintenance(Database&, AutomationStorage&, WorkflowStorage&, PolicyApprovals&, const std::function<Seconds()>& clock,
                const ErrorReporter& automationError, const ErrorReporter& storageContention);

    Seconds settleApprovals();
    std::optional<Seconds> fireDueAutomations();
    Seconds advanceWorkflows();
    void settleApprovalsIfChanged();
    void fireDueAutomationsIfChanged();
    void advanceWorkflowsIfChanged();

    void deferContendedWork(std::string_view operation, const auto& work) {
        try {
            work();
        } catch (const StorageError& error) {
            if (!error.contended()) throw;
            if (storageContention) storageContention(std::string(operation) + ": " + error.what());
        }
    }
private:
    struct ScanStamp {
        std::pair<Id, Id> stamp;
        Seconds second;
        bool operator==(const ScanStamp&) const = default;
    };

    void scanIfChanged(std::optional<ScanStamp>& completed, std::string_view operation, const auto& work);

    Database& database;
    AutomationStorage& automationStorage;
    WorkflowStorage& workflowStorage;
    PolicyApprovals& approvals;
    const std::function<Seconds()>& clock;
    const ErrorReporter& automationError;
    const ErrorReporter& storageContention;
    std::optional<ScanStamp> approvalsCheckedAt, automationsCheckedAt, workflowsCheckedAt;
};
}
