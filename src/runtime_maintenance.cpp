#include "runtime_maintenance.hpp"

namespace flockv0 {
Runtime::Maintenance::Maintenance(Store::Impl& db, const std::function<Seconds()>& clock, const ErrorReporter& automationError,
                                  const ErrorReporter& storageContention)
    : db(db), clock(clock), automationError(automationError), storageContention(storageContention) {}

Seconds Runtime::Maintenance::settleApprovals() {
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto now = clock();
    db.settlePolicyApprovals(now);
    tx.commit();
    return now;
}

std::optional<Seconds> Runtime::Maintenance::fireDueAutomations() {
    try {
        const auto now = clock();
        db.fireDueAutomations(now);
        return now;
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        if (!automationError) throw;
        automationError(error.what());
        return std::nullopt;
    }
}

Seconds Runtime::Maintenance::advanceWorkflows() {
    const auto now = clock();
    db.advanceWorkflows(now);
    return now;
}

void Runtime::Maintenance::scanIfChanged(std::optional<ScanStamp>& completed, std::string_view operation, const auto& work) {
    const auto now = clock();
    std::optional<ScanStamp> beforeScan;
    deferContendedWork("database change stamp", [&] { beforeScan.emplace(db.changeStamp(), now); });
    if (beforeScan && beforeScan == completed) return;
    deferContendedWork(operation, [&] {
        const std::optional<Seconds> scannedAt = work();
        if (beforeScan && scannedAt) completed.emplace(beforeScan->stamp, *scannedAt);
    });
}

void Runtime::Maintenance::settleApprovalsIfChanged() {
    scanIfChanged(approvalsCheckedAt, "policy approvals", [&] { return settleApprovals(); });
}

void Runtime::Maintenance::fireDueAutomationsIfChanged() {
    scanIfChanged(automationsCheckedAt, "automations", [&] { return fireDueAutomations(); });
}

void Runtime::Maintenance::advanceWorkflowsIfChanged() {
    scanIfChanged(workflowsCheckedAt, "workflows", [&] { return advanceWorkflows(); });
}
}
