#include "runtime_task_decisions.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_peer_exchanges.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_session_history.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/session_question.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/outbox_instruction.hpp"

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
constexpr int maxToolErrors = 6;

std::string gitInvocation(const TaskDecision& decision) {
    return "attempt/" + std::to_string(decision.attemptId);
}
}

Runtime::Components::TaskDecisions::TaskDecisions(Store::Impl& db, TaskUpdates& updates, RunReports& reports, SessionHistory& history,
                                                  PeerExchanges& peerExchanges, OutboxDispatch& dispatch, const std::string& sessionRead,
                                                  GitHost* gitHost)
    : db(db), updates(updates), reports(reports), history(history), peerExchanges(peerExchanges), dispatch(dispatch),
      sessionRead(sessionRead), gitHost(gitHost) {}

bool Runtime::Components::TaskDecisions::canComplete(const TaskData& task, const Json& event) const {
    if (task.replanning) return false;
    if (!task.newSessionRequired && RunReports::hasSettledRun(task) && task.lastRun->assignment.revisionId == task.currentRevision &&
        !reports.sessionReportUnavailable(task))
        return true;
    const auto peer = peerExchanges.peerResult(task, event);
    if (!peer) return false;
    if (task.newSessionRequired) return peer->environment == task.environmentId && event.at("status") == "completed";
    return task.currentRevision.has_value() || (peer->kind == "consult" && event.at("status") == "completed");
}

void Runtime::Components::TaskDecisions::rejectDecision(const TaskData& task, const Json& event, const TaskDecision& decision,
                                                        std::string_view message, bool replay) {
    updates.change(task, [&](TaskData& current) {
        const bool unknown = replay && isGitTool(decision.tool) && db.gitStorage.gitEffectStarted(gitInvocation(decision), current.id);
        const auto why = unknown ? "The host outcome is unknown. Check the target before retrying." : bounded(message, 2048);
        ToolErrorEntry entry{event.at("id").get<Id>(), bounded(decision.tool, 128), why, unknown, {}};
        const auto args = decision.args.dump();
        if (args.size() <= 2048)
            entry.arguments = decision.args;
        else
            entry.arguments = bounded(args, 2048);
        db.journalStorage.journal(current.id, entry);
        db.exec("UPDATE events SET failures=0,retry_at=0 WHERE id=?", {event["id"]});
        current.feedback.reset();
        if (event.at("decisionFeedback").size() + 1 >= maxToolErrors) db.waitStorage.wait(current, ModelFailureWait{}, why);
        return true;
    });
}

void Runtime::Components::TaskDecisions::acceptReplyOrCompletion(const TaskData& snapshot, const Json& event,
                                                                 const TaskDecision& decision) {
    std::lock_guard lock(db.mutex);
    Transaction transaction(db.db);
    const auto id = snapshot.id;
    auto current = db.taskRecords.load(id);
    if (current.generation != snapshot.generation || current.eventId != event["id"] ||
        current.currentRevision != snapshot.currentRevision || current.state != TaskState::deciding)
        return;
    if (decision.tool == "reply_to_bot") {
        if (peerExchanges.replyPeer(current, event, decision, actionId())) updates.saveTask(current);
    } else {
        if (db.peerStorage.peerExchange(id) || !canComplete(current, event)) return;
        complete(current, event, decision);
    }
    transaction.commit();
}

void Runtime::Components::TaskDecisions::complete(TaskData& current, const Json& event, const TaskDecision& decision) {
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
    db.journalStorage.journal(current.id,
                              CompletionEntry{field(decision.args, "report"), current.currentRevision,
                                              TaskCompletion{decision.reason, current.doneWhen, completionProvenance(current, event)}});
    updates.saveTask(current);
}

void Runtime::Components::TaskDecisions::accept(const TaskData& snapshot, const Json& event, const TaskDecision& decision,
                                                std::string_view approvedInvocation) {
    const auto& args = decision.args;
    const auto& tool = decision.tool;
    if (isGitTool(tool)) {
        Json result;
        const auto invocation = approvedInvocation.empty() ? gitInvocation(decision) : std::string(approvedInvocation);
        try {
            result = db.gitStorage.gitTool(*gitHost, snapshot.botId, snapshot.id, snapshot.environmentId, snapshot.projectId, decision,
                                           invocation, snapshot.generation);
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception& error) {
            result = {{"ok", false}, {"error", db.secretStorage.sharedRedactor().text(error.what())}};
        }
        if (result.value("policyWaiting", false)) return;
        updates.change(snapshot, [&](TaskData& current) {
            db.exec("UPDATE policy_approvals SET resume=NULL WHERE operation=?", {"git/" + std::to_string(current.id) + "/" + invocation});
            auto row = db.query("SELECT data FROM events WHERE id=? AND task_id=?", {event.at("id"), current.id});
            if (!row.next()) return false;
            auto data = row.json(0);
            if (!data.contains("gitResults")) data["gitResults"] = Json::array();
            if (result.contains("readId"))
                data["gitResults"].push_back({{"tool", tool}, {"readId", result.at("readId")}});
            else
                data["gitResults"].push_back(
                    db.secretStorage.redactor(current.botId).record(Json{{"tool", tool}, {"result", std::move(result)}}));
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
        if (sessionRead != "on_demand") throw Conflict("Only the terminal report can be read for this event");
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
            const auto waitId =
                db.waitStorage.wait(current,
                                    ModelQuestionWait{args.value("choices", std::vector<std::string>{}), decision.reason,
                                                      current.question ? current.question->target.requestId : std::string{}},
                                    field(args, "question"));
            if (current.question) current.question->target = questionTarget(current, waitId);
        } else if (work && !current.runTargets().empty()) {
            dispatch.queueDestinations(current, action, instruction, args, event, charge);
        } else {
            dispatch.queueAction(current, action, commandToolFromName(tool).value(), instruction, args, event, charge);
        }
        return true;
    });
}

bool Runtime::Components::TaskDecisions::continueStage(const TaskData& task) {
    if ((!task.workflow && !task.freshContextRequested) || !RunReports::hasSettledRun(task) || task.pendingInstructions.empty())
        return false;
    return updates.change(task, [&](TaskData& current) {
        auto event = db.taskEvents.currentEvent(current);
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
        dispatch.queueAction(current, action, tool, "Continue this Flock task.", decision.args, event, charge);
        return true;
    });
}

bool Runtime::Components::TaskDecisions::resumeGitApproval(const TaskData& task) {
    Json saved, event;
    {
        std::lock_guard lock(db.mutex);
        auto row = db.query("SELECT resume FROM policy_approvals WHERE task=? AND generation=?"
                            " AND turn=0 AND resume IS NOT NULL AND status<>'pending' ORDER BY id LIMIT 1",
                            {task.id, task.generation});
        if (!row.next()) return false;
        saved = row.json(0);
        event = db.taskEvents.currentEvent(task);
    }
    if (!gitHost || event.is_null()) return false;
    const TaskDecision decision{{field(saved, "tool"), saved.at("args"), field(saved, "reason")}};
    accept(task, event, decision, field(saved, "invocation"));
    return true;
}

CompletionProvenance Runtime::Components::TaskDecisions::completionProvenance(const TaskData& task, const Json& event) {
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
