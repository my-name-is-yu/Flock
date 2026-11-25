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
#include "runtime_task_decisions.hpp"
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
    TaskDecisions decisions;
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
                   shutdown.get_token()),
          decisions(db, updates, reports, history, peerExchanges, dispatch, options.sessionRead, options.gitHost.get()) {
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
                dispatch.queueAction(current, actionId(), CommandTool::delegate, "", Json::object(), Json::object(), false);
                return true;
            }
            if (current.eventId == 0) db.event(current, "intake", {{"kind", "intake"}});
            current.state = TaskState::deciding;
            return true;
        });
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
            if (decisions.canComplete(task, event)) tools.push_back("complete");
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
                decisions.rejectDecision(task, event, *decision, decision->rejection);
                return;
            }
            if (replay) {
                auto call = db.checkModelCall(task.botId, *decision, tools, false);
                if (!call.rejection.empty()) {
                    decisions.rejectDecision(task, event, TaskDecision{std::move(call), decision->usage, decision->attemptId},
                                             call.rejection, true);
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
                decisions.accept(task, event, *decision);
            } catch (const Conflict& error) {
                decisions.rejectDecision(task, event, *decision, error.what());
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

    void step(const TaskScheduler::Work& work) {
        if (shutdown.stop_requested()) return;
        const auto id = work.taskId;
        auto task = updates.snapshot(id);
        const auto state = task.state;
        if (work.destinationAction) {
            auto& run = destinationRun(task, *work.destinationAction);
            if (run.outstanding()) dispatch.stepDestination(task, run);
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
            if (!decisions.resumeGitApproval(task) && !decisions.continueStage(task)) decide(task);
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
