#include "runtime_outbox_dispatch.hpp"
#include "outbox_storage.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/run_report.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace flockv0 {
namespace {
constexpr std::size_t maxDestinationEvidenceReads = 3;
}

bool Runtime::Components::OutboxDispatch::destinationDeadline(TaskData& task, DestinationRun& run, Seconds now) {
    if (task.state == TaskState::cancelling || !run.outstanding()) return false;
    const auto& action = run.actionId;
    const auto& environment = run.environmentId;
    const bool missing = !run.deadline;
    if (missing) {
        auto launch = db.query(R"SQL(
SELECT time FROM journal WHERE task_id=? AND kind='send_started'
AND action_id=? ORDER BY id LIMIT 1)SQL",
                               {task.id, action});
        const auto startedAt = launch.next() ? launch.number(0) : now;
        run.deadline = startedAt + db.environmentTimeout(task.botId, environment, task.config);
    }
    const auto deadline = *run.deadline;
    if (deadline <= 0 || now < deadline || task.state == TaskState::waiting) return missing;
    db.wait(task, DeadlineWait{VerificationDeadline{deadline, action}}, run.name + " reached its deadline", WaitRun{run.ref, environment});
    return true;
}

void Runtime::Components::OutboxDispatch::stepDestination(const TaskData& task, const DestinationRun& run) {
    const auto id = task.id;
    const auto& key = run.actionId;
    std::optional<Outbox> action;
    {
        std::lock_guard lock(db.mutex);
        action = outboxStorage.outbox(task, key);
    }
    if (!action) return;
    const auto phase = action->phase;
    const auto now = clock();
    const bool cancelling = task.state == TaskState::cancelling;
    const auto& ref = run.ref;
    const bool stopRequested = destinationStopRequested(action, ref);
    const bool stopping = cancelling || stopRequested;
    if (stopping && phase == OutboxPhase::ready) {
        applyDelivery(id, *action, {Delivery::Kind::NotAccepted, {}, "Cancelled before dispatch", true});
        return;
    }
    if (stopRequested && phase == OutboxPhase::started) {
        if (now < run.retryAt.value_or(0)) return;
        try {
            const auto cmd = command(task, *action);
            auto reconciled = t3.reconcile(cmd);
            if (reconciled.kind == Delivery::Kind::Accepted && hasRun(reconciled.run)) {
                applyDelivery(id, *action, std::move(reconciled));
                return;
            }
        } catch (const std::exception&) {
        }
        updates.changeTask(id, [&](TaskData& current) {
            auto& saved = destinationRun(current, key);
            if (!saved.outstanding()) return false;
            setDestinationStatus(saved, DestinationRunPhase::confirmation_pending,
                                 "T3 acceptance is unconfirmed; Flock will not retry this Run after the stop request.");
            saved.retryAt = now + 5;
            return true;
        });
        return;
    }
    const bool unconfirmedRunNeedsDeadlineCheck =
        phase == OutboxPhase::started && !stopping && (!run.deadline || (task.state != TaskState::waiting && now >= *run.deadline));
    if (unconfirmedRunNeedsDeadlineCheck)
        updates.changeTask(id, [&](TaskData& current) { return destinationDeadline(current, destinationRun(current, key), now); });
    if (now < run.retryAt.value_or(0)) return;
    if (phase == OutboxPhase::ready) {
        std::lock_guard lock(db.mutex);
        if (!db.projectRouteReady(id, key)) return;
    }
    if ((phase == OutboxPhase::ready || phase == OutboxPhase::started) && !stopRequested) return deliver(task, std::move(*action));
    if (phase != OutboxPhase::accepted) return;
    const auto cmd = command(task, *action);
    const auto observed = runs.observeRun(cmd.ownerBotId, cmd.environmentId, cmd.config, ref);
    if (observed.kind == Observation::Kind::Finished) {
        updates.changeTask(id, [&](TaskData& current) {
            auto& saved = destinationRun(current, key);
            if (!saved.outstanding() || saved.ref.threadId != ref.threadId || saved.ref.runId != ref.runId) return false;
            if (!stopping && (observed.commitEvidenceUnavailable || observed.evidence == RunEvidence::Availability::ReportUnavailable) &&
                saved.evidenceAttempts + 1 < maxDestinationEvidenceReads) {
                ++saved.evidenceAttempts;
                setDestinationStatus(saved, DestinationRunPhase::confirmation_pending,
                                     "The Run ended. Flock is retrieving its commit and report.");
                saved.retryAt = now + 5;
                return true;
            }
            writeDestinationReport(current, saved, observed);
            return true;
        });
        return;
    }
    using Kind = Observation::Kind;
    updates.changeTask(id, [&](TaskData& current) {
        auto& saved = destinationRun(current, key);
        if (!saved.outstanding() || saved.ref.threadId != ref.threadId || saved.ref.runId != ref.runId) return false;
        const auto savedAction = outboxStorage.outbox(current, key);
        const bool stopping = current.state == TaskState::cancelling || destinationStopRequested(savedAction, ref);
        bool changed = false;
        if (observed.kind == Kind::Unavailable) {
            const bool sessionMissing =
                stopping && db.query(R"SQL(
SELECT 1 FROM session_message_waits WHERE task_id=? AND action_id=? AND environment=?
  AND gateway_environment=? AND thread=? AND run=? AND session_missing=1 LIMIT 1
)SQL",
                                     {id, key, saved.environmentId, saved.ref.gatewayEnvironmentId, ref.threadId, ref.runId})
                                .next();
            changed = setDestinationStatus(
                saved, DestinationRunPhase::confirmation_pending,
                sessionMissing ? "T3 could not find this Session when Flock tried to stop it. Check its delegated Sessions in T3. "
                                 "Flock is keeping the project on hold until the work settles."
                               : "Flock could not confirm the Run's status in T3.");
        } else if (observed.kind == Kind::Question || observed.kind == Kind::ExternalInput)
            changed = setDestinationStatus(saved, DestinationRunPhase::confirmation_pending,
                                           "Review the pending input or approval in this T3 Session.");
        else if (!stopping)
            changed =
                setDestinationStatus(saved, observed.status == "queued" ? DestinationRunPhase::queued : DestinationRunPhase::running, {});
        return (!stopping && destinationDeadline(current, saved, now)) || changed;
    });
    if (!stopping || observed.kind == Kind::Settling || observed.kind == Kind::Unavailable || now < run.stopRetryAt.value_or(0)) return;
    if (!updates.changeTask(id, [&](TaskData& current) {
            const auto savedAction = outboxStorage.outbox(current, key);
            auto& saved = destinationRun(current, key);
            if (shutdown.stop_requested() || (current.state != TaskState::cancelling && !destinationStopRequested(savedAction, ref)) ||
                saved.ref.threadId != ref.threadId || saved.ref.runId != ref.runId)
                return false;
            saved.stopRetryAt = now + 5;
            return true;
        }))
        return;
    if (!cancelling && action->stop && action->stop->approval) {
        ApprovalGate gate = ApprovalGate::stale;
        {
            std::lock_guard lock(db.mutex);
            Transaction tx(db.db);
            const auto current = db.load(id);
            const auto resource = Resource{Action::t3_send, Resource::sessionRead(cmd.environmentId, ref.threadId).target};
            const auto& approval = *action->stop->approval;
            auto origin = db.taskPolicyOrigin(current);
            origin.scope = approval.scope;
            origin.requester = approval.requester;
            origin.anchor = approval.anchor;
            origin.turn = approval.turn;
            origin.background = approval.background;
            origin.task = id;
            origin.generation = approval.generation;
            origin.revision = approval.revisionId;
            const auto& operationId = action->stop->operation;
            gate = db.approveAction(origin, resource, std::string(runStopOperation) + operationId, stopPolicyJson(approval), now, true,
                                    Store::Impl::ApprovalWait::defer);
            if (gate != ApprovalGate::waiting) {
                auto saved = outboxStorage.outbox(current, key);
                if (saved && saved->stop && saved->stop->operation == operationId) {
                    if (gate == ApprovalGate::allow)
                        saved->stop->approval.reset();
                    else
                        saved->stop.reset();
                    outboxStorage.saveOutbox(*saved);
                }
            }
            tx.commit();
        }
        if (gate != ApprovalGate::allow) {
            destinationStatus(id, key,
                              gate == ApprovalGate::waiting ? DestinationRunPhase::confirmation_pending : DestinationRunPhase::running,
                              gate == ApprovalGate::waiting ? "Waiting for approval to stop the recovered T3 Run"
                                                            : "The recovered T3 Run stop was denied by policy");
            return;
        }
    }
    try {
        const SessionDispatchFileLock dispatch(db.path, SessionDispatchFileLock::Mode::shared);
        t3.interrupt(cmd.environmentId, cmd.config, ref);
    } catch (const std::exception&) {
        destinationStatus(id, key, DestinationRunPhase::confirmation_pending, "T3 did not confirm the stop request.");
    }
}
}
