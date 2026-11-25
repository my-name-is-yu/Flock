#pragma once

#include "runtime_components.hpp"
#include "storage.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/runtime.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0 {
class Runtime::Components::Maintenance {
    using ErrorReporter = std::function<void(std::string_view)>;
public:
    Maintenance(Store::Impl&, const std::function<Seconds()>& clock, const ErrorReporter& automationError,
                const ErrorReporter& storageContention);

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

    Store::Impl& db;
    const std::function<Seconds()>& clock;
    const ErrorReporter& automationError;
    const ErrorReporter& storageContention;
    std::optional<ScanStamp> approvalsCheckedAt, automationsCheckedAt, workflowsCheckedAt;
};
}
