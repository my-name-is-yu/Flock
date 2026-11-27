#include "runtime_task_turn.hpp"
#include "flock/git_host.hpp"

#include "memory_storage.hpp"
#include "bot_setup_storage.hpp"
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
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/bot_notes_error.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/wait.hpp"
#include "flock/outbox_instruction.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "git_storage.hpp"
#include "model_calls.hpp"
#include "peer_storage.hpp"
#include "secret_storage.hpp"
#include "task_events.hpp"
#include "task_records.hpp"
#include "task_routing.hpp"
#include "wait_storage.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
runtime::TaskTurn::TaskTurn(Database& database, GitStorage& gitStorage, ModelCalls& modelCalls, PeerStorage& peerStorage,
                            SecretStorage& secretStorage, TaskEvents& taskEvents, TaskRecords& taskRecords, TaskRouting& taskRouting,
                            WaitStorage& waitStorage, MemoryStorage& memoryStorage, Model& model, const SoulLoader& soul,
                            const std::function<Seconds()>& clock, GitHost* gitHost, const std::string& sessionRead, std::stop_token stop,
                            TaskUpdates& updates, RunReports& reports, SessionHistory& sessionHistory, PeerExchanges& peerExchanges,
                            RunObserver& runs, SessionPrompts& prompts, TaskDecisions& decisions, BotSetupStorage& botSetup,
                            const IntakeResolver& intake, T3& t3, bool requireT3Connection)
    : database(database), gitStorage(gitStorage), modelCalls(modelCalls), peerStorage(peerStorage), secretStorage(secretStorage),
      taskEvents(taskEvents), taskRecords(taskRecords), taskRouting(taskRouting), waitStorage(waitStorage), memoryStorage(memoryStorage),
      model(model), soul(soul), clock(clock), gitHost(gitHost), sessionRead(sessionRead), stop(std::move(stop)), updates(updates),
      reports(reports), sessionHistory(sessionHistory), peerExchanges(peerExchanges), runs(runs), prompts(prompts), decisions(decisions),
      botSetup(botSetup), intake(intake), t3(t3), requireT3Connection(requireT3Connection) {}

std::vector<std::string> runtime::TaskTurn::available(const TaskData& task, const Json& event, const std::optional<PeerExchange>& peer,
                                                      bool canSend, bool canRead) {
    if (event.at("kind") == "delivery_unconfirmed") {
        std::vector<std::string> tools{"ask_user"};
        if (task.activeRun && task.question)
            tools.push_back("answer_session_question");
        else {
            const bool canFollowUp = RunReports::hasSettledRun(task) && !task.newSessionRequired;
            if (canFollowUp) tools.push_back("follow_up");
            if (!task.selectedSession || !canFollowUp) tools.push_back("delegate");
        }
        if (event.contains("sessionReadViews")) tools.push_back("session_read");
        return tools;
    }
    if (!task.runTargets().empty()) return {"delegate", "ask_user"};
    const auto id = task.id;
    if (peer && peer->sender == id && peer->phase == "question") return {"reply_to_bot", "ask_user"};
    if (task.activeRun && task.question) return {"answer_session_question", "ask_user"};
    const bool receiving = peer && peer->recipient == id && peer->phase == "working";
    if (receiving && peer->kind == "consult") return {"reply_to_bot"};
    if (task.projectId.empty()) return {"delegate", receiving ? "reply_to_bot" : "ask_user"};
    std::vector<std::string> tools{"delegate", receiving ? "reply_to_bot" : "ask_user"};
    if (RunReports::hasSettledRun(task)) {
        if (!task.newSessionRequired) {
            tools.push_back("follow_up");
            if (task.selectedSession) std::erase(tools, "delegate");
        }
        if (event.contains("sessionReadViews")) tools.push_back("session_read");
    }
    if (!receiving) {
        std::lock_guard lock(database.mutex);
        if (decisions.canComplete(task, event)) tools.push_back("complete");
        if (!peer && !task.replanning) tools.push_back("wait_for_event");
    }
    if (canRead) tools.push_back("read_bot_evidence");
    if (canSend && (!task.selectedSession || RunReports::hasSettledRun(task))) tools.push_back("send_bot_message");
    return tools;
}

std::optional<Json> runtime::TaskTurn::launchDefaults(const TaskData& task) const {
    if (task.selectedSession && !task.newSessionRequired) return std::nullopt;
    const auto live = botSetup.liveIntake(intake, task.botId, task.intent);
    if (!live) return std::nullopt;
    return live->config.value("session", Json::object());
}

void runtime::TaskTurn::recordFailure(TaskData& task, const Json& event, std::string_view detail, bool transient, FailureCause cause) {
    const auto why = bounded(secretStorage.redactor(task.botId).text(detail), 2048);
    database.writeJournal(task.id, "model_failure", {{"eventId", event["id"]}, {"text", why}});
    task.feedback = why;
    if (transient) {
        database.exec("UPDATE events SET failures=min(failures+1,7),retry_at=?+min(1 << failures,60) WHERE id=?", {clock(), event["id"]});
    } else if (cause == FailureCause::UnsupportedReasoningEffort)
        waitStorage.wait(
            task, ModelFailureWait{WaitFailureCode::unsupported_reasoning_effort},
            "This model doesn’t support the selected reasoning effort. Choose another effort or Model default in Bot settings.");
    else if (cause == FailureCause::Authentication)
        waitStorage.wait(task, ModelFailureWait{WaitFailureCode::authentication_required}, "Open Flock Settings and sign in to ChatGPT.");
    else if (cause == FailureCause::BotNotes)
        waitStorage.wait(task, SimpleWaitReason::bot_notes_unavailable, "Restore access to this Bot’s Role and memory. " + why);
    else
        waitStorage.wait(task, ModelFailureWait{}, why);
}

bool runtime::TaskTurn::resume(const TaskData& task, const Wait& wait) {
    const auto reason = wait.reason();
    if (reason != WaitReason::model_failure && reason != WaitReason::bot_notes_unavailable) return false;
    auto config = task.config;
    if (const auto* failure = std::get_if<ModelFailureWait>(&wait.details)) {
        botSetup.refreshPlannerModel(task.botId, config);
        if (config == task.config && !(failure->code == WaitFailureCode::authentication_required && model.hasAuthentication())) return true;
    } else {
        try {
            memoryStorage.loadNotes(task.botId, soul, clock());
        } catch (const BotNotesUnavailable&) {
            return true;
        }
    }
    updates.change(task, [&](TaskData& current) {
        const bool requeue = taskRouting.isUndispatchedNotesWait(current);
        waitStorage.clearWait(current, "bot_setup_restored");
        current.config = std::move(config);
        current.feedback.reset();
        const auto event = taskEvents.currentEvent(current);
        const bool hasDecisionEvent = !event.is_null() && event.at("consumed") != true;
        if (current.eventId) database.exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {current.eventId});
        current.state = requeue                                          ? TaskState::queued
                        : !current.actionId.empty() && !hasDecisionEvent ? TaskState::sending
                                                                         : TaskState::deciding;
        return true;
    });
    return true;
}

void runtime::TaskTurn::failure(const TaskData& task, const Json& event, std::string_view detail, bool transient, FailureCause cause) {
    updates.change(task, [&](TaskData& current) {
        recordFailure(current, event, detail, transient, cause);
        return true;
    });
}

std::optional<TaskDecision> runtime::TaskTurn::persistTaskInference(Id id) {
    const auto found = taskInferences.find(id);
    const auto& inference = found->second;
    Transaction transaction(database.db);
    auto current = taskRecords.load(id);
    if (!inference.usage.empty()) database.exec("UPDATE model_attempts SET usage=? WHERE id=?", {inference.usage, inference.attemptId});
    std::optional<TaskDecision> decision;
    if (current.state == TaskState::deciding && current.generation == inference.generation && current.eventId == inference.eventId &&
        current.currentRevision == inference.revisionId) {
        if (const auto* error = std::get_if<InferenceFailure>(&inference.outcome)) {
            if (error->config) current.config = *error->config;
            recordFailure(current, taskEvents.currentEvent(current), error->detail, error->transient, error->cause);
            updates.saveTask(current);
        } else {
            decision = std::get<TaskDecision>(inference.outcome);
            decision->usage = inference.usage;
            decision->attemptId = inference.attemptId;
            taskRecords.saveDecisionCheckpoint(current, {inference.generation, inference.eventId, inference.revisionId, *decision});
        }
    }
    transaction.commit();
    taskInferences.erase(found);
    return decision;
}

void runtime::TaskTurn::persistTaskInferences() {
    std::lock_guard lock(database.mutex);
    while (!taskInferences.empty()) persistTaskInference(taskInferences.begin()->first);
}

std::optional<TaskDecision> runtime::TaskTurn::inferDecision(const TaskData& task, Json event, const Json& history,
                                                             const std::vector<std::string>& tools,
                                                             std::span<const OutboxInstruction> pending, Json choices,
                                                             const std::optional<PeerExchange>& peer,
                                                             const std::vector<T3Project>& projects) {
    if (event.at("kind") != "delivery_unconfirmed") {
        const auto prefill = sessionHistory.prefillHistory(task, event);
        if (prefill == SessionHistory::PrefillResult::Persisted) return std::nullopt;
        if (prefill == SessionHistory::PrefillResult::UnavailableWithSettledReport) event["historyUnavailable"] = true;
    }
    const auto bot = memoryStorage.loadNotes(task.botId, soul, clock());
    Json summary = {{"id", task.id},
                    {"botId", task.botId},
                    {"request", task.intent},
                    {"doneWhen", task.doneWhen},
                    {"revisionId", revisionJson(task.currentRevision)}};
    if (!projects.empty()) {
        summary["environmentId"] = task.environmentId;
        summary["projects"] = projectChoices(projects, secretStorage.redactor(task.botId));
    }
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
    if (!task.sourceResultRefs.empty()) summary["sourceResultRefs"] = sourceResultsJson(task.sourceResultRefs);
    if (!choices.empty()) summary["peers"] = std::move(choices);
    if (std::ranges::find(tools, "send_bot_message") != tools.end()) {
        std::lock_guard lock(database.mutex);
        if (auto forwarded = peerExchanges.forwardableUserMessage(task))
            summary["forwardableUserMessage"] = encodeForwardedPeerMessage(*forwarded);
    }
    if (peer)
        summary["peer"] = {{"exchangeId", peer->id},
                           {"requestKind", peer->kind},
                           {"replyKinds", peerExchanges.replyKinds(task, *peer)},
                           {"phase", peer->phase},
                           {"role", peer->sender == task.id ? "sender" : "recipient"}};
    Id attempt = 0;
    std::string learningRunId;
    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (stop.stop_requested()) return std::nullopt;
        auto current = taskRecords.load(task.id);
        if (current.generation != task.generation || current.eventId != event["id"] || current.currentRevision != task.currentRevision ||
            current.state != TaskState::deciding)
            return std::nullopt;
        if (std::ranges::find(tools, "complete") != tools.end())
            if (const auto source = decisions.learningSource(current, event)) learningRunId = source->runId;
        attempt = modelCalls.recordAttempt(modelCalls.taskModelScope(task.id), "task_decision", 0, task.id, event["id"]);
        transaction.commit();
    }
    auto evidence = std::move(event.at("evidence"));
    event.erase("evidence");
    event.erase("consumed");
    event.erase("failures");
    event.erase("retryAt");
    if (gitHost && event.at("kind") == "result") gitStorage.projectTaskGitReads(*gitHost, task, event);
    if (event.at("kind") == "external_check") gitStorage.projectExternalCheck(gitHost, task, event);
    TaskInference inference{task.generation, event.at("id"), attempt, task.currentRevision};
    auto config = task.config;
    if (botSetup) botSetup.refreshPlannerModel(task.botId, config);
    ModelInput input{summary,
                     event,
                     history,
                     bot,
                     tools,
                     task.feedback.value_or(""),
                     std::move(evidence),
                     std::move(config),
                     needsDoneWhen(task),
                     summary.contains("forwardableUserMessage"),
                     {},
                     std::move(learningRunId)};
    try {
        auto turn = modelCalls.callModel(task.botId, model, input);
        inference.usage = std::move(turn.usage);
        std::optional<Json> setup;
        if (turn.calls.size() == 2) {
            const auto edit = std::ranges::find(turn.calls, botSetupTool, &ModelCall::tool);
            if (edit != turn.calls.end() && edit->rejection.empty()) {
                setup = std::move(edit->args);
                turn.calls.erase(edit);
            }
        }
        if (turn.calls.size() == 1 && turn.calls.front().tool != botSetupTool) {
            auto& call = turn.calls.front();
            const auto rejection = call.rejection;
            inference.outcome = TaskDecision{std::move(call), {}, 0, rejection, std::move(setup)};
        } else {
            inference.outcome = TaskDecision{Decision{}, {}, 0, "Return one task action and optionally one Bot setup change"};
        }
    } catch (const ModelError& error) {
        inference.usage = error.usage;
        if (repairableModelOutput(error.code))
            inference.outcome = TaskDecision{Decision{}, {}, 0, error.what()};
        else {
            auto cause = FailureCause::Model;
            if (error.code == ModelErrorCode::unsupported_reasoning_effort) cause = FailureCause::UnsupportedReasoningEffort;
            if (error.code == ModelErrorCode::authentication_required) cause = FailureCause::Authentication;
            const bool transient = error.transient && cause == FailureCause::Model;
            inference.outcome =
                InferenceFailure{error.what(), transient, cause, transient ? std::nullopt : std::optional{std::move(input.config)}};
        }
    }
    const auto id = task.id;
    std::lock_guard lock(database.mutex);
    taskInferences.insert_or_assign(id, std::move(inference));
    return persistTaskInference(id);
}

void runtime::TaskTurn::decide(const TaskData& task) {
    if (task.activeRun && !runs.questionStillOpen(task)) return;
    if (stop.stop_requested()) return;
    Json event, history = Json::array();
    std::optional<PeerExchange> peer;
    std::optional<TaskDecision> decision;
    {
        std::lock_guard lock(database.mutex);
        event = taskEvents.currentEvent(task);
        if (event.is_null() || event["consumed"] == true || event["retryAt"].get<Seconds>() > clock()) return;
        peer = peerStorage.peerExchange(task.id);
        if (task.pendingDecision && TaskUpdates::checkpointMatches(*task.pendingDecision, task)) decision = task.pendingDecision->decision;
        if (event["kind"] != "result" && task.lastRun) {
            if (const auto report = reports.settledSessionReport(task)) event["sessionResult"] = encodeRunReport(report->data);
        }
        if (event["kind"] == "delivery_unconfirmed") {
            if (sessionRead != "off" && event.at("action").contains("session")) event["sessionReadViews"] = {"messages", "activity"};
        } else if (RunReports::hasSettledRun(task)) {
            auto views = sessionRead == "on_demand" ? Json::array({"messages", "activity"}) : Json::array();
            if (reports.reportRetry(task, event)) views.push_back("report");
            if (!views.empty()) event["sessionReadViews"] = std::move(views);
        }
        auto errors = database.query(R"SQL(
SELECT id,data FROM journal WHERE task_id=? AND kind='tool_error' AND feedback_event_id=? ORDER BY id DESC LIMIT 6
)SQL",
                                     {task.id, event["id"]});
        event["decisionFeedback"] = Json::array();
        while (errors.next()) event["decisionFeedback"].push_back(encodeToolError(decodeToolError(errors.number(0), errors.json(1))));
        std::reverse(event["decisionFeedback"].begin(), event["decisionFeedback"].end());
        if (!decision) {
            auto rows = database.query(R"SQL(
SELECT id,kind,data,json_extract(data,'$.run.threadId'),json_extract(data,'$.resultRunId'),
       json_extract(data,'$.itemId'),coalesce(json_extract(data,'$.environmentId'),json_extract(data,'$.run.environmentId')) FROM journal WHERE task_id=?
AND kind IN ('decision','report','user_answer','session_answered','bot_message','git_webhook') ORDER BY id DESC LIMIT 6
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
            std::lock_guard lock(database.mutex);
            pending = prompts.pendingInstructions(task, event);
        }
        auto tools = available(task, event, peer, !choices.empty(), canRead);
        if (gitHost && !task.activeRun && event["kind"] == "result")
            tools.insert(tools.end(), gitTools(false).begin(), gitTools(false).end());
        if (!peer && botSetup.taskAvailable(task)) tools.emplace_back(botSetupTool);
        if (task.pendingInstructions.size() != pending.size()) {
            std::erase(tools, "complete");
            std::erase(tools, "wait_for_event");
        }
        std::vector<T3Project> projects;
        if (task.projectId.empty() && std::ranges::find(tools, "delegate") != tools.end()) {
            auto catalog = taskRouting.readProjectCatalog(t3, task.botId, {task.environmentId, {}, task.gatewayEnvironmentId.value_or("")},
                                                          requireT3Connection);
            if (const auto* problem = std::get_if<std::string>(&catalog)) {
                updates.change(task, [&](TaskData& current) {
                    waitStorage.wait(current, SimpleWaitReason::settings, *problem);
                    return true;
                });
                return;
            }
            projects = std::get<std::vector<T3Project>>(std::move(catalog));
            if (projects.empty()) {
                updates.change(task, [&](TaskData& current) {
                    waitStorage.wait(current, SimpleWaitReason::settings, "Add or unarchive a project in this T3 environment.");
                    return true;
                });
                return;
            }
        }
        const bool replay = decision.has_value();
        if (!decision) decision = inferDecision(task, event, history, tools, pending, std::move(choices), peer, projects);
        if (!decision || stop.stop_requested()) return;
        if (task.activeRun && !runs.questionStillOpen(task)) return;
        if (!decision->rejection.empty()) {
            decisions.rejectDecision(task, event, *decision, decision->rejection);
            return;
        }
        if (replay) {
            auto call = modelCalls.checkModelCall(task.botId, *decision, tools, false);
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
            if (!decision->reason.empty() && !validDecisionReason(decision->reason)) throw Conflict("The reason is too long");
            if (decision->setup && !botSetup.applyForTask(task, *decision->setup, clock(), decision->attemptId)) return;
            const auto defaults = decision->tool == "delegate" ? launchDefaults(task) : std::nullopt;
            decisions.accept(task, event, *decision, {}, defaults ? &*defaults : nullptr, projects);
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
