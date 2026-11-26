#include "runtime_run_observer.hpp"
#include "workflow_storage.hpp"
#include "outbox_storage.hpp"
#include "journal.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_task_questions.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "task_interaction_storage.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"
#include "bot_settings_storage.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "journal_storage.hpp"
#include "peer_storage.hpp"
#include "run_ref_storage.hpp"
#include "secret_storage.hpp"
#include "task_events.hpp"
#include "task_records.hpp"
#include "wait_storage.hpp"

#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace flockv0 {
bool runtime::RunObserver::shouldStop(const TaskData& task) {
    if (peerStorage.replanReady(task)) return true;
    if (task.state != TaskState::cancelling) return false;
    std::lock_guard lock(database.mutex);
    const auto peer = peerStorage.peerExchange(task.id);
    return !peer || peer->recipient == task.id;
}

Observation runtime::RunObserver::observeRun(const std::string& bot, const std::string& environment, const Json& config,
                                             const RunRef& run) {
    try {
        auto observed = t3.observe(environment, config, run);
        const auto scrub = secretStorage.redactor(bot);
        redactQuestion(observed.question, scrub);
        for (auto* value : {&observed.resultRunId, &observed.itemId, &observed.checkedCommit})
            if (scrub.text(*value) != *value) value->clear();
        observed.report = scrub.text(observed.report);
        observed.detail = scrub.text(observed.detail);
        if (observed.status != "auth" && observed.status != "settings" && observed.status != "completed" && observed.status != "failed" &&
            observed.status != "cancelled" && observed.status != "interrupted" && observed.status != "rolled_back")
            observed.status = scrub.text(observed.status);
        return observed;
    } catch (const std::exception&) {
        Observation observation;
        observation.kind = Observation::Kind::Unavailable;
        return observation;
    }
}

Observation runtime::RunObserver::observe(const TaskData& task) {
    const auto& run = task.activeRun.value();
    return observeRun(task.botId, run.environmentId, task.config, run.ref);
}

bool runtime::RunObserver::ingest(const TaskData& snapshot, const Observation& observed) {
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    auto task = taskRecords.load(snapshot.id);
    if (task.generation != snapshot.generation || task.eventId != snapshot.eventId || task.currentRevision != snapshot.currentRevision ||
        task.activeRun != snapshot.activeRun)
        return false;
    if (task.workflow) workflowStorage.stageObservation(task, observed, clock());
    std::optional<DecisionCheckpoint> resumedDecision;
    const bool changed = [&] {
        const auto run = task.activeRun.value().ref;
        const auto state = task.state;
        const auto wait = waitStorage.currentWait(task.id);
        const auto reason = wait ? std::optional{wait->reason()} : std::nullopt;
        const bool connectionWait = reason == WaitReason::auth || reason == WaitReason::settings;
        const bool replanning = task.replanning;
        if (observed.kind == Observation::Kind::Finished) {
            questions.closeQuestion(task, "run_finished");
            if (!task.actionId.empty()) {
                const auto action = outboxStorage.outbox(task);
                const bool supersededAnswer = action && action->command.tool == CommandTool::answer_session_question;
                if (supersededAnswer) {
                    outboxStorage.setOutboxPhase(action->command.actionId, OutboxPhase::closed);
                    task.actionId.clear();
                }
            }
            task.lastRun = std::move(task.activeRun);
            task.activeRun.reset();
            task.queuedInT3 = false;
            updates.consumeEvent(task);
            SettledRunReport report{
                task.lastRun.value(),
                observed.resultRunId.empty() ? run.runId : observed.resultRunId,
                observed.status,
                {observed.evidence, observed.report, observed.itemId, observed.detail, observed.truncated, observed.sessionCreatedAt},
                observed.status == "interrupted" && (state == TaskState::cancelling || replanning)};
            database.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'result',?)", {task.environmentId, run.threadId, run.runId});
            if (sqlite3_changes(database.db)) {
                if (observed.evidence == RunEvidence::Availability::ReportUnavailable && !observed.resultRunId.empty() &&
                    state != TaskState::cancelling && !replanning) {
                    journalStorage.journalRunSettled(task.id, report);
                    task.pendingRunReport = PendingRunReport{std::move(report), clock() + 5, 0};
                } else
                    reports.writeRunReport(task, report);
            }
            if (task.runStop) {
                database.writeJournal(task.id, "interrupt_settled", runStopJson(*task.runStop));
                task.runStop.reset();
            }
            if (state == TaskState::cancelling)
                cancelDone(task);
            else if (!terminal(task))
                task.state = TaskState::deciding;
            return true;
        }
        if (state == TaskState::cancelling || replanning) return false;
        if (!task.actionId.empty()) return false;
        bool leftQueue = false;
        if (task.selectedSession && observed.kind != Observation::Kind::Unavailable) {
            if (observed.kind == Observation::Kind::Running && observed.status == "queued") {
                if (connectionWait) {
                    waitStorage.clearWait(task, "connection_restored");
                    task.state = TaskState::running;
                }
                if (task.queuedInT3) return connectionWait;
                task.queuedInT3 = true;
                task.deadline = 0;
                return true;
            }
            leftQueue = std::exchange(task.queuedInT3, false);
            if (leftQueue) task.deadline = clock() + botSettingsStorage.environmentTimeout(task.botId, task.environmentId, task.config);
        }
        if (observed.kind == Observation::Kind::Question) {
            if (observed.question.requestId.empty()) return false;
            if (task.question && task.question->target.requestId == observed.question.requestId) {
                if (!connectionWait && state != TaskState::running) return false;
                auto saved = task.question->target;
                saved.waitId.reset();
                if (saved != questionTarget(task)) return false;
                resumedDecision = questions.resumeQuestion(task);
                return true;
            }
            database.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'question',?)",
                          {task.environmentId, run.threadId, observed.question.requestId});
            if (!sqlite3_changes(database.db)) return false;
            questions.closeQuestion(task, "session_question", "replaced");
            updates.consumeEvent(task);
            const Json data = {{"kind", "question"}, {"run", runJson(run)}, {"question", questionJson(observed.question)}};
            taskEvents.event(task, "question/" + task.environmentId + "/" + run.threadId + "/" + observed.question.requestId, data);
            task.question =
                TaskQuestion{observed.question.fields, {std::nullopt, task.eventId, taskWaitRun(task), observed.question.requestId}};
            database.writeJournal(task.id, "session_question", data);
            task.state = TaskState::deciding;
            return true;
        }
        if (observed.kind == Observation::Kind::ExternalInput) {
            if (reason == WaitReason::t3_input) return false;
            waitStorage.unlockQuestion(task, "requires_t3");
            updates.consumeEvent(task);
            waitStorage.wait(task, SimpleWaitReason::t3_input, bounded(observed.status, 2048));
            return true;
        }
        if (observed.kind == Observation::Kind::Unavailable) {
            if (observed.status == "auth" && reason != WaitReason::auth) {
                questions.suspendQuestion(task);
                waitStorage.wait(task, SimpleWaitReason::auth, "T3 authentication is unavailable");
                return true;
            }
            if (observed.status == "settings" && reason != WaitReason::settings) {
                questions.suspendQuestion(task);
                waitStorage.wait(task, SimpleWaitReason::settings, "T3 connection settings need attention");
                return true;
            }
        }
        const bool available = observed.kind != Observation::Kind::Unavailable;
        if (available && (reason == WaitReason::t3_input || task.question)) {
            questions.closeQuestion(task, "session_question_resolved", "resolved_in_t3");
            updates.consumeEvent(task);
            task.state = TaskState::running;
            return true;
        }
        if (available && connectionWait) {
            waitStorage.clearWait(task, "connection_restored");
            task.state = TaskState::running;
            return true;
        }
        if (state == TaskState::running && task.deadline > 0 && clock() >= task.deadline) {
            waitStorage.wait(task, DeadlineWait{}, "This request reached its deadline");
            return true;
        }
        return leftQueue;
    }();
    if (changed) updates.saveTask(task, std::move(resumedDecision));
    tx.commit();
    return changed;
}

bool runtime::RunObserver::questionStillOpen(const TaskData& task) {
    const auto observed = observe(task);
    if (ingest(task, observed) || observed.kind != Observation::Kind::Question) return false;
    if (!task.question || task.question->target.requestId != observed.question.requestId) return false;
    auto target = questionTarget(task);
    const auto& savedTarget = task.question->target;
    if (savedTarget == target) return true;
    target.eventId = savedTarget.eventId;
    return savedTarget == target && peerExchanges.forwardedQuestionAnswer(task, savedTarget);
}

void runtime::RunObserver::interruptRun(const TaskData& task) {
    const auto& stop = task.runStop.value();
    if (!task.activeRun || task.activeRun != stop.run || (task.state == TaskState::cancelling) != (stop.intent == RunStopIntent::cancel))
        return;
    const auto observed = observe(task);
    if (ingest(task, observed)) return;
    if (observed.kind == Observation::Kind::Settling) return stopNote(task, "Waiting for the remaining work on this request to finish");
    if (observed.kind == Observation::Kind::Unavailable && !(task.workflow && stop.intent == RunStopIntent::cancel))
        return stopNote(task, "Flock couldn't check the status of this work in T3");
    const auto now = clock();
    if (now < stop.retryAt.value_or(0)) return;
    TaskData requested;
    if (!updates.change(
            task,
            [&](TaskData& current) {
                if (shutdown.stop_requested()) return false;
                current.runStop.value().retryAt = now + 5;
                return true;
            },
            &requested))
        return;
    InterruptOutcome outcome;
    try {
        const auto& run = task.activeRun.value();
        outcome = t3.interrupt(run.environmentId, task.config, run.ref);
    } catch (const std::exception&) {
        return stopNote(requested, "T3 did not confirm the interrupt request");
    }
    if (outcome != InterruptOutcome::Requested) return stopNote(requested, "Waiting for T3 to confirm this work has stopped");
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    journalStorage.journalInterrupt(task.id, stop);
    transaction.commit();
}

void runtime::RunObserver::cancelDone(TaskData& task) {
    questions.closeQuestion(task, "cancelled");
    if (!task.actionId.empty()) {
        const auto action = outboxStorage.outbox(task);
        if (action && outboxStorage.setOutboxPhase(action->command.actionId, OutboxPhase::closed, OutboxPhase::ready))
            updates.refundUnsentWork(task, *action);
        task.actionId.clear();
    }
    task.runStop.reset();
    task.replanning = false;
    task.pendingDestination.reset();
    task.queuedInT3 = false;
    task.cancelledInstructions = std::exchange(task.pendingInstructions, {});
    task.activeRun.reset();
    task.state = TaskState::cancelled;
    journalStorage.journalCancelled(task.id);
    if (const auto peer = peerStorage.peerExchange(task.id); peer && peer->recipient == task.id)
        peerExchanges.finishPeer(*peer, task, taskRecords.load(peer->sender), "cancelled",
                                 task.cancelledByBotPause ? "The receiving Bot is paused." : "The receiving Bot's task was cancelled.",
                                 actionId());
}

void runtime::RunObserver::finishReplan(const TaskData& task) {
    updates.change(task, [&](TaskData& current) {
        const auto id = current.id;
        const auto peer = peerStorage.peerExchange(id);
        if (peer && !peer->allowsReplan(id)) return false;
        const auto oldEnvironment = current.environmentId;
        const auto oldProject = current.projectId;
        const auto oldGateway =
            current.gatewayEnvironmentId && !current.gatewayEnvironmentId->empty() ? *current.gatewayEnvironmentId : oldEnvironment;
        const auto destination = current.pendingDestination;
        const bool moving =
            destination && (destination->route.environmentId != oldEnvironment || destination->route.gatewayEnvironmentId != oldGateway ||
                            (destination->pinned && destination->route.projectId != oldProject));
        const auto event = taskEvents.currentEvent(current);
        const bool preservePeerAnswer = peer && !event.is_null() && event["consumed"] != true && event["kind"] == "bot_answer";
        if (moving) {
            current.environmentId = destination->route.environmentId;
            current.projectId = destination->pinned ? destination->route.projectId : std::string{};
            if (destination->route.gatewayEnvironmentId == destination->route.environmentId)
                current.gatewayEnvironmentId.reset();
            else
                current.gatewayEnvironmentId = destination->route.gatewayEnvironmentId;
            current.newSessionRequired = true;
            current.selectedSession.reset();
            database.writeJournal(id, "session_destination_changed",
                                  {{"from", oldEnvironment},
                                   {"to", destination->route.environmentId},
                                   {"fromProject", oldProject},
                                   {"toProject", current.projectId},
                                   {"revisionId", revisionJson(current.currentRevision)}});
        }
        if (destination && (destination->pinned || destination->route.projectId.empty())) current.pendingDestination.reset();
        questions.closeQuestion(current, "user_instruction");
        if (!preservePeerAnswer) {
            updates.consumeEvent(current);
            const auto report = reports.settledSessionReport(current);
            taskEvents.event(current, "replan/" + std::to_string(current.generation),
                             report ? encodeRunReport(report->data) : Json{{"kind", "intake"}});
        }
        const bool needsQueue =
            moving || current.state == TaskState::queued || current.projectId.empty() || (current.workflow && !current.lastRun);
        current.state = needsQueue ? TaskState::queued : TaskState::deciding;
        current.replanning = false;
        current.replanningDetail.reset();
        current.runStop.reset();
        current.feedback.reset();
        return true;
    });
}

void runtime::RunObserver::stopNote(const TaskData& snapshot, const std::string& detail) {
    updates.change(snapshot, [&](TaskData& task) {
        const bool cancel = task.state == TaskState::cancelling;
        auto& saved = cancel ? task.cancellationDetail : task.replanningDetail;
        if (saved == detail) return false;
        saved = detail;
        database.writeJournal(task.id, cancel ? "cancelling" : "replanning",
                              {{"detail", detail}, {"run", assignedRunJson(task.activeRun)}});
        return true;
    });
}
}
