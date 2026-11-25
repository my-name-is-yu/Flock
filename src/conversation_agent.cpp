#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace flockv0 {
using conversation_storage::integer;
using conversation_storage::identifier;
using conversation_storage::japanese;
using conversation_storage::text;
using conversation_storage::ToolError;
using conversation_storage::messageSize;

namespace {
using conversation::Message;
using conversation::PrincipalSource;

const std::vector<std::string> channelTools{"channel_edit_message", "channel_react", "channel_send_attachment", "publish_artifact",
                                            "read_channel_attachment"};

constexpr int activeTaskLimit = 12;
constexpr int terminalTaskLimit = 8;
constexpr int findLimit = 10;
constexpr std::size_t resultChunk = 6000;
constexpr std::size_t sourceSize = 8192;

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
const std::vector<std::string> webTools{"start_workflow",       "workflow_status",        "instruct_workflow_stage",
                                        "stop_workflow_stage",  "stop_workflow",          "compact_conversation",
                                        "expand_context",       "continue_fresh_session", "list_secrets",
                                        "request_secret_input", "list_connections",       "check_environment_readiness",
                                        "update_bot_settings",  "request_bot_change",     "read_memory",
                                        "propose_memory_edit",  "apply_memory_edit",      "revert_memory_edit",
                                        "memory_history",       "create_schedule",        "list_schedules",
                                        "remove_schedule",      "create_reminder",        "create_trigger"};

std::string_view modelFailureReason(ModelErrorCode code) {
    switch (code) {
    case ModelErrorCode::authentication_required: return "authentication_required";
    case ModelErrorCode::unsupported_reasoning_effort: return "unsupported_reasoning_effort";
    case ModelErrorCode::invalid_tool_count: return "invalid_tool_count";
    case ModelErrorCode::invalid_tool_output: return "invalid_tool_output";
    case ModelErrorCode::unavailable_tool: return "unavailable_tool";
    case ModelErrorCode::invalid_tool_arguments: return "invalid_tool_arguments";
    case ModelErrorCode::inference_failed: return "inference_failed";
    }
    return "inference_failed";
}

bool repairableModelOutput(ModelErrorCode code) {
    return code == ModelErrorCode::invalid_tool_count || code == ModelErrorCode::invalid_tool_output ||
           code == ModelErrorCode::unavailable_tool || code == ModelErrorCode::invalid_tool_arguments;
}

std::string modelFailureReply(ModelErrorCode code, bool ja) {
    if (code == ModelErrorCode::authentication_required)
        return ja ? "ChatGPTへのサインインが必要です。Flockの設定でサインインしてから、もう一度メッセージを送ってください。"
                  : "This Flock needs a ChatGPT sign-in. Open Flock Settings, sign in to ChatGPT, then send your message again.";
    if (code == ModelErrorCode::unsupported_reasoning_effort)
        return ja ? "このモデルは選択された推論レベルに対応していません。Botの設定で別のレベルか「Model default」を選んでください。"
                  : "This model doesn’t support the selected reasoning effort. Choose another effort or Model default in Bot settings.";
    return ja ? "このメッセージを処理できませんでした。もう一度送ってください。" : "I couldn't process this message. Please send it again.";
}

Json waitView(const Json& wait) {
    if (!wait.is_object()) return nullptr;
    Json view = {{"waitId", wait.value("id", Id{0})}, {"reason", field(wait, "reason")}, {"text", bounded(field(wait, "text"), 1000)}};
    if (const auto choices = wait.find("choices"); choices != wait.end() && choices->is_array()) {
        auto& list = view["choices"] = Json::array();
        for (const auto& choice : *choices)
            if (choice.is_string() && list.size() < 12) list.push_back(bounded(choice.get<std::string>(), 200));
    }
    return view;
}

std::string requestName(const Json& task) {
    const auto request = field(task, "request");
    return bounded(request.substr(0, request.find('\n')), 160);
}

std::string effectStatus(const Json& actions) {
    std::string status = "done";
    for (const auto& action : actions) {
        const auto effect = field(action, "effect");
        if (effect == "replanning" || effect == "cancelling" || effect == "interrupt_requested") return "stopping";
        if (effect == "started" || effect == "accepted" || effect == "answered" || effect == "delivered") status = "working";
    }
    return status;
}
}

Message Store::Impl::loadTurnMessage(Id id) const {
    auto row = query(R"SQL(
SELECT m.reply_to,m.bot_id,m.text,c.scope_key,m.routing,m.person_id,m.person_root,m.principal_source FROM messages m
LEFT JOIN channel_messages c ON c.message_id=m.id WHERE m.id=?
)SQL",
                     {id});
    if (!row.next()) throw Conflict("Conversation message is missing");
    Message message{id,
                    row.number(0),
                    row.string(1),
                    row.string(2),
                    row.string(3),
                    row.json(4),
                    {row.string(5), row.number(6) != 0},
                    static_cast<PrincipalSource>(row.number(7)),
                    std::nullopt};
    if (!message.replyTo || message.replyTo >= id) return message;
    auto anchor = query(R"SQL(
SELECT q.id,q.task_id FROM journal target
LEFT JOIN conversation_turns turn ON turn.reply_id=target.id
LEFT JOIN messages original ON original.id=turn.message_id
LEFT JOIN channel_messages source ON source.message_id=original.id
JOIN journal q ON q.id=CASE
  WHEN target.kind='waiting' THEN target.id
  WHEN target.kind='conversation_reply' AND original.bot_id=? AND (?='' OR source.scope_key=?)
    THEN json_extract(target.data,'$.taskQuestionMessageId')
END
JOIN tasks t ON t.id=q.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE target.id=? AND q.id<=target.id AND q.kind='waiting' AND json_extract(q.data,'$.reason')='user_question'
  AND t.bot_id=? AND (?='' OR c.scope_key=?)
)SQL",
                        {message.bot, message.channelScope, message.channelScope, message.replyTo, message.bot, message.channelScope,
                         message.channelScope});
    if (anchor.next()) message.question = conversation::QuestionAnchor{anchor.number(0), anchor.number(1)};
    return message;
}

const char* Store::Impl::questionAnswerError(const Json& task, const Message& message) const {
    const auto& wait = task.at("wait");
    if (task.at("state") != "waiting" || !wait.is_object()) return "That question is no longer open";
    if (field(wait, "reason") != "user_question") return "Only user_question waits can be answered; approvals use their own controls";
    const auto asked = integer(wait, "messageId");
    if (asked >= message.id) return "That question opened after this message was sent, so this message cannot answer it";
    if (message.replyTo && (!message.question || message.question->messageId != asked))
        return "This message replies to something else, so it can only answer the question it replies to";
    if (!asked || wait.value("run", Json(nullptr)) != task.value("activeRun", Json(nullptr)) ||
        integer(wait, "eventId") != task.value("eventId", Id{0}))
        return "That question is no longer current";
    if (replanReady(task)) return "The task is replanning, so this question will close. Its Bot asks again if it still needs to know";
    return nullptr;
}

Json Store::Impl::taskState(const Json& task, const Message& message, bool fullContract) const {
    auto wait = waitView(task.at("wait"));
    if (wait.is_object()) wait["answerable"] = !questionAnswerError(task, message);
    Json view = {{"generation", task.at("generation")},
                 {"revisionId", task.value("currentRevision", Id{0})},
                 {"state", task.at("state")},
                 {"replanning", task.value("replanning", false)},
                 {"wait", std::move(wait)}};
    if (task.value("queuedInT3", false)) view["queuedInT3"] = true;
    if (task.contains("runTargets") || task.contains("runResults")) {
        if (task.contains("requestedCommit")) view["requestedCommit"] = task.at("requestedCommit");
        if (task.contains("runOutcome")) view["runOutcome"] = task.at("runOutcome");
        auto& results = view["runResults"] = Json::array();
        if (task.contains("runResults")) {
            for (const auto& run : task.at("runResults")) {
                Json summary = Json::object();
                for (const auto* key :
                     {"name", "environmentId", "projectId", "status", "commit", "threadId", "runId", "link", "evidenceStatus"})
                    if (run.contains(key)) summary[key] = run.at(key);
                if (fullContract && run.contains("report")) summary["report"] = bounded(field(run, "report"), 2000);
                if (run.contains("detail")) summary["detail"] = bounded(field(run, "detail"), 320);
                results.push_back(std::move(summary));
            }
        } else {
            for (const auto& target : task.at("runTargets")) {
                auto summary = target;
                summary["status"] = task.at("state") == "cancelled" ? "cancelled" : "queued";
                results.push_back(std::move(summary));
            }
        }
    }
    const auto taskOrigin = task.value("sessionOrigin", Json::object());
    const auto turnOrigin = message.routing.value("sessionOrigin", Json::object());
    if (!taskOrigin.empty() && !turnOrigin.empty() && taskOrigin.at("environmentId") == turnOrigin.at("environmentId") &&
        taskOrigin.at("threadId") == turnOrigin.at("threadId")) {
        const auto& run = task.at("activeRun").is_object() ? task.at("activeRun") : task.at("lastRun");
        if (run.is_object() && !task.contains("selectedSession"))
            view["session"] = {{"environmentId", run.at("environmentId")}, {"threadId", run.at("threadId")}};
        if (task.contains("launchWarning")) view["launchWarning"] = task.at("launchWarning");
    }
    const auto project = [&](const char* key, const std::string& value, std::size_t limit) {
        view[key] = fullContract ? value : bounded(value, limit);
        if (view[key].get_ref<const std::string&>().size() < value.size()) view[std::string(key) + "Truncated"] = true;
    };
    project("doneWhen", effectiveDoneWhen(task), 1600);
    if (task.contains("requestedDoneWhen")) {
        view["doneWhenSource"] = "requested";
        if (task.contains("criteriaSourceMessageId")) view["criteriaSourceMessageId"] = task.at("criteriaSourceMessageId");
    }
    if (task.contains("workflowId")) {
        view["workflowId"] = task.at("workflowId");
        view["stageId"] = task.at("stageId");
    }
    if (const auto reason = field(task, "blockerReason"); !reason.empty()) view["blockerReason"] = reason;
    if (const auto intent = taskIntent(task); !intent.empty()) project("intent", intent, 1000);
    return view;
}

Json Store::Impl::ownTask(Id taskId, const Message& message) const {
    auto row = query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.id=? AND t.bot_id=? AND (?='' OR c.scope_key=?)
)SQL",
                     {taskId, message.bot, message.channelScope, message.channelScope});
    if (row.next()) return load(taskId);
    throw ToolError("No task with taskId " + std::to_string(taskId));
}

Json Store::Impl::resultReference(Id messageId, const Message& message) const {
    auto row = query(R"SQL(
SELECT t.id,t.state,j.kind,j.data,json_extract(t.data,'$.request'),coalesce(json_extract(t.data,'$.currentRevision'),0)
FROM journal j JOIN tasks t ON t.id=j.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE j.id=? AND t.bot_id=? AND j.kind IN ('report','completed','cancelled') AND (?='' OR c.scope_key=?)
)SQL",
                     {messageId, message.bot, message.channelScope, message.channelScope});
    if (!row.next()) throw ToolError("No result with messageId " + std::to_string(messageId));
    const auto kind = row.string(2);
    const auto data = row.json(3);
    Json provenance = nullptr;
    if (kind == "report") {
        provenance = {{"environmentId", field(data, "environmentId")},
                      {"threadId", data.contains("run") ? field(data["run"], "threadId") : ""},
                      {"runId", field(data, "resultRunId")},
                      {"itemId", field(data, "itemId")}};
    } else if (data.contains("source"))
        provenance = data.at("source");
    Json result = {{"taskId", row.number(0)},
                   {"messageId", messageId},
                   {"revisionId", integer(data, "revisionId")},
                   {"text", field(data, kind == "report" ? "report" : "text")},
                   {"kind", kind},
                   {"taskState", row.string(1)},
                   {"request", row.string(4)},
                   {"currentRevision", row.number(5)},
                   {"truncated", data.value("truncated", false)}};
    if (!provenance.is_null()) result["run"] = provenance;
    return redactor(message.bot).record(std::move(result));
}

namespace {
Json sourceRef(const Json& result) {
    Json source = {{"taskId", result.at("taskId")},
                   {"messageId", result.at("messageId")},
                   {"revisionId", result.at("revisionId")},
                   {"text", bounded(field(result, "text"), sourceSize)}};
    if (result.contains("run")) source["run"] = result.at("run");
    return source;
}
}

Json Store::Impl::conversationSteps(const Message& message, int step, std::vector<ModelImage>& images) {
    Json turn = Json::array();
    auto steps = query("SELECT decision,result,step FROM conversation_steps WHERE message_id=? AND step<?"
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
            auto result = channelToolStatus(message.id, message.bot, results.at(i), &images);
            if (args.is_object() && args.contains("docId")) {
                const bool reading = decision.at("tool") == "read_memory" || decision.at("tool") == "memory_history";
                const auto action = reading ? Action::shared_memory_read : Action::shared_memory_write;
                const auto document = field(args, "docId");
                const auto policy = Policy(*this);
                if (!policy.allows(turnActor(message), action, document) || policy.forbidden(message.bot, action, document)) {
                    args = Json::object();
                    result = {{"ok", false}, {"error", "Memory access denied"}};
                } else {
                    const auto shared = sharedRedactor();
                    args = shared.record(std::move(args));
                    result = shared.record(std::move(result));
                }
            }
            const auto tool = field(decision, "tool");
            if ((decision.contains("rejection") && result.value("untrustedData", false)) ||
                modelArgumentsContainProtected(
                    modelCallRedactor(message.bot, tool, args, message.principalSource == PrincipalSource::gitWebhook), tool, args)) {
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

Json Store::Impl::turnInput(const Message& message, int step, Seconds now, Json machines, std::vector<ModelImage>& images,
                            const LinkedTaskDestinations& linked) {
    if (message.principalSource == PrincipalSource::gitWebhook) {
        auto event = Json{{"kind", "conversation_turn"},
                          {"messageId", message.id},
                          {"text", message.text},
                          {"currentTime", now},
                          {"replyTo", nullptr},
                          {"turn", conversationSteps(message, step, images)},
                          {"gitEvent", sharedRedactor().record(message.routing.at("gitEvent"))}};
        return {{"task", Json::object()}, {"event", std::move(event)}, {"history", Json::array()}};
    }
    const auto scrub = redactor(message.bot);
    Json tasks = Json::array();
    std::set<Id> listed;
    const bool web = message.channelScope.empty();
    const auto summaries = web ? compactions(message.bot, 0, 1, message.id) : Json::array();
    const Id boundary = summaries.empty() ? 0 : summaries.front().at("endMessageId").get<Id>();
    const auto latestResult = [&](Id taskId) -> Json {
        auto row = query(R"SQL(
SELECT id,kind,data FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')
ORDER BY id DESC LIMIT 1
)SQL",
                         {taskId});
        if (!row.next()) return nullptr;
        const auto kind = row.string(1);
        const auto data = row.json(2);
        Json result = {{"messageId", row.number(0)}, {"kind", kind}, {"revisionId", integer(data, "revisionId")}};
        if (row.number(0) > boundary) result["excerpt"] = bounded(scrub.text(field(data, kind == "report" ? "report" : "text")), 600);
        return result;
    };
    const auto addTask = [&](Id taskId) {
        if (!listed.insert(taskId).second) return;
        const auto task = load(taskId);
        Json view = {
            {"taskId", taskId}, {"request", bounded(field(task, "request"), 800)}, {"environmentId", field(task, "environmentId")}};
        view.update(taskState(task, message));
        if (!localMachines.empty() && task.value("activeRun", Json()).is_object())
            view["activeSession"] = {{"threadId", field(task.at("activeRun"), "threadId")},
                                     {"runId", field(task.at("activeRun"), "runId")}};
        if (task.contains("sourceTaskId")) view["sourceTaskId"] = task.at("sourceTaskId");
        if (auto result = latestResult(taskId); !result.is_null()) view["latestResult"] = std::move(result);
        tasks.push_back(std::move(view));
    };
    bool moreTasks = false;
    {
        auto rows = query(R"SQL(
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
        auto rows = query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND t.state IN ('completed','cancelled') AND (?='' OR c.scope_key=?)
ORDER BY t.id DESC LIMIT ?
)SQL",
                          {message.bot, message.channelScope, message.channelScope, terminalTaskLimit});
        while (rows.next()) addTask(rows.number(0));
    }

    Json replyTo = nullptr;
    if (message.replyTo) {
        auto row = query(R"SQL(
SELECT j.kind,j.task_id,j.data,m.text FROM journal j LEFT JOIN tasks t ON t.id=j.task_id
LEFT JOIN messages m ON m.id=j.id
WHERE j.id=? AND j.id<? AND (j.conversation_bot_id=? OR t.bot_id=?)
)SQL",
                         {message.replyTo, message.id, message.bot, message.bot});
        if (row.next()) {
            const auto kind = row.string(0);
            const Id taskId = row.number(1);
            const auto data = row.json(2);
            replyTo = {{"messageId", message.replyTo}};
            if (kind == "waiting") {
                const auto task = load(taskId);
                const auto& current = task.at("wait");
                replyTo.update(
                    {{"kind", "task_question"},
                     {"taskId", taskId},
                     {"waitId", integer(data, "id")},
                     {"reason", field(data, "reason")},
                     {"text", bounded(field(data, "text"), 1000)},
                     {"revisionId", task.value("currentRevision", Id{0})},
                     {"generation", task.at("generation")},
                     {"current", current.is_object() && integer(current, "id") == integer(data, "id") && waitIsActionable(task)}});
                if (const auto question = waitView(data); question.contains("choices")) replyTo["choices"] = question.at("choices");
                addTask(taskId);
            } else if (kind == "report" || kind == "completed" || kind == "cancelled") {
                replyTo.update(
                    {{"kind", "result"}, {"taskId", taskId}, {"text", bounded(field(data, kind == "report" ? "report" : "text"), 2000)}});
                addTask(taskId);
            } else if (kind == "pr_review_requested") {
                replyTo.update({{"kind", "pr_review"},
                                {"taskId", taskId},
                                {"itemId", integer(data, "itemId")},
                                {"title", scrub.text(field(data, "title"))},
                                {"text", bounded(scrub.text(field(data, "text")), 2000)},
                                {"prUrl", scrub.text(field(data, "prUrl"))}});
                addTask(taskId);
            } else if (kind == "conversation_reply") {
                replyTo.update({{"kind", "bot_message"}, {"text", bounded(field(data, "text"), 2000)}});
                if (data.contains("suggestions")) replyTo["suggestions"] = data.at("suggestions");
                if (message.question) addTask(message.question->taskId);
                auto original = query("SELECT id,text,reply_to FROM messages WHERE id=? AND bot_id=?",
                                      {integer(data, "replyToMessageId"), message.bot});
                if (original.next()) {
                    replyTo["inReplyTo"] = {{"messageId", original.number(0)}, {"text", bounded(original.string(1), messageSize)}};
                    if (original.number(2)) replyTo["inReplyTo"]["replyToMessageId"] = original.number(2);
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
    const auto initial = taskEnvironment(message.bot, message.routing);
    if (!initial.empty()) environments.push_back({{"name", initial}, {"environmentId", initial}, {"default", true}});
    if (field(message.routing, "environmentId").empty())
        for (const auto& environment : registeredT3Environments(message.bot))
            if (environment != initial) environments.push_back({{"name", environment}, {"environmentId", environment}});
    const auto& config = message.routing.at("config");
    if (const auto aliases = config.find("conversationEnvironments"); aliases != config.end() && aliases->is_object()) {
        for (auto alias = aliases->begin(); alias != aliases->end(); ++alias) {
            const auto id = taskEnvironment(message.bot, message.routing, alias.key());
            if (!id.empty()) {
                auto choice = Json{{"name", alias.key()}, {"environmentId", id}};
                const auto gateway = conversationGatewayEnvironmentId(alias.value());
                if (!gateway.empty()) choice["gatewayEnvironmentId"] = gateway;
                environments.push_back(std::move(choice));
            }
        }
    }
    for (const auto& linkedChoice : linked.choices) {
        environments.push_back({{"name", scrub.text(linkedChoice.name)},
                                {"environmentId", linkedChoice.route.environmentId},
                                {"projectId", linkedChoice.route.projectId},
                                {"gatewayEnvironmentId", linkedChoice.route.gatewayEnvironmentId},
                                {"linked", true}});
    }

    const auto channel = channelContext(message.id, message.bot, &images);
    auto turn = conversationSteps(message, step, images);
    const auto expandedBytes = conversation_storage::boundExpandedContext(turn);
    auto history = conversationHistory(message, summaries, expandedBytes);
    Json event{{"kind", "conversation_turn"}, {"messageId", message.id},       {"text", message.text},
               {"currentTime", now},          {"replyTo", std::move(replyTo)}, {"turn", std::move(turn)}};
    if (message.routing.contains("sessionOrigin")) event["sessionOrigin"] = message.routing.at("sessionOrigin");
    for (const auto* key : {"sessionWake", "sessionInvitation"})
        if (message.routing.contains(key)) event[key] = message.routing.at(key);
    event.update(channel);
    if (web) event["attachments"] = attachmentViews(message.id);
    Json task{{"tasks", std::move(tasks)},
              {"moreTasks", moreTasks},
              {"environments", std::move(environments)},
              {"localMachines", std::move(machines)}};
    if (!linked.unavailable.empty()) task["linkedEnvironmentIssues"] = linked.unavailable;
    if (web) {
        auto& workflows = task["workflows"] = Json::array();
        auto recent = query("SELECT id,CASE WHEN message_id>? THEN request ELSE '' END FROM workflows "
                            "WHERE bot_id=? AND message_id<=? ORDER BY id DESC LIMIT 12",
                            {boundary, message.bot, message.id});
        while (recent.next())
            workflows.push_back({{"workflowId", recent.number(0)}, {"request", bounded(scrub.text(recent.string(1)), 300)}});
    }
    return {{"task", std::move(task)}, {"event", std::move(event)}, {"history", std::move(history)}};
}

Json Store::Impl::applyTurnTool(const Message& message, int step, const Decision& decision, Seconds now, const WorkSession* selectedSession,
                                const LinkedTaskDestinations& linked) {
    const auto& args = decision.args;
    if (!args.is_object()) throw ToolError("Tool arguments must be an object");
    const auto& tool = decision.tool;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    if (tool == "review_preference") {
        if (step != 1 || !canReviewPreference(message)) throw ToolError("Automatic memory is unavailable");
        return {{"ok", true}};
    }
    if (tool == "remember_preference") return rememberPreference(message, step, args, now);
    if (tool == "skip_preference") return {{"ok", true}};
    if (tool == "read_tool_result") {
        const auto sourceMessage = identifier(args, "messageId", true);
        const auto sourceStep = identifier(args, "step", true);
        const auto index = identifier(args, "callIndex", true);
        if (sourceMessage != message.id || sourceStep >= step || index > static_cast<Id>(maxToolCalls))
            throw ToolError("Unknown tool result in this turn");
        auto saved = query("SELECT json_extract(decision,?) FROM conversation_steps "
                           "WHERE message_id=? AND step=? AND result IS NOT NULL",
                           {"$[" + std::to_string(index - 1) + "].tool", message.id, sourceStep});
        if (!saved.next() || !conversation_storage::canRereadToolResult(saved.string(0)))
            throw ToolError("Unknown tool result in this turn");
        return {{"ok", true}};
    }
    if (auto result = channelTool(message.id, bot, decision); !result.is_null()) return result;
    if (auto result = localMachineTool(bot, message.channelScope, decision); !result.is_null()) return result;
    if (std::ranges::find(webTools, tool) != webTools.end()) {
        if (!message.channelScope.empty()) throw ToolError("Use Flock Web for this action");
        const auto freshTask = tool == "continue_fresh_session" ? ownTask(identifier(args, "taskId", true), message) : Json{};
        if (tool == "continue_fresh_session" && (freshTask.contains("runTargets") || freshTask.contains("runResults")))
            throw ToolError("Use a new request to run more work across T3 environments");
        if (tool == "compact_conversation" || tool == "expand_context" || tool == "continue_fresh_session")
            return contextTool(tool, args, message, step);
        if (tool == "start_workflow" || tool == "workflow_status" || tool == "instruct_workflow_stage" || tool == "stop_workflow_stage" ||
            tool == "stop_workflow")
            return workflowTool(tool, args, bot, message.id, step, routing);
        const auto actor = turnActor(message);
        auto result = botSettingsTool(bot, actor, message.id, routing, decision);
        if (result.is_null()) result = memory(bot, actor, tool, args, now, true);
        if (result.is_null()) result = automationTool(bot, actor, routing, decision, now);
        if (result.is_null()) throw ToolError("Unknown conversation tool");
        result["ok"] = true;
        return result;
    }
    const auto expectGeneration = [&](const Json& task) {
        const auto expected = identifier(args, "generation", true, 0);
        if (expected != task.at("generation").get<Id>())
            throw ToolError("stale", {{"applied", 0},
                                      {"taskId", task.at("id")},
                                      {"expected", {{"generation", expected}}},
                                      {"current", taskState(task, message)}});
    };
    const auto active = [&](const Json& task) {
        if (terminal(task) || task.at("state") == "cancelling")
            throw ToolError("The task is " + task.at("state").get<std::string>(),
                            {{"taskId", task.at("id")}, {"current", taskState(task, message)}});
    };
    const auto source = [&](const Json* target) -> Json {
        const auto sourceId = identifier(args, "sourceResultMessageId", false);
        if (!sourceId) return nullptr;
        const auto result = resultReference(sourceId, message);
        if (target && (result.at("taskId") != target->at("id") || result.at("revisionId") != target->value("currentRevision", Id{0})))
            throw ToolError("sourceResultMessageId must be a result of this task's current revision");
        return result;
    };

    if (tool == "start_task") {
        const auto request = text(args, "request", messageSize, true);
        const auto doneWhen = text(args, "doneWhen", 8000, false);
        const bool resume = args.value("resume", false);
        auto [destination, targets, commit] = startDestinations(message, args, resume, selectedSession, linked);
        const auto result = source(nullptr);
        if (resume && (result.is_null() || result.at("taskState") != "cancelled"))
            throw ToolError("resume requires sourceResultMessageId from a cancelled task");
        const auto id = startConversationTask(routing, bot, request, destination, message.id,
                                              result.is_null() ? Json(nullptr) : sourceRef(result), resume, selectedSession);
        if (!doneWhen.empty() || !targets.empty()) {
            auto task = load(id);
            if (!doneWhen.empty()) {
                task["requestedDoneWhen"] = doneWhen;
                task["criteriaSourceMessageId"] = message.id;
            }
            if (!targets.empty()) task["runTargets"] = std::move(targets);
            if (!commit.empty()) task["requestedCommit"] = std::move(commit);
            save(task);
        }
        Json receipt{{"ok", true}, {"effect", "started"}, {"taskId", id}};
        if (selectedSession) receipt["existingSession"] = true;
        return receipt;
    }
    if (tool == "send_to_task" || tool == "change_task") {
        auto task = ownTask(identifier(args, "taskId", true), message);
        expectGeneration(task);
        active(task);
        if (tool == "change_task" && (task.contains("runTargets") || task.contains("runResults")))
            throw ToolError("Task-wide changes are not available for tasks with destination Runs yet");
        if (tool == "send_to_task" && task.contains("runTargets"))
            throw ToolError("A destination Session is unavailable until its T3 Thread is saved");
        if (tool == "send_to_task" && task.contains("runResults"))
            throw ToolError("Send this destination instruction on its own to use the saved T3 Thread");
        const auto instruction = text(args, "instruction", messageSize, true);
        const auto result = source(&task);
        std::optional<TaskChange> change;
        if (tool == "change_task") {
            const auto revision = identifier(args, "revisionId", true, 0);
            if (revision != task.value("currentRevision", Id{0}))
                throw ToolError("stale", {{"applied", 0},
                                          {"taskId", task.at("id")},
                                          {"expected", {{"revisionId", revision}}},
                                          {"current", taskState(task, message)}});
            change = taskChange(message, args, task, linked);
        }
        const bool conflicting = acceptInstruction(task, message.id, step, instruction,
                                                   result.is_null() ? Json(nullptr) : sourceRef(result), change ? &*change : nullptr);
        return {{"ok", true}, {"effect", conflicting ? "replanning" : "accepted"}, {"taskId", task.at("id")}};
    }
    if (tool == "answer_task_question") {
        const auto taskId = identifier(args, "taskId", true);
        const auto waitId = identifier(args, "waitId", true);
        const auto task = ownTask(taskId, message);
        const auto answer = text(args, "text", 8000, true);
        if (integer(task.at("wait"), "id") != waitId)
            throw ToolError("That question is no longer open", {{"taskId", task.at("id")}, {"current", taskState(task, message)}});
        if (const auto error = questionAnswerError(task, message))
            throw ToolError(error, {{"taskId", task.at("id")}, {"current", taskState(task, message)}});
        const TaskTarget target{bot, task.at("id"), waitId, identifier(args, "revisionId", true, 0),
                                identifier(args, "generation", true, 0)};
        operate(turnActor(message), target, "answer", {{"text", answer}}, message.id, InboxKind::user_question);
        return {{"ok", true}, {"effect", "answered"}, {"taskId", task.at("id")}};
    }
    if (tool == "record_task_blocker" || tool == "withdraw_task_question") {
        if (!Policy(*this).allows(turnActor(message), Action::bot_decide, bot)) throw DecisionDenied{};
        auto task = ownTask(identifier(args, "taskId", true), message);
        expectGeneration(task);
        active(task);
        const bool record = tool == "record_task_blocker";
        const auto reason = record ? args.at("blockerReason").get<std::string>() : std::string{};
        if (record)
            recordTaskBlocker(task, reason, identifier(args, "waitId", false));
        else
            withdrawTaskQuestion(task, identifier(args, "waitId", true));
        const auto effect = record ? (reason.empty() ? "blocker_cleared" : "blocker_recorded") : "question_withdrawn";
        return {{"ok", true}, {"effect", effect}, {"taskId", task.at("id")}, {"current", taskState(task, message)}};
    }
    if (tool == "cancel_task") {
        if (hasDestinationSelector(args)) throw ToolError("Cancel one destination in a standalone cancel_task action");
        const auto task = ownTask(identifier(args, "taskId", true), message);
        expectGeneration(task);
        active(task);
        operate(turnActor(message), TaskTarget::from(task, 0), "cancel", Json::object());
        const auto state = load(task.at("id")).at("state");
        return {{"ok", true}, {"effect", state == "cancelled" ? "cancelled" : "cancelling"}, {"taskId", task.at("id")}};
    }
    if (tool == "read_result") {
        const auto result = resultReference(identifier(args, "messageId", true), message);
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
        const auto viewTask = [&](const Json& task, bool fullContract) {
            const Id id = task.at("id");
            Json view = {
                {"taskId", id}, {"request", bounded(field(task, "request"), 300)}, {"environmentId", field(task, "environmentId")}};
            view.update(taskState(task, message, fullContract));
            auto result = query("SELECT id,kind FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')"
                                " ORDER BY id DESC LIMIT 1",
                                {id});
            if (result.next()) view["latestResult"] = {{"messageId", result.number(0)}, {"kind", result.string(1)}};
            return view;
        };
        if (const auto id = identifier(args, "taskId", false))
            return {{"ok", true}, {"tasks", Json::array({viewTask(ownTask(id, message), true)})}};

        const auto search = text(args, "query", 400, false);
        const auto before = identifier(args, "before", false);
        auto rows = query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND (?='' OR instr(lower(json_extract(t.data,'$.request')),lower(?))>0)
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
            found.push_back(viewTask(load(last), false));
        }
        Json response = {{"ok", true}, {"tasks", std::move(found)}};
        if (more) response["before"] = last;
        return response;
    }
    throw ToolError("Unknown tool " + tool + "; use one of the provided tools");
}

void Store::Impl::finishTurn(const Message& message, std::string text, const Json& suggestions, std::string_view route,
                             const conversation::QuestionAnchor* question) {
    const auto receipt = sessionSendReceipts.find(message.id);
    if (receipt != sessionSendReceipts.end()) persistSessionSendReceipt(message.id, receipt->second);
    if (route == "session_access" || route == "access" || route == "channel_access" || route == "bot_paused") {
        auto pending = query(R"SQL(
SELECT step FROM conversation_steps WHERE message_id=? AND result IS NULL
  AND json_extract(effect,'$.kind')='message_session'
  AND json_extract(effect,'$.sendState')='prepared'
)SQL",
                             {message.id});
        std::vector<int> steps;
        while (pending.next()) steps.push_back(static_cast<int>(pending.number(0)));
        for (const auto step : steps)
            saveTurnResults(message.id, step,
                            Json::array({redactor(message.bot)
                                             .record({{"ok", false},
                                                      {"delivery", "not_accepted"},
                                                      {"error", "The turn ended before the T3 Session message was sent."}})}));
    }
    Json actions = Json::array();
    std::set<Id> touched;
    Json artifacts = Json::array();
    auto steps = query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND result IS NOT NULL"
                       " ORDER BY step",
                       {message.id});
    while (steps.next()) {
        const auto calls = steps.json(0);
        const auto results = steps.json(1);
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& result = results.at(i);
            if (result.contains("artifact") && channelToolStatus(message.id, message.bot, result).value("ok", false))
                artifacts.push_back(result.at("artifact"));
            if (!result.value("ok", false) || !result.contains("effect")) continue;
            Json action = {{"tool", calls.at(i).at("tool")}, {"effect", result.at("effect")}};
            if (result.contains("taskId")) {
                action["taskId"] = result.at("taskId");
                touched.insert(result.at("taskId").get<Id>());
            }
            for (const auto* key : {"workflowId", "stageId"})
                if (result.contains(key)) action[key] = result.at(key);
            actions.push_back(std::move(action));
        }
    }
    if (route == "error" && !actions.empty()) {
        const bool ja = japanese(message.text);
        text += ja ? "\n\nこのメッセージで実行済みの操作:" : "\n\nAlready done for this message:";
        for (const auto& action : actions) {
            text += "\n- " + field(action, "effect") + ": ";
            if (action.contains("taskId"))
                text += requestName(load(action.at("taskId")));
            else if (action.contains("workflowId")) {
                auto workflow = query("SELECT request FROM workflows WHERE id=?", {action.at("workflowId")});
                workflow.next();
                text += bounded(workflow.string(0), 160);
            }
        }
    }
    const auto status = effectStatus(actions);
    const Json linked = message.principalSource == PrincipalSource::gitWebhook ? message.routing.at("gitEvent").at("taskId")
                        : touched.size() == 1                                  ? Json(*touched.begin())
                                                                               : Json(nullptr);
    exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {linked, unixTime(), message.bot});
    const auto reply = last();
    Json data = {{"messageId", reply},
                 {"replyToMessageId", message.id},
                 {"text", redactor(message.bot).text(text)},
                 {"route", route},
                 {"status", status},
                 {"actions", actions}};
    if (suggestions.is_array() && !suggestions.empty()) data["suggestions"] = redactor(message.bot).payload(suggestions);
    if (question) data["taskQuestionMessageId"] = question->messageId;
    if (!artifacts.empty()) data["artifacts"] = std::move(artifacts);
    if (!message.channelScope.empty() && channelMessageAuthorized(message.id, message.bot)) queueChannelReply(message.id, reply, data);
    exec("UPDATE journal SET data=? WHERE id=?", {data, reply});
    exec("UPDATE journal SET task_id=? WHERE id=?", {linked, message.id});
    releaseModelCall(takeReservedConversationCall(message.id));
    exec("UPDATE conversation_turns SET status='done',reply_id=? WHERE message_id=?", {reply, message.id});
    exec("DELETE FROM conversation_retries WHERE message_id=?", {message.id});
    std::erase_if(unpersistedToolResults, [&](const auto& entry) { return entry.first.first == message.id; });
    if (receipt != sessionSendReceipts.end()) sessionSendReceipts.erase(receipt);
}

Person Store::Impl::turnActor(const Message& message) {
    const auto& admitted = message.admittedActor;
    switch (message.principalSource) {
    case PrincipalSource::trusted: return admitted;
    case PrincipalSource::channel: return {admitted.id, false};
    case PrincipalSource::gitWebhook: return {};
    case PrincipalSource::session: {
        try {
            return sessionActor(admitted.id);
        } catch (const std::exception&) {
            return {};
        }
    }
    case PrincipalSource::web: {
        if (!resolveWebIdentity) return {};
        try {
            if (const auto root = resolveWebIdentity(admitted.id)) return {admitted.id, *root};
        } catch (const std::exception&) {
            return {};
        }
        return {};
    }
    }
    return {};
}

bool Store::Impl::authorizeTurn(const Message& message) {
    if (!isNextTurn(message.id) || botPaused(message.bot)) return false;
    if (message.principalSource == PrincipalSource::gitWebhook) {
        if (canReceiveGitWebhook(message.bot, message.routing.at("gitEvent"))) return true;
        releaseModelCall(takeReservedConversationCall(message.id));
        exec("UPDATE conversation_turns SET status='done' WHERE message_id=?", {message.id});
        exec("DELETE FROM conversation_retries WHERE message_id=?", {message.id});
        return false;
    }
    const auto wake = message.routing.find("sessionWake");
    const bool isWake = wake != message.routing.end();
    const bool stoppedWake =
        isWake && sessionParticipationStopped(message.bot, {field(*wake, "environmentId"), field(*wake, "threadId")}, message.id);
    const bool usesSessionOrigin =
        message.principalSource == PrincipalSource::session || (isWake && message.routing.contains("sessionOrigin"));
    // A wake can be queued before a stop commits and reach this turn after it commits.
    if (stoppedWake || (usesSessionOrigin && !activeSessionOrigin(message))) {
        finishTurn(message, "The T3 Session participation has ended.", nullptr, "session_access");
        return false;
    }
    if (message.principalSource == PrincipalSource::channel) {
        if (!message.channelScope.empty() && channelMessageAuthorized(message.id, message.bot)) return true;
    } else if (Policy(*this).allows(turnActor(message), Action::bot_talk, message.bot))
        return true;
    if (const auto attempt = takeReservedConversationCall(message.id)) releaseModelCall(attempt);
    if (message.principalSource == PrincipalSource::channel)
        finishTurn(message, "This channel connection is no longer authorized.", nullptr, "channel_access");
    else
        finishTurn(message, "You no longer have permission to message this Bot.", nullptr, "access");
    return false;
}

void Store::Impl::runTurn(Id id, Model& model, T3& t3, const Runtime::SoulLoader& soul, const std::function<Seconds()>& clock,
                          const BotTools& settings, GitHost* gitHost, const ApplyBotChange& applyBotChange,
                          const ReconcileBotCreation& reconcileBotCreation, const IntakeResolver& intake, const PeerResolver& peers,
                          std::stop_token shutdown) {
    Message message;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested() || !isNextTurn(id) ||
            query("SELECT 1 FROM conversation_retries WHERE message_id=? AND retry_at>?", {id, clock()}).next())
            return;
        message = loadTurnMessage(id);
        transaction.commit();
    }
    const bool notification = message.principalSource == PrincipalSource::gitWebhook;
    const bool web = message.channelScope.empty() && !notification;
    auto tools = notification ? std::vector<std::string>{"reply", "read_tool_result"} : turnTools;
    if (gitHost && notification) {
        appendGitNotificationTools(message.routing.at("gitEvent"), tools);
    } else if (gitHost && web)
        tools.insert(tools.end(), gitTools().begin(), gitTools().end());
    if (web) {
        tools.insert(tools.end(), webTools.begin(), webTools.end());
        tools.insert(tools.end(), botToolNames().begin(), botToolNames().end());
    }
    if (!message.channelScope.empty()) tools.insert(tools.end(), channelTools.begin(), channelTools.end());
    bool currentCheck = true;
    if (gitHost && notification) {
        const auto& event = message.routing.at("gitEvent");
        if (field(event, "followup") == "failed_check") {
            bool shouldVerify = false;
            {
                std::lock_guard lock(mutex);
                if (canReceiveGitWebhook(message.bot, event)) {
                    const auto followup = gitPrFollowup(message, intake);
                    shouldVerify = followup && followup->status != GitPrFollowup::Status::attempted;
                }
            }
            currentCheck = shouldVerify && currentGitPrCheck(*gitHost, message);
        }
    }
    const auto available = [&](int step, bool reviewPreference) {
        if (reviewPreference) {
            std::vector<std::string> reviewTools{"skip_preference"};
            if (canReviewPreference(message)) reviewTools.push_back("remember_preference");
            return std::tuple{std::move(reviewTools), Json::array(), std::string_view{}};
        }
        auto offered = tools;
        if (step == 1 && canReviewPreference(message)) offered.push_back("review_preference");
        auto machines = Json::array();
        std::string_view followupStatus;
        if (notification) {
            if (const auto followup = gitHost ? gitPrFollowup(message, intake) : std::nullopt) {
                switch (followup->status) {
                case GitPrFollowup::Status::available:
                    if (currentCheck) offered.push_back("fix_pull_request");
                    break;
                case GitPrFollowup::Status::attempted: followupStatus = "already_requested"; break;
                case GitPrFollowup::Status::limitReached:
                    if (currentCheck) followupStatus = "limit_reached";
                    break;
                }
            }
        } else {
            for (const auto& [machineId, machine] : localMachines)
                if (permitsLocalMachineOwner(machine->owner(), message.bot)) machines.push_back(machineId);
            if (!machines.empty()) offered.push_back("request_local_machine_connection");
        }
        return std::tuple{std::move(offered), std::move(machines), followupStatus};
    };
    const bool ja = japanese(message.text);
    const auto finish = [&](std::string text, std::string_view route) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (authorizeTurn(message)) finishTurn(message, std::move(text), nullptr, route);
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
    const auto persistModelCheckpoint = [&] {
        std::lock_guard lock(mutex);
        const auto entry = conversationCheckpoints.find(id);
        if (entry == conversationCheckpoints.end()) return true;
        const auto& checkpoint = entry->second;
        Transaction transaction(db);
        const auto* completedCalls = std::get_if<std::vector<ModelCall>>(&checkpoint.outcome);
        const bool ownsTurnQueue = isNextTurn(id);
        if (!ownsTurnQueue && !completedCalls) return false;
        if (!checkpoint.usage.empty()) exec("UPDATE model_attempts SET usage=? WHERE id=?", {checkpoint.usage, checkpoint.attempt});
        bool hasDecision = false;
        if (!ownsTurnQueue || authorizeTurn(message)) {
            if (completedCalls) {
                exec("UPDATE conversation_steps SET decision=? WHERE message_id=? AND step=? AND result IS NULL",
                     {encodeCalls(*completedCalls), id, checkpoint.step});
                exec("DELETE FROM conversation_retries WHERE message_id=?", {id});
                hasDecision = true;
            } else {
                const auto& error = std::get<ConversationModelFailure>(checkpoint.outcome);
                const bool isRepair =
                    checkpoint.attempt &&
                    query("SELECT 1 FROM conversation_model_failures WHERE repair_attempt_id=?", {checkpoint.attempt}).next();
                Id repair = 0;
                if (checkpoint.attempt && repairableModelOutput(error.code) &&
                    !query("SELECT 1 FROM conversation_model_failures WHERE message_id=? AND repair_attempt_id IS NOT NULL", {id}).next()) {
                    repair = reserveModelCall(messageModelBudget(id), "conversation_turn", id, 0, 0);
                    if (repair) exec("UPDATE conversation_turns SET reserved_attempt=? WHERE message_id=?", {repair, id});
                }
                if (checkpoint.attempt)
                    exec("INSERT INTO conversation_model_failures(attempt_id,message_id,step,reason,repair_attempt_id)"
                         " VALUES(?,?,?,?,?)",
                         {checkpoint.attempt, id, checkpoint.step, modelFailureReason(error.code), repair ? Json(repair) : Json(nullptr)});
                if (!repair) {
                    if (error.retryFrom && !isRepair)
                        deferConversation(id, *error.retryFrom);
                    else
                        finishTurn(message, modelFailureReply(error.code, ja), nullptr, "error");
                }
            }
        }
        transaction.commit();
        conversationCheckpoints.erase(entry);
        return hasDecision;
    };
    if (!persistModelCheckpoint()) return;
    for (int step = 1; step < std::numeric_limits<int>::max() && !shutdown.stop_requested(); ++step) {
        if (!message.channelScope.empty()) deliverChannels(shutdown);
        std::vector<ModelCall> calls;
        std::string effectType;
        bool hasWebEvidence = false;
        bool reviewPreference = false;
        {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!authorizeTurn(message)) {
                transaction.commit();
                return;
            }
            reviewPreference = preferenceReviewPending(id, step);
            auto row = query("SELECT decision,result,json_type(effect) FROM conversation_steps WHERE message_id=? AND step=?", {id, step});
            if (row.next()) {
                if (!row.string(1).empty()) continue;
                effectType = row.string(2);
                if (!row.string(0).empty()) {
                    const auto [offered, machines, followupStatus] = available(step, reviewPreference);
                    bool rejectionChanged = false;
                    for (const auto& saved : row.json(0)) {
                        Decision call{saved.at("tool"), saved.at("args"), field(saved, "reason")};
                        if (auto rejection = saved.value("rejection", std::string{}); !rejection.empty())
                            calls.push_back({std::move(call), std::move(rejection)});
                        else {
                            auto checked = checkModelCall(message.bot, std::move(call), offered, notification);
                            rejectionChanged |= !checked.rejection.empty();
                            calls.push_back(std::move(checked));
                        }
                    }
                    if (rejectionChanged)
                        exec("UPDATE conversation_steps SET decision=? WHERE message_id=? AND step=? AND result IS NULL",
                             {encodeCalls(calls), id, step});
                }
            }
            hasWebEvidence = query(R"SQL(
SELECT 1 FROM conversation_steps s, json_each(s.decision) call
WHERE s.message_id=? AND s.step<?
  AND json_extract(call.value,'$.tool') IN ('web_search','web_fetch')
  AND json_extract(s.result,'$[' || call.key || '].untrustedData') = 1
LIMIT 1
)SQL",
                                   {id, step})
                                 .next();
            transaction.commit();
        }
        if (calls.empty()) {
            const auto now = clock();
            BotNotes botNotes;
            try {
                if (!notification && !reviewPreference) botNotes = loadNotes(message.bot, soul, now);
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception& error) {
                const auto diagnostic =
                    Json::parse(Json(error.what()).dump(-1, ' ', false, Json::error_handler_t::replace)).get<std::string>();
                finish(ja ? "このBotのノートを読み込めませんでした (" + diagnostic + ")。ファイルを修正してから、もう一度送ってください。"
                          : "I couldn't read this Bot's notes (" + diagnostic + "). Fix the file and send your message again.",
                       "error");
                return;
            }
            const auto linkedDestinations = reviewPreference ? LinkedTaskDestinations{} : discoverLinkedTaskDestinations(t3, message.bot);
            ModelInput input;
            {
                std::lock_guard lock(mutex);
                Transaction transaction(db);
                if (shutdown.stop_requested()) return;
                if (!authorizeTurn(message)) {
                    transaction.commit();
                    return;
                }
                auto repair = query(R"SQL(
SELECT f.repair_attempt_id, t.reserved_attempt IS f.repair_attempt_id, f.reason
FROM conversation_model_failures f JOIN conversation_turns t USING(message_id)
WHERE f.message_id=? AND f.step=? AND f.repair_attempt_id IS NOT NULL
)SQL",
                                    {id, step});
                std::string feedback;
                if (repair.next()) {
                    if (!repair.number(1)) {
                        // A previous worker claimed the repair, but no result survived.
                        exec("INSERT INTO conversation_model_failures(attempt_id,message_id,step,reason) VALUES(?,?,?,?)",
                             {repair.number(0), id, step, "repair_interrupted"});
                        finishTurn(message, modelFailureReply(ModelErrorCode::inference_failed, ja), nullptr, "error");
                        transaction.commit();
                        return;
                    }
                    feedback = "The previous model response was rejected before executing any of its tools. "
                               "Reason: " +
                               repair.string(2) +
                               ". Return offered tools with valid arguments. "
                               "Batch independent reads, and propose replies or actions that make changes on their own.";
                }
                auto [offered, machines, followupStatus] = available(step, reviewPreference);
                if (reviewPreference) {
                    auto prepared = preferenceReviewInput(message, std::move(offered), std::move(feedback), now);
                    if (!prepared) {
                        const std::vector<ModelCall> skipped{ModelCall{Decision{"skip_preference", Json::object(), {}}, {}}};
                        exec(R"SQL(
INSERT INTO conversation_steps(message_id,step,decision,result) VALUES(?,?,?,?)
ON CONFLICT(message_id,step) DO UPDATE SET decision=excluded.decision,result=excluded.result
WHERE conversation_steps.result IS NULL
)SQL",
                             {id, step, encodeCalls(skipped), Json::array({{{"ok", true}}})});
                        transaction.commit();
                        continue;
                    }
                    input = std::move(*prepared);
                }
                if (!query(R"SQL(
SELECT 1 WHERE NOT EXISTS(SELECT 1 FROM model_budgets WHERE id=? AND attempts=call_limit)
    OR EXISTS(SELECT 1 FROM conversation_turns WHERE message_id=? AND reserved_attempt IS NOT NULL)
)SQL",
                           {messageModelBudget(id).id, id})
                         .next()) {
                    pauseTurn(message);
                    transaction.commit();
                    return;
                }
                if (!reviewPreference) {
                    std::vector<ModelImage> images;
                    auto context = turnInput(message, step, now, std::move(machines), images, linkedDestinations);
                    if (!followupStatus.empty()) context["event"]["gitFollowupStatus"] = followupStatus;
                    input = {.task = context.at("task"),
                             .event = context.at("event"),
                             .history = context.at("history"),
                             .notes = botNotes,
                             .tools = std::move(offered),
                             .feedback = std::move(feedback),
                             .config = message.routing.at("config"),
                             .images = std::move(images)};
                }
                transaction.commit();
            }
            ConversationModelCheckpoint checkpoint{0, step, Json::object(), ConversationModelFailure{}};
            try {
                if (!reviewPreference) {
                    authorizeSessionHistory(message, input.event.at("turn"), t3);
                    authorizeBotToolHistory(message.bot, id, input);
                }
                {
                    std::lock_guard lock(mutex);
                    Transaction transaction(db);
                    if (shutdown.stop_requested()) return;
                    if (!authorizeTurn(message)) {
                        transaction.commit();
                        return;
                    }
                    auto& attempt = checkpoint.attempt;
                    attempt = takeReservedConversationCall(id);
                    if (!attempt) attempt = reserveModelCall(messageModelBudget(id), "conversation_turn", id, 0, 0);
                    if (!attempt)
                        pauseTurn(message);
                    else
                        exec("INSERT INTO conversation_steps(message_id,step,attempt_id) VALUES(?,?,?)"
                             " ON CONFLICT(message_id,step) DO UPDATE SET attempt_id=excluded.attempt_id",
                             {id, step, attempt});
                    transaction.commit();
                    if (!attempt) return;
                }
                auto response = callModel(message.bot, model, input);
                checkpoint.usage = std::move(response.usage);
                calls = std::move(response.calls);
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
                std::lock_guard lock(mutex);
                conversationCheckpoints.insert_or_assign(id, std::move(checkpoint));
            }
            if (!persistModelCheckpoint()) return;
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
                                                      .shutdown = shutdown,
                                                      .hasWebEvidence = hasWebEvidence});
        if (outcome != CallOutcome::advance) return;
    }
    if (shutdown.stop_requested()) return;
    finish(ja ? "このメッセージの処理が長くなりすぎたため中断しました。必要なら、もう一度送ってください。"
              : "I stopped because handling this message took too many steps. Send it again if needed.",
           "error");
}
}
