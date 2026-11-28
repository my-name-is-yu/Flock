#include "conversation_call.hpp"
#include "session_send_receipts.hpp"
#include "turn_results.hpp"
#include "channel_connectors.hpp"
#include "session_participation_records.hpp"
#include "channel_delivery_queue.hpp"
#include "conversation_messages.hpp"
#include "flock/board_inbox.hpp"
#include "web_identity_state.hpp"
#include "wait_storage.hpp"
#include "task_routing.hpp"
#include "task_records.hpp"
#include "task_operations.hpp"
#include "session_read_storage.hpp"
#include "session_participation_storage.hpp"
#include "session_message_storage.hpp"
#include "secret_storage.hpp"
#include "model_calls.hpp"
#include "memory_storage.hpp"
#include "local_machine_storage.hpp"
#include "journal_storage.hpp"
#include "git_storage.hpp"
#include "conversation_tools.hpp"
#include "conversation_turns.hpp"
#include "turn_runner.hpp"
#include "channel_storage.hpp"
#include "bot_settings_storage.hpp"
#include "bot_setup_storage.hpp"
#include "tool_batch.hpp"
#include "bot_lifecycle_storage.hpp"
#include "automation_storage.hpp"
#include "attachment_storage.hpp"
#include "flock/limits.hpp"
#include "conversation_context_storage.hpp"
#include "conversation_storage.hpp"
#include "journal.hpp"
#include "journal_task.hpp"
#include "journal_conversation.hpp"
#include "outbox_storage.hpp"
#include "record_decode.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/bot_change.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"
#include "flock/wait.hpp"
#include "flock/outbox_instruction.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "flock/board_inbox.hpp"
#include "flock/policy.hpp"
#include "workspace_internal.hpp"
#include "flock/git_host.hpp"
#include "flock/task_target.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace flockv0 {
using conversation_storage::integer;
using conversation_storage::identifier;
using conversation_storage::japanese;
using conversation_storage::text;
using conversation_storage::ToolError;
using limits::messages::maxBodyBytes;

namespace {
using conversation::Message;
using conversation::PrincipalSource;

const std::vector<std::string> channelTools{"channel_edit_message", "channel_react", "channel_send_attachment", "publish_artifact",
                                            "read_channel_attachment"};

constexpr int activeTaskLimit = 12;
constexpr int terminalTaskLimit = 8;
constexpr int findLimit = 10;
constexpr std::size_t resultChunk = 6000;

struct TaskResult {
    std::string text;
    Id revisionId = 0;
    bool truncated = false;
    Json source;
};

TaskResult taskResult(Id id, std::string_view kind, const Json& data) {
    if (kind == "report") {
        auto report = decodeRunReport(id, data);
        return {std::move(report.evidence.report),
                report.revisionId.value_or(0),
                report.evidence.truncated,
                {{"environmentId", report.environmentId},
                 {"threadId", report.run.threadId},
                 {"runId", report.resultRunId},
                 {"itemId", report.evidence.itemId}}};
    }
    if (kind == "completed") {
        auto completion = decodeCompletion(id, data);
        const auto* source = completion.source();
        return {std::move(completion.text), completion.revisionId.value_or(0), false, source ? sourceRunJson(*source) : Json(nullptr)};
    }
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [](const Json&, const RecordDecoder&) { return TaskResult{}; });
}

const std::vector<std::string> turnTools{"start_task",
                                         "send_to_task",
                                         "change_task",
                                         "answer_task_question",
                                         "record_task_blocker",
                                         "withdraw_task_question",
                                         "cancel_task",
                                         "message_session",
                                         "invite_bot",
                                         "leave_session",
                                         "interrupt_session",
                                         "read_result",
                                         "find_tasks",
                                         "flock_session_search",
                                         "flock_session_events",
                                         "read_tool_result",
                                         "reply"};
const std::vector<std::string> webTools{"compact_conversation",
                                        "expand_context",
                                        "continue_fresh_session",
                                        "list_secrets",
                                        "request_secret_input",
                                        "list_connections",
                                        "check_environment_readiness",
                                        "request_bot_change",
                                        "read_memory",
                                        "memory_history",
                                        "create_schedule",
                                        "list_schedules",
                                        "remove_schedule",
                                        "create_reminder",
                                        "create_trigger"};

std::string_view modelFailureReason(ModelErrorCode code) {
    switch (code) {
    case ModelErrorCode::authentication_required: return "authentication_required";
    case ModelErrorCode::unsupported_reasoning_effort: return "unsupported_reasoning_effort";
    case ModelErrorCode::image_rejected: return "image_rejected";
    case ModelErrorCode::invalid_tool_count: return "invalid_tool_count";
    case ModelErrorCode::invalid_tool_output: return "invalid_tool_output";
    case ModelErrorCode::unavailable_tool: return "unavailable_tool";
    case ModelErrorCode::invalid_tool_arguments: return "invalid_tool_arguments";
    case ModelErrorCode::inference_failed: return "inference_failed";
    }
    return "inference_failed";
}

std::string modelFailureReply(ModelErrorCode code, bool ja) {
    if (code == ModelErrorCode::authentication_required)
        return ja ? "ChatGPTへのサインインが必要です。Flockの設定からサインインしてください。"
                  : "This Flock needs a ChatGPT sign-in. Open Flock settings and sign in to ChatGPT.";
    if (code == ModelErrorCode::unsupported_reasoning_effort)
        return ja ? "このモデルは選択された推論レベルに対応していません。Botの設定で別のレベルか「Model default」を選んでください。"
                  : "This model doesn’t support the selected reasoning effort. Choose another effort or Model default in Bot settings.";
    if (code == ModelErrorCode::image_rejected)
        return ja ? "添付画像を読み込めませんでした。別の画像に差し替えるか、画像を外して、メッセージを送り直してください。"
                  : "I couldn’t read an attached image. Replace it or remove it, then send your message again.";
    return ja ? "このBotは応答を作成できませんでした。" : "This Bot couldn’t produce a reply.";
}

Json waitView(const Wait& wait) {
    Json view = {{"waitId", wait.id}, {"reason", waitReasonName(wait.reason())}, {"text", bounded(wait.text, 1000)}};
    if (wait.reason() == WaitReason::user_question) {
        auto& list = view["choices"] = Json::array();
        for (const auto& choice : wait.choices().first(std::min(wait.choices().size(), std::size_t{12})))
            list.push_back(bounded(choice, 200));
    }
    return view;
}

std::string requestName(const TaskData& task) {
    const std::string_view request = task.request;
    return bounded(request.substr(0, request.find('\n')), 160);
}

ConversationReplyStatus effectStatus(const Json& actions) {
    using enum ConversationReplyStatus;
    auto status = done;
    for (const auto& action : actions) {
        const auto effect = field(action, "effect");
        if (effect == "replanning" || effect == "cancelling" || effect == "interrupt_requested") return stopping;
        if (effect == "started" || effect == "accepted" || effect == "answered" || effect == "delivered") status = working;
    }
    return status;
}
}

bool conversation::allowsTurnTool(const Message& message, std::string_view tool) {
    if (!message.botOrigin) return true;
    constexpr std::string_view personTools[]{
        "answer_task_question", "withdraw_task_question", botSetupTool,           "request_bot_change",
        "write_memory",         "revert_memory_edit",     "read_memory",          "memory_history",
        "request_secret_input", "list_secrets",           "compact_conversation", "continue_fresh_session",
        "create_schedule",      "list_schedules",         "remove_schedule",      "create_reminder",
        "create_trigger",       "workspace_write_file",   "mcp_call_tool"};
    if (std::ranges::find(personTools, tool) != std::ranges::end(personTools)) return false;
    // Direct Bot messages already have a smaller tool set than Session participants.
    return !message.routing.contains("botMessage") || (!isBotTool(tool) && std::ranges::find(webTools, tool) == webTools.end());
}

const char* ConversationTurns::questionAnswerError(const TaskData& task, const Message& message, const std::optional<Wait>& wait) const {
    if (task.state != TaskState::waiting || !wait) return "That question is no longer open";
    if (wait->reason() != WaitReason::user_question) return "Only user_question waits can be answered; approvals use their own controls";
    if (message.botOrigin) return "Another Bot's message cannot answer a question for the person";
    const auto asked = waitStorage.waitAnchor(task.id, wait->id);
    if (asked >= message.id) return "That question opened after this message was sent, so this message cannot answer it";
    if (message.replyTo && (!message.question || message.question->messageId != asked))
        return "This message replies to something else, so it can only answer the question it replies to";
    if (!asked || wait->run != taskWaitRun(task) || wait->eventId != task.eventId) return "That question is no longer current";
    if (task.replanning) return "The task is replanning, so this question will close. Its Bot asks again if it still needs to know";
    return nullptr;
}

std::optional<TaskData> ConversationTurns::wokenTask(const Message& message) const {
    const auto wake = message.routing.find("sessionWake");
    if (wake == message.routing.end() || !wake->contains("taskId")) return std::nullopt;
    auto task = taskRecords.load(wake->at("taskId").get<Id>());
    if (task.botId != message.bot || task.state != TaskState::deciding || !task.actionId.empty() || task.activeRun ||
        !task.pendingInstructions.empty())
        return std::nullopt;
    auto current = database.query(R"SQL(
SELECT 1 FROM events WHERE id=? AND task_id=? AND consumed=1
  AND EXISTS (SELECT 1 FROM messages WHERE id=? AND client_message_id=?)
)SQL",
                                  {task.eventId, task.id, message.id, taskWakeClientId(task.id, task.eventId)});
    if (!current.next()) return std::nullopt;
    return task;
}

namespace {
std::optional<std::pair<Id, RunReportEntry>> currentWakeReport(Database& database, const TaskData& task) {
    if (!task.lastRun || task.newSessionRequired) return std::nullopt;
    auto row = database.query("SELECT id,data FROM journal WHERE task_id=? AND kind='report' ORDER BY id DESC LIMIT 1", {task.id});
    if (!row.next()) return std::nullopt;
    const auto id = row.number(0);
    auto report = decodeRunReport(id, row.json(1));
    if (!report.matches(*task.lastRun) || report.revisionId != task.currentRevision || report.resultRunId.empty() ||
        report.evidence.availability != RunEvidence::Availability::Available || !conversation_storage::hasText(report.evidence.report))
        return std::nullopt;
    return std::pair{id, std::move(report)};
}

std::optional<std::pair<std::string, Json>> wakeEvidenceLookup(const Json& call) {
    const auto tool = field(call, "tool");
    const auto& args = call.at("args");
    if (!args.is_object()) return std::nullopt;
    if (tool == "find_tasks") {
        auto task = args.value("taskId", Json{});
        if (!task.is_null()) return std::pair{tool, Json{{"taskId", std::move(task)}}};
        auto query = args.value("query", Json{});
        if (query.is_null() || (query.is_string() && !conversation_storage::hasText(query.get_ref<const std::string&>()))) query = "";
        return std::pair{tool, Json{{"query", std::move(query)}, {"before", std::max<Id>(0, integer(args, "before"))}}};
    }
    if (tool == "read_result" && args.contains("messageId")) return std::pair{tool, args.at("messageId")};
    if (tool == "flock_session_search" || tool == "flock_session_events") return std::pair{tool, args};
    return std::nullopt;
}
}

std::optional<TurnRunner::WorkLearningSource> TurnRunner::wokenLearningSource(const TaskData& task) {
    if (const auto current = currentWakeReport(database, task); current && memoryStorage.canLearnFrom(task))
        return WorkLearningSource{current->first, current->second.resultRunId};
    return std::nullopt;
}

Json TurnRunner::applyWokenTaskDecision(const Message& message, const Decision& decision, Seconds now) {
    auto task = conversationTurns.wokenTask(message);
    if (!task) throw ToolError("The task is no longer waiting for this conversation");
    const auto& args = decision.args;
    if (decision.tool == "wait_for_event") {
        if (args.size() != 2 || !args.contains("note") || !args.contains("checkAgainAfterMinutes"))
            throw ToolError("Provide a waiting note and a re-check time");
        waitForExternalEvent(*task, args, now);
        taskRecords.save(*task);
    } else {
        const auto learnings = args.find("learnings");
        if (args.size() != (learnings == args.end() ? 1u : 2u)) throw ToolError("Provide a report and optional learnings");
        const auto source = learnings == args.end() ? std::nullopt : wokenLearningSource(*task);
        if (learnings != args.end() && !source) throw ToolError("This task's Session report cannot be learned from");
        const auto completion = taskRecords.completeWithReport(
            *task, text(args, "report", limits::session_prompt::maxDecisionTextBytes, true), decision.reason);
        if (source) memoryStorage.learnFromWork(*task, completion, source->report, source->runId, *learnings, now);
    }
    return {{"ok", true}, {"taskId", task->id}, {"effect", taskStateName(task->state)}};
}

Json ConversationTurns::taskState(const TaskData& task, const Message& message, bool fullContract) const {
    const auto waiting = waitStorage.currentWait(task.id);
    auto wait = waiting ? waitView(*waiting) : Json(nullptr);
    if (waiting) wait["answerable"] = !questionAnswerError(task, message, waiting);
    Json view = {{"generation", task.generation},
                 {"revisionId", task.currentRevision.value_or(0)},
                 {"state", taskStateName(task.state)},
                 {"replanning", task.replanning},
                 {"wait", std::move(wait)}};
    if (task.queuedInT3) view["queuedInT3"] = true;
    if (task.hasDestinations()) {
        if (task.requestedCommit) view["requestedCommit"] = *task.requestedCommit;
        if (const auto outcome = task.runOutcome()) view["runOutcome"] = runOutcomeName(*outcome);
        auto& results = view["runResults"] = Json::array();
        if (!task.runResults().empty()) {
            for (const auto& run : task.runResults()) {
                auto summary = destinationRunSummaryJson(run);
                if (fullContract && run.report) summary["report"] = bounded(run.report->text, 2000);
                if (run.detail) summary["detail"] = bounded(*run.detail, 320);
                results.push_back(std::move(summary));
            }
        } else {
            for (const auto& target : task.runTargets()) {
                auto summary = runTargetJson(target);
                summary["status"] = task.state == TaskState::cancelled ? "cancelled" : "queued";
                results.push_back(std::move(summary));
            }
        }
    }
    if (const auto origin = message.routing.find("sessionOrigin");
        task.sessionOrigin && origin != message.routing.end() &&
        origin->at("environmentId") == Json(task.sessionOrigin->target.environmentId) &&
        origin->at("threadId") == Json(task.sessionOrigin->target.threadId)) {
        const auto& run = task.activeRun ? task.activeRun : task.lastRun;
        if (run && !task.selectedSession) view["session"] = {{"environmentId", run->environmentId}, {"threadId", run->ref.threadId}};
        if (task.launchWarning) view["launchWarning"] = *task.launchWarning;
    }
    const auto project = [&](const char* key, const std::string& value, std::size_t limit) {
        view[key] = fullContract ? value : bounded(value, limit);
        if (view[key].get_ref<const std::string&>().size() < value.size()) view[std::string(key) + "Truncated"] = true;
    };
    project("doneWhen", effectiveDoneWhen(task), 1600);
    if (task.criteriaProposal) {
        view["doneWhenSource"] = "requested";
        if (task.criteriaProposal->sourceMessageId) view["criteriaSourceMessageId"] = *task.criteriaProposal->sourceMessageId;
    }
    if (task.blocker && !task.blocker->reason.empty()) view["blockerReason"] = task.blocker->reason;
    if (!task.intent.empty()) project("intent", task.intent, 1000);
    return view;
}

TaskData ConversationTurns::ownTask(Id taskId, const Message& message) const {
    auto row = database.query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.id=? AND t.bot_id=? AND (?='' OR c.scope_key=?)
)SQL",
                              {taskId, message.bot, message.channelScope, message.channelScope});
    if (row.next()) return taskRecords.load(taskId);
    throw ToolError("No task with taskId " + std::to_string(taskId));
}

Json ConversationTurns::resultReference(Id messageId, const Message& message) const {
    auto row = database.query(R"SQL(
SELECT t.id,t.state,j.kind,j.data,t.request,coalesce(t.current_revision,0)
FROM journal j JOIN tasks t ON t.id=j.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE j.id=? AND t.bot_id=? AND j.kind IN ('report','completed','cancelled') AND (?='' OR c.scope_key=?)
)SQL",
                              {messageId, message.bot, message.channelScope, message.channelScope});
    if (!row.next()) throw ToolError("No result with messageId " + std::to_string(messageId));
    const auto taskId = row.number(0);
    const auto state = decodeTaskState(taskId, row.string(1));
    const auto kind = row.string(2);
    auto decoded = taskResult(messageId, kind, row.json(3));
    Json result = {{"taskId", taskId},
                   {"messageId", messageId},
                   {"revisionId", decoded.revisionId},
                   {"text", std::move(decoded.text)},
                   {"kind", kind},
                   {"taskState", taskStateName(state)},
                   {"request", row.string(4)},
                   {"currentRevision", row.number(5)},
                   {"truncated", decoded.truncated}};
    if (!decoded.source.is_null()) result["run"] = std::move(decoded.source);
    return secretStorage.redactor(message.bot).record(std::move(result));
}

namespace {
std::optional<OutboxSource> sourceRef(const Json& result) {
    if (result.is_null()) return std::nullopt;
    OutboxSource source{.taskId = result.at("taskId"),
                        .messageId = result.at("messageId"),
                        .revisionId = result.at("revisionId").get<Id>(),
                        .text = bounded(field(result, "text"), limits::session_prompt::maxQuoteTextBytes)};
    if (const auto run = result.find("run"); run != result.end())
        source.run = decodeOutboxSourceRun(*run, {"journal", std::to_string(source.messageId), "run"});
    return source;
}
}

Json TurnRunner::conversationSteps(const Message& message, int step, std::vector<ModelImage>& images) {
    Json turn = Json::array();
    auto steps = database.query("SELECT decision,result,step FROM conversation_steps WHERE message_id=? AND step<?"
                                " AND result IS NOT NULL ORDER BY step",
                                {message.id, step});
    while (steps.next()) {
        const auto calls = steps.json(0);
        const auto results = steps.json(1);
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const auto& decision = calls.at(i);
            auto args = decision.at("args");
            if (args.is_object() && decision["tool"] == "channel_send_attachment") args.erase("contentBase64");
            if (args.is_object() && decision["tool"] == "publish_artifact") args.erase("html");
            auto result = channelStorage.channelToolStatus(message.id, message.bot, results.at(i), &images);
            if (args.is_object() && args.contains("docId")) {
                const bool reading = decision.at("tool") == "read_memory" || decision.at("tool") == "memory_history";
                const auto action = reading ? Action::shared_memory_read : Action::shared_memory_write;
                const auto document = field(args, "docId");
                const auto policy = permissions;
                if (!policy.allows(conversationMessages.actor(message), action, document) ||
                    policy.forbidden(message.bot, action, document)) {
                    args = Json::object();
                    result = {{"ok", false}, {"error", "Memory access denied"}};
                } else {
                    const auto shared = secretStorage.sharedRedactor();
                    args = shared.record(std::move(args));
                    result = shared.record(std::move(result));
                }
            }
            const auto tool = field(decision, "tool");
            if ((decision.contains("rejection") && result.value("untrustedData", false)) ||
                modelArgumentsContainProtected(
                    modelCalls.modelCallRedactor(message.bot, tool, args, message.principalSource == PrincipalSource::gitWebhook), tool,
                    args)) {
                args = Json::object();
                result = toolFailure("Saved tool history is unavailable. Check the target before retrying.");
            }
            turn.push_back({{"tool", decision.at("tool")},
                            {"args", std::move(args)},
                            {"result", std::move(result)},
                            {"_ref", {{"messageId", message.id}, {"step", steps.number(2)}, {"callIndex", i + 1}}}});
        }
    }
    return turn;
}

Json TurnRunner::turnInput(const Message& message, int step, Seconds now, Json machines, std::vector<ModelImage>& images,
                           const TaskDestinations& linked) {
    if (message.principalSource == PrincipalSource::gitWebhook) {
        const auto& gitEvent = message.routing.at("gitEvent");
        const auto task = taskRecords.load(gitEvent.at("taskId").get<Id>());
        const auto scrub = secretStorage.sharedRedactor();
        auto event = Json{{"kind", "conversation_turn"},
                          {"messageId", message.id},
                          {"text", message.text},
                          {"currentTime", now},
                          {"replyTo", nullptr},
                          {"turn", conversationSteps(message, step, images)},
                          {"gitEvent", scrub.record(gitEvent)}};
        if (auto pullRequests = gitStorage.watchedPullRequests(message); !pullRequests.empty())
            event["watchedPullRequests"] = scrub.record(std::move(pullRequests));
        Json summary{{"request", task.intent}, {"doneWhen", task.doneWhen}};
        if (auto note = taskRecords.externalWaitingNote(task)) summary["waitingNote"] = std::move(*note);
        return {{"task", scrub.record(std::move(summary))}, {"event", std::move(event)}, {"history", Json::array()}};
    }
    const auto scrub = secretStorage.redactor(message.bot);
    Json tasks = Json::array();
    std::set<Id> listed;
    const bool web = message.channelScope.empty();
    const auto summaries = web ? conversationContextStorage.compactions(message.bot, 0, 1, message.id) : Json::array();
    const Id boundary = summaries.empty() ? 0 : summaries.front().at("endMessageId").get<Id>();
    const auto latestResult = [&](Id taskId) -> Json {
        auto row = database.query(R"SQL(
SELECT id,kind,data FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')
ORDER BY id DESC LIMIT 1
)SQL",
                                  {taskId});
        if (!row.next()) return nullptr;
        const auto messageId = row.number(0);
        const auto kind = row.string(1);
        const auto decoded = taskResult(messageId, kind, row.json(2));
        Json result = {{"messageId", messageId}, {"kind", kind}, {"revisionId", decoded.revisionId}};
        if (messageId > boundary) result["excerpt"] = bounded(scrub.text(decoded.text), 600);
        return result;
    };
    const auto addTask = [&](Id taskId) {
        if (!listed.insert(taskId).second) return;
        const auto task = taskRecords.load(taskId);
        Json view = {{"taskId", taskId}, {"request", bounded(task.request, 800)}, {"environmentId", task.environmentId}};
        view.update(conversationTurns.taskState(task, message));
        if (localMachineStorage.hasMachines() && task.activeRun)
            view["activeSession"] = {{"threadId", task.activeRun->ref.threadId}, {"runId", task.activeRun->ref.runId}};
        if (task.sourceTaskId) view["sourceTaskId"] = *task.sourceTaskId;
        if (auto result = latestResult(taskId); !result.is_null()) view["latestResult"] = std::move(result);
        tasks.push_back(std::move(view));
    };
    bool moreTasks = false;
    {
        auto rows = database.query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND t.state NOT IN ('completed','cancelled') AND (?='' OR c.scope_key=?)
ORDER BY t.id DESC LIMIT ?
)SQL",
                                   {message.bot, message.channelScope, message.channelScope, activeTaskLimit + 1});
        for (int i = 0; rows.next(); ++i) {
            if (i == activeTaskLimit) {
                moreTasks = true;
                break;
            }
            addTask(rows.number(0));
        }
    }
    {
        auto rows = database.query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND t.state IN ('completed','cancelled') AND (?='' OR c.scope_key=?)
ORDER BY t.id DESC LIMIT ?
)SQL",
                                   {message.bot, message.channelScope, message.channelScope, terminalTaskLimit});
        while (rows.next()) addTask(rows.number(0));
    }

    Json replyTo = nullptr;
    if (message.replyTo) {
        auto row = database.query(R"SQL(
SELECT j.kind,j.task_id,j.data,m.text,original.id,original.text,original.reply_to
FROM journal j LEFT JOIN tasks t ON t.id=j.task_id
LEFT JOIN messages m ON m.id=j.id
LEFT JOIN messages original ON original.id=j.reply_to_message_id AND original.bot_id=?3
WHERE j.id=?1 AND j.id<?2 AND (j.conversation_bot_id=?3 OR t.bot_id=?3)
)SQL",
                                  {message.replyTo, message.id, message.bot});
        if (row.next()) {
            const auto kind = row.string(0);
            const Id taskId = row.number(1);
            const auto data = row.json(2);
            replyTo = {{"messageId", message.replyTo}};
            if (kind == "waiting") {
                const auto task = taskRecords.load(taskId);
                const auto current = waitStorage.currentWait(taskId);
                const auto asked = decodeWaitingEntry(message.replyTo, data);
                replyTo.update(waitView(asked));
                replyTo.update({{"kind", "task_question"},
                                {"taskId", taskId},
                                {"revisionId", task.currentRevision.value_or(0)},
                                {"generation", task.generation},
                                {"current", current && current->id == asked.id && waitStorage.waitIsActionable(task)}});
                addTask(taskId);
            } else if (kind == "report" || kind == "completed" || kind == "cancelled") {
                const auto decoded = taskResult(message.replyTo, kind, data);
                replyTo.update({{"kind", "result"}, {"taskId", taskId}, {"text", bounded(decoded.text, 2000)}});
                addTask(taskId);
            } else if (kind == "conversation_reply") {
                const auto reply = decodeConversationReply(message.replyTo, data);
                replyTo.update({{"kind", "bot_message"}, {"text", bounded(reply.text, 2000)}});
                if (!reply.suggestions.is_null()) replyTo["suggestions"] = reply.suggestions;
                if (message.question) addTask(message.question->taskId);
                if (!row.null(4)) {
                    replyTo["inReplyTo"] = {{"messageId", row.number(4)}, {"text", bounded(row.string(5), maxBodyBytes)}};
                    if (row.number(6)) replyTo["inReplyTo"]["replyToMessageId"] = row.number(6);
                }
            } else if (kind == "conversation_message") {
                replyTo.update({{"kind", "user_message"}, {"text", bounded(row.string(3), 2000)}});
            } else {
                replyTo.update({{"kind", kind}, {"text", bounded(field(data, "text"), 2000)}});
            }
        } else
            replyTo = {{"messageId", message.replyTo}, {"kind", "unavailable"}};
        if (message.question) replyTo["taskQuestionMessageId"] = message.question->messageId;
    }

    Json environments = Json::array();
    for (const auto& [name, route] : linked.direct) {
        Json choice{{"name", name.empty() ? route.environmentId : name}, {"environmentId", route.environmentId}};
        if (name.empty()) choice["default"] = true;
        if (const auto* projects = linked.projects(route); projects && !projects->empty()) {
            choice["projects"] = projectChoices(*projects, scrub);
            const auto preferred =
                route.projectId.empty() ? taskRouting.rememberedT3Project(message.bot, route.environmentId) : route.projectId;
            if (std::ranges::find(*projects, preferred, &T3Project::id) != projects->end()) choice["projectId"] = preferred;
        }
        environments.push_back(std::move(choice));
    }
    for (const auto& linkedChoice : linked.linked) {
        Json choice{{"name", scrub.text(linkedChoice.name)},
                    {"environmentId", linkedChoice.route.environmentId},
                    {"projectId", linkedChoice.route.projectId},
                    {"gatewayEnvironmentId", linkedChoice.route.gatewayEnvironmentId},
                    {"linked", true}};
        if (const auto access = linked.access(linkedChoice.route); !access.empty()) choice["access"] = access;
        environments.push_back(std::move(choice));
    }

    const auto channel = channelStorage.channelContext(message.id, message.bot, &images);
    auto turn = conversationSteps(message, step, images);
    const auto expandedBytes = ConversationContextStorage::boundExpandedContext(turn);
    auto history = conversationContextStorage.conversationHistory(message, summaries, expandedBytes);
    Json event{{"kind", "conversation_turn"}, {"messageId", message.id},       {"text", message.text},
               {"currentTime", now},          {"replyTo", std::move(replyTo)}, {"turn", std::move(turn)}};
    if (message.routing.contains("sessionOrigin")) event["sessionOrigin"] = message.routing.at("sessionOrigin");
    for (const auto* key : {"sessionWake", "sessionInvitation", "botMessage"})
        if (message.routing.contains(key)) event[key] = message.routing.at(key);
    if (const auto woken = conversationTurns.wokenTask(message))
        event["task"] = {{"taskId", woken->id},
                         {"request", bounded(scrub.text(woken->intent), 4000)},
                         {"doneWhen", bounded(scrub.text(effectiveDoneWhen(*woken)), 1600)},
                         {"report", bounded(scrub.text(taskRecords.latestReportText(woken->id)), 8000)}};
    event.update(channel);
    if (web) event["attachments"] = attachments.attachmentViews(message.id);
    Json task{{"tasks", std::move(tasks)},
              {"moreTasks", moreTasks},
              {"environments", std::move(environments)},
              {"localMachines", std::move(machines)}};
    if (!linked.unavailable.empty()) task["linkedEnvironmentIssues"] = linked.unavailable;
    return {{"task", std::move(task)}, {"event", std::move(event)}, {"history", std::move(history)}};
}

Json TurnRunner::applyTurnTool(const Message& message, int step, const Decision& decision, Seconds now, const WorkSession* selectedSession,
                               const TaskDestinations& linked, const BotTools& settings) {
    const auto& args = decision.args;
    if (!args.is_object()) throw ToolError("Tool arguments must be an object");
    const auto& tool = decision.tool;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    if (tool == "wait_for_event" || tool == "complete")
        return routing.contains("gitEvent") ? gitStorage.applyGitTaskDecision(message, decision)
                                            : applyWokenTaskDecision(message, decision, now);
    if (tool == "read_memory" || tool == "memory_history" || tool == "write_memory" || tool == "revert_memory_edit") {
        auto result = memoryStorage.conversationMemory(message, tool, args, now);
        result["ok"] = true;
        return result;
    }
    if (tool == "read_tool_result") {
        const auto sourceMessage = identifier(args, "messageId", true);
        const auto sourceStep = identifier(args, "step", true);
        const auto index = identifier(args, "callIndex", true);
        if (sourceMessage != message.id || sourceStep >= step || index > static_cast<Id>(maxToolCalls))
            throw ToolError("Unknown tool result in this turn");
        auto saved = database.query("SELECT json_extract(decision,?) FROM conversation_steps "
                                    "WHERE message_id=? AND step=? AND result IS NOT NULL",
                                    {"$[" + std::to_string(index - 1) + "].tool", message.id, sourceStep});
        if (!saved.next() || !conversation_storage::canRereadToolResult(saved.string(0)))
            throw ToolError("Unknown tool result in this turn");
        return {{"ok", true}};
    }
    if (auto result = channelStorage.channelTool(message.id, bot, decision); !result.is_null()) return result;
    if (auto result = localMachineStorage.localMachineTool(bot, message.channelScope, decision); !result.is_null()) return result;
    if (std::ranges::find(webTools, tool) != webTools.end()) {
        if (!message.channelScope.empty()) throw ToolError("Use Flock Web for this action");
        if (tool == "continue_fresh_session") {
            auto task = conversationTurns.ownTask(identifier(args, "taskId", true), message);
            if (task.hasDestinations()) throw ToolError("Use a new request to run more work across T3 environments");
            if (terminal(task) || task.state == TaskState::cancelling) throw Conflict("Task is closed or stopping");
            const auto instruction =
                args.contains("instruction")
                    ? conversation_storage::text(args, "instruction", limits::session_prompt::maxDecisionTextBytes, true)
                    : "Continue with the same completion criteria.";
            task.newSessionRequired = true;
            task.freshContextRequested = true;
            task.replanning = true;
            taskOperations.acceptInstruction(task, message.id, step, instruction, std::nullopt, nullptr);
            return {{"ok", true}, {"freshContext", true}, {"taskId", task.id}, {"effect", "replanning"}};
        }
        if (tool == "compact_conversation" || tool == "expand_context")
            return conversationContextStorage.contextTool(tool, args, message, step);
        const auto actor = conversationMessages.actor(message);
        auto result = botSettingsStorage.botSettingsTool(bot, message.id, settings, decision);
        if (result.is_null()) result = automationStorage.automationTool(bot, actor, routing, decision, now);
        if (result.is_null()) throw ToolError("Unknown conversation tool");
        result["ok"] = true;
        return result;
    }
    const auto expectGeneration = [&](const TaskData& task) {
        const auto expected = identifier(args, "generation", true, 0);
        if (expected != task.generation)
            throw ToolError("stale", {{"applied", 0},
                                      {"taskId", task.id},
                                      {"expected", {{"generation", expected}}},
                                      {"current", conversationTurns.taskState(task, message)}});
    };
    const auto active = [&](const TaskData& task) {
        if (terminal(task) || task.state == TaskState::cancelling)
            throw ToolError("The task is " + std::string(taskStateName(task.state)),
                            {{"taskId", task.id}, {"current", conversationTurns.taskState(task, message)}});
    };
    const auto source = [&](const TaskData* target) -> Json {
        const auto sourceId = identifier(args, "sourceResultMessageId", false);
        if (!sourceId) return nullptr;
        const auto result = conversationTurns.resultReference(sourceId, message);
        if (target && (result.at("taskId") != target->id || result.at("revisionId") != target->currentRevision.value_or(0)))
            throw ToolError("sourceResultMessageId must be a result of this task's current revision");
        return result;
    };

    if (tool == "start_task") {
        const auto request = text(args, "request", maxBodyBytes, true);
        auto files = attachments.selectForTask(message, args);
        const auto doneWhen = text(args, "doneWhen", limits::session_prompt::maxDecisionTextBytes, false);
        const bool resume = args.value("resume", false);
        auto [destination, targets, commit] = taskRouting.startDestinations(message, args, resume, selectedSession, linked);
        const auto result = source(nullptr);
        if (resume && (result.is_null() || result.at("taskState") != "cancelled"))
            throw ToolError("resume requires sourceResultMessageId from a cancelled task");
        const auto id = taskOperations.startConversationTask(routing, bot, request, destination, message.id, sourceRef(result), resume,
                                                             selectedSession);
        if (args.contains("projectId")) taskRouting.rememberT3Project(bot, destination.environmentId, destination.projectId);
        if (const auto selected = args.find("environments"); selected != args.end())
            for (std::size_t i = 0; i < targets.size(); ++i)
                if (selected->at(i).contains("projectId"))
                    taskRouting.rememberT3Project(bot, targets[i].route.environmentId, targets[i].route.projectId);
        if (!doneWhen.empty() || !targets.empty() || !files.empty()) {
            auto task = taskRecords.load(id);
            task.initialAttachments = std::move(files);
            if (!doneWhen.empty()) task.criteriaProposal = TaskCriteriaProposal{doneWhen, message.id};
            if (!targets.empty()) task.destinations = std::move(targets);
            if (!commit.empty()) task.requestedCommit = std::move(commit);
            taskRecords.save(task);
        }
        Json receipt{{"ok", true}, {"effect", "started"}, {"taskId", id}};
        if (selectedSession) receipt["existingSession"] = true;
        return receipt;
    }
    if (tool == "send_to_task" || tool == "change_task") {
        auto task = conversationTurns.ownTask(identifier(args, "taskId", true), message);
        expectGeneration(task);
        active(task);
        if (tool == "change_task" && task.hasDestinations())
            throw ToolError("Task-wide changes are not available for tasks with destination Runs yet");
        if (tool == "send_to_task" && !task.runTargets().empty())
            throw ToolError("A destination Session is unavailable until its T3 Thread is saved");
        if (tool == "send_to_task" && !task.runResults().empty())
            throw ToolError("Send this destination instruction on its own to use the saved T3 Thread");
        const auto instruction = text(args, "instruction", maxBodyBytes, true);
        const auto result = source(&task);
        std::optional<TaskChange> change;
        if (tool == "change_task") {
            const auto revision = identifier(args, "revisionId", true, 0);
            if (revision != task.currentRevision.value_or(0))
                throw ToolError("stale", {{"applied", 0},
                                          {"taskId", task.id},
                                          {"expected", {{"revisionId", revision}}},
                                          {"current", conversationTurns.taskState(task, message)}});
            change = taskRouting.taskChange(message, args, task, linked);
        }
        auto files = tool == "send_to_task" ? attachments.selectForTask(message, args) : std::vector<TaskAttachmentRef>{};
        const bool conflicting = taskOperations.acceptInstruction(task, message.id, step, instruction, sourceRef(result),
                                                                  change ? &*change : nullptr, std::move(files));
        if (change && args.contains("projectId")) taskRouting.rememberT3Project(bot, change->route.environmentId, change->route.projectId);
        return {{"ok", true}, {"effect", conflicting ? "replanning" : "accepted"}, {"taskId", task.id}};
    }
    if (tool == "answer_task_question") {
        const auto taskId = identifier(args, "taskId", true);
        const auto waitId = identifier(args, "waitId", true);
        const auto task = conversationTurns.ownTask(taskId, message);
        const auto answer = text(args, "text", limits::messages::maxWaitAnswerBytes, true);
        const auto wait = waitStorage.currentWait(taskId);
        if (!wait || wait->id != waitId)
            throw ToolError("That question is no longer open",
                            {{"taskId", task.id}, {"current", conversationTurns.taskState(task, message)}});
        if (const auto error = conversationTurns.questionAnswerError(task, message, wait))
            throw ToolError(error, {{"taskId", task.id}, {"current", conversationTurns.taskState(task, message)}});
        const TaskTarget target{bot, task.id, waitId, identifier(args, "revisionId", true, 0), identifier(args, "generation", true, 0)};
        taskOperations.operate(conversationMessages.actor(message), target, "answer", {{"text", answer}}, message.id,
                               InboxKind::user_question);
        return {{"ok", true}, {"effect", "answered"}, {"taskId", task.id}};
    }
    if (tool == "record_task_blocker" || tool == "withdraw_task_question") {
        if (!permissions.allows(conversationMessages.actor(message), Action::bot_decide, bot)) throw AccessDenied{};
        auto task = conversationTurns.ownTask(identifier(args, "taskId", true), message);
        expectGeneration(task);
        active(task);
        const bool record = tool == "record_task_blocker";
        const auto reason = record ? args.at("blockerReason").get<std::string>() : std::string{};
        if (record)
            taskOperations.recordTaskBlocker(task, reason, identifier(args, "waitId", false));
        else
            taskOperations.withdrawTaskQuestion(task, identifier(args, "waitId", true));
        const auto effect = record ? (reason.empty() ? "blocker_cleared" : "blocker_recorded") : "question_withdrawn";
        return {{"ok", true}, {"effect", effect}, {"taskId", task.id}, {"current", conversationTurns.taskState(task, message)}};
    }
    if (tool == "cancel_task") {
        if (hasDestinationSelector(args)) throw ToolError("Cancel one destination in a standalone cancel_task action");
        const auto task = conversationTurns.ownTask(identifier(args, "taskId", true), message);
        expectGeneration(task);
        active(task);
        taskOperations.operate(conversationMessages.actor(message), TaskTarget::from(task, 0), "cancel", Json::object());
        const auto state = taskRecords.load(task.id).state;
        return {{"ok", true}, {"effect", state == TaskState::cancelled ? "cancelled" : "cancelling"}, {"taskId", task.id}};
    }
    if (tool == "read_result") {
        const auto result = conversationTurns.resultReference(identifier(args, "messageId", true), message);
        const auto& body = result.at("text").get_ref<const std::string&>();
        std::size_t offset = 0;
        if (args.contains("offset")) {
            if (!args["offset"].is_number_integer() || args["offset"].get<long double>() < 0)
                throw ToolError("offset must be a non-negative integer");
            offset = std::min<std::size_t>(args["offset"].get<std::size_t>(), body.size());
        }
        while (offset < body.size() && (static_cast<unsigned char>(body[offset]) & 0xc0) == 0x80) ++offset;
        const auto chunk = bounded(std::string_view(body).substr(offset), resultChunk);
        const auto next = offset + chunk.size();
        Json view = {{"ok", true},
                     {"messageId", result.at("messageId")},
                     {"taskId", result.at("taskId")},
                     {"request", bounded(field(result, "request"), 800)},
                     {"kind", result.at("kind")},
                     {"revisionId", result.at("revisionId")},
                     {"currentRevision", result.at("revisionId") == result.at("currentRevision")},
                     {"text", chunk},
                     {"offset", offset},
                     {"end", next >= body.size()},
                     {"truncatedWhenSaved", result.at("truncated")}};
        if (next < body.size()) view["nextOffset"] = next;
        return view;
    }
    if (tool == "find_tasks") {
        const auto viewTask = [&](const TaskData& task, bool fullContract) {
            const Id id = task.id;
            Json view = {{"taskId", id}, {"request", bounded(task.request, 300)}, {"environmentId", task.environmentId}};
            view.update(conversationTurns.taskState(task, message, fullContract));
            auto result = database.query("SELECT id,kind FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')"
                                         " ORDER BY id DESC LIMIT 1",
                                         {id});
            if (result.next()) view["latestResult"] = {{"messageId", result.number(0)}, {"kind", result.string(1)}};
            return view;
        };
        if (const auto id = identifier(args, "taskId", false))
            return {{"ok", true}, {"tasks", Json::array({viewTask(conversationTurns.ownTask(id, message), true)})}};

        const auto search = text(args, "query", 400, false);
        const auto before = identifier(args, "before", false);
        auto rows = database.query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND (?='' OR instr(lower(t.request),lower(?))>0)
AND (?=0 OR t.id<?) AND (?='' OR c.scope_key=?) ORDER BY t.id DESC LIMIT ?
)SQL",
                                   {bot, search, search, before, before, message.channelScope, message.channelScope, findLimit + 1});
        Json found = Json::array();
        Id last = 0;
        bool more = false;
        while (rows.next()) {
            if (static_cast<int>(found.size()) == findLimit) {
                more = true;
                break;
            }
            last = rows.number(0);
            found.push_back(viewTask(taskRecords.load(last), false));
        }
        Json response = {{"ok", true}, {"tasks", std::move(found)}};
        if (more) response["before"] = last;
        return response;
    }
    throw ToolError("Unknown tool " + tool + "; use one of the provided tools");
}

void ConversationTurns::finishTurn(const Message& message, std::string text, const Json& suggestions, ConversationReplyRoute route,
                                   const conversation::QuestionAnchor* question) {
    using enum ConversationReplyRoute;
    const bool pendingReceipt = sessionSendReceipts.persistPending(message.id);
    if (route == session_access || route == access || route == channel_access || route == bot_paused) {
        auto pending = database.query(R"SQL(
SELECT step FROM conversation_steps WHERE message_id=? AND result IS NULL
  AND json_extract(effect,'$.kind')='message_session'
  AND json_extract(effect,'$.sendState')='prepared'
)SQL",
                                      {message.id});
        std::vector<int> steps;
        while (pending.next()) steps.push_back(static_cast<int>(pending.number(0)));
        for (const auto step : steps)
            turnResults.save(message.id, step,
                             Json::array({secretStorage.redactor(message.bot)
                                              .record({{"ok", false},
                                                       {"delivery", "not_accepted"},
                                                       {"error", "The turn ended before the T3 Session message was sent."}})}));
    }
    auto woken = wokenTask(message);
    std::set<std::pair<std::string, Json>> unresolvedEvidence;
    Json actions = Json::array();
    std::set<Id> touched;
    Json artifacts = Json::array();
    auto steps = database.query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND result IS NOT NULL"
                                " ORDER BY step",
                                {message.id});
    while (steps.next()) {
        const auto calls = steps.json(0);
        const auto results = steps.json(1);
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& result = results.at(i);
            const bool ok = result.value("ok", false);
            if (woken && route == reply && i < calls.size() && !result.value("notRun", false))
                if (const auto evidence = wakeEvidenceLookup(calls.at(i))) {
                    const auto& outcome = result.contains("result") ? result.at("result") : result;
                    if (outcome.value("ok", false))
                        unresolvedEvidence.erase(*evidence);
                    else
                        unresolvedEvidence.insert(*evidence);
                }
            if (result.contains("artifact") && channelStorage.channelToolStatus(message.id, message.bot, result).value("ok", false))
                artifacts.push_back(result.at("artifact"));
            if (!ok || !result.contains("effect")) continue;
            const auto& call = calls.at(i);
            Json action = {{"tool", call.at("tool")}, {"effect", result.at("effect")}};
            if (action.at("tool") == "message_bot") {
                // The answer arrives in a later turn, where history is the only record of what was asked.
                const auto& sent = call.at("args");
                action["botId"] = sent.at("botId");
                action["text"] = bounded(secretStorage.redactor(message.bot).text(sent.at("text").get<std::string>()), 2000);
            }
            if (result.contains("taskId")) {
                action["taskId"] = result.at("taskId");
                touched.insert(result.at("taskId").get<Id>());
            }
            actions.push_back(std::move(action));
        }
    }
    if (woken) {
        const auto reason = field(message.routing.at("sessionWake"), "reason");
        const bool reviewed = route == reply && unresolvedEvidence.empty();
        const auto report = reviewed && reason == "finished" ? currentWakeReport(database, *woken) : std::nullopt;
        if (report && report->second.status == "completed") {
            taskRecords.completeWithReport(*woken, report->second.evidence.report, "Nobody asked for more work after the Session finished");
        } else {
            const auto note = !reviewed              ? "Flock could not finish reviewing this task."
                              : reason == "failed"   ? "The Session failed."
                              : reason == "finished" ? "The Session finished without a readable report for this request."
                                                     : "This task stopped before its work finished.";
            waitStorage.wait(*woken, ModelQuestionWait{}, std::string(note) + " Reply with what to do next, or cancel it.");
            taskRecords.save(*woken);
        }
    }
    if (route == error && !actions.empty()) {
        const bool ja = japanese(message.text);
        text += ja ? "\n\nこのメッセージで実行済みの操作:" : "\n\nAlready done for this message:";
        for (const auto& action : actions) {
            text += "\n- " + field(action, "effect") + ": ";
            if (action.contains("taskId")) text += requestName(taskRecords.load(action.at("taskId")));
            if (action.contains("botId")) text += field(action, "botId");
        }
    }
    std::optional<Id> linked;
    if (message.principalSource == PrincipalSource::gitWebhook)
        linked = message.routing.at("gitEvent").at("taskId").get<Id>();
    else if (touched.size() == 1)
        linked = *touched.begin();
    const auto scrub = secretStorage.redactor(message.bot);
    const ConversationReplyEntry entry{.replyToMessageId = message.id,
                                       .text = scrub.text(text),
                                       .status = effectStatus(actions),
                                       .route = route,
                                       .taskQuestionMessageId = question ? std::optional{question->messageId} : std::nullopt,
                                       .actions = std::move(actions),
                                       .suggestions = suggestions.is_array() && !suggestions.empty() ? scrub.payload(suggestions) : Json(),
                                       .artifacts = artifacts.empty() ? Json() : std::move(artifacts)};
    const auto replyId = journalStorage.journalConversationReply(linked, message.bot, entry);
    if (!message.channelScope.empty() && channelConnectors.messageAuthorized(message.id, message.bot))
        channelDeliveries.reply(message.id, replyId, entry.text, entry.suggestions.is_null() ? Json::array() : entry.suggestions);
    database.exec("UPDATE journal SET task_id=? WHERE id=?", {linked ? Json(*linked) : Json(nullptr), message.id});
    modelCalls.releaseModelCall(modelCalls.takeReservedConversationCall(message.id));
    database.exec("UPDATE conversation_turns SET status='done',reply_id=? WHERE message_id=?", {replyId, message.id});
    database.exec("DELETE FROM conversation_retries WHERE message_id=?", {message.id});
    turnResults.discardUnpersisted(message.id);
    if (pendingReceipt) sessionSendReceipts.forgetPending(message.id);
}

bool ConversationTurns::authorizeTurn(const Message& message) {
    if (!isNextTurn(message.id) || botLifecycle.botPaused(message.bot)) return false;
    if (message.principalSource == PrincipalSource::gitWebhook) {
        if (gitStorage.canReceiveGitWebhook(message.bot, message.routing.at("gitEvent"))) return true;
        modelCalls.releaseModelCall(modelCalls.takeReservedConversationCall(message.id));
        database.exec("UPDATE conversation_turns SET status='done' WHERE message_id=?", {message.id});
        database.exec("DELETE FROM conversation_retries WHERE message_id=?", {message.id});
        return false;
    }
    const auto wake = message.routing.find("sessionWake");
    const bool isWake = wake != message.routing.end();
    const bool stoppedWake =
        isWake && sessionParticipations.stopped(message.bot, {field(*wake, "environmentId"), field(*wake, "threadId")}, message.id);
    const bool usesSessionOrigin =
        message.principalSource == PrincipalSource::session || (isWake && message.routing.contains("sessionOrigin"));
    // A wake can be queued before a stop commits and reach this turn after it commits.
    if (stoppedWake || (usesSessionOrigin && !sessionParticipations.activeOrigin(message))) {
        finishTurn(message, "The T3 Session participation has ended.", nullptr, ConversationReplyRoute::session_access);
        return false;
    }
    if (message.principalSource == PrincipalSource::channel) {
        if (!message.channelScope.empty() && channelConnectors.messageAuthorized(message.id, message.bot)) return true;
    } else if (permissions.allows(conversationMessages.actor(message), Action::bot_talk, message.bot))
        return true;
    if (const auto attempt = modelCalls.takeReservedConversationCall(message.id)) modelCalls.releaseModelCall(attempt);
    if (message.principalSource == PrincipalSource::channel)
        finishTurn(message, "This channel connection is no longer authorized.", nullptr, ConversationReplyRoute::channel_access);
    else
        finishTurn(message, "You no longer have permission to message this Bot.", nullptr, ConversationReplyRoute::access);
    return false;
}

void TurnRunner::runTurn(Id id, Model& model, T3& t3, const SoulLoader& soul, const std::function<Seconds()>& clock,
                         const BotTools& settings, GitHost* gitHost, const ApplyBotChange& applyBotChange,
                         const ReconcileBotCreation& reconcileBotCreation, const IntakeResolver& intake, const PeerResolver& peers,
                         std::stop_token shutdown) {
    Message message;
    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (shutdown.stop_requested() || !conversationTurns.isNextTurn(id) ||
            database.query("SELECT 1 FROM conversation_retries WHERE message_id=? AND retry_at>?", {id, clock()}).next())
            return;
        message = conversationMessages.load(id);
        transaction.commit();
    }
    const auto refreshSetup = [&] {
        if (message.principalSource != PrincipalSource::web && message.principalSource != PrincipalSource::trusted) return;
        if (const auto live = botSetup.liveIntake(intake, message.bot, message.text)) {
            message.routing["environmentId"] = live->environmentId;
            message.routing["projectId"] = live->projectId;
            message.routing["config"] = live->config;
        }
    };
    const bool notification = message.principalSource == PrincipalSource::gitWebhook;
    const bool web = message.channelScope.empty() && !notification;
    auto tools = notification ? std::vector<std::string>{"reply", "read_tool_result"} : turnTools;
    if (gitHost && notification) {
        appendGitNotificationTools(message.routing.at("gitEvent"), tools);
    } else if (gitHost && web) {
        if (message.botOrigin)
            appendReadOnlyGitTools(tools);
        else
            tools.insert(tools.end(), gitTools().begin(), gitTools().end());
    }
    if (web) tools.emplace_back("message_bot");
    if (web) {
        tools.insert(tools.end(), webTools.begin(), webTools.end());
        tools.insert(tools.end(), botToolNames().begin(), botToolNames().end());
    }
    if (botSetup && (message.principalSource == PrincipalSource::web || message.principalSource == PrincipalSource::trusted))
        tools.emplace_back(botSetupTool);
    if (message.botOrigin) std::erase_if(tools, [&](const auto& tool) { return !allowsTurnTool(message, tool); });
    if (!message.channelScope.empty()) tools.insert(tools.end(), channelTools.begin(), channelTools.end());
    bool shouldVerifyFollowup = false;
    if (gitHost && notification) {
        const auto& event = message.routing.at("gitEvent");
        std::lock_guard lock(database.mutex);
        if (gitStorage.canReceiveGitWebhook(message.bot, event)) {
            const auto followup = gitStorage.gitPrFollowup(message, intake);
            shouldVerifyFollowup = followup && followup->status == GitStorage::GitPrFollowup::Status::available;
        }
    }
    const bool currentPr = shouldVerifyFollowup && gitStorage.gitPrReadyForFollowup(*gitHost, message);
    const auto available = [&] {
        auto offered = tools;
        if (!notification && memoryStorage.canEditFrom(message)) {
            offered.push_back("write_memory");
            offered.push_back("revert_memory_edit");
        }
        auto machines = Json::array();
        std::string_view followupStatus;
        if (notification) {
            const auto& event = message.routing.at("gitEvent");
            const auto task = taskRecords.load(event.at("taskId").get<Id>());
            if (task.actionId.empty() && !task.activeRun && task.pendingInstructions.empty() && taskRecords.externalWaitingNote(task)) {
                offered.push_back("wait_for_event");
                if (gitStorage.gitNotificationCanComplete(message)) offered.push_back("complete");
            }
            if (const auto followup = gitHost ? gitStorage.gitPrFollowup(message, intake) : std::nullopt) {
                switch (followup->status) {
                case GitStorage::GitPrFollowup::Status::available:
                    if (currentPr) offered.push_back("fix_pull_request");
                    break;
                case GitStorage::GitPrFollowup::Status::attempted: followupStatus = "already_requested"; break;
                }
            }
        } else {
            if (conversationTurns.wokenTask(message)) {
                offered.push_back("complete");
                offered.push_back("wait_for_event");
            }
            machines = localMachineStorage.availableMachines(message.bot);
            if (!machines.empty()) offered.push_back("request_local_machine_connection");
        }
        return std::tuple{std::move(offered), std::move(machines), followupStatus};
    };
    const bool ja = japanese(message.text);
    const auto finishError = [&](std::string text) {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (conversationTurns.authorizeTurn(message))
            conversationTurns.finishTurn(message, std::move(text), nullptr, ConversationReplyRoute::error);
        transaction.commit();
    };
    const auto encodeCalls = [](const std::vector<ModelCall>& calls) {
        auto saved = Json::array();
        for (const auto& call : calls) {
            Json entry{{"tool", call.tool}, {"args", call.args}};
            if (!call.reason.empty()) entry["reason"] = call.reason;
            if (!call.rejection.empty()) entry["rejection"] = call.rejection;
            saved.push_back(std::move(entry));
        }
        return saved;
    };
    const auto splitCalls = [](const std::vector<ModelCall>& calls) {
        std::vector<std::vector<ModelCall>> steps;
        const bool setupDeliversItsReply = calls.size() == 2 && calls[0].tool == botSetupTool && calls[1].tool == "reply";
        if (setupDeliversItsReply) return std::vector<std::vector<ModelCall>>{calls};
        for (const auto& indexes : tool_batch::planSteps(calls)) {
            auto& step = steps.emplace_back();
            for (const auto index : indexes) step.push_back(calls[index]);
        }
        return steps;
    };
    const auto persistModelCheckpoint = [&] {
        std::lock_guard lock(database.mutex);
        const auto entry = conversationCheckpoints.find(id);
        if (entry == conversationCheckpoints.end()) return true;
        const auto& checkpoint = entry->second;
        Transaction transaction(database.db);
        const auto* completedCalls = std::get_if<std::vector<ModelCall>>(&checkpoint.outcome);
        const bool ownsTurnQueue = conversationTurns.isNextTurn(id);
        if (!ownsTurnQueue && !completedCalls) return false;
        if (!checkpoint.usage.empty())
            database.exec("UPDATE model_attempts SET usage=? WHERE id=?", {checkpoint.usage, checkpoint.attempt});
        bool hasDecision = false;
        if (!ownsTurnQueue || conversationTurns.authorizeTurn(message)) {
            if (completedCalls) {
                const auto steps = splitCalls(*completedCalls);
                database.exec("UPDATE conversation_steps SET decision=? WHERE message_id=? AND step=? AND result IS NULL",
                              {encodeCalls(steps.front()), id, checkpoint.step});
                if (sqlite3_changes(database.db))
                    for (std::size_t later = 1; later < steps.size(); ++later)
                        database.exec("INSERT INTO conversation_steps(message_id,step,decision) VALUES(?,?,?)"
                                      " ON CONFLICT(message_id,step) DO NOTHING",
                                      {id, checkpoint.step + static_cast<int>(later), encodeCalls(steps[later])});
                database.exec("DELETE FROM conversation_retries WHERE message_id=?", {id});
                hasDecision = true;
            } else {
                const auto& error = std::get<ConversationModelFailure>(checkpoint.outcome);
                Id repair = 0;
                if (checkpoint.attempt && !error.retryFrom && repairableModelOutput(error.code) &&
                    !database.query("SELECT 1 FROM conversation_model_failures WHERE message_id=? AND repair_attempt_id IS NOT NULL", {id})
                         .next()) {
                    repair = modelCalls.recordAttempt(modelCalls.messageModelScope(id), "conversation_turn", id, 0, 0);
                    database.exec("UPDATE conversation_turns SET reserved_attempt=? WHERE message_id=?", {repair, id});
                    database.exec("DELETE FROM conversation_retries WHERE message_id=?", {id});
                }
                if (checkpoint.attempt)
                    database.exec(
                        "INSERT INTO conversation_model_failures(attempt_id,message_id,step,reason,repair_attempt_id)"
                        " VALUES(?,?,?,?,?)",
                        {checkpoint.attempt, id, checkpoint.step, modelFailureReason(error.code), repair ? Json(repair) : Json(nullptr)});
                if (!repair) {
                    if (error.retryFrom)
                        conversationTurns.deferConversation(id, *error.retryFrom);
                    else
                        conversationTurns.finishTurn(message, modelFailureReply(error.code, ja), nullptr, ConversationReplyRoute::error);
                }
            }
        }
        transaction.commit();
        conversationCheckpoints.erase(entry);
        return hasDecision;
    };
    if (!persistModelCheckpoint()) return;
    for (int step = 1; !shutdown.stop_requested(); ++step) {
        refreshSetup();
        if (!message.channelScope.empty()) channelStorage.deliverChannels(shutdown);
        std::vector<ModelCall> calls;
        std::string effectType;
        {
            std::lock_guard lock(database.mutex);
            Transaction transaction(database.db);
            if (!conversationTurns.authorizeTurn(message)) {
                transaction.commit();
                return;
            }
            auto row = database.query("SELECT decision,result,json_type(effect) FROM conversation_steps WHERE message_id=? AND step=?",
                                      {id, step});
            if (row.next()) {
                if (!row.string(1).empty()) continue;
                effectType = row.string(2);
                if (!row.string(0).empty()) {
                    const auto [offered, machines, followupStatus] = available();
                    bool rejectionChanged = false;
                    for (const auto& saved : row.json(0)) {
                        Decision call{saved.at("tool"), saved.at("args"), field(saved, "reason")};
                        if (auto rejection = saved.value("rejection", std::string{}); !rejection.empty())
                            calls.push_back({std::move(call), std::move(rejection)});
                        else {
                            auto checked = modelCalls.checkModelCall(message.bot, std::move(call), offered, notification);
                            rejectionChanged |= !checked.rejection.empty();
                            calls.push_back(std::move(checked));
                        }
                    }
                    if (rejectionChanged)
                        database.exec("UPDATE conversation_steps SET decision=? WHERE message_id=? AND step=? AND result IS NULL",
                                      {encodeCalls(calls), id, step});
                }
            }
            transaction.commit();
        }
        if (calls.empty()) {
            const auto now = clock();
            BotNotes botNotes;
            try {
                if (!notification) botNotes = memoryStorage.loadNotes(message.bot, soul, now);
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception& error) {
                const auto diagnostic =
                    Json::parse(Json(error.what()).dump(-1, ' ', false, Json::error_handler_t::replace)).get<std::string>();
                finishError(ja ? "このBotのノートを読み込めませんでした (" + diagnostic + ")。ファイルを確認してください。"
                               : "I couldn’t read this Bot’s notes (" + diagnostic + "). Check its Role and memory.");
                return;
            }
            const auto linkedDestinations = taskRouting.discoverTaskDestinations(t3, message, true);
            ModelInput input;
            {
                std::lock_guard lock(database.mutex);
                Transaction transaction(database.db);
                if (shutdown.stop_requested()) return;
                if (!conversationTurns.authorizeTurn(message)) {
                    transaction.commit();
                    return;
                }
                auto repair = database.query(R"SQL(
SELECT f.repair_attempt_id, t.reserved_attempt IS f.repair_attempt_id, f.reason,
       EXISTS (SELECT 1 FROM conversation_retries r WHERE r.message_id=f.message_id)
FROM conversation_model_failures f JOIN conversation_turns t USING(message_id)
WHERE f.message_id=? AND f.step=? AND f.repair_attempt_id IS NOT NULL
)SQL",
                                             {id, step});
                std::string feedback;
                if (repair.next()) {
                    if (!repair.number(1) && !repair.number(3)) {
                        // A previous worker claimed the repair, but no result survived.
                        database.exec("INSERT INTO conversation_model_failures(attempt_id,message_id,step,reason) VALUES(?,?,?,?)",
                                      {repair.number(0), id, step, "repair_interrupted"});
                        conversationTurns.finishTurn(message, modelFailureReply(ModelErrorCode::inference_failed, ja), nullptr,
                                                     ConversationReplyRoute::error);
                        transaction.commit();
                        return;
                    }
                    feedback = "The previous model response was rejected before executing any of its tools. "
                               "Reason: " +
                               repair.string(2) +
                               ". Return offered tools with valid arguments. "
                               "Reply only after you have seen the results it describes.";
                }
                auto [offered, machines, followupStatus] = available();
                std::string learningRunId;
                if (const auto woken = conversationTurns.wokenTask(message))
                    if (const auto source = wokenLearningSource(*woken)) learningRunId = source->runId;
                std::vector<ModelImage> images;
                auto context = turnInput(message, step, now, std::move(machines), images, linkedDestinations);
                if (!followupStatus.empty()) context["event"]["gitFollowupStatus"] = followupStatus;
                if (peers) {
                    auto bots = Json::array();
                    const auto actor = conversationMessages.actor(message);
                    for (const auto& peer : peers(message.bot)) {
                        if (!permissions.allows(actor, Action::bot_decide, peer.intake.botId)) continue;
                        Json bot{{"botId", peer.intake.botId}, {"name", peer.name}};
                        if (!peer.role.empty()) bot["role"] = peer.role;
                        bots.push_back(std::move(bot));
                    }
                    if (!bots.empty()) context["task"]["bots"] = std::move(bots);
                }
                if (std::ranges::find(offered, "write_memory") != offered.end()) context["event"]["memoryVersion"] = botNotes.memoryVersion;
                input = {.task = context.at("task"),
                         .event = context.at("event"),
                         .history = context.at("history"),
                         .notes = std::move(botNotes),
                         .tools = std::move(offered),
                         .feedback = std::move(feedback),
                         .config = message.routing.at("config"),
                         .images = std::move(images),
                         .learningRunId = learningRunId,
                         .cacheScope = Json::array({"conversation", message.bot, message.channelScope}).dump(),
                         .gitReadOnly = message.botOrigin};
                transaction.commit();
            }
            ConversationModelCheckpoint checkpoint{0, step, Json::object(), ConversationModelFailure{{}, ModelErrorCode::inference_failed}};
            try {
                sessionReadStorage.authorizeSessionHistory(message, input.event.at("turn"), t3);
                conversationTools.authorizeBotToolHistory(message.bot, id, input);
                {
                    std::lock_guard lock(database.mutex);
                    Transaction transaction(database.db);
                    if (shutdown.stop_requested()) return;
                    if (!conversationTurns.authorizeTurn(message)) {
                        transaction.commit();
                        return;
                    }
                    auto& attempt = checkpoint.attempt;
                    attempt = modelCalls.takeReservedConversationCall(id);
                    if (!attempt) attempt = modelCalls.recordAttempt(modelCalls.messageModelScope(id), "conversation_turn", id, 0, 0);
                    database.exec("INSERT INTO conversation_steps(message_id,step,attempt_id) VALUES(?,?,?)"
                                  " ON CONFLICT(message_id,step) DO UPDATE SET attempt_id=excluded.attempt_id",
                                  {id, step, attempt});
                    transaction.commit();
                }
                auto response = modelCalls.callModel(message.bot, model, input);
                checkpoint.usage = std::move(response.usage);
                calls = std::move(response.calls);
                if (calls.size() == 2 && calls[0].tool == "reply" && calls[1].tool == botSetupTool) std::swap(calls[0], calls[1]);
                if (calls.empty() || calls.size() > maxToolCalls)
                    throw ModelError("Invalid Flock call count", ModelErrorCode::invalid_tool_count);
                checkpoint.outcome = calls;
            } catch (const ModelError& error) {
                if (!error.usage.empty()) checkpoint.usage = error.usage;
                checkpoint.outcome = ConversationModelFailure{error.transient ? std::optional<Seconds>{clock()} : std::nullopt, error.code};
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception&) {
            }
            {
                std::lock_guard lock(database.mutex);
                conversationCheckpoints.insert_or_assign(id, std::move(checkpoint));
            }
            if (!persistModelCheckpoint()) return;
            calls = std::move(splitCalls(calls).front());
        }

        if (shutdown.stop_requested()) return;
        const auto outcome = executeConversationStep(message, step, calls, effectType, settings,
                                                     {.t3 = t3,
                                                      .clock = clock,
                                                      .gitHost = gitHost,
                                                      .applyBotChange = applyBotChange,
                                                      .reconcileBotCreation = reconcileBotCreation,
                                                      .intake = intake,
                                                      .peers = peers,
                                                      .shutdown = shutdown});
        if (outcome != CallOutcome::advance) return;
    }
}
}
