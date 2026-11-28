#include "session_dispatch_lock.hpp"
#include "session_participation_records.hpp"
#include "t3_connected_environments.hpp"
#include "conversation_messages.hpp"
#include "runtime_outbox_dispatch.hpp"

#include "flock/commit_check.hpp"
#include "conversation_storage.hpp"
#include "flock/bot_notes_error.hpp"
#include "flock/limits.hpp"
#include "flock/participation.hpp"
#include "journal.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_prompts.hpp"
#include "runtime_task_questions.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/session_prompt.hpp"
#include "flock/session_settings.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"
#include "bot_lifecycle_storage.hpp"
#include "attachment_storage.hpp"
#include "conversation_turns.hpp"
#include "database.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"
#include "journal_storage.hpp"
#include "policy_approvals.hpp"
#include "run_ref_storage.hpp"
#include "session_participation_storage.hpp"
#include "session_read_storage.hpp"
#include "task_events.hpp"
#include "task_interaction_storage.hpp"
#include "task_records.hpp"
#include "task_routing.hpp"
#include "task_run_reports.hpp"
#include "wait_storage.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
constexpr Seconds deliveryCheckInterval = 5;
}

void runtime::OutboxDispatch::stop(TaskData task) {
    const bool cancel = task.state == TaskState::cancelling;
    const auto intent = cancel ? RunStopIntent::cancel : RunStopIntent::replan;
    if (task.pendingRunReport) {
        if (!updates.change(
                task,
                [&](TaskData& current) {
                    const auto report = std::move(current.pendingRunReport.value().report);
                    current.pendingRunReport.reset();
                    reports.writeRunReport(current, report);
                    return true;
                },
                &task))
            return;
    }
    std::optional<Outbox> action;
    {
        std::lock_guard lock(database.mutex);
        action = outboxStorage.outbox(task);
    }
    if (task.activeRun || (action && action->phase == OutboxPhase::started)) {
        const auto& saved = task.runStop;
        if (!saved || saved->intent != intent || saved->run != task.activeRun) {
            if (!updates.change(
                    task,
                    [&](TaskData& current) {
                        if (!current.runStop || current.runStop->run != current.activeRun)
                            current.runStop = RunStop{intent, current.activeRun};
                        else
                            current.runStop->intent = intent;
                        return true;
                    },
                    &task))
                return;
        }
    }
    if (action && action->phase == OutboxPhase::started) {
        const auto now = clock();
        const bool reply = action->command.tool == CommandTool::answer_session_question;
        if (now >= task.runStop.value().reconcileAt.value_or(0) && updates.change(task, [&](TaskData& current) {
                current.runStop.value().reconcileAt = now + 3;
                return true;
            }))
            deliver(task, *action);
        if (reply && task.activeRun) runs.interruptRun(updates.snapshot(task.id));
        return;
    }
    if (!cancel && action && action->phase == OutboxPhase::ready) {
        updates.change(task, [&](TaskData& current) {
            if (!outboxStorage.setOutboxPhase(action->command.actionId, OutboxPhase::superseded, OutboxPhase::ready)) return false;
            current.actionId.clear();
            current.t3CheckAt.reset();
            waitStorage.clearWait(current, "superseded_instruction");
            database.writeJournal(current.id, JournalKind::delivery_superseded,
                                  {{"actionId", action->command.actionId}, {"revisionId", revisionJson(current.currentRevision)}});
            return true;
        });
        return;
    }
    if (task.activeRun)
        runs.interruptRun(task);
    else if (cancel)
        updates.change(task, [&](TaskData& current) {
            runs.cancelDone(current);
            return true;
        });
    else
        runs.finishReplan(task);
}

Command runtime::OutboxDispatch::command(const TaskData& task, const Outbox& action) {
    Command c{action.command};
    c.authGeneration = action.authGeneration;
    c.ownerBotId = task.botId;
    c.attachmentCount = action.attachments.size();
    if (action.phase == OutboxPhase::started && c.tool != CommandTool::answer_session_question)
        c.instruction = SessionPrompts::dispatchedWorkPrompt(action);
    c.config = task.config;
    if (action.commit) {
        const auto& commit = action.commit->expected;
        c.config["session"]["workspaceStrategy"] = {{"type", "worktree"}, {"baseRef", commit}, {"startFromOrigin", false}};
        c.config["commitVerification"] = {{"expectedCommit", commit}, {"projectId", c.projectId}};
    }
    if (c.gatewayEnvironmentId.empty()) c.gatewayEnvironmentId = c.target.gatewayEnvironmentId;
    if (c.gatewayEnvironmentId.empty() && !action.destinationAction)
        c.gatewayEnvironmentId = task.gatewayEnvironmentId.value_or(c.environmentId);
    if (c.target.gatewayEnvironmentId.empty() && c.gatewayEnvironmentId != c.environmentId)
        c.target.gatewayEnvironmentId = c.gatewayEnvironmentId;
    if (c.tool == CommandTool::delegate) c.sessionOrigin = task.sessionOrigin;
    return c;
}

void runtime::OutboxDispatch::instructionDelivery(TaskData& task, const Outbox& action) {
    if (action.instructions.empty()) return;
    auto& pending = task.pendingInstructions;
    for (const auto& item : action.instructions) {
        const auto copy =
            std::ranges::find_if(pending, [&](const OutboxInstruction& p) { return p.messageId == item.messageId && p.step == item.step; });
        if (copy != pending.end()) pending.erase(copy);
        journalStorage.journal(task.id, DeliveredInstructionEntry{action.command.actionId, item, action.revisionId, task.activeRun});
    }
}

bool runtime::OutboxDispatch::assignedRunMatches(const TaskData& task, const CommandPayload& command, const RunRef& run) {
    if (command.receipt &&
        (command.receipt->threadId != run.threadId || (!command.receipt->runId.empty() && command.receipt->runId != run.runId)))
        return false;
    const auto& environment = command.environmentId;
    auto owner = database.query("SELECT action_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                                {environment, run.threadId, run.runId});
    if (owner.next() && owner.string(0) != command.actionId) return false;
    if (command.tool == CommandTool::start_in_session) return run.threadId == command.target.threadId;
    if (command.tool == CommandTool::follow_up) {
        const auto& target = command.target;
        if (run.threadId != target.threadId || run.runId == target.runId) return false;
        auto prior = database.query("SELECT 1 FROM runs WHERE task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                                    {task.id, environment, target.threadId, target.runId});
        return prior.next();
    }
    auto prior = database.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=? AND action_id<>? LIMIT 1",
                                {environment, run.threadId, command.actionId});
    return !prior.next();
}

runtime::OutboxDispatch::DeliveryOutcome runtime::OutboxDispatch::classifyDelivery(const TaskData& task, const Outbox& action,
                                                                                   Delivery& outcome, bool reply) {
    const auto scrub = policy.secretRedactor(task.botId);
    if (scrub.payload(runJson(outcome.run)) != runJson(outcome.run))
        outcome = {Delivery::Kind::Unknown, {}, "T3 returned a protected Run identifier"};
    outcome.detail = scrub.text(outcome.detail);
    if (outcome.kind == Delivery::Kind::Accepted) {
        if (reply || (hasRun(outcome.run) && assignedRunMatches(task, action.command, outcome.run))) return DeliveryOutcome::Accepted;
        outcome.detail = "The returned Run does not identify the assigned work";
        outcome.run = {};
    } else if (outcome.definitelyUnsent || outcome.kind == Delivery::Kind::NotAccepted)
        return DeliveryOutcome::DefinitelyUnsent;
    return DeliveryOutcome::Unknown;
}

AssignedRun runtime::OutboxDispatch::recordRunAssignment(const TaskData& task, const Outbox& action, const RunRef& ref,
                                                         DestinationRun* destination) {
    const auto& cmd = action.command;
    database.exec("INSERT OR IGNORE INTO runs(action_id,task_id,environment_id,thread_id,run_id,link) VALUES(?,?,?,?,?,?)",
                  {cmd.actionId, task.id, cmd.environmentId, ref.threadId, ref.runId, ref.link});
    AssignedRun run{ref, cmd.environmentId, {task.botId, cmd.projectId, action.revisionId}};
    if (destination) {
        destination->assignment = run.assignment;
        destination->ref = run.ref;
    }
    auto sources = Json::array();
    for (const auto& source : action.sourceResultRefs) sources.push_back(sourceJson(source));
    Json entry{{"run", destination ? destinationRunJson(*destination) : assignedRunJson(run)},
               {"actionId", cmd.actionId},
               {"revisionId", revisionJson(action.revisionId)},
               {"sourceResultRefs", std::move(sources)}};
    if (!action.attachments.empty()) entry["attachmentDelivery"] = "accepted_by_t3";
    database.writeJournal(task.id, JournalKind::run_started, entry);
    return run;
}

void runtime::OutboxDispatch::writeDestinationReport(TaskData& task, DestinationRun& run, const Observation& observed) {
    if (taskRunReports.recordDestinationReport(task, run, observed) && task.state == TaskState::cancelling) runs.cancelDone(task);
}

bool runtime::OutboxDispatch::setDestinationStatus(DestinationRun& run, DestinationRunPhase status, std::string detail) {
    const auto currentDetail = run.detail ? std::string_view(*run.detail) : std::string_view{};
    if (run.inPhase(status) && currentDetail == detail) return false;
    run.status = status;
    run.detail = std::move(detail);
    return true;
}

void runtime::OutboxDispatch::destinationStatus(Id id, std::string_view action, DestinationRunPhase status, std::string detail) {
    updates.changeTask(id, [&](TaskData& task) {
        auto& run = destinationRun(task, action);
        return run.outstanding() && setDestinationStatus(run, status, std::move(detail));
    });
}

bool runtime::OutboxDispatch::deferDestinationDelivery(TaskData& task, DestinationRun& run, const Outbox& action, const Delivery& outcome,
                                                       DeliveryOutcome delivery) {
    const bool cancelling = task.state == TaskState::cancelling || action.stop.has_value();
    const auto& key = action.command.actionId;
    const auto phase = action.phase;
    const auto detail = bounded(outcome.detail.empty() ? "T3 has not confirmed the assigned Run." : outcome.detail, 2048);
    const bool rejected = outcome.kind == Delivery::Kind::NotAccepted || outcome.kind == Delivery::Kind::Settings;
    const bool closeUnsentDelivery =
        delivery == DeliveryOutcome::DefinitelyUnsent && (cancelling || phase == OutboxPhase::started || rejected);
    if (closeUnsentDelivery) {
        outboxStorage.setOutboxPhase(key, OutboxPhase::closed);
        Observation observed;
        observed.status = cancelling ? "cancelled" : "failed";
        observed.detail = detail;
        writeDestinationReport(task, run, observed);
        return true;
    }
    setDestinationStatus(run, DestinationRunPhase::confirmation_pending, detail);
    run.retryAt = clock() + 5;
    return true;
}

bool runtime::OutboxDispatch::deferSingleDelivery(TaskData& task, const Outbox& action, OutboxPhase attemptedPhase, const Delivery& outcome,
                                                  DeliveryOutcome delivery) {
    const bool cancelling = task.state == TaskState::cancelling;
    const auto& key = action.command.actionId;
    const bool unsent = attemptedPhase == OutboxPhase::ready && delivery == DeliveryOutcome::DefinitelyUnsent;
    auto detail = outcome.detail.empty() ? "T3 did not confirm delivery." : bounded(outcome.detail, 2048);
    if (unsent) {
        outboxStorage.setOutboxPhase(key, cancelling ? OutboxPhase::closed : OutboxPhase::ready);
        if (cancelling) {
            task.actionId.clear();
            task.t3CheckAt.reset();
        }
    }
    if (cancelling || task.replanning) {
        auto& savedDetail = cancelling ? task.cancellationDetail : task.replanningDetail;
        if (savedDetail == detail && !unsent) return false;
        savedDetail = detail;
        database.writeJournal(task.id, cancelling ? JournalKind::cancelling : JournalKind::replanning,
                              {{"detail", detail}, {"actionId", key}});
        return true;
    }
    task.t3CheckAt = clock() + deliveryCheckInterval;
    if (outcome.kind == Delivery::Kind::Auth || outcome.kind == Delivery::Kind::Settings ||
        (unsent && outcome.kind == Delivery::Kind::NotAccepted)) {
        const auto reason = outcome.kind == Delivery::Kind::Auth ? SimpleWaitReason::auth : SimpleWaitReason::settings;
        if (reason == SimpleWaitReason::auth) detail = "Sign in again in T3 Code or reconnect this environment in Flock settings.";
        const auto wait = waitStorage.currentWait(task.id);
        if (!wait || wait->reason() != waitReason(reason) || wait->text != detail) waitStorage.wait(task, reason, std::move(detail));
        return true;
    }
    if (!unsent && !database
                        .query("SELECT 1 FROM journal WHERE task_id=? AND kind='delivery_unconfirmed' "
                               "AND json_extract(data,'$.actionId')=? LIMIT 1",
                               {task.id, key})
                        .next())
        database.writeJournal(task.id, JournalKind::delivery_unconfirmed, {{"actionId", key}, {"detail", std::move(detail)}});
    waitStorage.clearWait(task, "delivery_check_pending");
    task.state = TaskState::sending;
    return true;
}

void runtime::OutboxDispatch::applyDelivery(Id id, const Outbox& action, Delivery outcome, std::optional<Id> readyGeneration) {
    updates.changeTask(id, [&](TaskData& task) {
        if (terminal(task)) return false;
        const auto& key = action.command.actionId;
        const bool destination = action.destinationAction;
        if (!destination && task.actionId != key) return false;
        auto saved = outboxStorage.outbox(task, key);
        if (!saved || (saved->phase != OutboxPhase::ready && saved->phase != OutboxPhase::started)) return false;
        if (outcome.run.gatewayEnvironmentId.empty()) {
            auto gateway = action.command.gatewayEnvironmentId;
            if (gateway.empty() && !destination) gateway = task.gatewayEnvironmentId.value_or("");
            if (gateway != action.command.environmentId) outcome.run.gatewayEnvironmentId = std::move(gateway);
        }
        if (!outcome.launchWarning.empty()) {
            task.launchWarning = policy.secretRedactor(task.botId).text(outcome.launchWarning);
            database.writeJournal(id, JournalKind::session_lineage_unavailable, {{"actionId", key}, {"text", *task.launchWarning}});
        }
        const bool cancelling = task.state == TaskState::cancelling;
        const bool reply = action.command.tool == CommandTool::answer_session_question;
        const auto delivery = classifyDelivery(task, *saved, outcome, reply);

        const bool accepted = delivery == DeliveryOutcome::Accepted && saved->phase == OutboxPhase::started;
        if (accepted) outboxStorage.setOutboxPhase(key, OutboxPhase::accepted);
        if (delivery == DeliveryOutcome::Unknown && action.phase == OutboxPhase::ready && saved->phase == OutboxPhase::started && !reply &&
            !outcome.run.threadId.empty() &&
            (action.command.target.threadId.empty() || action.command.target.threadId == outcome.run.threadId)) {
            saved->command.receipt = outcome.run;
            outboxStorage.saveOutbox(*saved, OutboxPhase::started);
            database.writeJournal(id, JournalKind::run_id_pending, {{"actionId", key}, {"thread", runJson(outcome.run)}});
            if (destination) {
                auto& run = destinationRun(task, key);
                run.ref.link = outcome.run.link;
                run.ref.threadId = outcome.run.threadId;
            }
        }
        if (destination) {
            auto& run = destinationRun(task, key);
            if (!accepted) return deferDestinationDelivery(task, run, *saved, outcome, delivery);
            if (saved->stop && !saved->stop->run) {
                saved->stop->run = OutboxRunIdentity{outcome.run.threadId, outcome.run.runId};
                outboxStorage.saveOutbox(*saved);
            }
            run.status = DestinationRunPhase::running;
            run.detail.reset();
            run.retryAt.reset();
            recordRunAssignment(task, action, outcome.run, &run);
            task.initialAttachments.clear();
            return true;
        }
        if (accepted) {
            waitStorage.clearWait(task, "delivery_confirmed");
            if (reply)
                waitStorage.unlockQuestion(task, "answered");
            else {
                task.activeRun = recordRunAssignment(task, action, outcome.run);
                if (action.sessionParticipationId)
                    database.exec(R"SQL(
INSERT OR IGNORE INTO session_queue_cancellations(participation_id,run,stop_id)
SELECT id,?,stopped_through FROM session_participations WHERE id=? AND stopped_through>=?
)SQL",
                                  {outcome.run.runId, *action.sessionParticipationId, task.sourceMessageId.value()});
                instructionDelivery(task, action);
                if (action.command.tool == CommandTool::delegate || action.command.tool == CommandTool::start_in_session)
                    task.initialAttachments.clear();
                if ((action.command.tool == CommandTool::delegate || action.command.tool == CommandTool::start_in_session) &&
                    !task.replanning) {
                    task.newSessionRequired = false;
                    task.freshContextRequested = false;
                }
            }
            if (reply)
                database.writeJournal(id, JournalKind::session_answered,
                                      {{"requestId", action.command.questionId}, {"actionId", action.command.actionId}});
            task.actionId.clear();
            task.t3CheckAt.reset();
            if (!cancelling) task.state = TaskState::running;
            return true;
        }
        if (reply && (outcome.kind == Delivery::Kind::QuestionGone || outcome.kind == Delivery::Kind::NotAccepted)) {
            const bool gone = outcome.kind == Delivery::Kind::QuestionGone;
            outboxStorage.setOutboxPhase(key, OutboxPhase::closed);
            questions.closeQuestion(task, gone ? "question_gone" : "answer_rejected", gone ? "resolved_or_expired" : "requires_t3");
            task.actionId.clear();
            task.t3CheckAt.reset();
            if (!cancelling) {
                task.state = TaskState::running;
                if (!gone) waitStorage.wait(task, SimpleWaitReason::t3_input, "Check the Session's question in T3.");
            }
            return true;
        }
        const bool sendStarted = saved->phase == OutboxPhase::started;
        if (action.phase == OutboxPhase::ready && delivery == DeliveryOutcome::DefinitelyUnsent && !cancelling && !task.replanning &&
            readyGeneration && task.generation == *readyGeneration + (sendStarted ? 1 : 0))
            policyApprovals.keepUnsentTaskApproval(task, key, *readyGeneration, sendStarted, clock());
        return deferSingleDelivery(task, *saved, action.phase, outcome, delivery);
    });
}

void runtime::OutboxDispatch::deliver(const TaskData& task, Outbox action) {
    if (shutdown.stop_requested() || (action.phase != OutboxPhase::ready && action.phase != OutboxPhase::started)) return;
    auto cmd = command(task, action);
    const auto id = task.id;
    const bool destination = action.destinationAction;
    const auto applyT3Delivery = [&](Delivery observed) {
        if (!destination && observed.kind == Delivery::Kind::Settings && action.attachments.empty())
            observed.detail = "Check the Bot’s Session settings and the T3 connection in Flock settings.";
        applyDelivery(id, action, std::move(observed), task.generation);
    };
    std::optional<SessionDispatchFileLock> dispatch;
    if (destination) dispatch.emplace(path, SessionDispatchFileLock::Mode::shared);
    if (action.phase == OutboxPhase::ready || destination) {
        std::lock_guard lock(database.mutex);
        const auto current = taskRecords.load(id);
        if (action.phase == OutboxPhase::ready &&
            (botLifecycle.botPaused(cmd.ownerBotId) || current.state == TaskState::cancelling || current.generation != task.generation))
            return;
        if (destination) {
            const auto saved = outboxStorage.outbox(current, cmd.actionId);
            if (saved && saved->stop) return;
        }
    }
    const auto contact = [&](const auto& request) {
        Delivery outcome;
        try {
            outcome = request();
        } catch (const std::exception&) {
            outcome.detail = "Flock couldn’t reach T3 to confirm delivery.";
        }
        applyT3Delivery(std::move(outcome));
    };
    if (action.phase == OutboxPhase::started) return contact([&] { return t3.reconcile(cmd); });

    const bool usesExistingSession = cmd.tool == CommandTool::follow_up || cmd.tool == CommandTool::start_in_session;
    const bool sendsToSession = usesExistingSession || cmd.tool == CommandTool::answer_session_question;
    const bool preparesTaskContract = cmd.tool == CommandTool::delegate || cmd.tool == CommandTool::start_in_session;
    const bool sendsWork = preparesTaskContract || usesExistingSession;
    const auto sessionActionAllowed = [&](const TaskData& current) {
        if (!cmd.sessionOrigin && !sendsToSession) return true;
        std::lock_guard lock(database.mutex);
        if (const auto& origin = current.sessionOrigin) {
            const auto source = conversationMessages.load(current.sourceMessageId.value());
            const auto actor = conversationMessages.actor(source);
            const auto& parent = origin->target;
            if (!sessionParticipations.activeOrigin(cmd.ownerBotId, actor.id, parent, cmd.environmentId, source.id) ||
                !policy.allows(actor, Action::bot_decide, cmd.ownerBotId) ||
                !policy.allows(cmd.ownerBotId, Action::environment_inspect, parent.environmentId))
                return false;
            if (!sendsToSession) return true;
        }
        if (sessionParticipations.stopped(cmd.ownerBotId,
                                          {cmd.environmentId, cmd.target.threadId,
                                           cmd.gatewayEnvironmentId == cmd.environmentId ? std::string{} : cmd.gatewayEnvironmentId},
                                          current.sourceMessageId.value_or(0)))
            return false;
        if (!usesExistingSession || !current.selectedSession) return cmd.tool != CommandTool::start_in_session;
        const auto& selected = *current.selectedSession;
        if (selected.target.environmentId != cmd.environmentId || selected.target.threadId != cmd.target.threadId ||
            selected.projectId != cmd.projectId)
            return false;
        return sessionReadStorage.permitsWorkSession(current.sourceMessageId.value_or(0), selected);
    };
    std::optional<Resource> resource;
    if (cmd.tool == CommandTool::delegate)
        resource = Resource{Action::environment_launch, cmd.environmentId};
    else if (sendsToSession) {
        resource = Resource{Action::t3_send, Resource::sessionRead(cmd.environmentId, cmd.target.threadId).target,
                            cmd.tool == CommandTool::answer_session_question};
    }
    const auto operation = std::string(destination ? runOutboxOperation : "outbox/") + cmd.actionId;
    const auto refused = [&](ApprovalGate gate, OutboxPhase phase = OutboxPhase::ready) {
        if (destination) return applyDelivery(id, action, {Delivery::Kind::NotAccepted, {}, std::string(approvalError(gate)), true});
        updates.changeTask(id, [&](TaskData& current) {
            if (current.actionId != action.command.actionId) return false;
            if (!outboxStorage.setOutboxPhase(action.command.actionId, OutboxPhase::closed, phase)) return false;
            current.actionId.clear();
            current.t3CheckAt.reset();
            waitStorage.clearWait(current, "action_closed");
            if (current.state != TaskState::cancelling) {
                if (current.activeRun) {
                    current.state = TaskState::running;
                    if (cmd.tool == CommandTool::answer_session_question) {
                        waitStorage.unlockQuestion(current, "policy_rejected");
                        waitStorage.wait(current, SimpleWaitReason::t3_input, "Check the Session's question in T3.");
                    }
                } else {
                    current.state = TaskState::deciding;
                    taskEvents.consumeEvent(current);
                    taskEvents.event(current, "policy/" + cmd.actionId, {{"kind", "policy_outcome"}, {"text", approvalError(gate)}});
                }
            }
            database.writeJournal(id, JournalKind::policy_action_closed, {{"actionId", cmd.actionId}, {"text", approvalError(gate)}});
            return true;
        });
    };
    const auto disconnected = [&] {
        const auto& connectionEnvironment = cmd.gatewayEnvironmentId.empty() ? cmd.environmentId : cmd.gatewayEnvironmentId;
        return requireT3Connection && !t3EnvironmentConnected(database, connectionEnvironment);
    };
    const auto connectionUnavailable = [&] {
        applyDelivery(id, action, {Delivery::Kind::Auth, {}, "Connect this T3 environment in Flock settings before sending work.", true},
                      task.generation);
    };
    if (disconnected()) return connectionUnavailable();
    if (!sessionActionAllowed(task)) return refused(ApprovalGate::denied);
    if (resource && policy.mode() == "full" && policy.forbidden(cmd.ownerBotId, resource->action, resource->target))
        return applyDelivery(id, action, {Delivery::Kind::Settings, {}, "Session action denied by installation policy.", true});
    if (preparesTaskContract) {
        try {
            std::lock_guard lock(database.mutex);
            if (action.instructionBase) {
                prompts.prepareDelegation(task, action);
                if (!outboxStorage.prepareOutbox(action)) {
                    auto saved = outboxStorage.outbox(task, cmd.actionId);
                    if (!saved || saved->phase != OutboxPhase::ready || saved->instructionBase) return;
                    action = std::move(*saved);
                }
            }
            cmd.instruction = action.command.instruction;
            cmd.attachmentCount = action.attachments.size();
        } catch (const Conflict& error) {
            return applyDelivery(id, action, {Delivery::Kind::Settings, {}, error.what(), true});
        }
    }
    if (sendsWork) {
        const auto scrub = policy.secretRedactor(cmd.ownerBotId);
        const auto marker = sessionPrompt(cmd.actionId, {});
        if (cmd.instruction.ends_with(marker)) cmd.instruction.resize(cmd.instruction.size() - marker.size());
        action.sessionContext = {};
        action.command.instruction = scrub.text(cmd.instruction);
        cmd.instruction = SessionPrompts::dispatchedWorkPrompt(action);
        if (cmd.instruction.size() > limits::session_prompt::maxBytes)
            return applyDelivery(id, action, {Delivery::Kind::Settings, {}, SessionPrompts::ContextLimit{}.what(), true});
    }
    auto payload = Json{{"tool", commandToolName(cmd.tool)},
                        {"instruction", cmd.instruction},
                        {"questionId", cmd.questionId},
                        {"answers", cmd.answers},
                        {"run", runJson(cmd.target)},
                        {"projectId", cmd.projectId},
                        {"config", cmd.config},
                        {"environmentId", cmd.environmentId}};
    if (cmd.sessionOrigin) payload["delegatedFrom"] = sessionOriginJson(*cmd.sessionOrigin);
    if (!action.attachments.empty()) payload["attachments"] = attachmentRefsJson(action.attachments);
    const auto assess = [&](const TaskData& current, bool claim) {
        const auto origin = policyApprovals.taskPolicyOrigin(current);
        if (usesExistingSession) {
            const Resource launch{Action::environment_launch, cmd.environmentId, true};
            const auto launchPolicy = policy.check(cmd.ownerBotId, launch);
            if (launchPolicy == PolicyResult::deny) return ApprovalGate::denied;
            if (launchPolicy == PolicyResult::ask && policy.check(cmd.ownerBotId, *resource) == PolicyResult::allow)
                return policyApprovals.approveAction(origin, launch, operation, payload, clock(), claim);
        }
        return resource ? policyApprovals.approveAction(origin, *resource, operation, payload, clock(), claim) : ApprovalGate::allow;
    };
    if (resource) {
        ApprovalGate gate;
        {
            std::lock_guard lock(database.mutex);
            Transaction tx(database.db);
            const auto current = destination ? taskRecords.load(id) : task;
            if (destination && current.state == TaskState::cancelling) return;
            gate = assess(current, false);
            tx.commit();
        }
        if (gate == ApprovalGate::waiting) {
            if (destination) destinationStatus(id, cmd.actionId, DestinationRunPhase::confirmation_pending, "Waiting for launch approval");
            return;
        }
        if (gate != ApprovalGate::allow) return refused(gate);
    }
    std::optional<Delivery> stopped;
    try {
        stopped = t3.preflight(cmd);
        if (!stopped)
            cmd.authGeneration =
                t3.authGeneration(cmd.gatewayEnvironmentId.empty() ? cmd.environmentId : cmd.gatewayEnvironmentId, cmd.config);
    } catch (const std::exception& error) {
        stopped = Delivery{Delivery::Kind::Settings, {}, error.what()};
    }
    if (stopped) {
        stopped->definitelyUnsent = true;
        return applyT3Delivery(std::move(*stopped));
    }
    if (!dispatch) dispatch.emplace(path, SessionDispatchFileLock::Mode::shared);
    if (disconnected()) return connectionUnavailable();
    auto policyGate = ApprovalGate::allow;
    const auto startSend = [&](TaskData& current) {
        if (shutdown.stop_requested() || terminal(current) || current.state == TaskState::cancelling ||
            botLifecycle.botPaused(cmd.ownerBotId))
            return false;
        if (destination) {
            if (!taskRouting.projectRouteReady(id, cmd.actionId)) return false;
        } else if (current.actionId != action.command.actionId)
            return false;
        if (!sessionActionAllowed(current)) {
            policyGate = ApprovalGate::denied;
            return false;
        }
        if (resource) {
            policyGate = assess(current, true);
            if (policyGate != ApprovalGate::allow) return false;
        }
        if (usesExistingSession && current.sourceMessageId) {
            const auto source = conversationMessages.load(*current.sourceMessageId);
            const SessionTarget target{cmd.environmentId, cmd.target.threadId,
                                       cmd.gatewayEnvironmentId == cmd.environmentId ? std::string{} : cmd.gatewayEnvironmentId};
            auto participant = database.query(R"SQL(
SELECT id,person_id FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=? AND state='active'
)SQL",
                                              {cmd.ownerBotId, target.environmentId, target.threadId});
            if (participant.next() && (participant.string(1) == conversationMessages.actor(source).id ||
                                       sessionReadStorage.connectedSessionAccess(source, target)))
                action.sessionParticipationId = participant.number(0);
        }
        if (sendsWork) cmd.instruction = prompts.prepareStandingContext(action, current, clock());
        if (!action.attachments.empty())
            cmd.attachments = attachments.taskAttachments(cmd.ownerBotId, action.attachments, policy.secretRedactor(cmd.ownerBotId));
        if (!outboxStorage.startOutbox(action, cmd.authGeneration)) return false;
        // A refused send is retried until T3 accepts it. One entry per action is enough for the history.
        if (!database
                 .query("SELECT 1 FROM journal WHERE task_id=? AND action_id=? AND kind='send_started' LIMIT 1",
                        {current.id, action.command.actionId})
                 .next())
            journalStorage.journal(current.id, SendStartedEntry{action.command.actionId});
        return true;
    };
    try {
        if (!(destination ? updates.changeTask(id, startSend) : updates.change(task, startSend))) {
            if (policyGate != ApprovalGate::allow && policyGate != ApprovalGate::waiting) refused(policyGate);
            return;
        }
    } catch (const BotNotesUnavailable& error) {
        if (!destination && updates.change(task, [&](TaskData& current) {
                if (current.actionId != cmd.actionId) return false;
                waitStorage.wait(current, SimpleWaitReason::bot_notes_unavailable,
                                 "Restore access to this Bot’s Role and memory. " +
                                     bounded(policy.secretRedactor(current.botId).text(error.what()), 2048));
                return true;
            }))
            return;
        return applyDelivery(id, action, {Delivery::Kind::Settings, {}, error.what(), true});
    } catch (const Conflict& error) {
        return applyDelivery(id, action, {Delivery::Kind::Settings, {}, error.what(), true});
    }
    if (!sessionActionAllowed(task) || (resource && policy.forbidden(cmd.ownerBotId, resource->action, resource->target)) ||
        (usesExistingSession && policy.forbidden(cmd.ownerBotId, Action::environment_launch, cmd.environmentId)))
        return refused(ApprovalGate::denied, OutboxPhase::started);
    contact([&] { return t3.send(cmd); });
}

bool runtime::OutboxDispatch::refreshSessionSettings(TaskData& task, const Outbox& action) {
    if (!intake || action.command.tool == CommandTool::start_in_session) return true;
    Json session;
    try {
        const auto current = intake(task.botId, task.request);
        const auto& environment = task.environmentId;
        const auto& project = task.projectId;
        const bool sameRoute = environment == current.environmentId ? (current.projectId.empty() || project == current.projectId)
                                                                    : current.environmentId.empty();
        if (!sameRoute) return true;
        session = normalizeSessionSettings(current.config.value("session", Json::object()));
    } catch (const std::exception&) {
        return true;
    }
    if (session == task.config.value("session", Json::object())) return true;
    return updates.change(
        task,
        [&](TaskData& current) {
            current.config["session"] = std::move(session);
            return true;
        },
        &task);
}

bool runtime::OutboxDispatch::step(TaskData& task, const Wait* wait) {
    const auto state = task.state;
    if (task.actionId.empty() || clock() < task.t3CheckAt.value_or(0)) return false;
    if (state != TaskState::sending && state != TaskState::deciding && state != TaskState::waiting) return false;
    std::optional<Outbox> action;
    {
        std::lock_guard lock(database.mutex);
        action = outboxStorage.outbox(task);
    }
    if (!action) return false;
    const bool connectionWait = wait && (wait->reason() == WaitReason::auth || wait->reason() == WaitReason::settings);
    if (state == TaskState::waiting && !connectionWait && action->phase != OutboxPhase::started) return false;
    if (action->phase == OutboxPhase::ready && action->command.tool == CommandTool::delegate && action->command.projectId.empty()) {
        if (task.projectId.empty() || task.environmentId != action->command.environmentId) return false;
        const auto& gateway = action->command.gatewayEnvironmentId;
        if (task.gatewayEnvironmentId.value_or(task.environmentId) != (gateway.empty() ? task.environmentId : gateway)) return false;
        std::lock_guard lock(database.mutex);
        if (taskRecords.load(task.id).generation != task.generation) return false;
        action->command.projectId = task.projectId;
        if (!outboxStorage.saveOutbox(*action, OutboxPhase::ready)) return false;
    }
    if (action->phase == OutboxPhase::ready && wait && wait->reason() == WaitReason::settings && !refreshSessionSettings(task, *action))
        return false;
    deliver(task, std::move(*action));
    return true;
}

bool runtime::OutboxDispatch::queueAction(TaskData& task, const std::string& id, CommandTool tool, std::string instruction,
                                          const Json& args, const Json& event, const TaskRoute* destination) {
    if (!destination && !task.actionId.empty()) return false;
    const auto& run = tool == CommandTool::follow_up ? task.lastRun : task.activeRun;
    auto target = run ? run->ref : RunRef{};
    if (tool == CommandTool::delegate && task.selectedSession) {
        if (task.newSessionRequired)
            task.selectedSession.reset();
        else {
            target = {task.selectedSession->target.threadId, {}, {}, {}};
            tool = CommandTool::start_in_session;
        }
    }
    SessionPrompts::WorkPrompt prompt;
    if (tool == CommandTool::follow_up) {
        prompt = prompts.workPrompt(task, event, id.size());
        const auto scrub = policy.secretRedactor(task.botId);
        redactInstructions(prompt.pending, scrub);
        instruction += prompt.instruction;
        instruction = scrub.text(instruction);
    }
    auto route = destination ? *destination : TaskRoute{task.environmentId, task.projectId, task.gatewayEnvironmentId.value_or("")};
    Outbox action;
    auto& cmd = action.command;
    cmd.actionId = id;
    cmd.tool = tool;
    cmd.environmentId = std::move(route.environmentId);
    cmd.projectId = std::move(route.projectId);
    if (route.gatewayEnvironmentId != cmd.environmentId) cmd.gatewayEnvironmentId = std::move(route.gatewayEnvironmentId);
    cmd.target = std::move(target);
    if (tool == CommandTool::answer_session_question) {
        cmd.questionId = field(args, "requestId");
        cmd.answers = args.value("answers", Json::object());
    }
    action.revisionId = task.currentRevision;
    const Id taskId = task.id;
    action.instructions = std::move(prompt.pending);
    if (tool == CommandTool::delegate || tool == CommandTool::start_in_session) action.attachments = task.initialAttachments;
    for (const auto& item : action.instructions)
        action.attachments.insert(action.attachments.end(), item.attachments.begin(), item.attachments.end());
    action.contextIds = std::move(prompt.contextIds);
    action.sourceResultRefs = std::move(prompt.resultReferences);
    action.destinationAction = destination != nullptr;
    if (tool == CommandTool::delegate || tool == CommandTool::start_in_session) {
        if (destination && task.requestedCommit) {
            action.commit = OutboxCommit{*task.requestedCommit};
            instruction = commit_check::instructions(instruction, action.commit->expected);
        }
        action.instructionBase = policy.secretRedactor(task.botId).text(instruction);
    } else
        cmd.instruction = std::move(instruction);
    outboxStorage.insertOutbox(taskId, action);
    if (destination) return true;
    task.actionId = id;
    task.t3CheckAt.reset();
    task.state = TaskState::sending;
    return true;
}

void runtime::OutboxDispatch::queueDestinations(TaskData& task, const std::string& first, std::string_view instruction, const Json& args,
                                                const Json& event) {
    RunResults results;
    for (const auto& target : task.runTargets()) {
        const auto& route = target.route;
        const auto key = results.runs.empty() ? first : actionId();
        queueAction(task, key, CommandTool::delegate, std::string(instruction), args, event, &route);
        DestinationRun run{.actionId = key, .name = target.name, .environmentId = route.environmentId};
        run.assignment.projectId = route.projectId;
        if (route.gatewayEnvironmentId != route.environmentId) run.ref.gatewayEnvironmentId = route.gatewayEnvironmentId;
        results.runs.push_back(std::move(run));
    }
    task.destinations = std::move(results);
    task.state = TaskState::running;
}
}
