#include "session_dispatch_lock.hpp"
#include "runtime_outbox_dispatch.hpp"

#include "memory_storage.hpp"
#include "commit_check.hpp"
#include "conversation_storage.hpp"
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
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "flock/session_ref.hpp"
#include "flock/wait.hpp"
#include "flock/outbox.hpp"
#include "flock/outbox_instruction.hpp"
#include "bot_lifecycle_storage.hpp"
#include "bot_settings_storage.hpp"
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
            updates.refundUnsentWork(current, *action);
            current.actionId.clear();
            waitStorage.clearWait(current, "superseded_instruction");
            database.writeJournal(current.id, "delivery_superseded",
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
    if (action.phase == OutboxPhase::started && c.tool != CommandTool::answer_session_question)
        c.instruction = SessionPrompts::dispatchedWorkPrompt(action);
    c.config = task.config;
    if (action.commit) {
        const auto& commit = action.commit->expected;
        c.config["session"]["workspaceStrategy"] = {{"type", "worktree"}, {"baseRef", commit}, {"startFromOrigin", false}};
        c.config["commitVerification"] = {
            {"expectedCommit", commit}, {"projectId", c.projectId}, {"markedChecks", action.commit->markedChecks}};
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
    database.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)",
                  {cmd.actionId, task.id, cmd.environmentId, ref.threadId, ref.runId, ref.link});
    AssignedRun run{ref, cmd.environmentId, {task.botId, cmd.projectId, action.revisionId}};
    if (destination) {
        destination->assignment = run.assignment;
        destination->ref = run.ref;
    }
    auto sources = Json::array();
    for (const auto& source : action.sourceResultRefs) sources.push_back(sourceJson(source));
    database.writeJournal(task.id, "run_started",
                          {{"run", destination ? destinationRunJson(*destination) : assignedRunJson(run)},
                           {"actionId", cmd.actionId},
                           {"revisionId", revisionJson(action.revisionId)},
                           {"sourceResultRefs", std::move(sources)}});
    return run;
}

void runtime::OutboxDispatch::rememberRunCandidate(Outbox action, const RunRef& ref) {
    action.command.candidate = ref;
    if (action.command.tool == CommandTool::delegate) action.command.launchReceiptThreadId = ref.threadId;
    outboxStorage.saveOutbox(action);
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
    if (phase == OutboxPhase::started && !outcome.run.threadId.empty() && !action.command.candidate) {
        rememberRunCandidate(action, outcome.run);
        run.ref.link = outcome.run.link;
        run.ref.threadId = outcome.run.threadId;
    }
    setDestinationStatus(run, DestinationRunPhase::confirmation_pending, detail);
    run.retryAt = clock() + 5;
    return true;
}

void runtime::OutboxDispatch::applyDelivery(Id id, const Outbox& action, Delivery outcome) {
    updates.changeTask(id, [&](TaskData& task) {
        if (terminal(task)) return false;
        const auto& key = action.command.actionId;
        const bool destination = action.destinationAction;
        auto saved = destination ? outboxStorage.outbox(task, key) : std::nullopt;
        if (destination ? (!saved || (saved->phase != OutboxPhase::ready && saved->phase != OutboxPhase::started))
                        : task.actionId != action.command.actionId)
            return false;
        if (outcome.run.gatewayEnvironmentId.empty()) {
            auto gateway = action.command.gatewayEnvironmentId;
            if (gateway.empty() && !destination) gateway = task.gatewayEnvironmentId.value_or("");
            if (gateway != action.command.environmentId) outcome.run.gatewayEnvironmentId = std::move(gateway);
        }
        if (!outcome.launchWarning.empty()) {
            task.launchWarning = policy.secretRedactor(task.botId).text(outcome.launchWarning);
            database.writeJournal(id, "session_lineage_unavailable", {{"actionId", key}, {"text", *task.launchWarning}});
        }
        const bool cancelling = task.state == TaskState::cancelling;
        const bool reply = action.command.tool == CommandTool::answer_session_question;
        const auto delivery = classifyDelivery(task, action, outcome, reply);

        const bool accepted = delivery == DeliveryOutcome::Accepted && (!destination || saved->phase == OutboxPhase::started);
        if (accepted) outboxStorage.setOutboxPhase(key, OutboxPhase::accepted);
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
                if (task.workflow) {
                    const auto now = clock();
                    database.exec("UPDATE workflow_stages SET confirmed_state='running',confirmed_at=?,unreachable=0,"
                                  "status=CASE WHEN status='pending' THEN 'running' ELSE status END,"
                                  "deadline=CASE WHEN deadline=0 AND json_extract(spec,'$.softDeadlineSeconds')>0 "
                                  "THEN ?+json_extract(spec,'$.softDeadlineSeconds') ELSE deadline END WHERE task_id=?",
                                  {now, now, id});
                }
                instructionDelivery(task, action);
                if ((action.command.tool == CommandTool::delegate || action.command.tool == CommandTool::start_in_session) &&
                    !task.replanning) {
                    task.newSessionRequired = false;
                    task.freshContextRequested = false;
                }
            }
            if (reply)
                database.writeJournal(id, "session_answered",
                                      {{"requestId", action.command.questionId}, {"actionId", action.command.actionId}});
            task.actionId.clear();
            if (!cancelling) task.state = TaskState::running;
            return true;
        }
        if (outcome.kind == Delivery::Kind::QuestionGone && reply) {
            outboxStorage.setOutboxPhase(key, OutboxPhase::closed);
            questions.closeQuestion(task, "question_gone", "resolved_or_expired");
            task.actionId.clear();
            if (!cancelling) task.state = TaskState::running;
            return true;
        }
        if (delivery == DeliveryOutcome::Unknown && !outcome.run.threadId.empty() && action.phase == OutboxPhase::ready && !reply &&
            !cancelling) {
            rememberRunCandidate(action, outcome.run);
            database.writeJournal(id, "run_id_pending", {{"actionId", action.command.actionId}, {"thread", runJson(outcome.run)}});
            return true;
        }
        const auto reason = outcome.kind == Delivery::Kind::Auth       ? SimpleWaitReason::auth
                            : outcome.kind == Delivery::Kind::Settings ? SimpleWaitReason::settings
                                                                       : SimpleWaitReason::unknown_delivery;
        auto detail = outcome.detail.empty() ? "The assigned side effect could not be confirmed" : bounded(outcome.detail, 2048);
        if (reply && delivery == DeliveryOutcome::DefinitelyUnsent) detail = bounded("The Session answer was not sent. " + detail, 2048);
        if (delivery == DeliveryOutcome::DefinitelyUnsent) {
            outboxStorage.setOutboxPhase(key, cancelling ? OutboxPhase::closed : OutboxPhase::ready);
            if (!reply) task.deadline = 0;
            if (cancelling) {
                updates.refundUnsentWork(task, action);
                task.actionId.clear();
            }
        }
        if (cancelling) {
            if (task.cancellationDetail == detail && delivery == DeliveryOutcome::Unknown) return false;
            task.cancellationDetail = detail;
            database.writeJournal(id, "cancelling", {{"detail", detail}, {"actionId", action.command.actionId}});
        } else {
            const auto wait = waitStorage.currentWait(id);
            if (!wait || wait->reason() != waitReason(reason) || wait->text != detail)
                waitStorage.wait(task, reason, std::move(detail));
            else if (delivery == DeliveryOutcome::Unknown)
                return false;
        }
        return true;
    });
}

void runtime::OutboxDispatch::deliver(const TaskData& task, Outbox action) {
    if (shutdown.stop_requested()) return;
    auto cmd = command(task, action);
    const auto id = task.id;
    Delivery outcome;
    const bool usesExistingSession = cmd.tool == CommandTool::follow_up || cmd.tool == CommandTool::start_in_session;
    const bool sendsToSession = usesExistingSession || cmd.tool == CommandTool::answer_session_question;
    const bool preparesTaskContract = cmd.tool == CommandTool::delegate || cmd.tool == CommandTool::start_in_session;
    const bool sendsWork = preparesTaskContract || usesExistingSession;
    const auto sessionActionAllowed = [&](const TaskData& current) {
        if (!cmd.sessionOrigin && !sendsToSession) return true;
        std::lock_guard lock(database.mutex);
        if (const auto& origin = current.sessionOrigin) {
            const auto source = conversationTurns.loadTurnMessage(current.sourceMessageId.value());
            const auto actor = conversationTurns.turnActor(source);
            const auto& parent = origin->target;
            if (!sessionParticipationStorage.activeSessionOrigin(cmd.ownerBotId, actor.id, parent, cmd.environmentId, source.id) ||
                !policy.allows(actor, Action::bot_decide, cmd.ownerBotId) ||
                !policy.allows(cmd.ownerBotId, Action::environment_inspect, parent.environmentId))
                return false;
            if (!sendsToSession) return true;
        }
        if (sessionParticipationStorage.sessionParticipationStopped(
                cmd.ownerBotId,
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
    const bool destination = action.destinationAction;
    const bool firstSend = action.phase == OutboxPhase::ready;
    std::optional<SessionDispatchFileLock> dispatch;
    if (destination) dispatch.emplace(path, SessionDispatchFileLock::Mode::shared);
    if (firstSend || destination) {
        std::lock_guard lock(database.mutex);
        const auto current = taskRecords.load(id);
        if (firstSend &&
            (botLifecycle.botPaused(cmd.ownerBotId) || current.state == TaskState::cancelling || current.generation != task.generation))
            return;
        if (destination) {
            const auto saved = outboxStorage.outbox(current, cmd.actionId);
            if (saved && saved->stop) return;
        }
    }
    const auto operation = std::string(destination ? runOutboxOperation : "outbox/") + cmd.actionId;
    const auto refused = [&](ApprovalGate gate) {
        if (destination) return applyDelivery(id, action, {Delivery::Kind::NotAccepted, {}, std::string(approvalError(gate)), true});
        updates.change(task, [&](TaskData& current) {
            if (current.actionId != action.command.actionId) return false;
            if (!outboxStorage.setOutboxPhase(action.command.actionId, OutboxPhase::closed, OutboxPhase::ready)) return false;
            updates.refundUnsentWork(current, action);
            current.actionId.clear();
            current.state = !current.activeRun ? TaskState::deciding : TaskState::running;
            current.feedback = approvalError(gate);
            if (current.state == TaskState::deciding)
                taskEvents.event(current, "policy/" + cmd.actionId, {{"kind", "policy_outcome"}, {"text", approvalError(gate)}});
            database.writeJournal(id, "policy_action_closed", {{"actionId", cmd.actionId}, {"text", approvalError(gate)}});
            return true;
        });
    };
    const auto disconnected = [&] {
        const auto& connectionEnvironment = cmd.gatewayEnvironmentId.empty() ? cmd.environmentId : cmd.gatewayEnvironmentId;
        return requireT3Connection && !sessionParticipationStorage.t3EnvironmentConnected(connectionEnvironment);
    };
    const auto connectionUnavailable = [&] {
        applyDelivery(id, action, {Delivery::Kind::Auth, {}, "Connect this T3 environment in Flock settings before sending work.", true});
    };
    if (firstSend) {
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
            } catch (const Conflict& error) {
                return applyDelivery(id, action, {Delivery::Kind::Settings, {}, error.what(), true});
            }
        }
        if (sendsWork) {
            const auto scrub = policy.secretRedactor(cmd.ownerBotId);
            if (task.workflow && scrub.text(task.workflow->inputArtifactVersion) != task.workflow->inputArtifactVersion)
                return applyDelivery(
                    id, action, {Delivery::Kind::Settings, {}, "The pinned workflow artifact version contains a protected value", true});
            const auto marker = sessionPrompt(cmd.actionId, {});
            if (cmd.instruction.ends_with(marker)) cmd.instruction.resize(cmd.instruction.size() - marker.size());
            action.sessionMemory.reset();
            cmd.instruction = scrub.text(cmd.instruction);
            action.command.instruction = cmd.instruction;
            cmd.instruction = sessionPrompt(cmd.actionId, cmd.instruction);
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
        const auto assess = [&](const TaskData& current, bool claim) {
            if (cmd.tool == CommandTool::delegate)
                return policyApprovals.approveTaskLaunch(current, {cmd.environmentId, cmd.projectId, cmd.gatewayEnvironmentId}, operation,
                                                         payload, clock(), claim);
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
                if (destination)
                    destinationStatus(id, cmd.actionId, DestinationRunPhase::confirmation_pending, "Waiting for launch approval");
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
            return applyDelivery(id, action, *stopped);
        }
        if (!dispatch) dispatch.emplace(path, SessionDispatchFileLock::Mode::shared);
        if (disconnected()) return connectionUnavailable();
        const auto sharesMemory = [](const Json& config) { return config.value("shareMemoryWithSessions", Json(false)) == true; };
        bool includeMemory = false;
        if (sendsWork) {
            try {
                includeMemory = intake ? sharesMemory(intake(cmd.ownerBotId, {}).config) : sharesMemory(cmd.config);
            } catch (const std::exception&) {
            }
        }
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
            if (const auto message = current.requestMessageId(); usesExistingSession && message) {
                const auto source = conversationTurns.loadTurnMessage(*message);
                const SessionTarget target{cmd.environmentId, cmd.target.threadId,
                                           cmd.gatewayEnvironmentId == cmd.environmentId ? std::string{} : cmd.gatewayEnvironmentId};
                auto participant = database.query(R"SQL(
SELECT id,person_id FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=? AND state='active'
)SQL",
                                                  {cmd.ownerBotId, target.environmentId, target.threadId});
                if (participant.next() && (participant.string(1) == conversationTurns.turnActor(source).id ||
                                           sessionReadStorage.connectedSessionAccess(source, target)))
                    action.sessionParticipationId = participant.number(0);
            }
            if (includeMemory) {
                if (const auto memory = memoryStorage.sessionMemory(cmd, clock())) {
                    action.sessionMemory = *memory;
                    cmd.instruction = SessionPrompts::dispatchedWorkPrompt(action);
                }
            }
            if (!outboxStorage.startOutbox(action, cmd.authGeneration)) return false;
            if (cmd.tool != CommandTool::answer_session_question) {
                auto* run = destination ? &destinationRun(current, cmd.actionId) : nullptr;
                if (!(run ? run->deadline.value_or(0) : current.deadline)) {
                    const auto deadline =
                        clock() + botSettingsStorage.environmentTimeout(cmd.ownerBotId, cmd.environmentId, current.config);
                    if (run)
                        run->deadline = deadline;
                    else
                        current.deadline = deadline;
                }
            }
            journalStorage.journal(current.id, SendStartedEntry{action.command.actionId});
            return true;
        };
        if (!(destination ? updates.changeTask(id, startSend) : updates.change(task, startSend))) {
            if (policyGate != ApprovalGate::allow && policyGate != ApprovalGate::waiting) refused(policyGate);
            return;
        }
    } else if (action.phase != OutboxPhase::started)
        return;
    try {
        if (firstSend)
            outcome = !sessionActionAllowed(task) || (resource && policy.forbidden(cmd.ownerBotId, resource->action, resource->target)) ||
                              (usesExistingSession && policy.forbidden(cmd.ownerBotId, Action::environment_launch, cmd.environmentId))
                          ? Delivery{Delivery::Kind::NotAccepted, {}, "Session action denied by installation policy.", true}
                          : t3.send(cmd);
        else
            outcome = t3.reconcile(cmd);
    } catch (const std::exception&) {
        outcome.detail = "T3 communication failed; check the saved action before retrying";
    }
    applyDelivery(id, action, std::move(outcome));
}

void runtime::OutboxDispatch::step(const TaskData& task) {
    const auto state = task.state;
    const bool reconcile = state == TaskState::waiting && (task.workflow || task.selectedSession) && !task.actionId.empty();
    if (state != TaskState::sending && !reconcile) return;
    std::optional<Outbox> action;
    {
        std::lock_guard lock(database.mutex);
        action = outboxStorage.outbox(task);
    }
    if (!action) return;
    if (reconcile) {
        const auto now = clock();
        const bool selectedStart = action->command.tool == CommandTool::start_in_session;
        if (!selectedStart && !task.workflow) return;
        const auto reconcileAt = selectedStart ? &TaskData::sessionReconcileAt : &TaskData::workflowReconcileAt;
        if (action->phase != OutboxPhase::started || now < (task.*reconcileAt).value_or(0)) return;
        if (!updates.change(task, [&](TaskData& current) {
                current.*reconcileAt = now + 5;
                return true;
            }))
            return;
    }
    deliver(task, std::move(*action));
}

void runtime::OutboxDispatch::queueAction(TaskData& task, const std::string& id, CommandTool tool, std::string instruction,
                                          const Json& args, const Json& event, bool charge, const TaskRoute* destination) {
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
    if (tool != CommandTool::answer_session_question) task.deadline = 0;
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
    action.contextIds = std::move(prompt.contextIds);
    action.sourceResultRefs = std::move(prompt.resultReferences);
    action.chargesBudget = charge;
    action.destinationAction = destination != nullptr;
    if (tool == CommandTool::delegate || tool == CommandTool::start_in_session) {
        if (destination && task.requestedCommit) {
            action.commit = OutboxCommit{*task.requestedCommit, true};
            instruction = commit_check::instructions(instruction, action.commit->expected);
        }
        action.instructionBase = policy.secretRedactor(task.botId).text(instruction);
    } else
        cmd.instruction = std::move(instruction);
    outboxStorage.insertOutbox(taskId, action);
    if (destination) return;
    task.actionId = id;
    task.state = TaskState::sending;
}

void runtime::OutboxDispatch::queueDestinations(TaskData& task, const std::string& first, std::string_view instruction, const Json& args,
                                                const Json& event, bool charge) {
    RunResults results;
    for (const auto& target : task.runTargets()) {
        const auto& route = target.route;
        const auto key = results.runs.empty() ? first : actionId();
        queueAction(task, key, CommandTool::delegate, std::string(instruction), args, event, charge, &route);
        DestinationRun run{.actionId = key, .name = target.name, .environmentId = route.environmentId};
        run.assignment.projectId = route.projectId;
        if (route.gatewayEnvironmentId != route.environmentId) run.ref.gatewayEnvironmentId = route.gatewayEnvironmentId;
        results.runs.push_back(std::move(run));
    }
    task.destinations = std::move(results);
    task.state = TaskState::running;
}
}
