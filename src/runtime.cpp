#include "storage.hpp"
#include "flock/limits.hpp"
#include "journal.hpp"
#include "journal_peer.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "runtime_maintenance.hpp"
#include "runtime_task_scheduler.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_session_prompts.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_task_questions.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
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
struct TaskInference {
    Id generation, eventId, attemptId;
    std::optional<Id> revisionId;
    Json usage = Json::object();
    std::variant<ModelCall, ModelError> outcome{};
};

std::string gitInvocation(const TaskDecision& decision) {
    return "attempt/" + std::to_string(decision.attemptId);
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
    SessionPrompts prompts;
    TaskUpdates updates;
    TaskQuestions questions;
    SessionHistory history;
    RunReports reports;
    PeerExchanges peerExchanges;
    std::future<void> channelWorker, sessionWakeWorker;
    std::stop_source shutdown;
    RunObserver runs;
    OutboxDispatch dispatch;
    int lockFd = -1;
    std::map<TaskScheduler::Work, std::future<void>> workers;
    // Keep provider outcomes until the Task decision and attempt usage are committed together.
    std::map<Id, TaskInference> taskInferences;
    std::map<std::string, std::future<void>> conversationWorkers;

    void runTurn(Id id) {
        db.runTurn(id, model, t3, soul, options.clock, *options.botTools, options.gitHost.get(), options.applyBotChange,
                   options.reconcileBotCreation, options.intake, options.peers, shutdown.get_token());
    }

    Impl(Store& store, Model& m, T3& adapter, SoulLoader loader, RuntimeOptions opts)
        : db(*store.p_), policy(store.policy()), model(m), t3(adapter), soul(std::move(loader)), options(std::move(opts)),
          maintenance(db, options.clock, options.automationError, options.storageContention), taskScheduler(db), prompts(db), updates(db),
          questions(db), history(db, t3, policy, updates, options.sessionRead), reports(db, t3, policy, updates, history, options.clock),
          peerExchanges(db, policy, updates, reports, history, options.peers),
          runs(db, t3, updates, questions, reports, peerExchanges, options.clock, shutdown.get_token()),
          dispatch(db, t3, policy, updates, questions, reports, runs, prompts, options.clock, options.intake, options.requireT3Connection,
                   shutdown.get_token()) {
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

    static constexpr unsigned batchLimit = 4;

    bool selectProject(TaskData& task) {
        const auto bot = task.botId;
        const auto environment = task.environmentId;
        {
            std::lock_guard lock(db.mutex);
            if (db.projectQuestionOpen(bot, environment)) return false;
        }
        auto catalog = db.readProjectCatalog(t3, bot, environment, options.requireT3Connection);
        return updates.change(
                   task,
                   [&](TaskData& current) { return !shutdown.stop_requested() && db.applyProjectCatalog(current, std::move(catalog)); },
                   &task) &&
               !task.projectId.empty();
    }

    void activate(TaskData task) {
        if (task.projectId.empty() && !selectProject(task)) return;
        updates.change(task, [&](TaskData& current) {
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
            dispatch.applyDelivery(id, *action, {Delivery::Kind::NotAccepted, {}, "Cancelled before dispatch", true});
            return;
        }
        if (stopRequested && phase == OutboxPhase::started) {
            if (now < run.retryAt.value_or(0)) return;
            try {
                const auto cmd = dispatch.command(task, *action);
                auto reconciled = t3.reconcile(cmd);
                if (reconciled.kind == Delivery::Kind::Accepted && hasRun(reconciled.run)) {
                    dispatch.applyDelivery(id, *action, std::move(reconciled));
                    return;
                }
            } catch (const std::exception&) {
            }
            updates.changeTask(id, [&](TaskData& current) {
                auto& saved = destinationRun(current, key);
                if (!saved.outstanding()) return false;
                dispatch.setDestinationStatus(saved, DestinationRunPhase::confirmation_pending,
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
        if ((phase == OutboxPhase::ready || phase == OutboxPhase::started) && !stopRequested)
            return dispatch.deliver(task, std::move(*action));
        if (phase != OutboxPhase::accepted) return;
        const auto cmd = dispatch.command(task, *action);
        const auto observed = runs.observeRun(cmd.ownerBotId, cmd.environmentId, cmd.config, ref);
        if (observed.kind == Observation::Kind::Finished) {
            updates.changeTask(id, [&](TaskData& current) {
                auto& saved = destinationRun(current, key);
                if (!saved.outstanding() || saved.ref.threadId != ref.threadId || saved.ref.runId != ref.runId) return false;
                if (!stopping &&
                    (observed.commitEvidenceUnavailable || observed.evidence == RunEvidence::Availability::ReportUnavailable) &&
                    saved.evidenceAttempts + 1 < SessionHistory::maxEvidenceReads) {
                    ++saved.evidenceAttempts;
                    dispatch.setDestinationStatus(saved, DestinationRunPhase::confirmation_pending,
                                                  "The Run ended. Flock is retrieving its commit and report.");
                    saved.retryAt = now + 5;
                    return true;
                }
                dispatch.writeDestinationReport(current, saved, observed);
                return true;
            });
            return;
        }
        using Kind = Observation::Kind;
        updates.changeTask(id, [&](TaskData& current) {
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
                changed = dispatch.setDestinationStatus(
                    saved, DestinationRunPhase::confirmation_pending,
                    sessionMissing ? "T3 could not find this Session when Flock tried to stop it. Check its delegated Sessions in T3. "
                                     "Flock is keeping the project on hold until the work settles."
                                   : "Flock could not confirm the Run's status in T3.");
            } else if (observed.kind == Kind::Question || observed.kind == Kind::ExternalInput)
                changed = dispatch.setDestinationStatus(saved, DestinationRunPhase::confirmation_pending,
                                                        "Review the pending input or approval in this T3 Session.");
            else if (!stopping)
                changed = dispatch.setDestinationStatus(
                    saved, observed.status == "queued" ? DestinationRunPhase::queued : DestinationRunPhase::running, {});
            return (!stopping && destinationDeadline(current, saved, now)) || changed;
        });
        if (!stopping || observed.kind == Kind::Settling || observed.kind == Kind::Unavailable || now < run.stopRetryAt.value_or(0)) return;
        if (!updates.changeTask(id, [&](TaskData& current) {
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
                dispatch.destinationStatus(
                    id, key, gate == ApprovalGate::waiting ? DestinationRunPhase::confirmation_pending : DestinationRunPhase::running,
                    gate == ApprovalGate::waiting ? "Waiting for approval to stop the recovered T3 Run"
                                                  : "The recovered T3 Run stop was denied by policy");
                return;
            }
        }
        try {
            const SessionDispatchFileLock dispatch(db.path, SessionDispatchFileLock::Mode::shared);
            t3.interrupt(cmd.environmentId, cmd.config, ref);
        } catch (const std::exception&) {
            dispatch.destinationStatus(id, key, DestinationRunPhase::confirmation_pending, "T3 did not confirm the stop request.");
        }
    }

    bool canComplete(const TaskData& task, const Json& event) const {
        if (task.replanning) return false;
        if (!task.newSessionRequired && RunReports::hasSettledRun(task) && task.lastRun->assignment.revisionId == task.currentRevision &&
            !reports.sessionReportUnavailable(task))
            return true;
        const auto peer = peerExchanges.peerResult(task, event);
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
        if (RunReports::hasSettledRun(task)) {
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
        if (canRead && event["evidence"].size() < SessionHistory::maxEvidenceReads) tools.push_back("read_bot_evidence");
        if (canSend && (!task.selectedSession || RunReports::hasSettledRun(task))) tools.push_back("send_bot_message");
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
        updates.change(task, [&](TaskData& current) {
            recordFailure(current, event, detail, transient, cause);
            return true;
        });
    }

    static constexpr int maxToolErrors = 6;

    void rejectDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view message,
                        bool replay = false) {
        updates.change(task, [&](TaskData& current) {
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
        SessionPrompts::WorkPrompt prompt;
        if (tool == CommandTool::follow_up) {
            prompt = prompts.workPrompt(task, event, id.size());
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

    void acceptReplyOrCompletion(const TaskData& snapshot, const Json& event, const TaskDecision& decision) {
        std::lock_guard lock(db.mutex);
        Transaction transaction(db.db);
        const auto id = snapshot.id;
        auto current = db.load(id);
        if (current.generation != snapshot.generation || current.eventId != event["id"] ||
            current.currentRevision != snapshot.currentRevision || current.state != TaskState::deciding)
            return;
        if (decision.tool == "reply_to_bot") {
            if (peerExchanges.replyPeer(current, event, decision, actionId())) updates.saveTask(current);
        } else {
            if (db.peerExchange(id) || !canComplete(current, event)) return;
            complete(current, event, decision);
        }
        transaction.commit();
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
        updates.consumeEvent(current);
        updates.recordDecision(current, event, decision, actionId());
        current.pendingInstructions.clear();
        current.newSessionRequired = false;
        current.state = TaskState::completed;
        db.journal(current.id,
                   CompletionEntry{field(decision.args, "report"), current.currentRevision,
                                   TaskCompletion{decision.reason, current.doneWhen, peerExchanges.completionProvenance(current, event)}});
        updates.saveTask(current);
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
            updates.change(snapshot, [&](TaskData& current) {
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
            if (field(args, "view") == "report") return reports.retryRunReport(snapshot, event, decision);
            if (options.sessionRead != "on_demand") throw Conflict("Only the terminal report can be read for this event");
            history.readEvidence(snapshot, event, args, decision.usage);
            return;
        }
        if (tool == "read_bot_evidence") return peerExchanges.readPeerEvidence(snapshot, event, decision);
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
            auto peer = peerExchanges.allowedPeer(snapshot.botId, field(args, "botId"));
            updates.change(snapshot, [&](TaskData& current) {
                peerExchanges.sendPeer(current, event, decision, std::move(peer), actionId());
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
        updates.change(snapshot, [&](TaskData& current) {
            if (tool == "follow_up" && current.newSessionRequired) throw Conflict("Start a new Session in the current environment");
            bool charge = false;
            if (work && !updates.admitWork(current, decision, charge)) return true;
            if (work) updates.establishCriteria(current, args);
            updates.consumeEvent(current);
            const auto action = actionId();
            updates.recordDecision(current, event, decision, action);
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
                updates.saveTask(current);
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
        const auto prefill = this->history.prefillHistory(task, event);
        if (prefill == SessionHistory::PrefillResult::Persisted) return std::nullopt;
        if (prefill == SessionHistory::PrefillResult::UnavailableWithSettledReport) event["historyUnavailable"] = true;
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
            if (auto forwarded = peerExchanges.forwardableUserMessage(task))
                summary["forwardableUserMessage"] = encodeForwardedPeerMessage(*forwarded);
        }
        if (peer)
            summary["peer"] = {{"exchangeId", peer->id},
                               {"requestKind", peer->kind},
                               {"replyKinds", peerExchanges.replyKinds(task, *peer)},
                               {"phase", peer->phase},
                               {"role", peer->sender == task.id ? "sender" : "recipient"}};
        const auto evidence = event["evidence"];
        const bool canReadHistory =
            std::ranges::find(tools, "session_read") != tools.end() || std::ranges::find(tools, "read_bot_evidence") != tools.end();
        summary["remainingEvidenceReads"] = canReadHistory ? SessionHistory::maxEvidenceReads - evidence.size() : 0;
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
                updates.saveTask(current);
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
        if (task.activeRun && !runs.questionStillOpen(task)) return;
        if (shutdown.stop_requested()) return;
        Json event, history = Json::array();
        std::optional<PeerExchange> peer;
        std::optional<TaskDecision> decision;
        {
            std::lock_guard lock(db.mutex);
            event = db.currentEvent(task);
            if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > options.clock()) return;
            peer = db.peerExchange(task.id);
            if (task.pendingDecision && TaskUpdates::checkpointMatches(*task.pendingDecision, task))
                decision = task.pendingDecision->decision;
            if (event["kind"] != "result" && task.lastRun) {
                if (const auto report = reports.settledSessionReport(task)) event["sessionResult"] = encodeRunReport(report->data);
            }
            if (RunReports::hasSettledRun(task) && event.at("evidence").size() < SessionHistory::maxEvidenceReads) {
                auto views = options.sessionRead == "on_demand" ? Json::array({"messages", "activity"}) : Json::array();
                if (reports.reportRetry(task, event)) views.push_back("report");
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
                for (const auto& allowed : peerExchanges.peersFor(task.botId)) {
                    choices.push_back({{"botId", allowed.intake.botId}, {"name", allowed.name}});
                    if (event.contains("references") && !event["references"].empty() && event["fromBotId"] == allowed.intake.botId)
                        canRead = true;
                }
            }
            std::vector<OutboxInstruction> pending;
            if (!task.pendingInstructions.empty()) {
                std::lock_guard lock(db.mutex);
                pending = prompts.pendingInstructions(task, event);
            }
            auto tools = available(task, event, peer, !choices.empty(), canRead);
            if (options.gitHost && !task.activeRun && event["kind"] == "result")
                tools.insert(tools.end(), gitTools(false).begin(), gitTools(false).end());
            if (task.pendingInstructions.size() != pending.size()) std::erase(tools, "complete");
            const bool replay = decision.has_value();
            if (!decision) decision = inferDecision(task, event, history, tools, pending, std::move(choices), peer);
            if (!decision || shutdown.stop_requested()) return;
            if (task.activeRun && !runs.questionStillOpen(task)) return;
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

    bool continueStage(const TaskData& task) {
        if ((!task.workflow && !task.freshContextRequested) || !RunReports::hasSettledRun(task) || task.pendingInstructions.empty())
            return false;
        return updates.change(task, [&](TaskData& current) {
            auto event = db.currentEvent(current);
            if (!event.is_null() && event.at("consumed") != true && (event.at("kind") != "result" || event.contains("userAnswer")))
                return false;
            if (event.is_null()) event = Json::object();
            const auto tool = current.newSessionRequired ? CommandTool::delegate : CommandTool::follow_up;
            const TaskDecision decision{
                {std::string(commandToolName(tool)), Json::object(), "Apply accepted user instructions after the assigned Run settled"}};
            bool charge = false;
            if (!updates.admitWork(current, decision, charge)) return true;
            const auto action = actionId();
            if (event.contains("id")) updates.recordDecision(current, event, decision, action);
            updates.consumeEvent(current);
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
        auto task = updates.snapshot(id);
        const auto state = task.state;
        if (work.destinationAction) {
            auto& run = destinationRun(task, *work.destinationAction);
            if (run.outstanding()) stepDestination(task, run);
            return;
        }
        if (!task.runResults().empty()) return;
        if (task.pendingStageReport)
            updates.change(task, [&](TaskData& current) { return db.settleStageReport(current); });
        else if (runs.shouldStop(task))
            dispatch.stop(task);
        else if (task.pendingRunReport)
            reports.recoverRunReport(task);
        else if (state == TaskState::queued)
            activate(task);
        else if (state == TaskState::deciding) {
            if (!resumeGitApproval(task) && !continueStage(task)) decide(task);
        } else {
            if (task.activeRun && (state == TaskState::running || state == TaskState::waiting) && runs.ingest(task, runs.observe(task)))
                return;
            dispatch.step(task);
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
