#include "storage.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "journal_peer.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "runtime_maintenance.hpp"
#include "runtime_task_scheduler.hpp"
#include "task_context_storage.hpp"
#include "task_interaction_storage.hpp"
#include "task_storage.hpp"
#include "commit_check.hpp"
#include "conversation_storage.hpp"
#include "flock/bot_profile.hpp"
#include "flock/participation.hpp"

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <map>
#include <set>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace flockv0 {
namespace {
bool present(std::string_view value, std::size_t limit = limits::session_prompt::maxDecisionTextBytes) {
    return value.size() <= limit && value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}
bool hasRun(const RunRef& r) {
    return !r.threadId.empty() && !r.runId.empty();
}

Json revisionJson(std::optional<Id> revision) {
    return revision ? Json(*revision) : Json(nullptr);
}

struct SourceQuotes {
    const SecretRedactor& scrub;
    std::size_t remaining = limits::session_prompt::maxQuoteTextBytes;
    std::set<Id> included{};

    std::string quote(const OutboxSource& source, std::size_t limit = limits::session_prompt::maxQuoteTextBytes) {
        const auto id = source.messageId;
        if (!id || !included.insert(id).second) return {};
        auto text = "\n\nReferenced result " + std::to_string(id) + " from task " + std::to_string(source.taskId);
        if (source.stageId) text += "\nStage: " + *source.stageId;
        if (source.run) text += "\nSource Run: " + sourceRunJson(*source.run).dump();
        if (source.resultRunId) text += "\nReport Run: " + *source.resultRunId;
        text += " (reference only):";
        const auto excerpt = bounded(scrub.text(source.text.value_or("")), std::min(remaining, limit));
        remaining -= excerpt.size();
        if (!excerpt.empty()) text += "\n" + excerpt;
        return scrub.text(text);
    }
};

std::vector<OutboxInstruction> pendingBatch(std::span<const OutboxInstruction> pending, std::size_t promptBudgetBytes,
                                            SourceQuotes quotes) {
    std::vector<OutboxInstruction> batch;
    std::size_t serializedBytes = 0, promptBytes = 0;
    for (const auto& item : pending) {
        serializedBytes += instructionJson(item).dump().size();
        promptBytes += 2 + item.text.size();
        if (item.source) promptBytes += quotes.quote(*item.source).size();
        if (!batch.empty() && (serializedBytes > limits::session_prompt::pendingBatchJsonThresholdBytes || promptBytes > promptBudgetBytes))
            break;
        batch.push_back(item);
    }
    return batch;
}

struct TaskInference {
    Id generation, eventId, attemptId;
    std::optional<Id> revisionId;
    Json usage = Json::object();
    std::variant<ModelCall, ModelError> outcome{};
};

bool checkpointMatches(const DecisionCheckpoint& checkpoint, const TaskData& task) {
    return checkpoint.eventId == task.eventId && checkpoint.generation == task.generation && checkpoint.revisionId == task.currentRevision;
}

std::string gitInvocation(const TaskDecision& decision) {
    return "attempt/" + std::to_string(decision.attemptId);
}

struct SessionContextLimit : Conflict {
    SessionContextLimit()
        : Conflict("The Session prompt exceeds T3's size limit. "
                   "Continue the existing Session if possible, or revise the task before starting a new Session.") {}
};

bool needsDoneWhen(const TaskData& task) {
    return !task.currentRevision && (!task.criteriaProposal || task.criteriaProposal->doneWhen.empty());
}

void includeInstructions(std::string& instruction, std::span<const OutboxInstruction> pending) {
    if (pending.empty()) return;
    instruction += "\n\nUser instructions accepted for this task, oldest first. Apply later directions where they replace earlier ones:";
    for (const auto& item : pending) instruction += "\n\n" + item.text;
}

std::string dispatchedWorkPrompt(const Outbox& action) {
    auto instruction = action.command.instruction;
    if (action.sessionMemory) instruction += action.sessionMemory->section;
    return sessionPrompt(action.command.actionId, instruction);
}

}

struct Runtime::Impl {
    Store::Impl& db;
    Policy policy;
    Model& model;
    T3& t3;
    SoulLoader soul;
    RuntimeOptions options;
    Maintenance maintenance;
    TaskScheduler taskScheduler;
    std::future<void> channelWorker, sessionWakeWorker;
    std::stop_source shutdown;
    int lockFd = -1;
    std::map<TaskScheduler::Work, std::future<void>> workers;
    // Keep provider outcomes until the Task decision and attempt usage are committed together.
    std::map<Id, TaskInference> taskInferences;
    struct HistoryUnavailable : ModelError {
        using ModelError::ModelError;
    };
    std::map<std::string, std::future<void>> conversationWorkers;

    void runTurn(Id id) {
        db.runTurn(id, model, t3, soul, options.clock, *options.botTools, options.gitHost.get(), options.applyBotChange,
                   options.reconcileBotCreation, options.intake, options.peers, shutdown.get_token());
    }

    Impl(Store& store, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : db(*store.p_), policy(store.policy()), model(m), t3(adapter), soul(std::move(loader)), options(std::move(opts)),
          maintenance(db, options.clock, options.automationError, options.storageContention), taskScheduler(db) {
        if (!options.botTools) options.botTools = std::make_shared<BotTools>();
        const auto lockPath = std::filesystem::canonical(db.path).string() + ".runtime.lock";
        lockFd = open(lockPath.c_str(), O_CLOEXEC | O_CREAT | O_RDWR, 0600);
        if (lockFd < 0 || ::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
            if (lockFd >= 0) close(lockFd);
            throw Conflict("Cannot acquire the exclusive Runtime lock for this database");
        }
        {
            std::lock_guard lock(db.mutex);
            db.policyClock = options.clock;
        }
    }
    ~Impl() {
        shutdown.request_stop();
        conversationWorkers.clear();
        workers.clear();
        if (channelWorker.valid()) channelWorker.wait();
        if (sessionWakeWorker.valid()) sessionWakeWorker.wait();
        try {
            persistTaskInferences();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Flock Task inference shutdown: %s\n", error.what());
        }
        {
            std::lock_guard lock(db.mutex);
            db.policyClock = unixTime;
        }
        close(lockFd);
    }

    void saveTask(TaskData& task, std::optional<DecisionCheckpoint> resumedDecision = std::nullopt) {
        if (resumedDecision) resumedDecision->generation = task.generation + 1;
        task.pendingDecision = std::move(resumedDecision);
        db.save(task);
    }

    void consumeEvent(TaskData& task) {
        task.feedback.reset();
        db.consumeEvent(task);
    }

    template <class F> bool changeTask(Id id, F fn, TaskData* saved = nullptr) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        auto task = db.load(id);
        if (!fn(task)) return false;
        saveTask(task);
        transaction.commit();
        if (saved) *saved = std::move(task);
        return true;
    }

    template <class F> bool change(const TaskData& snapshot, F fn, TaskData* saved = nullptr) {
        return changeTask(
            snapshot.id,
            [&](TaskData& task) {
                return task.generation == snapshot.generation && task.eventId == snapshot.eventId &&
                       task.currentRevision == snapshot.currentRevision && fn(task);
            },
            saved);
    }

    static constexpr unsigned batchLimit = 4;

    TaskData snapshot(Id id) {
        std::lock_guard lock(db.mutex);
        return db.load(id);
    }

    bool selectProject(TaskData& task) {
        const auto bot = task.botId;
        const auto environment = task.environmentId;
        {
            std::lock_guard lock(db.mutex);
            if (db.projectQuestionOpen(bot, environment)) return false;
        }
        auto catalog = db.readProjectCatalog(t3, bot, environment, options.requireT3Connection);
        return change(
                   task,
                   [&](TaskData& current) { return !shutdown.stop_requested() && db.applyProjectCatalog(current, std::move(catalog)); },
                   &task) &&
               !task.projectId.empty();
    }

    void activate(TaskData task) {
        if (task.projectId.empty() && !selectProject(task)) return;
        change(task, [&](TaskData& current) {
            auto turn = db.query("SELECT 1 FROM task_project_slots WHERE id=? AND blocker_id IS NULL", {current.id});
            if (!turn.next()) return false;
            if (current.workflow && !current.lastRun) {
                queueAction(current, actionId(), CommandTool::delegate, "", Json::object(), Json::object(), false);
                return true;
            }
            if (current.eventId == 0) db.event(current, "intake", {{"kind", "intake"}});
            current.state = TaskState::deciding;
            return true;
        });
    }

    void refundUnsentWork(TaskData& task, const Outbox& action) {
        if (action.chargesBudget) {
            ++task.allowance.remaining;
            --task.allowance.additionalUsed;
        }
    }

    void cancelDone(TaskData& task) {
        closeQuestion(task, "cancelled");
        if (!task.actionId.empty()) {
            const auto action = db.outbox(task);
            if (action && db.setOutboxPhase(action->command.actionId, OutboxPhase::closed, OutboxPhase::ready))
                refundUnsentWork(task, *action);
            task.actionId.clear();
        }
        task.runStop.reset();
        task.replanning = false;
        task.pendingDestination.reset();
        task.queuedInT3 = false;
        task.cancelledInstructions = std::exchange(task.pendingInstructions, {});
        task.activeRun.reset();
        task.state = TaskState::cancelled;
        db.journalCancelled(task.id);
        if (const auto peer = db.peerExchange(task.id); peer && peer->recipient == task.id)
            finishPeer(*peer, task, db.load(peer->sender), "cancelled",
                       task.cancelledByBotPause ? "The receiving Bot is paused." : "The receiving Bot's task was cancelled.", actionId());
    }

    void stopNote(const TaskData& snapshot, const std::string& detail) {
        change(snapshot, [&](TaskData& task) {
            const bool cancel = task.state == TaskState::cancelling;
            auto& saved = cancel ? task.cancellationDetail : task.replanningDetail;
            if (saved == detail) return false;
            saved = detail;
            db.journal(task.id, cancel ? "cancelling" : "replanning", {{"detail", detail}, {"run", assignedRunJson(task.activeRun)}});
            return true;
        });
    }

    void closeQuestion(TaskData& task, std::string_view reason, std::string_view resolution = {}) {
        db.clearWait(task, reason);
        db.unlockQuestion(task, resolution.empty() ? reason : resolution);
    }

    void suspendQuestion(TaskData& task) {
        if (!task.question) return;
        auto& question = *task.question;
        if (const auto wait = db.currentWait(task.id)) {
            const auto reason = wait->reason();
            if (reason == WaitReason::user_question || reason == WaitReason::model_budget || reason == WaitReason::model_failure)
                question.suspendedWaitId = wait->id;
        }
        const auto& pending = task.pendingDecision;
        if (pending && checkpointMatches(*pending, task)) question.suspendedDecision = *pending;
    }

    std::optional<DecisionCheckpoint> resumeQuestion(TaskData& task) {
        auto& question = *task.question;
        const auto& target = question.target;
        const auto waitId = std::exchange(question.suspendedWaitId, std::nullopt);
        Id restoredQuestion = 0;
        db.clearWait(task, "connection_restored");
        task.state = TaskState::deciding;
        if (waitId) {
            auto row =
                db.query("SELECT id,reason,data FROM waits WHERE id=? AND task_id=? AND resolution='superseded'", {*waitId, task.id});
            if (row.next()) {
                auto wait = decodeWaitRow(row);
                const auto reason = wait.reason();
                const auto* asked = std::get_if<ModelQuestionWait>(&wait.details);
                if (wait.eventId == target.eventId && wait.run == target.run &&
                    (reason != WaitReason::user_question || (asked && asked->questionId == target.requestId))) {
                    const auto restored = db.wait(task, std::move(wait));
                    if (reason == WaitReason::user_question) restoredQuestion = restored;
                }
            }
        }
        auto decision = std::exchange(question.suspendedDecision, std::nullopt);
        question.target = questionTarget(task, restoredQuestion);
        if (decision && (decision->revisionId != task.currentRevision || decision->eventId != task.eventId)) return std::nullopt;
        return decision;
    }

    void writeRunReport(TaskData& task, const SettledRunReport& report) {
        const auto entry = runReportEntry(report);
        const auto& run = report.run.ref;
        if (task.state != TaskState::cancelling && !task.replanning) {
            db.event(task, "result/" + report.run.environmentId + "/" + run.threadId + "/" + run.runId, encodeRunReport(entry));
        }
        const auto reportId = db.journal(task.id, entry);
        if (task.workflow) {
            task.pendingStageReport = reportId;
            db.settleStageReport(task);
        }
    }

    RunEvidence fetchRunEvidence(const TaskData& task, const std::string& environment, RunRef source, std::string_view resultRunId,
                                 std::string_view status) {
        source.runId = resultRunId;
        RunEvidence evidence;
        try {
            evidence = t3.readRunEvidence(environment, task.config, source, status);
        } catch (const std::exception& error) {
            evidence.availability = RunEvidence::Availability::ReportUnavailable;
            evidence.detail = error.what();
        }
        const auto scrub = db.redactor(task.botId);
        evidence.report = scrub.text(evidence.report);
        evidence.detail = scrub.text(evidence.detail);
        if (scrub.text(evidence.itemId) != evidence.itemId) evidence.itemId.clear();
        return evidence;
    }

    void recoverRunReport(const TaskData& task) {
        const auto& pending = task.pendingRunReport.value();
        if (options.clock() < pending.retryAt) return;
        auto report = pending.report;
        auto evidence = fetchRunEvidence(task, report.run.environmentId, report.run.ref, report.resultRunId, report.status);
        if (evidence.sessionCreatedAt.empty()) evidence.sessionCreatedAt = std::move(report.evidence.sessionCreatedAt);
        report.evidence = std::move(evidence);
        change(task, [&](TaskData& current) {
            if (report.evidence.availability == RunEvidence::Availability::ReportUnavailable && pending.attempts == 0) {
                current.pendingRunReport = PendingRunReport{std::move(report), options.clock() + 10, 1};
            } else {
                current.pendingRunReport.reset();
                writeRunReport(current, report);
            }
            return true;
        });
    }

    bool ingest(const TaskData& snapshot, const Observation& observed) {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        auto task = db.load(snapshot.id);
        if (task.generation != snapshot.generation || task.eventId != snapshot.eventId ||
            task.currentRevision != snapshot.currentRevision || task.activeRun != snapshot.activeRun)
            return false;
        if (task.workflow) db.stageObservation(task, observed, options.clock());
        std::optional<DecisionCheckpoint> resumedDecision;
        const bool changed = [&] {
            const auto run = task.activeRun.value().ref;
            const auto state = task.state;
            const auto wait = db.currentWait(task.id);
            const auto reason = wait ? std::optional{wait->reason()} : std::nullopt;
            const bool connectionWait = reason == WaitReason::auth || reason == WaitReason::settings;
            const bool replanning = task.replanning;
            if (observed.kind == Observation::Kind::Finished) {
                closeQuestion(task, "run_finished");
                if (!task.actionId.empty()) {
                    const auto action = db.outbox(task);
                    const bool supersededAnswer = action && action->command.tool == CommandTool::answer_session_question;
                    if (supersededAnswer) {
                        db.setOutboxPhase(action->command.actionId, OutboxPhase::closed);
                        task.actionId.clear();
                    }
                }
                task.lastRun = std::move(task.activeRun);
                task.activeRun.reset();
                task.queuedInT3 = false;
                consumeEvent(task);
                SettledRunReport report{
                    task.lastRun.value(),
                    observed.resultRunId.empty() ? run.runId : observed.resultRunId,
                    observed.status,
                    {observed.evidence, observed.report, observed.itemId, observed.detail, observed.truncated, observed.sessionCreatedAt},
                    observed.status == "interrupted" && (state == TaskState::cancelling || replanning)};
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'result',?)", {task.environmentId, run.threadId, run.runId});
                if (sqlite3_changes(db.db)) {
                    if (observed.evidence == RunEvidence::Availability::ReportUnavailable && !observed.resultRunId.empty() &&
                        state != TaskState::cancelling && !replanning) {
                        db.journalRunSettled(task.id, report);
                        task.pendingRunReport = PendingRunReport{std::move(report), options.clock() + 5, 0};
                    } else
                        writeRunReport(task, report);
                }
                if (task.runStop) {
                    db.journal(task.id, "interrupt_settled", runStopJson(*task.runStop));
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
                        db.clearWait(task, "connection_restored");
                        task.state = TaskState::running;
                    }
                    if (task.queuedInT3) return connectionWait;
                    task.queuedInT3 = true;
                    task.deadline = 0;
                    return true;
                }
                leftQueue = std::exchange(task.queuedInT3, false);
                if (leftQueue) task.deadline = options.clock() + db.environmentTimeout(task.botId, task.environmentId, task.config);
            }
            if (observed.kind == Observation::Kind::Question) {
                if (observed.question.requestId.empty()) return false;
                if (task.question && task.question->target.requestId == observed.question.requestId) {
                    if (!connectionWait && state != TaskState::running) return false;
                    auto saved = task.question->target;
                    saved.waitId.reset();
                    if (saved != questionTarget(task)) return false;
                    resumedDecision = resumeQuestion(task);
                    return true;
                }
                db.exec("INSERT OR IGNORE INTO t3_events VALUES(?,?,'question',?)",
                        {task.environmentId, run.threadId, observed.question.requestId});
                if (!sqlite3_changes(db.db)) return false;
                closeQuestion(task, "session_question", "replaced");
                consumeEvent(task);
                const Json data = {{"kind", "question"}, {"run", runJson(run)}, {"question", questionJson(observed.question)}};
                db.event(task, "question/" + task.environmentId + "/" + run.threadId + "/" + observed.question.requestId, data);
                task.question =
                    TaskQuestion{observed.question.fields, {std::nullopt, task.eventId, taskWaitRun(task), observed.question.requestId}};
                db.journal(task.id, "session_question", data);
                task.state = TaskState::deciding;
                return true;
            }
            if (observed.kind == Observation::Kind::ExternalInput) {
                if (reason == WaitReason::t3_input) return false;
                db.unlockQuestion(task, "requires_t3");
                consumeEvent(task);
                db.wait(task, SimpleWaitReason::t3_input, bounded(observed.status, 2048));
                return true;
            }
            if (observed.kind == Observation::Kind::Unavailable) {
                if (observed.status == "auth" && reason != WaitReason::auth) {
                    suspendQuestion(task);
                    db.wait(task, SimpleWaitReason::auth, "T3 authentication is unavailable");
                    return true;
                }
                if (observed.status == "settings" && reason != WaitReason::settings) {
                    suspendQuestion(task);
                    db.wait(task, SimpleWaitReason::settings, "T3 connection settings need attention");
                    return true;
                }
            }
            const bool available = observed.kind != Observation::Kind::Unavailable;
            if (available && (reason == WaitReason::t3_input || task.question)) {
                closeQuestion(task, "session_question_resolved", "resolved_in_t3");
                consumeEvent(task);
                task.state = TaskState::running;
                return true;
            }
            if (available && connectionWait) {
                db.clearWait(task, "connection_restored");
                task.state = TaskState::running;
                return true;
            }
            if (state == TaskState::running && task.deadline > 0 && options.clock() >= task.deadline) {
                db.wait(task, DeadlineWait{}, "This request reached its deadline");
                return true;
            }
            return leftQueue;
        }();
        if (changed) saveTask(task, std::move(resumedDecision));
        tx.commit();
        return changed;
    }

    Observation observeRun(const std::string& bot, const std::string& environment, const Json& config, const RunRef& run) {
        try {
            auto observed = t3.observe(environment, config, run);
            const auto scrub = db.redactor(bot);
            redactQuestion(observed.question, scrub);
            for (auto* value : {&observed.resultRunId, &observed.itemId, &observed.checkedCommit})
                if (scrub.text(*value) != *value) value->clear();
            observed.report = scrub.text(observed.report);
            observed.detail = scrub.text(observed.detail);
            if (observed.status != "auth" && observed.status != "settings" && observed.status != "completed" &&
                observed.status != "failed" && observed.status != "cancelled" && observed.status != "interrupted" &&
                observed.status != "rolled_back")
                observed.status = scrub.text(observed.status);
            return observed;
        } catch (const std::exception&) {
            Observation observation;
            observation.kind = Observation::Kind::Unavailable;
            return observation;
        }
    }

    Observation observe(const TaskData& task) {
        const auto& run = task.activeRun.value();
        return observeRun(task.botId, run.environmentId, task.config, run.ref);
    }

    void interruptRun(const TaskData& task) {
        const auto& stop = task.runStop.value();
        if (!task.activeRun || task.activeRun != stop.run ||
            (task.state == TaskState::cancelling) != (stop.intent == RunStopIntent::cancel))
            return;
        const auto observed = observe(task);
        if (ingest(task, observed)) return;
        if (observed.kind == Observation::Kind::Settling) return stopNote(task, "Waiting for the remaining work on this request to finish");
        if (observed.kind == Observation::Kind::Unavailable && !(task.workflow && stop.intent == RunStopIntent::cancel))
            return stopNote(task, "Flock couldn't check the status of this work in T3");
        const auto now = options.clock();
        if (now < stop.retryAt.value_or(0)) return;
        TaskData requested;
        if (!change(
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
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        db.journalInterrupt(task.id, stop);
        transaction.commit();
    }

    void finishReplan(const TaskData& task) {
        change(task, [&](TaskData& current) {
            const auto id = current.id;
            const auto peer = db.peerExchange(id);
            if (peer && !peer->allowsReplan(id)) return false;
            const auto oldEnvironment = current.environmentId;
            const auto oldProject = current.projectId;
            const auto oldGateway =
                current.gatewayEnvironmentId && !current.gatewayEnvironmentId->empty() ? *current.gatewayEnvironmentId : oldEnvironment;
            const auto destination = current.pendingDestination;
            const bool moving = destination && (destination->route.environmentId != oldEnvironment ||
                                                destination->route.gatewayEnvironmentId != oldGateway ||
                                                (destination->pinned && destination->route.projectId != oldProject));
            const auto event = db.currentEvent(current);
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
                db.journal(id, "session_destination_changed",
                           {{"from", oldEnvironment},
                            {"to", destination->route.environmentId},
                            {"fromProject", oldProject},
                            {"toProject", current.projectId},
                            {"revisionId", revisionJson(current.currentRevision)}});
            }
            if (destination && (destination->pinned || destination->route.projectId.empty())) current.pendingDestination.reset();
            closeQuestion(current, "user_instruction");
            if (!preservePeerAnswer) {
                consumeEvent(current);
                const auto report = settledSessionReport(current);
                db.event(current, "replan/" + std::to_string(current.generation),
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

    void stop(TaskData task) {
        const bool cancel = task.state == TaskState::cancelling;
        const auto intent = cancel ? RunStopIntent::cancel : RunStopIntent::replan;
        if (task.pendingRunReport) {
            if (!change(
                    task,
                    [&](TaskData& current) {
                        const auto report = std::move(current.pendingRunReport.value().report);
                        current.pendingRunReport.reset();
                        writeRunReport(current, report);
                        return true;
                    },
                    &task))
                return;
        }
        std::optional<Outbox> action;
        {
            std::lock_guard lock(db.mutex);
            action = db.outbox(task);
        }
        if (task.activeRun || (action && action->phase == OutboxPhase::started)) {
            const auto& saved = task.runStop;
            if (!saved || saved->intent != intent || saved->run != task.activeRun) {
                if (!change(
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
            const auto now = options.clock();
            const bool reply = action->command.tool == CommandTool::answer_session_question;
            if (now >= task.runStop.value().reconcileAt.value_or(0) && change(task, [&](TaskData& current) {
                    current.runStop.value().reconcileAt = now + 3;
                    return true;
                }))
                deliver(task, *action);
            if (reply && task.activeRun) interruptRun(snapshot(task.id));
            return;
        }
        if (!cancel && action && action->phase == OutboxPhase::ready) {
            change(task, [&](TaskData& current) {
                if (!db.setOutboxPhase(action->command.actionId, OutboxPhase::superseded, OutboxPhase::ready)) return false;
                refundUnsentWork(current, *action);
                current.actionId.clear();
                db.clearWait(current, "superseded_instruction");
                db.journal(current.id, "delivery_superseded",
                           {{"actionId", action->command.actionId}, {"revisionId", revisionJson(current.currentRevision)}});
                return true;
            });
            return;
        }
        if (task.activeRun)
            interruptRun(task);
        else if (cancel)
            change(task, [&](TaskData& current) {
                cancelDone(current);
                return true;
            });
        else
            finishReplan(task);
    }

    Command command(const TaskData& task, const Outbox& action) {
        Command c{action.command};
        c.authGeneration = action.authGeneration;
        c.ownerBotId = task.botId;
        if (action.phase == OutboxPhase::started && c.tool != CommandTool::answer_session_question)
            c.instruction = dispatchedWorkPrompt(action);
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

    void instructionDelivery(TaskData& task, const Outbox& action) {
        if (action.instructions.empty()) return;
        auto& pending = task.pendingInstructions;
        for (const auto& item : action.instructions) {
            const auto copy = std::ranges::find_if(
                pending, [&](const OutboxInstruction& p) { return p.messageId == item.messageId && p.step == item.step; });
            if (copy != pending.end()) pending.erase(copy);
            db.journal(task.id, DeliveredInstructionEntry{action.command.actionId, item, action.revisionId, task.activeRun});
        }
    }

    bool assignedRunMatches(const TaskData& task, const CommandPayload& command, const RunRef& run) {
        const auto& environment = command.environmentId;
        auto owner = db.query("SELECT action_id FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                              {environment, run.threadId, run.runId});
        if (owner.next() && owner.string(0) != command.actionId) return false;
        if (command.tool == CommandTool::start_in_session) return run.threadId == command.target.threadId;
        if (command.tool == CommandTool::follow_up) {
            const auto& target = command.target;
            if (run.threadId != target.threadId || run.runId == target.runId) return false;
            auto prior = db.query("SELECT 1 FROM runs WHERE task_id=? AND environment_id=? AND thread_id=? AND run_id=?",
                                  {task.id, environment, target.threadId, target.runId});
            return prior.next();
        }
        auto prior = db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=? AND action_id<>? LIMIT 1",
                              {environment, run.threadId, command.actionId});
        return !prior.next();
    }

    enum class DeliveryOutcome { Accepted, DefinitelyUnsent, Unknown };

    DeliveryOutcome classifyDelivery(const TaskData& task, const Outbox& action, Delivery& outcome, bool reply) {
        const auto scrub = db.redactor(task.botId);
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

    AssignedRun recordRunAssignment(const TaskData& task, const Outbox& action, const RunRef& ref, DestinationRun* destination = nullptr) {
        const auto& cmd = action.command;
        db.exec("INSERT OR IGNORE INTO runs VALUES(?,?,?,?,?,?)",
                {cmd.actionId, task.id, cmd.environmentId, ref.threadId, ref.runId, ref.link});
        AssignedRun run{ref, cmd.environmentId, {task.botId, cmd.projectId, action.revisionId}};
        if (destination) {
            destination->assignment = run.assignment;
            destination->ref = run.ref;
        }
        auto sources = Json::array();
        for (const auto& source : action.sourceResultRefs) sources.push_back(sourceJson(source));
        db.journal(task.id, "run_started",
                   {{"run", destination ? destinationRunJson(*destination) : assignedRunJson(run)},
                    {"actionId", cmd.actionId},
                    {"revisionId", revisionJson(action.revisionId)},
                    {"sourceResultRefs", std::move(sources)}});
        return run;
    }

    void rememberRunCandidate(Outbox action, const RunRef& ref) {
        action.command.candidate = ref;
        if (action.command.tool == CommandTool::delegate) action.command.launchReceiptThreadId = ref.threadId;
        db.saveOutbox(action);
    }

    void writeDestinationReport(TaskData& task, DestinationRun& run, const Observation& observed) {
        if (db.recordDestinationReport(task, run, observed) && task.state == TaskState::cancelling) cancelDone(task);
    }

    static bool setDestinationStatus(DestinationRun& run, DestinationRunPhase status, std::string detail) {
        const auto currentDetail = run.detail ? std::string_view(*run.detail) : std::string_view{};
        if (run.inPhase(status) && currentDetail == detail) return false;
        run.status = status;
        run.detail = std::move(detail);
        return true;
    }

    void destinationStatus(Id id, std::string_view action, DestinationRunPhase status, std::string detail) {
        changeTask(id, [&](TaskData& task) {
            auto& run = destinationRun(task, action);
            return run.outstanding() && setDestinationStatus(run, status, std::move(detail));
        });
    }

    bool deferDestinationDelivery(TaskData& task, DestinationRun& run, const Outbox& action, const Delivery& outcome,
                                  DeliveryOutcome delivery) {
        const bool cancelling = task.state == TaskState::cancelling || action.stop.has_value();
        const auto& key = action.command.actionId;
        const auto phase = action.phase;
        const auto detail = bounded(outcome.detail.empty() ? "T3 has not confirmed the assigned Run." : outcome.detail, 2048);
        const bool rejected = outcome.kind == Delivery::Kind::NotAccepted || outcome.kind == Delivery::Kind::Settings;
        const bool closeUnsentDelivery =
            delivery == DeliveryOutcome::DefinitelyUnsent && (cancelling || phase == OutboxPhase::started || rejected);
        if (closeUnsentDelivery) {
            db.setOutboxPhase(key, OutboxPhase::closed);
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
        run.retryAt = options.clock() + 5;
        return true;
    }

    bool destinationDeadline(TaskData& task, DestinationRun& run, Seconds now) {
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
        db.wait(task, DeadlineWait{VerificationDeadline{deadline, action}}, run.name + " reached its deadline",
                WaitRun{run.ref, environment});
        return true;
    }

    void stepDestination(const TaskData& task, const DestinationRun& run) {
        const auto id = task.id;
        const auto& key = run.actionId;
        std::optional<Outbox> action;
        {
            std::lock_guard lock(db.mutex);
            action = db.outbox(task, key);
        }
        if (!action) return;
        const auto phase = action->phase;
        const auto now = options.clock();
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
            changeTask(id, [&](TaskData& current) {
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
            changeTask(id, [&](TaskData& current) { return destinationDeadline(current, destinationRun(current, key), now); });
        if (now < run.retryAt.value_or(0)) return;
        if (phase == OutboxPhase::ready) {
            std::lock_guard lock(db.mutex);
            if (!db.projectRouteReady(id, key)) return;
        }
        if ((phase == OutboxPhase::ready || phase == OutboxPhase::started) && !stopRequested) return deliver(task, std::move(*action));
        if (phase != OutboxPhase::accepted) return;
        const auto cmd = command(task, *action);
        const auto observed = observeRun(cmd.ownerBotId, cmd.environmentId, cmd.config, ref);
        if (observed.kind == Observation::Kind::Finished) {
            changeTask(id, [&](TaskData& current) {
                auto& saved = destinationRun(current, key);
                if (!saved.outstanding() || saved.ref.threadId != ref.threadId || saved.ref.runId != ref.runId) return false;
                if (!stopping &&
                    (observed.commitEvidenceUnavailable || observed.evidence == RunEvidence::Availability::ReportUnavailable) &&
                    saved.evidenceAttempts + 1 < maxEvidenceReads) {
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
        changeTask(id, [&](TaskData& current) {
            auto& saved = destinationRun(current, key);
            if (!saved.outstanding() || saved.ref.threadId != ref.threadId || saved.ref.runId != ref.runId) return false;
            const auto savedAction = db.outbox(current, key);
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
                changed = setDestinationStatus(
                    saved, observed.status == "queued" ? DestinationRunPhase::queued : DestinationRunPhase::running, {});
            return (!stopping && destinationDeadline(current, saved, now)) || changed;
        });
        if (!stopping || observed.kind == Kind::Settling || observed.kind == Kind::Unavailable || now < run.stopRetryAt.value_or(0)) return;
        if (!changeTask(id, [&](TaskData& current) {
                const auto savedAction = db.outbox(current, key);
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
                    auto saved = db.outbox(current, key);
                    if (saved && saved->stop && saved->stop->operation == operationId) {
                        if (gate == ApprovalGate::allow)
                            saved->stop->approval.reset();
                        else
                            saved->stop.reset();
                        db.saveOutbox(*saved);
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

    void applyDelivery(Id id, const Outbox& action, Delivery outcome) {
        changeTask(id, [&](TaskData& task) {
            if (terminal(task)) return false;
            const auto& key = action.command.actionId;
            const bool destination = action.destinationAction;
            auto saved = destination ? db.outbox(task, key) : std::nullopt;
            if (destination ? (!saved || (saved->phase != OutboxPhase::ready && saved->phase != OutboxPhase::started))
                            : task.actionId != action.command.actionId)
                return false;
            if (outcome.run.gatewayEnvironmentId.empty()) {
                auto gateway = action.command.gatewayEnvironmentId;
                if (gateway.empty() && !destination) gateway = task.gatewayEnvironmentId.value_or("");
                if (gateway != action.command.environmentId) outcome.run.gatewayEnvironmentId = std::move(gateway);
            }
            if (!outcome.launchWarning.empty()) {
                task.launchWarning = db.redactor(task.botId).text(outcome.launchWarning);
                db.journal(id, "session_lineage_unavailable", {{"actionId", key}, {"text", *task.launchWarning}});
            }
            const bool cancelling = task.state == TaskState::cancelling;
            const bool reply = action.command.tool == CommandTool::answer_session_question;
            const auto delivery = classifyDelivery(task, action, outcome, reply);

            const bool accepted = delivery == DeliveryOutcome::Accepted && (!destination || saved->phase == OutboxPhase::started);
            if (accepted) db.setOutboxPhase(key, OutboxPhase::accepted);
            if (destination) {
                auto& run = destinationRun(task, key);
                if (!accepted) return deferDestinationDelivery(task, run, *saved, outcome, delivery);
                if (saved->stop && !saved->stop->run) {
                    saved->stop->run = OutboxRunIdentity{outcome.run.threadId, outcome.run.runId};
                    db.saveOutbox(*saved);
                }
                run.status = DestinationRunPhase::running;
                run.detail.reset();
                run.retryAt.reset();
                recordRunAssignment(task, action, outcome.run, &run);
                return true;
            }
            if (accepted) {
                db.clearWait(task, "delivery_confirmed");
                if (reply)
                    db.unlockQuestion(task, "answered");
                else {
                    task.activeRun = recordRunAssignment(task, action, outcome.run);
                    if (action.sessionParticipationId)
                        db.exec(R"SQL(
INSERT OR IGNORE INTO session_queue_cancellations(participation_id,run,stop_id)
SELECT id,?,stopped_through FROM session_participations WHERE id=? AND stopped_through>=?
)SQL",
                                {outcome.run.runId, *action.sessionParticipationId, task.sourceMessageId.value()});
                    if (task.workflow) {
                        const auto now = options.clock();
                        db.exec("UPDATE workflow_stages SET confirmed_state='running',confirmed_at=?,unreachable=0,"
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
                    db.journal(id, "session_answered", {{"requestId", action.command.questionId}, {"actionId", action.command.actionId}});
                task.actionId.clear();
                if (!cancelling) task.state = TaskState::running;
                return true;
            }
            if (outcome.kind == Delivery::Kind::QuestionGone && reply) {
                db.setOutboxPhase(key, OutboxPhase::closed);
                closeQuestion(task, "question_gone", "resolved_or_expired");
                task.actionId.clear();
                if (!cancelling) task.state = TaskState::running;
                return true;
            }
            if (delivery == DeliveryOutcome::Unknown && !outcome.run.threadId.empty() && action.phase == OutboxPhase::ready && !reply &&
                !cancelling) {
                rememberRunCandidate(action, outcome.run);
                db.journal(id, "run_id_pending", {{"actionId", action.command.actionId}, {"thread", runJson(outcome.run)}});
                return true;
            }
            const auto reason = outcome.kind == Delivery::Kind::Auth       ? SimpleWaitReason::auth
                                : outcome.kind == Delivery::Kind::Settings ? SimpleWaitReason::settings
                                                                           : SimpleWaitReason::unknown_delivery;
            auto detail = outcome.detail.empty() ? "The assigned side effect could not be confirmed" : bounded(outcome.detail, 2048);
            if (reply && delivery == DeliveryOutcome::DefinitelyUnsent)
                detail = bounded("The Session answer was not sent. " + detail, 2048);
            if (delivery == DeliveryOutcome::DefinitelyUnsent) {
                db.setOutboxPhase(key, cancelling ? OutboxPhase::closed : OutboxPhase::ready);
                if (!reply) task.deadline = 0;
                if (cancelling) {
                    refundUnsentWork(task, action);
                    task.actionId.clear();
                }
            }
            if (cancelling) {
                if (task.cancellationDetail == detail && delivery == DeliveryOutcome::Unknown) return false;
                task.cancellationDetail = detail;
                db.journal(id, "cancelling", {{"detail", detail}, {"actionId", action.command.actionId}});
            } else {
                const auto wait = db.currentWait(id);
                if (!wait || wait->reason() != waitReason(reason) || wait->text != detail)
                    db.wait(task, reason, std::move(detail));
                else if (delivery == DeliveryOutcome::Unknown)
                    return false;
            }
            return true;
        });
    }

    void deliver(const TaskData& task, Outbox action) {
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
            std::lock_guard lock(db.mutex);
            if (const auto& origin = current.sessionOrigin) {
                const auto source = db.loadTurnMessage(current.sourceMessageId.value());
                const auto actor = db.turnActor(source);
                const auto& parent = origin->target;
                if (!db.activeSessionOrigin(cmd.ownerBotId, actor.id, parent, cmd.environmentId, source.id) ||
                    !policy.allows(actor, Action::bot_decide, cmd.ownerBotId) ||
                    !policy.allows(cmd.ownerBotId, Action::environment_inspect, parent.environmentId))
                    return false;
                if (!sendsToSession) return true;
            }
            if (db.sessionParticipationStopped(cmd.ownerBotId,
                                               {cmd.environmentId, cmd.target.threadId,
                                                cmd.gatewayEnvironmentId == cmd.environmentId ? std::string{} : cmd.gatewayEnvironmentId},
                                               current.sourceMessageId.value_or(0)))
                return false;
            if (!usesExistingSession || !current.selectedSession) return cmd.tool != CommandTool::start_in_session;
            const auto& selected = *current.selectedSession;
            if (selected.target.environmentId != cmd.environmentId || selected.target.threadId != cmd.target.threadId ||
                selected.projectId != cmd.projectId)
                return false;
            return db.permitsWorkSession(current.sourceMessageId.value_or(0), selected);
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
        if (destination) dispatch.emplace(db.path, SessionDispatchFileLock::Mode::shared);
        if (firstSend || destination) {
            std::lock_guard lock(db.mutex);
            const auto current = db.load(id);
            if (firstSend &&
                (db.botPaused(cmd.ownerBotId) || current.state == TaskState::cancelling || current.generation != task.generation))
                return;
            if (destination) {
                const auto saved = db.outbox(current, cmd.actionId);
                if (saved && saved->stop) return;
            }
        }
        const auto operation = std::string(destination ? runOutboxOperation : "outbox/") + cmd.actionId;
        const auto refused = [&](ApprovalGate gate) {
            if (destination) return applyDelivery(id, action, {Delivery::Kind::NotAccepted, {}, std::string(approvalError(gate)), true});
            change(task, [&](TaskData& current) {
                if (current.actionId != action.command.actionId) return false;
                if (!db.setOutboxPhase(action.command.actionId, OutboxPhase::closed, OutboxPhase::ready)) return false;
                refundUnsentWork(current, action);
                current.actionId.clear();
                current.state = !current.activeRun ? TaskState::deciding : TaskState::running;
                current.feedback = approvalError(gate);
                if (current.state == TaskState::deciding)
                    db.event(current, "policy/" + cmd.actionId, {{"kind", "policy_outcome"}, {"text", approvalError(gate)}});
                db.journal(id, "policy_action_closed", {{"actionId", cmd.actionId}, {"text", approvalError(gate)}});
                return true;
            });
        };
        const auto disconnected = [&] {
            const auto& connectionEnvironment = cmd.gatewayEnvironmentId.empty() ? cmd.environmentId : cmd.gatewayEnvironmentId;
            return options.requireT3Connection && !db.t3EnvironmentConnected(connectionEnvironment);
        };
        const auto connectionUnavailable = [&] {
            applyDelivery(id, action,
                          {Delivery::Kind::Auth, {}, "Connect this T3 environment in Flock settings before sending work.", true});
        };
        if (firstSend) {
            if (disconnected()) return connectionUnavailable();
            if (!sessionActionAllowed(task)) return refused(ApprovalGate::denied);
            if (resource && policy.mode() == "full" && policy.forbidden(cmd.ownerBotId, resource->action, resource->target))
                return applyDelivery(id, action, {Delivery::Kind::Settings, {}, "Session action denied by installation policy.", true});
            if (preparesTaskContract) {
                try {
                    std::lock_guard lock(db.mutex);
                    if (action.instructionBase) {
                        prepareDelegation(task, action);
                        if (!db.prepareOutbox(action)) {
                            auto saved = db.outbox(task, cmd.actionId);
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
                const auto scrub = db.redactor(cmd.ownerBotId);
                if (task.workflow && scrub.text(task.workflow->inputArtifactVersion) != task.workflow->inputArtifactVersion)
                    return applyDelivery(
                        id, action,
                        {Delivery::Kind::Settings, {}, "The pinned workflow artifact version contains a protected value", true});
                const auto marker = sessionPrompt(cmd.actionId, {});
                if (cmd.instruction.ends_with(marker)) cmd.instruction.resize(cmd.instruction.size() - marker.size());
                action.sessionMemory.reset();
                cmd.instruction = scrub.text(cmd.instruction);
                action.command.instruction = cmd.instruction;
                cmd.instruction = sessionPrompt(cmd.actionId, cmd.instruction);
                if (cmd.instruction.size() > limits::session_prompt::maxBytes)
                    return applyDelivery(id, action, {Delivery::Kind::Settings, {}, SessionContextLimit{}.what(), true});
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
                const auto origin = db.taskPolicyOrigin(current);
                if (usesExistingSession) {
                    const Resource launch{Action::environment_launch, cmd.environmentId, true};
                    const auto launchPolicy = policy.check(cmd.ownerBotId, launch);
                    if (launchPolicy == PolicyResult::deny) return ApprovalGate::denied;
                    if (launchPolicy == PolicyResult::ask && policy.check(cmd.ownerBotId, *resource) == PolicyResult::allow)
                        return db.approveAction(origin, launch, operation, payload, options.clock(), claim);
                }
                return resource ? db.approveAction(origin, *resource, operation, payload, options.clock(), claim) : ApprovalGate::allow;
            };
            if (resource) {
                ApprovalGate gate;
                {
                    std::lock_guard lock(db.mutex);
                    Transaction tx(db.db);
                    const auto current = destination ? db.load(id) : task;
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
            if (!dispatch) dispatch.emplace(db.path, SessionDispatchFileLock::Mode::shared);
            if (disconnected()) return connectionUnavailable();
            const auto sharesMemory = [](const Json& config) { return config.value("shareMemoryWithSessions", Json(false)) == true; };
            bool includeMemory = false;
            if (sendsWork) {
                try {
                    includeMemory = options.intake ? sharesMemory(options.intake(cmd.ownerBotId, {}).config) : sharesMemory(cmd.config);
                } catch (const std::exception&) {
                    // An unreadable Bot profile cannot authorize sharing.
                }
            }
            auto policyGate = ApprovalGate::allow;
            const auto startSend = [&](TaskData& current) {
                if (shutdown.stop_requested() || terminal(current) || current.state == TaskState::cancelling ||
                    db.botPaused(cmd.ownerBotId))
                    return false;
                if (destination) {
                    if (!db.projectRouteReady(id, cmd.actionId)) return false;
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
                    const auto source = db.loadTurnMessage(*current.sourceMessageId);
                    const SessionTarget target{cmd.environmentId, cmd.target.threadId,
                                               cmd.gatewayEnvironmentId == cmd.environmentId ? std::string{} : cmd.gatewayEnvironmentId};
                    auto participant = db.query(R"SQL(
SELECT id,person_id FROM session_participations WHERE bot_id=? AND environment_id=? AND thread_id=? AND state='active'
)SQL",
                                                {cmd.ownerBotId, target.environmentId, target.threadId});
                    if (participant.next() &&
                        (participant.string(1) == db.turnActor(source).id || db.connectedSessionAccess(source, target)))
                        action.sessionParticipationId = participant.number(0);
                }
                if (includeMemory) {
                    if (const auto memory = db.sessionMemory(cmd, options.clock())) {
                        action.sessionMemory = *memory;
                        cmd.instruction = dispatchedWorkPrompt(action);
                    }
                }
                if (!db.startOutbox(action, cmd.authGeneration)) return false;
                if (cmd.tool != CommandTool::answer_session_question) {
                    auto* run = destination ? &destinationRun(current, cmd.actionId) : nullptr;
                    if (!(run ? run->deadline.value_or(0) : current.deadline)) {
                        const auto deadline = options.clock() + db.environmentTimeout(cmd.ownerBotId, cmd.environmentId, current.config);
                        if (run)
                            run->deadline = deadline;
                        else
                            current.deadline = deadline;
                    }
                }
                db.journal(current.id, SendStartedEntry{action.command.actionId});
                return true;
            };
            if (!(destination ? changeTask(id, startSend) : change(task, startSend))) {
                if (policyGate != ApprovalGate::allow && policyGate != ApprovalGate::waiting) refused(policyGate);
                return;
            }
        } else if (action.phase != OutboxPhase::started)
            return;
        try {
            if (firstSend)
                outcome = !sessionActionAllowed(task) ||
                                  (resource && policy.forbidden(cmd.ownerBotId, resource->action, resource->target)) ||
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

    bool hasSettledRun(const TaskData& task) const { return !task.activeRun && task.lastRun && hasRun(task.lastRun->ref); }

    static constexpr std::size_t maxEvidenceReads = 3;

    struct PeerResult {
        Id taskId;
        std::string kind, environment;
    };

    std::optional<PeerResult> peerResult(const TaskData& task, const Json& event) const {
        // A peer result is actionable only for the sender and the closing message of that exchange.
        if (event.at("kind") != "bot_result") return std::nullopt;
        auto exchange =
            db.query(R"SQL(
SELECT p.recipient_task, p.kind, recipient.environment_id
FROM peer_exchanges p JOIN tasks recipient ON recipient.id=p.recipient_task
WHERE p.id=? AND p.sender_task=? AND p.phase='closed' AND p.last_message=?
  AND p.sender_environment=? AND p.sender_revision IS ?
)SQL",
                     {event.at("exchangeId"), task.id, event.at("messageId"), task.environmentId, revisionJson(task.currentRevision)});
        if (!exchange.next()) return std::nullopt;
        return PeerResult{exchange.number(0), exchange.string(1), exchange.string(2)};
    }

    bool canComplete(const TaskData& task, const Json& event) const {
        if (task.replanning) return false;
        if (!task.newSessionRequired && hasSettledRun(task) && task.lastRun->assignment.revisionId == task.currentRevision &&
            !sessionReportUnavailable(task))
            return true;
        const auto peer = peerResult(task, event);
        if (!peer) return false;
        if (task.newSessionRequired) return peer->environment == task.environmentId && event.at("status") == "completed";
        return task.currentRevision.has_value() || (peer->kind == "consult" && event.at("status") == "completed");
    }

    std::vector<std::string> available(const TaskData& task, const Json& event, const std::optional<PeerExchange>& peer, bool canSend,
                                       bool canRead) {
        if (!task.runTargets().empty()) return {"delegate", "ask_user"};
        const auto id = task.id;
        if (peer && peer->sender == id && peer->phase == "question") return {"reply_to_bot", "ask_user"};
        if (task.activeRun && task.question) return {"answer_session_question", "ask_user"};
        const bool receiving = peer && peer->recipient == id && peer->phase == "working";
        if (receiving && peer->kind == "consult") return {"reply_to_bot"};
        std::vector<std::string> tools{"delegate", receiving ? "reply_to_bot" : "ask_user"};
        if (hasSettledRun(task)) {
            if (!task.newSessionRequired) {
                tools.push_back("follow_up");
                if (task.selectedSession) std::erase(tools, "delegate");
            }
            if (event.contains("sessionReadViews")) tools.push_back("session_read");
        }
        if (!receiving) {
            std::lock_guard lock(db.mutex);
            if (canComplete(task, event)) tools.push_back("complete");
        }
        if (canRead && event["evidence"].size() < maxEvidenceReads) tools.push_back("read_bot_evidence");
        if (canSend && (!task.selectedSession || hasSettledRun(task))) tools.push_back("send_bot_message");
        return tools;
    }
    enum class FailureCause { Model, BotNotes, UnsupportedReasoningEffort };

    void recordFailure(TaskData& task, const Json& event, std::string_view detail, bool transient,
                       FailureCause cause = FailureCause::Model) {
        const auto attempts = event["failures"].get<int>() + 1;
        const auto why = bounded(db.redactor(task.botId).text(std::string(detail)), 2048);
        db.exec("UPDATE events SET failures=?,retry_at=? WHERE id=?",
                {attempts, transient ? options.clock() + (1 << (attempts - 1)) : 0, event["id"]});
        db.journal(task.id, "model_failure", {{"eventId", event["id"]}, {"attempt", attempts}, {"text", why}});
        task.feedback = why;
        if (cause == FailureCause::UnsupportedReasoningEffort)
            db.wait(task, ModelFailureWait{WaitFailureCode::unsupported_reasoning_effort}, why);
        else if (attempts >= 3) {
            if (cause == FailureCause::BotNotes && db.automationBeforeDispatch(task.id))
                db.wait(task, SimpleWaitReason::bot_notes_unavailable, why);
            else
                db.wait(task, ModelFailureWait{}, why);
        }
    }

    void failure(const TaskData& task, const Json& event, std::string_view detail, bool transient,
                 FailureCause cause = FailureCause::Model) {
        change(task, [&](TaskData& current) {
            recordFailure(current, event, detail, transient, cause);
            return true;
        });
    }

    static constexpr int maxToolErrors = 6;

    void rejectDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view message,
                        bool replay = false) {
        change(task, [&](TaskData& current) {
            const bool unknown = replay && isGitTool(decision.tool) && db.gitEffectStarted(gitInvocation(decision), current.id);
            const auto why = unknown ? "The host outcome is unknown. Check the target before retrying." : bounded(message, 2048);
            ToolErrorEntry entry{event.at("id").get<Id>(), bounded(decision.tool, 128), why, unknown, {}};
            const auto args = decision.args.dump();
            if (args.size() <= 2048)
                entry.arguments = decision.args;
            else
                entry.arguments = bounded(args, 2048);
            db.journal(current.id, entry);
            db.exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {event["id"]});
            current.feedback.reset();
            if (event.at("decisionFeedback").size() + 1 >= maxToolErrors) db.wait(current, ModelFailureWait{}, why);
            return true;
        });
    }

    static std::size_t readOffset(const Json& args) {
        if (args.contains("offset") && (!args["offset"].is_number_integer() || args["offset"].get<Id>() < 0))
            throw Conflict("Invalid history offset");
        return args.value("offset", std::size_t{0});
    }

    SessionExcerptEntry readExcerpt(std::string_view bot, const ReadRequest& req) {
        const auto scrub = db.redactor(bot);
        const auto excerpt = [&] {
            try {
                return t3.read(req);
            } catch (const ModelError& error) {
                throw HistoryUnavailable(scrub.text(error.what()), error.transient);
            } catch (const std::exception& error) {
                throw HistoryUnavailable(scrub.text(error.what()), false);
            }
        }();
        if ((!excerpt.threadId.empty() && excerpt.threadId != req.threadId) || (!excerpt.runId.empty() && excerpt.runId != req.runId))
            throw Conflict("History source does not match the requested Run");
        scrub.requirePublic({excerpt.itemId, excerpt.nextCursor});
        const auto redacted = scrub.text(excerpt.text);
        auto text = bounded(redacted, limits::session_evidence::maxHistoryExcerptBytes);
        const bool clipped = text.size() < redacted.size();
        return {req.environmentId,
                req.threadId,
                req.runId,
                excerpt.itemId,
                std::move(text),
                excerpt.nextCursor,
                excerpt.nextOffset,
                excerpt.more || clipped,
                excerpt.nextOffset > req.offset || clipped};
    }

    bool readEvidence(const TaskData& task, const Json& event, const Json& args, const Json& usage = Json::object()) {
        ReadRequest req;
        req.environmentId = task.environmentId;
        req.gatewayEnvironmentId = task.gatewayEnvironmentId.value_or("");
        req.config = task.config;
        req.threadId = field(args, "threadId");
        req.runId = field(args, "runId");
        req.itemId = field(args, "itemId");
        req.cursor = field(args, "cursor");
        req.view = field(args, "view").empty() ? "messages" : field(args, "view");
        req.offset = readOffset(args);
        if (req.view != "messages" && req.view != "activity") throw Conflict("Unsupported history view");
        const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
        const auto active = runRef(report.value("run", Json::object()));
        if (active.threadId == req.threadId && (active.runId == req.runId || field(report, "resultRunId") == req.runId)) {
            req.environmentId = report.at("environmentId");
            req.gatewayEnvironmentId = active.gatewayEnvironmentId;
            if (req.gatewayEnvironmentId.empty() && !task.hasDestinations())
                req.gatewayEnvironmentId = task.gatewayEnvironmentId.value_or("");
        } else {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT DISTINCT environment_id FROM runs WHERE task_id=? AND thread_id=? AND run_id=? LIMIT 2",
                                {task.id, req.threadId, req.runId});
            if (!row.next()) throw Conflict("History reference does not belong to this task");
            req.environmentId = row.string(0);
            if (row.next()) throw Conflict("History reference matches more than one Session");
        }
        if (req.threadId.empty() || req.runId.empty()) throw Conflict("History reference does not belong to this task");
        if (!policy.allows(task.botId, Resource::sessionRead(req.environmentId, req.threadId)))
            throw Conflict("Session read access required");
        auto entry = encodeSessionExcerpt(readExcerpt(task.botId, req));
        return change(task, [&](TaskData& current) {
            recordSessionRead(current, event, std::move(entry), usage);
            return true;
        });
    }

    void appendEventEvidence(const TaskData& task, const Json& event, const Json& entry) {
        auto evidence = event.at("evidence");
        if (evidence.size() >= maxEvidenceReads) throw Conflict("History read allowance exhausted");
        evidence.push_back(entry);
        db.exec("UPDATE events SET evidence=? WHERE id=? AND task_id=?", {evidence, event["id"], task.id});
    }

    void recordSessionRead(const TaskData& task, const Json& event, Json entry, const Json& usage) {
        appendEventEvidence(task, event, entry);
        if (!usage.empty()) entry["usage"] = usage;
        db.journal(task.id, "session_read", entry);
    }

    enum class PrefillResult { NotNeeded, Persisted, UnavailableWithSettledReport };

    PrefillResult prefillHistory(const TaskData& task, const Json& event) {
        if (options.sessionRead != "prefill" || !event["evidence"].empty() ||
            (event["kind"] != "result" && !event.contains("sessionResult")))
            return PrefillResult::NotNeeded;
        const auto& report = event.contains("sessionResult") ? event["sessionResult"] : event;
        const auto run = runRef(report["run"]);
        if (!policy.allows(task.botId, Resource::sessionRead(report.value("environmentId", task.environmentId), run.threadId)))
            return PrefillResult::NotNeeded;
        try {
            readEvidence(task, event, {{"threadId", run.threadId}, {"runId", run.runId}, {"view", "activity"}});
            return PrefillResult::Persisted;
        } catch (const HistoryUnavailable&) {
            return PrefillResult::UnavailableWithSettledReport;
        }
    }

    struct WorkPrompt {
        std::string instruction;
        std::vector<OutboxInstruction> pending;
        OutboxContext contextIds;
        std::vector<OutboxSource> resultReferences;
    };

    struct SessionContract {
        std::string intent, doneWhen;
        Id answersAfter;
    };

    SessionContract sessionContract(const TaskData& task) {
        auto row = db.query(R"SQL(
SELECT r.intent,r.done_when,
       coalesce((SELECT max(j.id) FROM journal j WHERE j.task_id=r.task_id AND
         ((r.number=1 AND j.kind='task_contract_changed') OR
          (r.number>1 AND j.kind='task_revision' AND j.revision_id=r.id))),
         r.source_message_id,0)
FROM task_revisions r WHERE r.id=? AND r.task_id=?
)SQL",
                            {revisionJson(task.currentRevision), task.id});
        if (!row.next()) throw Conflict("The new Session needs an established task revision");
        return {row.string(0), row.string(1), row.number(2)};
    }

    std::vector<OutboxSource> appendSessionDirections(std::string& instruction, const TaskData& task,
                                                      std::span<const OutboxInstruction> pending, Id answersAfter) {
        using InstructionId = std::pair<Id, int>;
        std::set<InstructionId> pendingIds;
        for (const auto& item : pending) pendingIds.emplace(item.messageId, item.step);
        auto rows = db.query(R"SQL(
SELECT j.id,j.kind,j.data,w.id,w.reason,w.data,
       CASE j.kind WHEN 'instruction_accepted' THEN EXISTS (
         SELECT 1 FROM journal d WHERE d.task_id=j.task_id AND d.kind='instruction_delivered'
           AND d.message_id=j.message_id AND d.instruction_step=j.instruction_step AND d.revision_id=?2)
       ELSE 0 END
FROM journal j
LEFT JOIN waits w ON j.kind='user_answer' AND w.id=j.wait_id AND w.task_id=j.task_id
WHERE j.task_id=?1 AND
      (j.kind='instruction_accepted' OR
       (j.kind='user_answer' AND j.id>?3 AND w.resolution='answer'))
ORDER BY j.id
)SQL",
                             {task.id, revisionJson(task.currentRevision), answersAfter});
        std::vector<OutboxSource> sources;
        bool first = true;
        while (rows.next()) {
            const auto recordId = rows.number(0);
            const bool isAnswer = rows.string(1) == "user_answer";
            const auto accepted = isAnswer ? std::nullopt : std::optional{decodeAcceptedInstruction(recordId, rows.json(2))};
            if (accepted && !rows.number(6) && !pendingIds.contains({accepted->messageId, accepted->step})) continue;
            if (std::exchange(first, false))
                instruction += "\n\nUser decisions for this task, oldest first. Apply later directions where they replace "
                               "earlier ones, under the current task intent and completion criteria above.";
            instruction += isAnswer ? "\n\nUser answer" : "\n\nAccepted instruction";
            instruction += " (record " + std::to_string(recordId);
            if (accepted) {
                instruction += ", message " + std::to_string(accepted->messageId) + ", step " + std::to_string(accepted->step) + "):\n" +
                               accepted->text;
            } else {
                const auto answer = decodeUserAnswer(recordId, rows.json(2));
                if (answer.messageId) instruction += ", message " + std::to_string(*answer.messageId);
                instruction += ", wait " + std::to_string(answer.waitId) + "):\n";
                instruction += decodeWaitRow(rows, 3).text + "\nAnswer: " + answer.text;
            }
            if (instruction.size() > limits::session_prompt::maxBytes) throw SessionContextLimit{};
            if (accepted && accepted->source) sources.push_back(*accepted->source);
        }
        return sources;
    }

    WorkPrompt workPrompt(const TaskData& task, const Json& event, std::size_t actionIdBytes, const SessionContract* contract = nullptr) {
        WorkPrompt prompt;
        auto& [instruction, pending, contextIds, resultReferences] = prompt;
        const auto scrub = db.redactor(task.botId);
        std::size_t reservedBytes =
            limits::session_prompt::instructionReserveBytes + std::max(limits::session_prompt::minActionIdReserveBytes, actionIdBytes);
        if (!contract && event.contains("userAnswer")) {
            contextIds.answerEventId = event.at("id").get<Id>();
            instruction += "\n\nUser answer:\n" + field(event, "userAnswer");
        }
        const bool moved = task.newSessionRequired;
        if (contract || moved || !task.lastRun || task.lastRun->assignment.revisionId != task.currentRevision) {
            const auto criteria = contract ? contract->doneWhen : effectiveDoneWhen(task);
            instruction += "\n\nTask intent:\n" + (contract ? contract->intent : task.intent) + "\n\nCompletion criteria:\n" + criteria;
            reservedBytes += std::max(criteria.size(), limits::session_prompt::maxDecisionTextBytes) - criteria.size();
        }
        if (contract || moved) {
            if (field(event, "kind") == "bot_answer") {
                contextIds.peerMessageId = event.at("messageId").get<std::string>();
                instruction += "\n\nAnswer from the requesting Bot:\n" + field(event, "text");
            }
        }
        if (task.sourceTaskId && !task.lastRun) {
            contextIds.sourceMessageId = task.sourceMessageId;
            instruction += "\n\nUser request:\n" + task.request;
        }
        if (const auto& restart = task.restartContext; restart && !task.lastRun) {
            const auto& delivered = restart->deliveredInstructions;
            if (!delivered.empty()) instruction += "\n\nEarlier user instructions delivered before cancellation (reference only):";
            for (const auto& item : delivered) instruction += "\n\n" + item.text;
        }
        SourceQuotes quotes{scrub};
        const auto quoteSource = [&](OutboxSource source, std::size_t limit = limits::session_prompt::maxQuoteTextBytes) {
            redactSource(source, scrub);
            const auto quote = quotes.quote(source, limit);
            if (quote.empty()) return;
            instruction += quote;
            source.text.reset();
            resultReferences.push_back(std::move(source));
        };
        constexpr std::size_t recentReportLimit = 3, reportExcerptBytes = 2000;
        const auto recentReportBudget = moved ? quotes.remaining / 2 : 0;
        quotes.remaining -= recentReportBudget;
        if (task.sourceTaskId && (moved || !task.lastRun)) {
            for (const auto& source : task.sourceResultRefs)
                if (source.taskId == *task.sourceTaskId) quoteSource(source);
        }
        if (task.workflow) {
            for (const auto& source : task.sourceResultRefs) quoteSource(source);
            db.appendStageRequirements(instruction, task);
        }
        quotes.remaining += recentReportBudget;
        if (moved) {
            auto reports = db.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT ?",
                                    {task.id, recentReportLimit});
            auto remainingReports = recentReportLimit;
            while (reports.next()) {
                const auto report = decodeRunReport(reports.number(0), reports.json(1));
                OutboxSource source{.taskId = task.id,
                                    .messageId = reports.number(0),
                                    .text = report.evidence.report,
                                    .resultRunId = report.resultRunId,
                                    .run = reportSourceRun(report)};
                quoteSource(std::move(source), std::min(reportExcerptBytes, quotes.remaining / remainingReports));
                --remainingReports;
            }
        }
        const bool resumedTask = task.restartContext && !task.lastRun;
        if (contract && !resumedTask)
            pending = task.pendingInstructions;
        else {
            constexpr auto maxPromptBytes = limits::session_prompt::maxBytes;
            const auto occupiedBytes = reservedBytes + instruction.size();
            pending = pendingBatch(task.pendingInstructions, maxPromptBytes - std::min(maxPromptBytes, occupiedBytes), quotes);
        }
        if (contract) {
            const auto sources = appendSessionDirections(instruction, task, pending, contract->answersAfter);
            if (resumedTask && pending.size() < task.pendingInstructions.size())
                instruction += "\n\nFlock will send the remaining inherited instructions in follow-up Runs in this Session. "
                               "Work only on the instructions included in this Run until the rest arrive.";
            for (const auto& source : sources) quoteSource(source);
        } else {
            includeInstructions(instruction, pending);
            for (const auto& item : pending)
                if (item.source) quoteSource(*item.source);
        }
        return prompt;
    }

    void prepareDelegation(const TaskData& task, Outbox& action) {
        auto instruction = *action.instructionBase;
        const auto contract = sessionContract(task);
        const auto event = db.currentEvent(task);
        auto prompt = workPrompt(task, event, action.command.actionId.size(), &contract);
        const auto scrub = db.redactor(task.botId);
        instruction += prompt.instruction;
        action.command.instruction = scrub.text(instruction);
        redactInstructions(prompt.pending, scrub);
        action.instructions = std::move(prompt.pending);
        action.contextIds = std::move(prompt.contextIds);
        action.sourceResultRefs = std::move(prompt.resultReferences);
        action.instructionBase.reset();
    }

    void queueAction(TaskData& task, const std::string& id, CommandTool tool, std::string instruction, const Json& args, const Json& event,
                     bool charge, const TaskRoute* destination = nullptr) {
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
        WorkPrompt prompt;
        if (tool == CommandTool::follow_up) {
            prompt = workPrompt(task, event, id.size());
            const auto scrub = db.redactor(task.botId);
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
            action.instructionBase = db.redactor(task.botId).text(instruction);
        } else
            cmd.instruction = std::move(instruction);
        db.insertOutbox(taskId, action);
        if (destination) return;
        task.actionId = id;
        task.state = TaskState::sending;
    }

    void queueDestinations(TaskData& task, const std::string& first, std::string_view instruction, const Json& args, const Json& event,
                           bool charge) {
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

    bool admitWork(TaskData& task, const TaskDecision& decision, bool& charge) {
        charge = task.lastRun || db.query("SELECT 1 FROM peer_exchanges WHERE sender_task=? AND kind='work' LIMIT 1", {task.id}).next();
        if (!charge) return true;
        if (task.allowance.remaining == 0) {
            db.wait(task, WorkBudgetWait{workBudgetTool(decision.tool), decision.reason, waitUsage(decision.usage)},
                    "This request used its allowance for follow-up work.");
            return false;
        }
        --task.allowance.remaining;
        ++task.allowance.additionalUsed;
        return true;
    }

    void establishCriteria(TaskData& task, const Json& args) {
        if (task.currentRevision) return;
        const auto source = task.sourceMessageId.value_or(0);
        if (const auto& proposal = task.criteriaProposal)
            db.addRevision(task, task.intent, proposal->doneWhen, proposal->sourceMessageId.value_or(source));
        else
            db.addRevision(task, task.intent, field(args, "doneWhen"), source);
        task.criteriaProposal.reset();
    }

    std::vector<PeerBot> peersFor(std::string_view bot) const {
        if (!options.peers) return {};
        return options.peers(bot);
    }

    PeerBot allowedPeer(std::string_view sender, std::string_view botId) const {
        if (botId.empty() || sender == botId) throw Conflict("Choose an allowed peer Bot");
        const auto peers = peersFor(sender);
        const auto peer = std::find_if(peers.begin(), peers.end(), [&](const PeerBot& p) { return p.intake.botId == botId; });
        if (peer == peers.end()) throw Conflict("The destination Bot is unavailable or not allowed");
        return *peer;
    }

    BotMessageEntry peerMessage(const PeerExchange& peer, const TaskData& from, const TaskData& to, std::string_view type,
                                std::string_view body, std::string_view messageId) {
        return {.exchangeId = peer.id,
                .messageId = std::string(messageId),
                .fromBotId = from.botId,
                .toBotId = to.botId,
                .messageType = std::string(type),
                .requestKind = peer.kind,
                .text = std::string(body),
                .replyTo = peer.lastMessage.empty() ? std::nullopt : std::optional{peer.lastMessage}};
    }

    // The caller owns the transaction and saves the acting task. This writes the other participant.
    void transitionPeer(const PeerExchange& peer, TaskData& actor, TaskData& task, const BotMessageEntry& message,
                        std::optional<Id> forwardedSource = {}) {
        const auto& type = message.messageType;
        const bool closing = message.status.has_value();
        const auto kind = closing ? "bot_result" : "bot_" + type;
        const auto phase = closing ? "closed" : type == "question" ? "question" : "working";
        if (type == "request")
            db.exec("INSERT INTO peer_exchanges(id,kind,phase,last_message,sender_task,recipient_task,"
                    "sender_environment,sender_revision) VALUES(?,?,?,?,?,?,?,?)",
                    {peer.id, peer.kind, phase, message.messageId, peer.sender, peer.recipient, actor.environmentId,
                     revisionJson(actor.currentRevision)});
        else
            db.exec("UPDATE peer_exchanges SET phase=?,last_message=? WHERE id=?", {phase, message.messageId, peer.id});
        db.peerMessage(peer, message, forwardedSource);
        consumeEvent(actor);
        if (!closing) actor.state = TaskState::peer_waiting;
        if (terminal(task) || task.state == TaskState::cancelling) return;
        if (closing) db.clearWait(task, kind);
        consumeEvent(task);
        auto event = encodeBotMessage(message);
        if (event.contains("evidence")) {
            event["references"] = std::move(event["evidence"]);
            event.erase("evidence");
        }
        event["kind"] = kind;
        db.event(task, "bot/" + message.messageId, std::move(event));
        if (task.state != TaskState::queued) task.state = TaskState::deciding;
        saveTask(task);
    }

    void recordDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view action) {
        db.journal(task.id, DecisionEntry{event.at("id").get<Id>(), task.currentRevision, std::string(action), decision,
                                          event.at("evidence"), decision.usage});
    }

    std::optional<ForwardedPeerMessage> forwardableUserMessage(const TaskData& task) const {
        if (!task.sourceMessageId || !task.sourceMessageCoversFullRequest.value_or(false)) return std::nullopt;
        const auto sourceId = *task.sourceMessageId;
        // A compound or clarified request may not represent one complete user message.
        auto row = db.query(R"SQL(
SELECT m.text,j.time FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND m.bot_id=? AND j.kind='conversation_message'
)SQL",
                            {sourceId, task.botId});
        if (!row.next()) return std::nullopt;
        return ForwardedPeerMessage{row.string(0), row.number(1)};
    }

    void sendPeer(TaskData& sender, const Json& event, const TaskDecision& decision, PeerBot peer, const std::string& action) {
        if (db.botPaused(peer.intake.botId)) throw BotPaused{};
        if (db.peerExchange(sender.id)) throw Conflict("Finish the current Bot exchange first");
        const auto& args = decision.args;
        const auto note = field(args, "text");
        std::optional<ForwardedPeerMessage> forwarded;
        if (args.value("forwardUserMessage", false)) {
            forwarded = forwardableUserMessage(sender);
            if (!forwarded) throw Conflict("This task has no complete user message to forward");
        }
        const auto kind = field(decision.args, "kind");
        const bool work = kind == "work";
        bool charge = false;
        if (work) {
            if (sender.activeRun || !sender.actionId.empty())
                throw Conflict("Let the assigned Run settle before requesting another Bot's work");
            peer.intake.environmentId = db.defaultT3Environment(peer.intake.botId, peer.intake.environmentId, sender.environmentId);
            if (peer.intake.environmentId.empty()) throw Conflict("Choose a T3 environment for the destination Bot in its configuration");
            if (!peer.intake.projectId.empty() && db.peerWorkRouteWouldCycle(sender.id, peer.intake.environmentId, peer.intake.projectId))
                throw Conflict("This work request would make two project queues wait for one another");
            if (!admitWork(sender, decision, charge)) return;
            establishCriteria(sender, args);
        }
        peer.intake.request = note.empty() && forwarded ? forwarded->text : note;
        const auto source = work ? TaskSource::handoff : TaskSource::consultation;
        const auto id = db.submit(peer.intake, 0, nullptr, source);
        auto recipient = db.load(id);
        const PeerExchange exchange{action, kind, "working", "", sender.id, id};
        auto message = peerMessage(exchange, sender, recipient, "request", note, action);
        message.chargesBudget = charge;
        message.forwarded = std::move(forwarded);
        transitionPeer(exchange, sender, recipient, message, message.forwarded ? sender.sourceMessageId : std::nullopt);
        recordDecision(sender, event, decision, action);
    }

    struct JournalReport {
        Id id;
        RunReportEntry data;
    };

    std::optional<JournalReport> settledSessionReport(const TaskData& task) const {
        if (!task.lastRun || !hasRun(task.lastRun->ref)) return std::nullopt;
        const auto& run = task.lastRun->ref;
        auto report = db.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT 1", {task.id});
        if (!report.next()) return std::nullopt;
        auto data = decodeRunReport(report.number(0), report.json(1));
        if (data.run.threadId != run.threadId || data.run.runId != run.runId) return std::nullopt;
        return JournalReport{report.number(0), std::move(data)};
    }

    bool sessionReportUnavailable(const TaskData& task) const {
        const auto report = settledSessionReport(task);
        return report && report->data.evidence.availability == RunEvidence::Availability::ReportUnavailable;
    }

    std::optional<JournalReport> reportRetry(const TaskData& task, const Json& event) const {
        if (!hasSettledRun(task) || task.replanning || task.newSessionRequired ||
            task.lastRun->assignment.revisionId != task.currentRevision)
            return std::nullopt;
        auto retry = settledSessionReport(task);
        if (!retry) return std::nullopt;
        const auto& report = retry->data;
        if (report.evidence.availability != RunEvidence::Availability::ReportUnavailable || !report.matches(*task.lastRun) ||
            report.environmentId != task.environmentId || report.resultRunId.empty())
            return std::nullopt;
        if (event.at("kind") == "result") {
            const auto matches = [&](std::string_view key, const Json& value) {
                const auto saved = event.find(key);
                return saved != event.end() && *saved == value;
            };
            if (!matches("environmentId", report.environmentId) || !matches("run", assignedRunJson(*task.lastRun)) ||
                !matches("resultRunId", report.resultRunId) || !matches("status", report.status) ||
                !matches("revisionId", revisionJson(report.revisionId)))
                return std::nullopt;
        }
        auto settled = db.query(R"SQL(
SELECT id,data FROM journal WHERE task_id=? AND kind='run_settled'
AND run_environment_id=? AND run_thread_id=? AND run_id=? AND result_run_id=? AND run_status=?
AND revision_id IS ?
)SQL",
                                {task.id, report.environmentId, report.run.threadId, report.run.runId, report.resultRunId, report.status,
                                 revisionJson(report.revisionId)});
        while (settled.next())
            if (decodeRunReport(settled.number(0), settled.json(1)).matches(*task.lastRun)) return retry;
        return std::nullopt;
    }

    void retryRunReport(const TaskData& task, const Json& event, const TaskDecision& decision) {
        const auto retry = [&] {
            std::lock_guard lock(db.mutex);
            return reportRetry(task, event);
        }();
        const auto& args = decision.args;
        if (!retry || args.contains("itemId") || args.contains("cursor") || args.contains("offset") ||
            field(args, "threadId") != retry->data.run.threadId || field(args, "runId") != retry->data.resultRunId)
            throw Conflict("Choose the exact settled result Run to retry its terminal report");
        const auto& report = retry->data;
        const auto& env = report.environmentId;
        const auto& thread = report.run.threadId;
        if (!policy.allows(task.botId, Resource::sessionRead(env, thread))) throw Conflict("Session read access required");
        const auto evidence = fetchRunEvidence(task, env, report.run, report.resultRunId, report.status);
        const bool available = evidence.availability != RunEvidence::Availability::ReportUnavailable;
        change(task, [&](TaskData& current) {
            const auto latest = db.currentEvent(current);
            if (latest.is_null() || latest.at("consumed") == true || current.state != TaskState::deciding) return false;
            const auto target = reportRetry(current, latest);
            if (!target || target->id != retry->id || target->data != report) return false;
            if (available) {
                const auto boundedEvidence = boundedReportEvidence(evidence);
                auto updated = target->data;
                updated.evidence = boundedEvidence;
                if (updated.evidence.sessionCreatedAt.empty()) updated.evidence.sessionCreatedAt = report.evidence.sessionCreatedAt;
                db.exec("UPDATE journal SET data=? WHERE id=?", {encodeRunReport(updated), target->id});
                if (latest.at("kind") == "result") {
                    auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=?", {latest.at("id"), current.id});
                    if (!row.next()) throw Conflict("The settled result event is no longer available");
                    auto data = row.json(0);
                    setReportEvidence(data, boundedEvidence);
                    db.exec("UPDATE events SET data=? WHERE id=? AND task_id=?", {data, latest.at("id"), current.id});
                }
            }
            recordSessionRead(current, latest,
                              {{"environmentId", env},
                               {"threadId", thread},
                               {"runId", target->data.resultRunId},
                               {"view", "report"},
                               {"reportAvailable", available},
                               {"text", bounded(evidence.detail, 2048)}},
                              decision.usage);
            return true;
        });
    }

    Json sessionResultReference(const RunRef& run, const Json& environment, std::string_view resultRun, std::string_view item,
                                std::string_view status) const {
        return {{"environmentId", environment},
                {"threadId", run.threadId},
                {"runId", resultRun},
                {"itemId", item},
                {"link", run.link},
                {"status", status}};
    }

    void finishPeer(const PeerExchange& exchange, TaskData& recipient, TaskData sender, std::string_view status, std::string_view text,
                    const std::string& action) {
        const auto type = status == "completed" ? std::string_view{"result"} : status;
        auto message = peerMessage(exchange, recipient, sender, type, text, action);
        message.status = status;
        if (const auto report = status != "cancelled" ? settledSessionReport(recipient) : std::nullopt) {
            const auto& data = report->data;
            message.evidence.push_back(
                {data.environmentId, data.run.threadId, data.resultRunId, data.evidence.itemId, data.run.link, data.status});
        }
        transitionPeer(exchange, recipient, sender, message);
    }

    Json replyKinds(const TaskData& task, const PeerExchange& peer) const {
        if (peer.sender == task.id && peer.phase == "question") return Json::array({"answer"});
        if (peer.recipient != task.id || peer.phase != "working") return Json::array();
        Json kinds = {"question"};
        const bool revisedWork = peer.kind == "work" && (!task.pendingInstructions.empty() || task.replanning || task.newSessionRequired ||
                                                         (task.lastRun && task.lastRun->assignment.revisionId != task.currentRevision));
        if (!revisedWork) {
            kinds.push_back("failed");
            if (!sessionReportUnavailable(task)) kinds.push_back("result");
        }
        return kinds;
    }

    bool replyPeer(TaskData& task, const Json& event, const TaskDecision& decision, const std::string& action) {
        const auto peer = db.peerExchange(task.id);
        if (!peer) throw Conflict("The Bot exchange has closed");
        const auto type = field(decision.args, "kind");
        const auto body = field(decision.args, "text");
        const auto kinds = replyKinds(task, *peer);
        if (std::find(kinds.begin(), kinds.end(), type) == kinds.end())
            throw Conflict("This reply does not match the open exchange and current task revision");
        const bool answering = type == "answer";
        const auto otherId = answering ? peer->recipient : peer->sender;
        auto other = db.load(otherId);
        if (db.botPaused(other.botId)) throw BotPaused{};
        if (terminal(other) || other.state != TaskState::peer_waiting) throw Conflict("The other Bot is no longer waiting");
        const bool closing = type == "result" || type == "failed";
        if (closing && db.conversationPending(task.botId)) return false;
        recordDecision(task, event, decision, action);
        if (closing) {
            const auto outcome = type == "result" ? TaskPeerOutcome::completed : TaskPeerOutcome::failed;
            const auto status = taskPeerOutcomeName(outcome);
            task.state = TaskState::completed;
            task.peerOutcome = outcome;
            finishPeer(*peer, task, std::move(other), status, body, action);
            db.journal(task.id, "peer_completed",
                       {{"text", body}, {"status", status}, {"reason", decision.reason}, {"doneWhen", task.doneWhen}});
        } else {
            transitionPeer(*peer, task, other, peerMessage(*peer, task, other, type, body, action));
        }
        return true;
    }

    void readPeerEvidence(const TaskData& task, const Json& event, const TaskDecision& decision) {
        const auto& args = decision.args;
        const auto messageId = field(args, "messageId");
        if (event["kind"] != "bot_result" || messageId.empty() || event["messageId"] != messageId)
            throw Conflict("Choose the Bot result for the current exchange");
        allowedPeer(task.botId, field(event, "fromBotId"));
        const auto refs = event.value("references", Json::array());
        if (refs.empty()) throw Conflict("The Bot result has no shared Session history");
        const auto& ref = refs.front();
        ReadRequest req;
        req.environmentId = field(ref, "environmentId");
        req.threadId = field(ref, "threadId");
        req.runId = field(ref, "runId");
        req.itemId = field(args, "itemId");
        req.cursor = field(args, "cursor");
        req.offset = readOffset(args);
        {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT 1 FROM peer_exchanges WHERE id=? AND sender_task=? AND last_message=?",
                                {event["exchangeId"], task.id, messageId});
            if (!row.next()) throw Conflict("The Bot result does not belong to this task");
            req.config = task.config;
        }
        if (req.threadId.empty() || req.runId.empty()) throw Conflict("The Bot result has no readable Run");
        if (!policy.allows(task.botId, Resource::sessionRead(req.environmentId, req.threadId)))
            throw Conflict("Session read access required");
        const PeerEvidenceEntry entry{event.at("exchangeId").get<std::string>(), messageId, readExcerpt(task.botId, req)};
        change(task, [&](TaskData& current) {
            appendEventEvidence(current, event, encodePeerEvidence(entry));
            db.journal(current.id, entry, decision.usage);
            return true;
        });
    }

    void acceptReplyOrCompletion(const TaskData& snapshot, const Json& event, const TaskDecision& decision) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        const auto id = snapshot.id;
        auto current = db.load(id);
        if (current.generation != snapshot.generation || current.eventId != event["id"] ||
            current.currentRevision != snapshot.currentRevision || current.state != TaskState::deciding)
            return;
        if (decision.tool == "reply_to_bot") {
            if (replyPeer(current, event, decision, actionId())) saveTask(current);
        } else {
            if (db.peerExchange(id) || !canComplete(current, event)) return;
            complete(current, event, decision);
        }
        transaction.commit();
    }

    CompletionProvenance completionProvenance(const TaskData& task, const Json& event) {
        const auto peer = peerResult(task, event);
        if (!peer) {
            const auto& report = event.contains("sessionResult") ? event.at("sessionResult") : event;
            if (report.is_null()) return std::monostate{};
            const auto run = runRef(report.value("run", Json(nullptr)));
            if (!hasRun(run)) return std::monostate{};
            return OutboxSourceRun{.threadId = run.threadId,
                                   .runId = field(report, "resultRunId"),
                                   .environmentId = report.at("environmentId").get<std::string>(),
                                   .link = run.link,
                                   .itemId = field(report, "itemId"),
                                   .status = field(report, "status")};
        }

        PeerCompletion result{peer->taskId,
                              event.at("exchangeId").get<std::string>(),
                              event.at("messageId").get<std::string>(),
                              field(event, "fromBotId"),
                              peer->kind,
                              event.at("status").get<std::string>(),
                              {}};
        const auto allowed = peersFor(task.botId);
        const bool peerAllowed =
            std::ranges::any_of(allowed, [&](const PeerBot& candidate) { return candidate.intake.botId == result.botId; });
        const auto addRef = [&](CompletionEvidence ref, bool fromPeer) {
            if ((fromPeer && !peerAllowed) || ref.runId.empty() ||
                !policy.allows(task.botId, Resource::sessionRead(ref.environmentId, ref.threadId)))
                return;
            if (ref.itemId && ref.itemId->empty()) ref.itemId.reset();
            if (fromPeer) ref.messageId = result.messageId;
            if (std::ranges::find(result.evidence, ref) == result.evidence.end()) result.evidence.push_back(std::move(ref));
        };
        const auto addEventRef = [&](const Json& record, CompletionEvidenceKind kind, bool fromPeer) {
            addRef({field(record, "environmentId"), field(record, "threadId"), field(record, "runId"), kind, field(record, "itemId"), {}},
                   fromPeer);
        };
        for (const auto& record : event.value("references", Json::array())) addEventRef(record, CompletionEvidenceKind::peer_report, true);
        auto reads = db.query(R"SQL(
SELECT id,data FROM journal WHERE task_id=? AND kind='read_bot_evidence'
  AND exchange_id=? AND peer_message_id=? ORDER BY id
)SQL",
                              {task.id, result.exchangeId, result.messageId});
        while (reads.next()) {
            auto record = decodePeerEvidence(reads.number(0), reads.json(1));
            auto& excerpt = record.excerpt;
            addRef({std::move(excerpt.environmentId),
                    std::move(excerpt.threadId),
                    std::move(excerpt.runId),
                    CompletionEvidenceKind::session_read,
                    std::move(excerpt.itemId),
                    {}},
                   true);
        }
        for (const auto& record : event.at("evidence"))
            if (!record.contains("exchangeId")) addEventRef(record, CompletionEvidenceKind::session_read, false);
        return result;
    }

    void complete(TaskData& current, const Json& event, const TaskDecision& decision) {
        const auto& pending = current.pendingInstructions;
        auto ids = Json::array();
        for (const auto& item : pending) ids.push_back({{"messageId", item.messageId}, {"step", item.step}});
        const bool requiresAcknowledgment = !pending.empty();
        if (requiresAcknowledgment != decision.args.contains("acknowledgedInstructions") ||
            decision.args.value("acknowledgedInstructions", Json::array()) != ids)
            throw Conflict("Completion must acknowledge each pending instruction by messageId and step, oldest first");
        if (db.conversationPending(current.botId)) return;
        consumeEvent(current);
        recordDecision(current, event, decision, actionId());
        current.pendingInstructions.clear();
        current.newSessionRequired = false;
        current.state = TaskState::completed;
        db.journal(current.id, CompletionEntry{field(decision.args, "report"), current.currentRevision,
                                               TaskCompletion{decision.reason, current.doneWhen, completionProvenance(current, event)}});
        saveTask(current);
    }

    void accept(const TaskData& snapshot, const Json& event, const TaskDecision& decision, std::string_view approvedInvocation = {}) {
        const auto& args = decision.args;
        const auto& tool = decision.tool;
        if (isGitTool(tool)) {
            Json result;
            const auto invocation = approvedInvocation.empty() ? gitInvocation(decision) : std::string(approvedInvocation);
            try {
                result = db.gitTool(*options.gitHost, snapshot.botId, snapshot.id, snapshot.environmentId, snapshot.projectId, decision,
                                    invocation, snapshot.generation);
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception& error) {
                result = {{"ok", false}, {"error", db.sharedRedactor().text(error.what())}};
            }
            if (result.value("policyWaiting", false)) return;
            change(snapshot, [&](TaskData& current) {
                db.exec("UPDATE policy_approvals SET resume=NULL WHERE operation=?",
                        {"git/" + std::to_string(current.id) + "/" + invocation});
                auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=?", {event.at("id"), current.id});
                if (!row.next()) return false;
                auto data = row.json(0);
                if (!data.contains("gitResults")) data["gitResults"] = Json::array();
                if (result.contains("readId"))
                    data["gitResults"].push_back({{"tool", tool}, {"readId", result.at("readId")}});
                else
                    data["gitResults"].push_back(db.redactor(current.botId).record(Json{{"tool", tool}, {"result", std::move(result)}}));
                if (data["gitResults"].size() > 4) data["gitResults"].erase(data["gitResults"].begin());
                db.exec("UPDATE events SET data=? WHERE id=?", {data, event.at("id")});
                return true;
            });
            return;
        }
        static const std::map<std::string_view, std::set<std::string_view>> arguments{
            {"delegate", {"instruction", "doneWhen"}},
            {"answer_session_question", {"requestId", "answers"}},
            {"follow_up", {"instruction"}},
            {"complete", {"report", "acknowledgedInstructions"}},
            {"ask_user", {"question", "choices"}},
            {"session_read", {"threadId", "runId", "itemId", "cursor", "view", "offset"}},
            {"send_bot_message", {"botId", "kind", "text", "doneWhen", "forwardUserMessage"}},
            {"reply_to_bot", {"kind", "text"}},
            {"read_bot_evidence", {"messageId", "itemId", "cursor", "offset"}}};
        for (auto item = args.begin(); item != args.end(); ++item) {
            if (!arguments.at(tool).contains(item.key())) throw Conflict("Unknown action argument");
        }
        const bool setsCriteria = tool == "delegate" || (tool == "send_bot_message" && field(args, "kind") == "work");
        const bool requireCriteria = setsCriteria && needsDoneWhen(snapshot);
        if (requireCriteria != args.contains("doneWhen") || (requireCriteria && !present(field(args, "doneWhen"))))
            throw Conflict("Only first work without saved criteria requires doneWhen");
        if (tool == "session_read") {
            if (field(args, "view") == "report") return retryRunReport(snapshot, event, decision);
            if (options.sessionRead != "on_demand") throw Conflict("Only the terminal report can be read for this event");
            readEvidence(snapshot, event, args, decision.usage);
            return;
        }
        if (tool == "read_bot_evidence") return readPeerEvidence(snapshot, event, decision);
        if (tool == "send_bot_message" || tool == "reply_to_bot") {
            if (args.contains("forwardUserMessage") && !args.at("forwardUserMessage").is_boolean())
                throw Conflict("forwardUserMessage must be a boolean");
            if (args.contains("forwardUserMessage") && !snapshot.sourceMessageCoversFullRequest.value_or(false))
                throw Conflict("Forwarding is unavailable for this request");
            const auto text = field(args, "text");
            if ((args.contains("text") && !args.at("text").is_string()) ||
                (!present(text) && !(text.empty() && args.value("forwardUserMessage", false))))
                throw Conflict("A Bot message requires text or a forwarded user message");
            if (tool == "reply_to_bot") return acceptReplyOrCompletion(snapshot, event, decision);
            const auto kind = field(args, "kind");
            if (kind != "consult" && kind != "work") throw Conflict("Choose a consultation or work request");
            auto peer = allowedPeer(snapshot.botId, field(args, "botId"));
            change(snapshot, [&](TaskData& current) {
                sendPeer(current, event, decision, std::move(peer), actionId());
                return true;
            });
            return;
        }
        const bool work = tool == "delegate" || tool == "follow_up";
        const auto instruction = field(args, "instruction");
        if (work && !present(instruction)) throw Conflict("A delegate/follow-up instruction is required");
        if (tool == "ask_user") {
            if (!present(field(args, "question"))) throw Conflict("A user question is required");
            if (args.contains("choices")) {
                const auto& choices = args["choices"];
                if (!choices.is_array() || choices.size() > 12 || std::any_of(choices.begin(), choices.end(), [](const Json& value) {
                        return !value.is_string() || !present(value.get<std::string>(), 200);
                    }))
                    throw Conflict("Expected up to twelve short textual choices");
            }
        }
        if (tool == "complete") {
            if (!present(field(args, "report"))) throw Conflict("A completion report is required");
            return acceptReplyOrCompletion(snapshot, event, decision);
        }
        if (tool == "answer_session_question") {
            if (!snapshot.question || field(args, "requestId") != snapshot.question->target.requestId)
                throw Conflict("The Session question no longer matches");
            sessionQuestionAnswers(snapshot.question->fields, args.value("answers", Json::object()));
        }
        change(snapshot, [&](TaskData& current) {
            if (tool == "follow_up" && current.newSessionRequired) throw Conflict("Start a new Session in the current environment");
            bool charge = false;
            if (work && !admitWork(current, decision, charge)) return true;
            if (work) establishCriteria(current, args);
            consumeEvent(current);
            const auto action = actionId();
            recordDecision(current, event, decision, action);
            if (tool == "ask_user") {
                const auto waitId = db.wait(current,
                                            ModelQuestionWait{args.value("choices", std::vector<std::string>{}), decision.reason,
                                                              current.question ? current.question->target.requestId : std::string{}},
                                            field(args, "question"));
                if (current.question) current.question->target = questionTarget(current, waitId);
            } else if (work && !current.runTargets().empty()) {
                queueDestinations(current, action, instruction, args, event, charge);
            } else {
                queueAction(current, action, commandToolFromName(tool).value(), instruction, args, event, charge);
            }
            return true;
        });
    }

    bool forwardedQuestionAnswer(const TaskData& task, const QuestionTarget& target) {
        std::lock_guard lock(db.mutex);
        const auto event = db.currentEvent(task);
        const auto peer = db.peerExchange(task.id);
        if (!peer || peer->recipient != task.id || peer->kind != "work" || peer->phase != "working" || event.is_null() ||
            event["consumed"] == true || field(event, "kind") != "bot_answer" || field(event, "messageType") != "answer" ||
            field(event, "requestKind") != peer->kind || field(event, "exchangeId") != peer->id ||
            field(event, "messageId") != peer->lastMessage || field(event, "toBotId") != task.botId)
            return false;
        const auto sender = db.load(peer->sender);
        if (field(event, "fromBotId") != sender.botId) return false;
        auto original = db.query("SELECT data FROM events WHERE id=? AND task_id=?", {target.eventId, task.id});
        if (!original.next()) return false;
        const auto question = original.json(0);
        const auto questionRun = question.value("run", Json::object());
        return target.run && field(question, "kind") == "question" && field(questionRun, "threadId") == target.run->run.threadId &&
               field(questionRun, "runId") == target.run->run.runId &&
               field(question.value("question", Json::object()), "requestId") == target.requestId;
    }

    bool questionStillOpen(const TaskData& task) {
        const auto observed = observe(task);
        if (ingest(task, observed) || observed.kind != Observation::Kind::Question) return false;
        if (!task.question || task.question->target.requestId != observed.question.requestId) return false;
        auto target = questionTarget(task);
        const auto& savedTarget = task.question->target;
        if (savedTarget == target) return true;
        target.eventId = savedTarget.eventId;
        return savedTarget == target && forwardedQuestionAnswer(task, savedTarget);
    }

    std::optional<TaskDecision> persistTaskInference(Id id) {
        const auto found = taskInferences.find(id);
        const auto& inference = found->second;
        Transaction transaction(db.db);
        auto current = db.load(id);
        if (!inference.usage.empty()) db.exec("UPDATE model_attempts SET usage=? WHERE id=?", {inference.usage, inference.attemptId});
        std::optional<TaskDecision> decision;
        if (current.state == TaskState::deciding && current.generation == inference.generation && current.eventId == inference.eventId &&
            current.currentRevision == inference.revisionId) {
            if (const auto* error = std::get_if<ModelError>(&inference.outcome)) {
                const auto cause = error->code == ModelErrorCode::unsupported_reasoning_effort ? FailureCause::UnsupportedReasoningEffort
                                                                                               : FailureCause::Model;
                recordFailure(current, db.currentEvent(current), error->what(), error->transient, cause);
                saveTask(current);
            } else {
                const auto& call = std::get<ModelCall>(inference.outcome);
                decision = TaskDecision{call, inference.usage, inference.attemptId, call.rejection};
                db.saveDecisionCheckpoint(current, {inference.generation, inference.eventId, inference.revisionId, *decision});
            }
        }
        transaction.commit();
        taskInferences.erase(found);
        return decision;
    }

    void persistTaskInferences() {
        std::lock_guard lock(db.mutex);
        while (!taskInferences.empty()) persistTaskInference(taskInferences.begin()->first);
    }

    std::optional<TaskDecision> inferDecision(const TaskData& task, Json event, const Json& history, const std::vector<std::string>& tools,
                                              std::span<const OutboxInstruction> pending, Json choices,
                                              const std::optional<PeerExchange>& peer) {
        const auto prefill = prefillHistory(task, event);
        if (prefill == PrefillResult::Persisted) return std::nullopt;
        if (prefill == PrefillResult::UnavailableWithSettledReport) event["historyUnavailable"] = true;
        const auto bot = db.loadNotes(task.botId, soul, options.clock());
        Json summary = {{"id", task.id},
                        {"botId", task.botId},
                        {"request", task.intent},
                        {"doneWhen", task.doneWhen},
                        {"remaining", task.allowance.remaining},
                        {"additionalUsed", task.allowance.additionalUsed},
                        {"revisionId", revisionJson(task.currentRevision)}};
        if (!task.runTargets().empty()) {
            summary["runTargets"] = runTargetsJson(task.runTargets());
            if (task.requestedCommit) summary["requestedCommit"] = *task.requestedCommit;
        }
        if (task.sourceMessageId) summary["sourceMessageId"] = *task.sourceMessageId;
        if (task.selectedSession && !task.newSessionRequired) summary["usesSelectedSession"] = true;
        if (task.criteriaProposal) summary["requestedDoneWhen"] = task.criteriaProposal->doneWhen;
        if (task.restartContext) summary["restartContext"] = taskRestartJson(*task.restartContext);
        if (!task.pendingInstructions.empty()) {
            summary["userInstructions"] = instructionsJson(pending);
            summary["pendingInstructionCount"] = task.pendingInstructions.size();
        }
        if (task.trigger) summary["trigger"] = taskTriggerJson(*task.trigger);
        if (!task.sourceResultRefs.empty() || task.workflow) summary["sourceResultRefs"] = sourceResultsJson(task.sourceResultRefs);
        if (!choices.empty()) summary["peers"] = std::move(choices);
        if (std::ranges::find(tools, "send_bot_message") != tools.end()) {
            std::lock_guard lock(db.mutex);
            if (auto forwarded = forwardableUserMessage(task)) summary["forwardableUserMessage"] = encodeForwardedPeerMessage(*forwarded);
        }
        if (peer)
            summary["peer"] = {{"exchangeId", peer->id},
                               {"requestKind", peer->kind},
                               {"replyKinds", replyKinds(task, *peer)},
                               {"phase", peer->phase},
                               {"role", peer->sender == task.id ? "sender" : "recipient"}};
        const auto evidence = event["evidence"];
        const bool canReadHistory =
            std::ranges::find(tools, "session_read") != tools.end() || std::ranges::find(tools, "read_bot_evidence") != tools.end();
        summary["remainingEvidenceReads"] = canReadHistory ? maxEvidenceReads - evidence.size() : 0;
        event.erase("evidence");
        event.erase("consumed");
        Id attempt = 0;
        {
            std::lock_guard lock(db.mutex);
            Transaction transaction(db.db);
            if (shutdown.stop_requested()) return std::nullopt;
            auto current = db.load(task.id);
            if (current.generation != task.generation || current.eventId != event["id"] ||
                current.currentRevision != task.currentRevision || current.state != TaskState::deciding)
                return std::nullopt;
            attempt = db.takeReservedTaskCall(event["id"]);
            if (!attempt) attempt = db.reserveModelCall(db.taskModelBudget(task.id), "task_decision", 0, task.id, event["id"]);
            if (!attempt) {
                db.wait(current, SimpleWaitReason::model_budget, "This request used its step allowance.");
                saveTask(current);
            }
            transaction.commit();
        }
        if (!attempt) return std::nullopt;
        if (options.gitHost && event.at("kind") == "result") db.projectTaskGitReads(*options.gitHost, task, event);
        TaskInference inference{task.generation, event.at("id"), attempt, task.currentRevision};
        try {
            auto turn = db.callModel(task.botId, model,
                                     {summary, event, history, bot, tools, task.feedback.value_or(""), evidence, task.config,
                                      needsDoneWhen(task), summary.contains("forwardableUserMessage")});
            inference.usage = std::move(turn.usage);
            if (turn.calls.size() == 1)
                inference.outcome = std::move(turn.calls.front());
            else
                inference.outcome = ModelError("Return one task action", false);
        } catch (const ModelError& error) {
            inference.usage = error.usage;
            inference.outcome = error;
        }
        const auto id = task.id;
        std::lock_guard lock(db.mutex);
        taskInferences.insert_or_assign(id, std::move(inference));
        return persistTaskInference(id);
    }

    void decide(const TaskData& task) {
        if (task.activeRun && !questionStillOpen(task)) return;
        if (shutdown.stop_requested()) return;
        Json event, history = Json::array();
        std::optional<PeerExchange> peer;
        std::optional<TaskDecision> decision;
        {
            std::lock_guard lock(db.mutex);
            event = db.currentEvent(task);
            if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > options.clock()) return;
            peer = db.peerExchange(task.id);
            if (task.pendingDecision && checkpointMatches(*task.pendingDecision, task)) decision = task.pendingDecision->decision;
            if (event["kind"] != "result" && task.lastRun) {
                if (const auto report = settledSessionReport(task)) event["sessionResult"] = encodeRunReport(report->data);
            }
            if (hasSettledRun(task) && event.at("evidence").size() < maxEvidenceReads) {
                auto views = options.sessionRead == "on_demand" ? Json::array({"messages", "activity"}) : Json::array();
                if (reportRetry(task, event)) views.push_back("report");
                if (!views.empty()) event["sessionReadViews"] = std::move(views);
            }
            auto errors = db.query(R"SQL(
SELECT id,data FROM journal WHERE task_id=? AND kind='tool_error' AND feedback_event_id=?
AND id>coalesce((SELECT max(id) FROM journal WHERE task_id=? AND kind='model_retry'),0) ORDER BY id
)SQL",
                                   {task.id, event["id"], task.id});
            event["decisionFeedback"] = Json::array();
            while (errors.next()) event["decisionFeedback"].push_back(encodeToolError(decodeToolError(errors.number(0), errors.json(1))));
            if (!decision) {
                auto rows = db.query(R"SQL(
SELECT id,kind,data,json_extract(data,'$.run.threadId'),json_extract(data,'$.resultRunId'),
       json_extract(data,'$.itemId'),coalesce(json_extract(data,'$.environmentId'),json_extract(data,'$.run.environmentId')) FROM journal WHERE task_id=?
AND kind IN ('decision','report','user_answer','session_answered','bot_message') ORDER BY id DESC LIMIT 6
)SQL",
                                     {task.id});
                std::size_t remaining = 16000;
                while (rows.next() && remaining > 100) {
                    const auto data = rows.string(2);
                    const auto excerpt = bounded(data, std::min<std::size_t>(remaining, 5000));
                    if (data.size() == excerpt.size()) {
                        auto record = Json::parse(data);
                        const auto kind = rows.string(1);
                        if (kind == "decision") record = encodeJournalDecision(decodeJournalDecision(rows.number(0), record));
                        history.push_back({{"id", rows.number(0)}, {"kind", kind}, {"data", std::move(record)}});
                    } else
                        history.push_back({{"id", rows.number(0)},
                                           {"kind", rows.string(1)},
                                           {"excerpt", excerpt},
                                           {"truncated", true},
                                           {"source",
                                            {{"environmentId", rows.string(6)},
                                             {"threadId", rows.string(3)},
                                             {"runId", rows.string(4)},
                                             {"itemId", rows.string(5)}}}});
                    remaining -= excerpt.size();
                }
                std::reverse(history.begin(), history.end());
            }
        }
        try {
            auto choices = Json::array();
            bool canRead = false;
            if (!peer && !task.activeRun && task.actionId.empty()) {
                for (const auto& allowed : peersFor(task.botId)) {
                    choices.push_back({{"botId", allowed.intake.botId}, {"name", allowed.name}});
                    if (event.contains("references") && !event["references"].empty() && event["fromBotId"] == allowed.intake.botId)
                        canRead = true;
                }
            }
            std::vector<OutboxInstruction> pending;
            if (!task.pendingInstructions.empty()) {
                std::lock_guard lock(db.mutex);
                pending = workPrompt(task, event, 0).pending;
            }
            auto tools = available(task, event, peer, !choices.empty(), canRead);
            if (options.gitHost && !task.activeRun && event["kind"] == "result")
                tools.insert(tools.end(), gitTools(false).begin(), gitTools(false).end());
            if (task.pendingInstructions.size() != pending.size()) std::erase(tools, "complete");
            const bool replay = decision.has_value();
            if (!decision) decision = inferDecision(task, event, history, tools, pending, std::move(choices), peer);
            if (!decision || shutdown.stop_requested()) return;
            if (task.activeRun && !questionStillOpen(task)) return;
            if (!decision->rejection.empty()) {
                rejectDecision(task, event, *decision, decision->rejection);
                return;
            }
            if (replay) {
                auto call = db.checkModelCall(task.botId, *decision, tools, false);
                if (!call.rejection.empty()) {
                    rejectDecision(task, event, TaskDecision{std::move(call), decision->usage, decision->attemptId}, call.rejection, true);
                    return;
                }
                decision->args = std::move(call.args);
                decision->reason = std::move(call.reason);
            }
            try {
                if (!decision->args.is_object() || decision->args.dump().size() > 20000 ||
                    std::ranges::find(tools, decision->tool) == tools.end())
                    throw Conflict("Return exactly one permitted action with valid arguments");
                if (recordsTaskDecisionReason(decision->tool) && !validDecisionReason(decision->reason))
                    throw Conflict("This action requires a brief reason");
                accept(task, event, *decision);
            } catch (const Conflict& error) {
                rejectDecision(task, event, *decision, error.what());
            }
        } catch (const BotNotesUnavailable& e) {
            failure(task, event, e.what(), false, FailureCause::BotNotes);
        } catch (const ModelError& e) {
            failure(task, event, e.what(), e.transient);
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception& e) {
            failure(task, event, e.what(), false);
        }
    }

    bool shouldStop(const TaskData& task) {
        if (db.replanReady(task)) return true;
        if (task.state != TaskState::cancelling) return false;
        std::lock_guard lock(db.mutex);
        const auto peer = db.peerExchange(task.id);
        return !peer || peer->recipient == task.id;
    }

    bool continueStage(const TaskData& task) {
        if ((!task.workflow && !task.freshContextRequested) || !hasSettledRun(task) || task.pendingInstructions.empty()) return false;
        return change(task, [&](TaskData& current) {
            auto event = db.currentEvent(current);
            if (!event.is_null() && event.at("consumed") != true && (event.at("kind") != "result" || event.contains("userAnswer")))
                return false;
            if (event.is_null()) event = Json::object();
            const auto tool = current.newSessionRequired ? CommandTool::delegate : CommandTool::follow_up;
            const TaskDecision decision{
                {std::string(commandToolName(tool)), Json::object(), "Apply accepted user instructions after the assigned Run settled"}};
            bool charge = false;
            if (!admitWork(current, decision, charge)) return true;
            const auto action = actionId();
            if (event.contains("id")) recordDecision(current, event, decision, action);
            consumeEvent(current);
            queueAction(current, action, tool, "Continue this Flock task.", decision.args, event, charge);
            return true;
        });
    }

    bool resumeGitApproval(const TaskData& task) {
        Json saved, event;
        {
            std::lock_guard lock(db.mutex);
            auto row = db.query("SELECT resume FROM policy_approvals WHERE task=? AND generation=?"
                                " AND turn=0 AND resume IS NOT NULL AND status<>'pending' ORDER BY id LIMIT 1",
                                {task.id, task.generation});
            if (!row.next()) return false;
            saved = row.json(0);
            event = db.currentEvent(task);
        }
        if (!options.gitHost || event.is_null()) return false;
        const TaskDecision decision{{field(saved, "tool"), saved.at("args"), field(saved, "reason")}};
        accept(task, event, decision, field(saved, "invocation"));
        return true;
    }

    void step(const TaskScheduler::Work& work) {
        if (shutdown.stop_requested()) return;
        const auto id = work.taskId;
        auto task = snapshot(id);
        const auto state = task.state;
        if (work.destinationAction) {
            auto& run = destinationRun(task, *work.destinationAction);
            if (run.outstanding()) stepDestination(task, run);
            return;
        }
        if (!task.runResults().empty()) return;
        if (task.pendingStageReport)
            change(task, [&](TaskData& current) { return db.settleStageReport(current); });
        else if (shouldStop(task))
            stop(task);
        else if (task.pendingRunReport)
            recoverRunReport(task);
        else if (state == TaskState::queued)
            activate(task);
        else if (state == TaskState::deciding) {
            if (!resumeGitApproval(task) && !continueStage(task)) decide(task);
        } else {
            if (task.activeRun && (state == TaskState::running || state == TaskState::waiting) && ingest(task, observe(task))) return;
            const bool reconcile = state == TaskState::waiting && (task.workflow || task.selectedSession) && !task.actionId.empty();
            if (state != TaskState::sending && !reconcile) return;
            std::optional<Outbox> action;
            {
                std::lock_guard lock(db.mutex);
                action = db.outbox(task);
            }
            if (!action) return;
            if (reconcile) {
                const auto now = options.clock();
                const bool selectedStart = action->command.tool == CommandTool::start_in_session;
                if (!selectedStart && !task.workflow) return;
                const auto reconcileAt = selectedStart ? &TaskData::sessionReconcileAt : &TaskData::workflowReconcileAt;
                if (action->phase != OutboxPhase::started || now < (task.*reconcileAt).value_or(0)) return;
                if (!change(task, [&](TaskData& current) {
                        current.*reconcileAt = now + 5;
                        return true;
                    }))
                    return;
            }
            deliver(task, std::move(*action));
        }
    }
};

Runtime::Runtime(Store& store, Model& model, T3& t3, SoulLoader soul, RuntimeOptions options)
    : p_(std::make_unique<Impl>(store, model, t3, std::move(soul), std::move(options))) {}
Runtime::~Runtime() = default;
void Runtime::requestStop() {
    p_->shutdown.request_stop();
}
void Runtime::runConversations() {
    if (p_->shutdown.stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->db.deliverGitWebhooks();
    p_->db.settleSessionQueueCancellations(p_->t3, p_->shutdown.get_token());
    p_->db.wakeSessionMessages(p_->t3, p_->shutdown.get_token());
    p_->db.deliverChannels(p_->shutdown.get_token());
    constexpr auto messagesPerTick = 20;
    for (int i = 0; i < messagesPerTick && !p_->shutdown.stop_requested(); ++i) {
        const auto pending = p_->db.pendingConversations(1, p_->options.clock());
        if (pending.empty()) break;
        p_->runTurn(pending.front().second);
    }
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tick() {
    if (p_->shutdown.stop_requested()) return;
    p_->maintenance.settleApprovals();
    p_->persistTaskInferences();
    p_->maintenance.fireDueAutomations();
    p_->db.deliverGitWebhooks();
    p_->db.settleSessionQueueCancellations(p_->t3, p_->shutdown.get_token());
    p_->db.wakeSessionMessages(p_->t3, p_->shutdown.get_token());
    for (const auto& [bot, id] : p_->db.pendingConversations(Impl::batchLimit, p_->options.clock())) p_->runTurn(id);
    p_->maintenance.advanceWorkflows();
    for (const auto& work : p_->taskScheduler.nextBatch(Impl::batchLimit, [this](const auto& job) { return p_->workers.contains(job); }))
        p_->step(work);
    p_->db.deliverChannels(p_->shutdown.get_token());
}
void Runtime::tickConcurrent() {
    if (p_->shutdown.stop_requested()) return;
    p_->maintenance.settleApprovalsIfChanged();
    const auto collect = [this](auto& workers, std::string_view operation) {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto result = std::move(it->second);
                it = workers.erase(it);
                p_->maintenance.deferContendedWork(operation, [&] { result.get(); });
            } else
                ++it;
        }
    };
    collect(p_->conversationWorkers, "conversation worker");
    collect(p_->workers, "task worker");
    if (p_->channelWorker.valid() && p_->channelWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        p_->maintenance.deferContendedWork("channel worker", [&] { p_->channelWorker.get(); });
    p_->maintenance.fireDueAutomationsIfChanged();
    p_->maintenance.deferContendedWork("Git webhooks", [&] { p_->db.deliverGitWebhooks(); });
    if (!p_->channelWorker.valid() && p_->db.channelDeliveryDue())
        p_->channelWorker = std::async(std::launch::async, [db = &p_->db, stop = p_->shutdown.get_token()] { db->deliverChannels(stop); });
    if (p_->sessionWakeWorker.valid() && p_->sessionWakeWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        p_->maintenance.deferContendedWork("Session wake worker", [&] { p_->sessionWakeWorker.get(); });
    if (!p_->sessionWakeWorker.valid())
        p_->sessionWakeWorker = std::async(std::launch::async, [impl = p_.get()] {
            impl->db.settleSessionQueueCancellations(impl->t3, impl->shutdown.get_token());
            impl->db.wakeSessionMessages(impl->t3, impl->shutdown.get_token());
        });
    constexpr auto maxConversationWorkers = Impl::batchLimit;
    if (p_->conversationWorkers.size() < maxConversationWorkers) {
        for (const auto& [bot, id] : p_->db.pendingConversations(maxConversationWorkers, p_->options.clock())) {
            if (p_->conversationWorkers.size() >= maxConversationWorkers) break;
            if (!p_->conversationWorkers.contains(bot)) {
                p_->conversationWorkers.emplace(bot, std::async(std::launch::async, [impl = p_.get(), id] { impl->runTurn(id); }));
            }
        }
    }
    p_->maintenance.advanceWorkflowsIfChanged();
    p_->maintenance.deferContendedWork("task scheduling", [&] {
        p_->persistTaskInferences();
        for (const auto& work : p_->taskScheduler.nextBatch(Impl::batchLimit - p_->workers.size(),
                                                            [this](const auto& job) { return p_->workers.contains(job); }))
            p_->workers.emplace(work, std::async(std::launch::async, [impl = p_.get(), work] { impl->step(work); }));
    });
}
} // namespace flockv0
