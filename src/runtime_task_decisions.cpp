#include "task_interaction_storage.hpp"
#include "runtime_task_decisions.hpp"
#include "flock/git_host.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/session_question.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/outbox_instruction.hpp"
#include "conversation_turns.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "git_storage.hpp"
#include "journal_storage.hpp"
#include "memory_storage.hpp"
#include "peer_storage.hpp"
#include "run_ref_storage.hpp"
#include "secret_storage.hpp"
#include "task_events.hpp"
#include "task_records.hpp"
#include "task_routing.hpp"
#include "wait_storage.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {

std::string gitInvocation(const TaskDecision& decision) {
    return "attempt/" + std::to_string(decision.attemptId);
}
}

runtime::TaskDecisions::TaskDecisions(Database& database, ConversationStorage& conversationTurns, GitStorage& gitStorage,
                                      JournalStorage& journalStorage, MemoryStorage& memoryStorage, PeerStorage& peerStorage,
                                      SecretStorage& secretStorage, TaskEvents& taskEvents, TaskRecords& taskRecords,
                                      TaskRouting& taskRouting, WaitStorage& waitStorage, TaskUpdates& updates, RunReports& reports,
                                      SessionHistory& history, PeerExchanges& peerExchanges, OutboxDispatch& dispatch,
                                      const std::string& sessionRead, GitHost* gitHost, const std::function<Seconds()>& clock)
    : database(database), conversationTurns(conversationTurns), gitStorage(gitStorage), journalStorage(journalStorage),
      memoryStorage(memoryStorage), peerStorage(peerStorage), secretStorage(secretStorage), taskEvents(taskEvents),
      taskRecords(taskRecords), taskRouting(taskRouting), waitStorage(waitStorage), updates(updates), reports(reports), history(history),
      peerExchanges(peerExchanges), dispatch(dispatch), sessionRead(sessionRead), gitHost(gitHost), clock(clock) {}

bool runtime::TaskDecisions::canComplete(const TaskData& task, const Json& event) const {
    if (task.replanning) return false;
    if (!task.newSessionRequired && RunReports::hasSettledRun(task) && task.lastRun->assignment.revisionId == task.currentRevision &&
        !reports.sessionReportUnavailable(task))
        return true;
    const auto peer = peerExchanges.peerResult(task, event);
    if (!peer) return false;
    if (task.newSessionRequired) return peer->environment == task.environmentId && event.at("status") == "completed";
    return task.currentRevision.has_value() || (peer->kind == "consult" && event.at("status") == "completed");
}

std::optional<runtime::TaskDecisions::LearningSource> runtime::TaskDecisions::learningSource(const TaskData& task, const Json& event) {
    if (!RunReports::hasSettledRun(task) || task.newSessionRequired || !memoryStorage.canLearnFrom(task)) return {};
    const auto source = completionProvenance(task, event);
    const auto* run = std::get_if<OutboxSourceRun>(&source);
    if (!run || run->runId.empty()) return {};
    const auto report = reports.settledSessionReport(task);
    if (!report || !report->data.matches(*task.lastRun) || report->data.revisionId != task.currentRevision ||
        report->data.resultRunId != run->runId || report->data.evidence.availability != RunEvidence::Availability::Available ||
        !present(report->data.evidence.report))
        return {};
    return LearningSource{report->id, run->runId};
}

void runtime::TaskDecisions::rejectDecision(const TaskData& task, const Json& event, const TaskDecision& decision, std::string_view message,
                                            bool replay) {
    updates.change(task, [&](TaskData& current) {
        const bool unknown = replay && isGitTool(decision.tool) && gitStorage.gitEffectStarted(gitInvocation(decision), current.id);
        const auto why = unknown ? "The host outcome is unknown. Check the target before retrying." : bounded(message, 2048);
        ToolErrorEntry entry{event.at("id").get<Id>(), bounded(decision.tool, 128), why, unknown, {}};
        const auto args = decision.args.dump();
        if (args.size() <= 2048)
            entry.arguments = decision.args;
        else
            entry.arguments = bounded(args, 2048);
        journalStorage.journal(current.id, entry);
        database.exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {event["id"]});
        current.feedback.reset();
        return true;
    });
}

void runtime::TaskDecisions::acceptReplyOrCompletion(const TaskData& snapshot, const Json& event, const TaskDecision& decision) {
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    const auto id = snapshot.id;
    auto current = taskRecords.load(id);
    if (current.generation != snapshot.generation || current.eventId != event["id"] ||
        current.currentRevision != snapshot.currentRevision || current.state != TaskState::deciding)
        return;
    if (decision.tool == "reply_to_bot") {
        if (peerExchanges.replyPeer(current, event, decision, actionId())) updates.saveTask(current);
    } else {
        if (peerStorage.peerExchange(id) || !canComplete(current, event)) return;
        complete(current, event, decision);
    }
    transaction.commit();
}

void runtime::TaskDecisions::complete(TaskData& current, const Json& event, const TaskDecision& decision) {
    requireInstructionAcknowledgments(current, decision.args);
    if (conversationTurns.conversationPending(current.botId)) return;
    auto source = completionProvenance(current, event);
    const auto learnings = decision.args.find("learnings");
    const auto learning = learnings == decision.args.end() ? std::nullopt : learningSource(current, event);
    updates.consumeEvent(current);
    updates.recordDecision(current, event, decision, actionId());
    const auto completion =
        taskRecords.complete(current, CompletionEntry{field(decision.args, "report"), current.currentRevision,
                                                      TaskCompletion{decision.reason, current.doneWhen, std::move(source)}});
    if (learning) memoryStorage.learnFromWork(current, completion, learning->reportId, learning->runId, *learnings, unixTime());
}

void runtime::TaskDecisions::accept(const TaskData& snapshot, const Json& event, const TaskDecision& decision,
                                    std::string_view approvedInvocation, const Json* launchDefaults, std::span<const T3Project> projects) {
    const auto& args = decision.args;
    const auto& tool = decision.tool;
    if (isGitTool(tool)) {
        Json result;
        const auto invocation = approvedInvocation.empty() ? gitInvocation(decision) : std::string(approvedInvocation);
        try {
            result = gitStorage.gitTool(*gitHost, snapshot.botId, snapshot.id, snapshot.environmentId, snapshot.projectId, decision,
                                        invocation, snapshot.generation);
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception& error) {
            result = {{"ok", false}, {"error", secretStorage.sharedRedactor().text(error.what())}};
        }
        if (result.value("policyWaiting", false)) return;
        updates.change(snapshot, [&](TaskData& current) {
            database.exec("UPDATE policy_approvals SET resume=NULL WHERE operation=?",
                          {"git/" + std::to_string(current.id) + "/" + invocation});
            auto row = database.query("SELECT data FROM events WHERE id=? AND task_id=?", {event.at("id"), current.id});
            if (!row.next()) return false;
            auto data = row.json(0);
            if (!data.contains("gitResults")) data["gitResults"] = Json::array();
            if (result.contains("readId"))
                data["gitResults"].push_back({{"tool", tool}, {"readId", result.at("readId")}});
            else
                data["gitResults"].push_back(
                    secretStorage.redactor(current.botId).record(Json{{"tool", tool}, {"result", std::move(result)}}));
            if (data["gitResults"].size() > 4) data["gitResults"].erase(data["gitResults"].begin());
            database.exec("UPDATE events SET data=? WHERE id=?", {data, event.at("id")});
            return true;
        });
        return;
    }
    static const std::map<std::string_view, std::set<std::string_view>> arguments{
        {"delegate", {"instruction", "doneWhen", "projectId"}},
        {"answer_session_question", {"requestId", "answers"}},
        {"follow_up", {"instruction"}},
        {"complete", {"report", "acknowledgedInstructions", "learnings"}},
        {"wait_for_event", {"note", "checkAgainAfterMinutes", "acknowledgedInstructions"}},
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
        if (sessionRead != "on_demand" && !(sessionRead != "off" && event.at("kind") == "delivery_unconfirmed"))
            throw Conflict("Only the terminal report can be read for this event");
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
    const auto project = field(args, "projectId");
    const bool choosingProject = tool == "delegate" && snapshot.projectId.empty();
    if (choosingProject && (project.empty() || std::ranges::find(projects, project, &T3Project::id) == projects.end()))
        throw Conflict("Choose a listed projectId.");
    if (!choosingProject && args.contains("projectId")) throw Conflict("This task already has a T3 project.");
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
        if (tool == "wait_for_event") {
            waitForExternalEvent(current, args, clock());
            updates.consumeEvent(current);
            updates.recordDecision(current, event, decision, actionId());
            return true;
        }
        if (tool == "follow_up" && current.newSessionRequired) throw Conflict("Start a new Session in the current environment");
        if (choosingProject) taskRouting.assignProject(current, project);
        if (work) updates.establishCriteria(current, args);
        if (launchDefaults) applyLaunchDefaults(current, *launchDefaults);
        updates.consumeEvent(current);
        const auto action = actionId();
        updates.recordDecision(current, event, decision, action);
        if (tool == "ask_user") {
            const auto waitId = waitStorage.wait(current,
                                                 ModelQuestionWait{args.value("choices", std::vector<std::string>{}), decision.reason,
                                                                   current.question ? current.question->target.requestId : std::string{}},
                                                 field(args, "question"));
            if (current.question) current.question->target = questionTarget(current, waitId);
        } else if (work && !current.runTargets().empty()) {
            dispatch.queueDestinations(current, action, instruction, args, event);
        } else {
            dispatch.queueAction(current, action, commandToolFromName(tool).value(), instruction, args, event);
            if (choosingProject) current.state = TaskState::queued;
        }
        return true;
    });
}

void runtime::TaskDecisions::applyLaunchDefaults(TaskData& task, const Json& defaults) {
    auto session = task.config.value("session", Json::object());
    if (!session.is_object()) session = Json::object();
    for (const auto* key : {"model", "providerInstanceId", "workspaceStrategy"}) {
        if (defaults.contains(key))
            session[key] = defaults.at(key);
        else
            session.erase(key);
    }
    if (session.empty())
        task.config.erase("session");
    else
        task.config["session"] = std::move(session);
}

bool runtime::TaskDecisions::continueFreshSession(const TaskData& task, const Json* launchDefaults) {
    if (task.projectId.empty() || !task.freshContextRequested || !RunReports::hasSettledRun(task) || task.pendingInstructions.empty())
        return false;
    return updates.change(task, [&](TaskData& current) {
        auto event = taskEvents.currentEvent(current);
        if (!event.is_null() && event.at("consumed") != true && (event.at("kind") != "result" || event.contains("userAnswer")))
            return false;
        if (event.is_null()) event = Json::object();
        const auto tool = current.newSessionRequired ? CommandTool::delegate : CommandTool::follow_up;
        const TaskDecision decision{
            {std::string(commandToolName(tool)), Json::object(), "Apply accepted user instructions after the assigned Run settled"}};
        const auto action = actionId();
        if (event.contains("id")) updates.recordDecision(current, event, decision, action);
        updates.consumeEvent(current);
        if (launchDefaults && tool == CommandTool::delegate) applyLaunchDefaults(current, *launchDefaults);
        dispatch.queueAction(current, action, tool, "Continue this Flock task.", decision.args, event);
        return true;
    });
}

bool runtime::TaskDecisions::resumeGitApproval(const TaskData& task) {
    Json saved, event;
    {
        std::lock_guard lock(database.mutex);
        auto row = database.query("SELECT resume FROM policy_approvals WHERE task=? AND generation=?"
                                  " AND turn=0 AND resume IS NOT NULL AND status<>'pending' ORDER BY id LIMIT 1",
                                  {task.id, task.generation});
        if (!row.next()) return false;
        saved = row.json(0);
        event = taskEvents.currentEvent(task);
    }
    if (!gitHost || event.is_null()) return false;
    const TaskDecision decision{{field(saved, "tool"), saved.at("args"), field(saved, "reason")}};
    accept(task, event, decision, field(saved, "invocation"));
    return true;
}

CompletionProvenance runtime::TaskDecisions::completionProvenance(const TaskData& task, const Json& event) {
    if (auto peer = peerExchanges.peerCompletion(task, event)) return std::move(*peer);
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
}
