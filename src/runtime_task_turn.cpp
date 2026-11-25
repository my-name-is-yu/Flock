#include "runtime_task_turn.hpp"

#include "flock/decision.hpp"
#include "journal_peer.hpp"
#include "journal_task.hpp"
#include "outbox_storage.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_observer.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
#include "runtime_session_prompts.hpp"
#include "runtime_task_decisions.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "task_context_storage.hpp"
#include "task_storage.hpp"

#include <algorithm>
#include <utility>

namespace flockv0 {
Runtime::TaskTurn::TaskTurn(Store::Impl& db, Model& model, const SoulLoader& soul, const std::function<Seconds()>& clock, GitHost* gitHost,
                            const std::string& sessionRead, std::stop_token stop, TaskUpdates& updates, RunReports& reports,
                            SessionHistory& sessionHistory, PeerExchanges& peerExchanges, RunObserver& runs, SessionPrompts& prompts,
                            TaskDecisions& decisions)
    : db(db), model(model), soul(soul), clock(clock), gitHost(gitHost), sessionRead(sessionRead), stop(std::move(stop)), updates(updates),
      reports(reports), sessionHistory(sessionHistory), peerExchanges(peerExchanges), runs(runs), prompts(prompts), decisions(decisions) {}

std::vector<std::string> Runtime::TaskTurn::available(const TaskData& task, const Json& event, const std::optional<PeerExchange>& peer,
                                                      bool canSend, bool canRead) {
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

void Runtime::TaskTurn::recordFailure(TaskData& task, const Json& event, std::string_view detail, bool transient, FailureCause cause) {
    const auto attempts = event["failures"].get<int>() + 1;
    const auto why = bounded(db.redactor(task.botId).text(std::string(detail)), 2048);
    db.exec("UPDATE events SET failures=?,retry_at=? WHERE id=?", {attempts, transient ? clock() + (1 << (attempts - 1)) : 0, event["id"]});
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

void Runtime::TaskTurn::failure(const TaskData& task, const Json& event, std::string_view detail, bool transient, FailureCause cause) {
    updates.change(task, [&](TaskData& current) {
        recordFailure(current, event, detail, transient, cause);
        return true;
    });
}

std::optional<TaskDecision> Runtime::TaskTurn::persistTaskInference(Id id) {
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

void Runtime::TaskTurn::persistTaskInferences() {
    std::lock_guard lock(db.mutex);
    while (!taskInferences.empty()) persistTaskInference(taskInferences.begin()->first);
}

std::optional<TaskDecision> Runtime::TaskTurn::inferDecision(const TaskData& task, Json event, const Json& history,
                                                             const std::vector<std::string>& tools,
                                                             std::span<const OutboxInstruction> pending, Json choices,
                                                             const std::optional<PeerExchange>& peer) {
    const auto prefill = sessionHistory.prefillHistory(task, event);
    if (prefill == SessionHistory::PrefillResult::Persisted) return std::nullopt;
    if (prefill == SessionHistory::PrefillResult::UnavailableWithSettledReport) event["historyUnavailable"] = true;
    const auto bot = db.loadNotes(task.botId, soul, clock());
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
        if (stop.stop_requested()) return std::nullopt;
        auto current = db.load(task.id);
        if (current.generation != task.generation || current.eventId != event["id"] || current.currentRevision != task.currentRevision ||
            current.state != TaskState::deciding)
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
    if (gitHost && event.at("kind") == "result") db.projectTaskGitReads(*gitHost, task, event);
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

void Runtime::TaskTurn::decide(const TaskData& task) {
    if (task.activeRun && !runs.questionStillOpen(task)) return;
    if (stop.stop_requested()) return;
    Json event, history = Json::array();
    std::optional<PeerExchange> peer;
    std::optional<TaskDecision> decision;
    {
        std::lock_guard lock(db.mutex);
        event = db.currentEvent(task);
        if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > clock()) return;
        peer = db.peerExchange(task.id);
        if (task.pendingDecision && TaskUpdates::checkpointMatches(*task.pendingDecision, task)) decision = task.pendingDecision->decision;
        if (event["kind"] != "result" && task.lastRun) {
            if (const auto report = reports.settledSessionReport(task)) event["sessionResult"] = encodeRunReport(report->data);
        }
        if (RunReports::hasSettledRun(task) && event.at("evidence").size() < SessionHistory::maxEvidenceReads) {
            auto views = sessionRead == "on_demand" ? Json::array({"messages", "activity"}) : Json::array();
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
        if (gitHost && !task.activeRun && event["kind"] == "result")
            tools.insert(tools.end(), gitTools(false).begin(), gitTools(false).end());
        if (task.pendingInstructions.size() != pending.size()) std::erase(tools, "complete");
        const bool replay = decision.has_value();
        if (!decision) decision = inferDecision(task, event, history, tools, pending, std::move(choices), peer);
        if (!decision || stop.stop_requested()) return;
        if (task.activeRun && !runs.questionStillOpen(task)) return;
        if (!decision->rejection.empty()) {
            decisions.rejectDecision(task, event, *decision, decision->rejection);
            return;
        }
        if (replay) {
            auto call = db.checkModelCall(task.botId, *decision, tools, false);
            if (!call.rejection.empty()) {
                decisions.rejectDecision(task, event, TaskDecision{std::move(call), decision->usage, decision->attemptId}, call.rejection,
                                         true);
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
}
