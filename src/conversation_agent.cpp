#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
using conversation_storage::hasText;
using conversation_storage::integer;
using conversation_storage::identifier;
using conversation_storage::text;
using conversation_storage::ToolError;
using conversation_storage::messageSize;
using conversation_storage::verifiedEnvironment;

namespace {
using conversation::Message;

const std::vector<std::string> channelTools{"channel_edit_message","channel_react","channel_send_attachment",
    "publish_artifact","read_channel_attachment"};

constexpr int activeTaskLimit = 12;
constexpr int terminalTaskLimit = 8;
constexpr int findLimit = 10;
constexpr int maxSteps = 24;
constexpr std::size_t resultChunk = 6000;
constexpr std::size_t sourceSize = 8192;

const std::vector<std::string> turnTools{"start_task","send_to_task","change_task","answer_task_question",
                                         "record_task_blocker","withdraw_task_question",
                                         "cancel_task","message_session","interrupt_session","read_result",
                                         "find_tasks","flock_session_search","flock_session_events","reply"};
const std::vector<std::string> webTools{"start_workflow","workflow_status","instruct_workflow_stage",
    "stop_workflow_stage","stop_workflow","compact_conversation","expand_context","continue_fresh_session",
    "list_grants","request_access","list_secrets","request_secret_input",
    "list_connections","check_environment_readiness","update_bot_settings","request_bot_change",
    "read_memory","propose_memory_edit","apply_memory_edit","revert_memory_edit","memory_history",
    "create_schedule","list_schedules","remove_schedule","create_reminder","create_trigger"};

bool japanese(std::string_view text) {
    return std::any_of(text.begin(),text.end(),[](unsigned char byte) {
        return byte == 0xe3 || (byte >= 0xe4 && byte <= 0xe9);
    });
}

Json waitView(const Json& wait) {
    if (!wait.is_object()) return nullptr;
    Json view = {{"waitId",wait.value("id",Id{0})},{"reason",field(wait,"reason")},
                 {"text",bounded(field(wait,"text"),1000)}};
    if (const auto choices = wait.find("choices"); choices != wait.end() && choices->is_array()) {
        auto& list = view["choices"] = Json::array();
        for (const auto& choice : *choices)
            if (choice.is_string() && list.size() < 12) list.push_back(bounded(choice.get<std::string>(),200));
    }
    return view;
}

std::string requestName(const Json& task) {
    const auto request = field(task,"request");
    return bounded(request.substr(0,request.find('\n')),160);
}

std::string effectStatus(const Json& actions) {
    std::string status = "done";
    for (const auto& action : actions) {
        const auto effect = field(action,"effect");
        if (effect == "replanning" || effect == "cancelling" || effect == "interrupt_requested") return "stopping";
        if (effect == "started" || effect == "accepted" || effect == "answered" || effect == "delivered") status = "working";
    }
    return status;
}
}

bool Store::Impl::turnPending(Id id) const {
    return query(R"SQL(
SELECT 1 FROM journal j
WHERE j.id=? AND j.kind IN ('conversation_message','git_webhook') AND json_extract(j.data,'$.status') IS NULL
AND NOT EXISTS (
  SELECT 1 FROM journal prior WHERE prior.conversation_bot_id=j.conversation_bot_id AND prior.id<j.id
  AND prior.kind IN ('conversation_message','git_webhook') AND json_extract(prior.data,'$.status') IS NULL)
)SQL",{id}).next();
}

Message Store::Impl::loadTurnMessage(Id id) const {
    auto row = query(R"SQL(
SELECT m.reply_to,m.bot_id,m.text,c.scope_key,m.routing,m.person_id,m.person_root FROM messages m
LEFT JOIN channel_messages c ON c.message_id=m.id WHERE m.id=?
)SQL",{id});
    if (!row.next()) throw Conflict("Conversation message is missing");
    Message message{id,row.number(0),row.string(1),row.string(2),row.string(3),row.json(4),{row.string(5),row.number(6) != 0}};
    if (!message.replyTo || message.replyTo >= id) return message;
    auto anchor = query(R"SQL(
WITH RECURSIVE targets(id) AS (
  VALUES(?)
  UNION ALL
  SELECT m.reply_to FROM targets
  JOIN conversation_turns turn ON turn.reply_id=targets.id
  JOIN messages m ON m.id=turn.message_id
  LEFT JOIN channel_messages c ON c.message_id=m.id
  WHERE m.bot_id=? AND m.reply_to<m.id AND (?='' OR c.scope_key=?)
)
SELECT j.id,j.task_id FROM targets JOIN journal j ON j.id=targets.id JOIN tasks t ON t.id=j.task_id
 LEFT JOIN channel_tasks c ON c.task_id=t.id
 WHERE j.kind='waiting' AND t.bot_id=? AND (?='' OR c.scope_key=?)
 )SQL",{message.replyTo,message.bot,message.channelScope,message.channelScope,
        message.bot,message.channelScope,message.channelScope});
    if (anchor.next()) {
        message.questionMessageId = anchor.number(0);
        message.questionTaskId = anchor.number(1);
    }
    return message;
}

const char* Store::Impl::questionAnswerError(const Json& task, const Message& message) const {
    const auto& wait = task.at("wait");
    if (task.at("state") != "waiting" || !wait.is_object()) return "That question is no longer open";
    if (field(wait,"reason") != "user_question")
        return "Only user_question waits can be answered; approvals use their own controls";
    const auto asked = integer(wait,"messageId");
    if (asked >= message.id) return "That question opened after this message was sent, so this message cannot answer it";
    if (message.replyTo && message.questionMessageId != asked)
        return "This message replies to something else, so it can only answer the question it replies to";
    if (!asked || wait.value("run",Json(nullptr)) != task.value("activeRun",Json(nullptr)) ||
        integer(wait,"eventId") != task.value("eventId",Id{0})) return "That question is no longer current";
    if (replanReady(task))
        return "The task is replanning, so this question will close. Its Bot asks again if it still needs to know";
    return nullptr;
}

Json Store::Impl::taskState(const Json& task, const Message& message) const {
    auto wait = waitView(task.at("wait"));
    if (wait.is_object()) wait["answerable"] = !questionAnswerError(task,message);
    Json view = {{"generation",task.at("generation")},{"revisionId",task.value("currentRevision",Id{0})},
                 {"state",task.at("state")},{"doneWhen",bounded(field(task,"doneWhen"),1600)},
                 {"replanning",task.value("replanning",false)},{"wait",std::move(wait)}};
    if (task.contains("workflowId")) {
        view["workflowId"] = task.at("workflowId");
        view["stageId"] = task.at("stageId");
    }
    if (const auto reason = field(task,"blockerReason"); !reason.empty()) view["blockerReason"] = reason;
    if (const auto intent = field(task,"intent"); !intent.empty()) view["intent"] = bounded(intent,1000);
    return view;
}

Json Store::Impl::ownTask(Id taskId, const Message& message) const {
    auto row = query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.id=? AND t.bot_id=? AND (?='' OR c.scope_key=?)
)SQL",{taskId,message.bot,message.channelScope,message.channelScope});
    if (row.next()) return load(taskId);
    throw ToolError("No task with taskId " + std::to_string(taskId));
}

Json Store::Impl::resultReference(Id messageId, const Message& message) const {
    auto row = query(R"SQL(
SELECT t.id,t.state,j.kind,j.data,json_extract(t.data,'$.request'),coalesce(json_extract(t.data,'$.currentRevision'),0)
FROM journal j JOIN tasks t ON t.id=j.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE j.id=? AND t.bot_id=? AND j.kind IN ('report','completed','cancelled') AND (?='' OR c.scope_key=?)
)SQL",{messageId,message.bot,message.channelScope,message.channelScope});
    if (!row.next()) throw ToolError("No result with messageId " + std::to_string(messageId));
    const auto kind = row.string(2);
    const auto data = row.json(3);
    Json provenance = nullptr;
    if (kind == "report") {
        provenance = {{"environmentId",field(data,"environmentId")},
            {"threadId",data.contains("run") ? field(data["run"],"threadId") : ""},
            {"runId",field(data,"resultRunId")},{"itemId",field(data,"itemId")}};
    } else if (data.contains("source")) provenance = data.at("source");
    Json result = {{"taskId",row.number(0)},{"messageId",messageId},{"revisionId",integer(data,"revisionId")},
                   {"text",field(data,kind == "report" ? "report" : "text")},{"option",0},
                   {"kind",kind},{"taskState",row.string(1)},{"request",row.string(4)},
                   {"currentRevision",row.number(5)},{"truncated",data.value("truncated",false)}};
    if (!provenance.is_null()) result["run"] = provenance;
    return redactor(message.bot).record(std::move(result));
}

namespace {
Json sourceRef(const Json& result) {
    Json source = {{"taskId",result.at("taskId")},{"messageId",result.at("messageId")},
                   {"revisionId",result.at("revisionId")},{"text",bounded(field(result,"text"),sourceSize)},
                   {"option",0}};
    if (result.contains("run")) source["run"] = result.at("run");
    return source;
}
}

Json Store::Impl::conversationSteps(const Message& message, int step, std::vector<ModelImage>& images) {
    Json turn = Json::array();
    auto steps = query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND step<?"
                       " AND result IS NOT NULL ORDER BY step",{message.id,step});
    while (steps.next()) {
        const auto calls = steps.json(0);
        const auto results = steps.json(1);
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const auto& decision = calls.at(i);
            auto args = decision.at("args");
            if (args.is_object() && decision["tool"] == "channel_send_attachment") args.erase("contentBase64");
            if (args.is_object() && decision["tool"] == "publish_artifact") args.erase("html");
            auto result = channelToolStatus(message.id,message.bot,results.at(i),&images);
            if (args.is_object() && args.contains("docId")) {
                const bool reading = decision.at("tool") == "read_memory" || decision.at("tool") == "memory_history";
                if (!Access(*this).permits(message.bot,{"shared_memory",field(args,"docId"),reading ? "read" : "write"},true)) {
                    args = Json::object();
                    result = {{"ok",false},{"error","Memory access denied"}};
                } else {
                    const auto shared = sharedRedactor();
                    args = shared.record(std::move(args));
                    result = shared.record(std::move(result));
                }
            }
            const auto tool = field(decision,"tool");
            if ((decision.contains("rejection") && result.value("untrustedData",false)) ||
                modelArgumentsContainProtected(modelCallRedactor(message.bot,tool,args,message.routing.contains("gitEvent")),
                                               tool,args)) {
                args = Json::object();
                result = toolFailure("Saved tool history is unavailable. Check the target before retrying.");
            }
            turn.push_back({{"tool",decision.at("tool")},{"args",std::move(args)},{"result",std::move(result)}});
        }
    }
    return turn;
}

Json Store::Impl::turnInput(const Message& message, int step, Seconds now, Json machines, std::vector<ModelImage>& images) {
    if (message.routing.contains("gitEvent")) {
        auto event = Json{{"kind","conversation_turn"},{"messageId",message.id},{"text",message.text},
            {"currentTime",now},{"replyTo",nullptr},{"turn",conversationSteps(message,step,images)},
            {"gitEvent",sharedRedactor().record(message.routing.at("gitEvent"))}};
        return {{"task",Json::object()},{"event",std::move(event)},{"history",Json::array()}};
    }
    const auto scrub = redactor(message.bot);
    Json tasks = Json::array();
    std::set<Id> listed;
    const bool web = message.channelScope.empty();
    const auto summaries = web ? compactions(message.bot,0,1,message.id) : Json::array();
    const Id boundary = summaries.empty() ? 0 : summaries.front().at("endMessageId").get<Id>();
    const auto latestResult = [&](Id taskId) -> Json {
        auto row = query(R"SQL(
SELECT id,kind,data FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')
ORDER BY id DESC LIMIT 1
)SQL",{taskId});
        if (!row.next()) return nullptr;
        const auto kind = row.string(1);
        const auto data = row.json(2);
        Json result = {{"messageId",row.number(0)},{"kind",kind},{"revisionId",integer(data,"revisionId")}};
        if (row.number(0)>boundary)
            result["excerpt"] = bounded(scrub.text(field(data,kind == "report" ? "report" : "text")),600);
        return result;
    };
    const auto addTask = [&](Id taskId) {
        if (!listed.insert(taskId).second) return;
        const auto task = load(taskId);
        Json view = {{"taskId",taskId},{"request",bounded(field(task,"request"),800)},
                     {"environmentId",field(task,"environmentId")}};
        view.update(taskState(task,message));
        if (!localMachines.empty() && task.value("activeRun",Json()).is_object())
            view["activeSession"] = {{"threadId",field(task.at("activeRun"),"threadId")},
                                     {"runId",field(task.at("activeRun"),"runId")}};
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
)SQL",{message.bot,message.channelScope,message.channelScope,activeTaskLimit + 1});
        for (int i = 0; rows.next(); ++i) {
            if (i == activeTaskLimit) { moreTasks = true; break; }
            addTask(rows.number(0));
        }
    }
    {
        auto rows = query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND t.state IN ('completed','cancelled') AND (?='' OR c.scope_key=?)
ORDER BY t.id DESC LIMIT ?
)SQL",{message.bot,message.channelScope,message.channelScope,terminalTaskLimit});
        while (rows.next()) addTask(rows.number(0));
    }

    Json replyTo = nullptr;
    if (message.replyTo) {
        auto row = query(R"SQL(
SELECT j.kind,j.task_id,j.data,m.text FROM journal j LEFT JOIN tasks t ON t.id=j.task_id
LEFT JOIN messages m ON m.id=j.id
WHERE j.id=? AND j.id<? AND (j.conversation_bot_id=? OR t.bot_id=?)
)SQL",{message.replyTo,message.id,message.bot,message.bot});
        if (row.next()) {
            const auto kind = row.string(0);
            const Id taskId = row.number(1);
            const auto data = row.json(2);
            replyTo = {{"messageId",message.replyTo}};
            if (kind == "waiting") {
                const auto task = load(taskId);
                const auto& current = task.at("wait");
                replyTo.update({{"kind","task_question"},{"taskId",taskId},{"waitId",integer(data,"id")},
                    {"reason",field(data,"reason")},{"text",bounded(field(data,"text"),1000)},
                    {"revisionId",task.value("currentRevision",Id{0})},{"generation",task.at("generation")},
                    {"current",current.is_object() && integer(current,"id") == integer(data,"id") && waitIsActionable(task)}});
                if (const auto question = waitView(data); question.contains("choices"))
                    replyTo["choices"] = question.at("choices");
                addTask(taskId);
            } else if (kind == "report" || kind == "completed" || kind == "cancelled") {
                replyTo.update({{"kind","result"},{"taskId",taskId},
                    {"text",bounded(field(data,kind == "report" ? "report" : "text"),2000)}});
                addTask(taskId);
            } else if (kind == "pr_review_requested") {
                replyTo.update({{"kind","pr_review"},{"taskId",taskId},{"itemId",integer(data,"itemId")},
                    {"title",scrub.text(field(data,"title"))},{"text",bounded(scrub.text(field(data,"text")),2000)},
                    {"prUrl",scrub.text(field(data,"prUrl"))}});
                addTask(taskId);
            } else if (kind == "conversation_reply") {
                replyTo.update({{"kind","bot_message"},{"text",bounded(field(data,"text"),2000)}});
                if (data.contains("suggestions")) replyTo["suggestions"] = data.at("suggestions");
                if (message.questionTaskId) addTask(message.questionTaskId);
                auto original = query("SELECT id,text,reply_to FROM messages WHERE id=? AND bot_id=?",
                                      {integer(data,"replyToMessageId"),message.bot});
                if (original.next()) {
                    replyTo["inReplyTo"] = {{"messageId",original.number(0)},{"text",bounded(original.string(1),messageSize)}};
                    if (original.number(2)) replyTo["inReplyTo"]["replyToMessageId"] = original.number(2);
                }
            } else if (kind == "conversation_message") {
                replyTo.update({{"kind","user_message"},{"text",bounded(row.string(3),2000)}});
            } else {
                replyTo.update({{"kind",kind},{"text",bounded(field(data,"text"),2000)}});
            }
        } else replyTo = {{"messageId",message.replyTo},{"kind","unavailable"}};
    }

    Json environments = Json::array();
    if (const auto initial = field(message.routing,"environmentId"); !initial.empty())
        environments.push_back({{"name",initial},{"environmentId",initial},{"default",true}});
    const auto& config = message.routing.at("config");
    if (const auto aliases = config.find("conversationEnvironments"); aliases != config.end() && aliases->is_object()) {
        for (auto alias = aliases->begin(); alias != aliases->end(); ++alias) {
            const auto id = verifiedEnvironment(message.routing,message.routing,alias.key());
            if (!id.empty()) environments.push_back({{"name",alias.key()},{"environmentId",id}});
        }
    }

    const auto channel = channelContext(message.id,message.bot,&images);
    auto turn = conversationSteps(message,step,images);
    const auto expandedBytes = conversation_storage::boundExpandedContext(turn);
    auto history = conversationHistory(message,summaries,expandedBytes);
    Json event{{"kind","conversation_turn"},{"messageId",message.id},{"text",message.text},
               {"currentTime",now},{"replyTo",std::move(replyTo)},{"turn",std::move(turn)}};
    event.update(channel);
    if (web) event["attachments"] = attachmentViews(message.id);
    Json task{{"tasks",std::move(tasks)},{"moreTasks",moreTasks},{"environments",std::move(environments)},
              {"localMachines",std::move(machines)}};
    if (web) {
        auto& workflows = task["workflows"] = Json::array();
        auto recent = query("SELECT id,CASE WHEN message_id>? THEN request ELSE '' END FROM workflows "
                            "WHERE bot_id=? AND message_id<=? ORDER BY id DESC LIMIT 12",{boundary,message.bot,message.id});
        while (recent.next())
            workflows.push_back({{"workflowId",recent.number(0)},{"request",bounded(scrub.text(recent.string(1)),300)}});
    }
    return {{"task",std::move(task)},{"event",std::move(event)},{"history",std::move(history)}};
}

Json Store::Impl::applyTurnTool(const Message& message, int step, const Decision& decision,
                                Seconds now) {
    const auto& args = decision.args;
    if (!args.is_object()) throw ToolError("Tool arguments must be an object");
    const auto& tool = decision.tool;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    if (auto result = channelTool(message.id,bot,decision); !result.is_null()) return result;
    if (auto result = localMachineTool(bot,message.channelScope,decision); !result.is_null()) return result;
    if (std::ranges::find(webTools,tool) != webTools.end()) {
        if (!message.channelScope.empty()) throw ToolError("Use Flock Web for this action");
        if (tool == "compact_conversation" || tool == "expand_context" || tool == "continue_fresh_session")
            return contextTool(tool,args,message,step);
        if (tool == "start_workflow" || tool == "workflow_status" || tool == "instruct_workflow_stage" ||
            tool == "stop_workflow_stage" || tool == "stop_workflow")
            return workflowTool(tool,args,bot,message.id,step,routing);
        auto result = accessTool(bot,message.id,routing,decision);
        if (result.is_null()) result = memory(bot,nullptr,tool,args,now);
        if (result.is_null()) result = automationTool(bot,routing,decision,now);
        if (result.is_null()) throw ToolError("Unknown conversation tool");
        result["ok"] = true;
        return result;
    }
    const auto expectGeneration = [&](const Json& task) {
        const auto expected = identifier(args,"generation",true,0);
        if (expected != task.at("generation").get<Id>())
            throw ToolError("stale",{{"applied",0},{"taskId",task.at("id")},{"expected",{{"generation",expected}}},
                                     {"current",taskState(task,message)}});
    };
    const auto active = [&](const Json& task) {
        if (terminal(task) || task.at("state") == "cancelling")
            throw ToolError("The task is " + task.at("state").get<std::string>(),
                            {{"taskId",task.at("id")},{"current",taskState(task,message)}});
    };
    const auto source = [&](const Json* target) -> Json {
        const auto sourceId = identifier(args,"sourceResultMessageId",false);
        if (!sourceId) return nullptr;
        const auto result = resultReference(sourceId,message);
        if (target && (result.at("taskId") != target->at("id") ||
                       result.at("revisionId") != target->value("currentRevision",Id{0})))
            throw ToolError("sourceResultMessageId must be a result of this task's current revision");
        return result;
    };

    if (tool == "start_task") {
        const auto request = text(args,"request",messageSize,true);
        const auto doneWhen = text(args,"doneWhen",8000,false);
        const auto environment = text(args,"environment",256,false);
        const bool resume = args.value("resume",false);
        if (field(routing,"environmentId").empty() || field(routing,"projectId").empty())
            throw ToolError("This Bot needs an execution environment and project before it can start work");
        if (!environment.empty() && verifiedEnvironment(routing,routing,environment).empty())
            throw ToolError("Unknown environment " + environment + "; use one of task.environments");
        const auto result = source(nullptr);
        if (resume && (result.is_null() || result.at("taskState") != "cancelled"))
            throw ToolError("resume requires sourceResultMessageId from a cancelled task");
        const auto id = startConversationTask(routing,bot,request,environment,message.id,
                                              result.is_null() ? Json(nullptr) : sourceRef(result),resume);
        if (!doneWhen.empty()) {
            auto task = load(id);
            task["requestedDoneWhen"] = doneWhen;
            task["criteriaSourceMessageId"] = message.id;
            save(task);
        }
        return {{"ok",true},{"effect","started"},{"taskId",id}};
    }
    if (tool == "send_to_task" || tool == "change_task") {
        auto task = ownTask(identifier(args,"taskId",true),message);
        expectGeneration(task);
        active(task);
        const auto instruction = text(args,"instruction",messageSize,true);
        const auto result = source(&task);
        std::optional<TaskChange> change;
        if (tool == "change_task") {
            const auto revision = identifier(args,"revisionId",true,0);
            if (revision != task.value("currentRevision",Id{0}))
                throw ToolError("stale",{{"applied",0},{"taskId",task.at("id")},
                    {"expected",{{"revisionId",revision}}},{"current",taskState(task,message)}});
            change = TaskChange{text(args,"intent",8000,false),text(args,"doneWhen",8000,false),
                                text(args,"environment",256,false)};
            if (change->intent.empty() && change->doneWhen.empty() && change->environment.empty())
                throw ToolError("change_task needs intent, doneWhen, or environment; use send_to_task for other instructions");
            if (!change->environment.empty() && verifiedEnvironment(task,routing,change->environment).empty())
                throw ToolError("Unknown environment " + change->environment + "; use one of task.environments");
        }
        const bool conflicting = acceptInstruction(task,message.id,step,
            instruction,result.is_null() ? Json(nullptr) : sourceRef(result),routing,change ? &*change : nullptr);
        return {{"ok",true},{"effect",conflicting ? "replanning" : "accepted"},{"taskId",task.at("id")}};
    }
    if (tool == "answer_task_question") {
        const auto task = ownTask(identifier(args,"taskId",true),message);
        const auto waitId = identifier(args,"waitId",true);
        const auto answer = text(args,"text",8000,true);
        if (integer(task.at("wait"),"id") != waitId)
            throw ToolError("That question is no longer open",{{"taskId",task.at("id")},{"current",taskState(task,message)}});
        if (const auto error = questionAnswerError(task,message))
            throw ToolError(error,{{"taskId",task.at("id")},{"current",taskState(task,message)}});
        const TaskTarget target{bot,task.at("id"),waitId,identifier(args,"revisionId",true,0),
                                identifier(args,"generation",true,0)};
        operate(message.actor,target,"answer",{{"text",answer}},message.id,InboxKind::user_question);
        return {{"ok",true},{"effect","answered"},{"taskId",task.at("id")}};
    }
    if (tool == "record_task_blocker" || tool == "withdraw_task_question") {
        if (!Access(*this).permitsPerson(message.actor,bot,Role::decide)) throw DecisionDenied{};
        auto task = ownTask(identifier(args,"taskId",true),message);
        expectGeneration(task);
        active(task);
        const bool record = tool == "record_task_blocker";
        const auto reason = record ? args.at("blockerReason").get<std::string>() : std::string{};
        if (record) recordTaskBlocker(task,reason,identifier(args,"waitId",false));
        else withdrawTaskQuestion(task,identifier(args,"waitId",true));
        const auto effect = record ? (reason.empty() ? "blocker_cleared" : "blocker_recorded") : "question_withdrawn";
        return {{"ok",true},{"effect",effect},
                {"taskId",task.at("id")},{"current",taskState(task,message)}};
    }
    if (tool == "cancel_task") {
        const auto task = ownTask(identifier(args,"taskId",true),message);
        expectGeneration(task);
        active(task);
        operate(message.actor,TaskTarget::from(task,0),"cancel",Json::object());
        const auto state = load(task.at("id")).at("state");
        return {{"ok",true},{"effect",state == "cancelled" ? "cancelled" : "cancelling"},{"taskId",task.at("id")}};
    }
    if (tool == "read_result") {
        const auto result = resultReference(identifier(args,"messageId",true),message);
        const auto& body = result.at("text").get_ref<const std::string&>();
        std::size_t offset = 0;
        if (args.contains("offset")) {
            if (!args["offset"].is_number_integer() || args["offset"].get<long double>() < 0)
                throw ToolError("offset must be a non-negative integer");
            offset = std::min<std::size_t>(args["offset"].get<std::size_t>(),body.size());
        }
        while (offset < body.size() && (static_cast<unsigned char>(body[offset]) & 0xc0) == 0x80) ++offset;
        const auto chunk = bounded(std::string_view(body).substr(offset),resultChunk);
        const auto next = offset + chunk.size();
        Json view = {{"ok",true},{"messageId",result.at("messageId")},{"taskId",result.at("taskId")},
                     {"request",bounded(field(result,"request"),800)},{"kind",result.at("kind")},
                     {"revisionId",result.at("revisionId")},
                     {"currentRevision",result.at("revisionId") == result.at("currentRevision")},
                     {"text",chunk},{"offset",offset},{"end",next >= body.size()},
                     {"truncatedWhenSaved",result.at("truncated")}};
        if (next < body.size()) view["nextOffset"] = next;
        return view;
    }
    if (tool == "find_tasks") {
        const auto search = text(args,"query",400,false);
        const auto before = identifier(args,"before",false);
        auto rows = query(R"SQL(
SELECT t.id FROM tasks t LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND (?='' OR instr(lower(json_extract(t.data,'$.request')),lower(?))>0)
AND (?=0 OR t.id<?) AND (?='' OR c.scope_key=?) ORDER BY t.id DESC LIMIT ?
)SQL",{bot,search,search,before,before,message.channelScope,message.channelScope,findLimit + 1});
        Json found = Json::array();
        Id last = 0;
        bool more = false;
        while (rows.next()) {
            if (static_cast<int>(found.size()) == findLimit) { more = true; break; }
            last = rows.number(0);
            const auto task = load(last);
            Json view = {{"taskId",last},{"request",bounded(field(task,"request"),300)},
                         {"environmentId",field(task,"environmentId")}};
            view.update(taskState(task,message));
            auto result = query("SELECT id,kind FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')"
                                " ORDER BY id DESC LIMIT 1",{last});
            if (result.next()) view["latestResult"] = {{"messageId",result.number(0)},{"kind",result.string(1)}};
            found.push_back(std::move(view));
        }
        Json response = {{"ok",true},{"tasks",std::move(found)}};
        if (more) response["before"] = last;
        return response;
    }
    throw ToolError("Unknown tool " + tool + "; use one of the provided tools");
}

Json Store::Impl::prepareSessionEffect(const Message& message, int step, const Decision& decision) {
    const auto& args = decision.args;
    if (!args.is_object()) throw ToolError("Tool arguments must be an object");
    const auto task = ownTask(identifier(args,"taskId",true),message);
    const auto expected = identifier(args,"generation",true,0);
    if (expected != task.at("generation").get<Id>())
        throw ToolError("stale",{{"applied",0},{"taskId",task.at("id")},{"expected",{{"generation",expected}}},
                                 {"current",taskState(task,message)}});
    if (terminal(task) || task.at("state") == "cancelling")
        throw ToolError("The task is " + task.at("state").get<std::string>(),{{"taskId",task.at("id")}});
    const auto& run = task.at("activeRun");
    if (!run.is_object() || field(run,"runId").empty())
        throw ToolError("The task has no running Session. Use send_to_task to pass it on at the next safe point",
                        {{"taskId",task.at("id")},{"current",taskState(task,message)}});
    Json effect = {{"kind",decision.tool == "message_session" ? "steer" : "interrupt"},{"taskId",task.at("id")},
                   {"environmentId",field(run,"environmentId").empty() ? field(task,"environmentId") : field(run,"environmentId")},
                   {"ownerBotId",message.bot},{"projectId",field(task,"projectId")},{"config",task.at("config")},{"run",run},
                   {"actionId","flock-turn-" + std::to_string(message.id) + "-" + std::to_string(step)}};
    if (decision.tool == "message_session") effect["text"] = text(args,"text",messageSize,true);
    return effect;
}

namespace {
Json performSessionEffect(T3& t3, const Json& effect) {
    const auto taskId = effect.at("taskId");
    const auto environment = field(effect,"environmentId");
    const auto& config = effect.at("config");
    const auto run = runRef(effect.at("run"));
    try {
        if (effect.at("kind") == "interrupt") {
            try {
                t3.interrupt(environment,config,run);
            } catch (const std::exception& error) {
                return {{"ok",false},{"taskId",taskId},{"delivery","unknown"},
                        {"error",std::string("T3 may have received the interrupt: ") + error.what()}};
            }
            return {{"ok",true},{"effect","interrupt_requested"},{"taskId",taskId}};
        }
        Command command;
        command.actionId = field(effect,"actionId");
        command.ownerBotId = field(effect,"ownerBotId");
        command.tool = "steer";
        command.environmentId = environment;
        command.projectId = field(effect,"projectId");
        command.instruction = field(effect,"text");
        command.config = config;
        command.target = run;
        command.authGeneration = t3.authGeneration(environment,config);
        const auto delivery = t3.send(command);
        if (delivery.kind == Delivery::Kind::Accepted)
            return {{"ok",true},{"effect","delivered"},{"taskId",taskId},{"run",runJson(delivery.run)}};
        Json failed = {{"ok",false},{"taskId",taskId},
            {"error",delivery.detail.empty() ? "T3 did not accept the note" : delivery.detail}};
        if (delivery.kind == Delivery::Kind::Unknown && !delivery.definitelyUnsent) failed["delivery"] = "unknown";
        return failed;
    } catch (const std::exception& error) {
        return {{"ok",false},{"taskId",taskId},{"error",error.what()}};
    }
}
}

void Store::Impl::finishTurn(const Message& message, std::string text, const Json& suggestions, std::string_view route) {
    Json actions = Json::array();
    std::set<Id> touched;
    Json artifacts = Json::array();
    auto steps = query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND result IS NOT NULL"
                       " ORDER BY step",{message.id});
    while (steps.next()) {
        const auto calls = steps.json(0);
        const auto results = steps.json(1);
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto& result = results.at(i);
            if (result.contains("artifact") && channelToolStatus(message.id,message.bot,result).value("ok",false))
                artifacts.push_back(result.at("artifact"));
            if (!result.value("ok",false) || !result.contains("effect")) continue;
            Json action = {{"tool",calls.at(i).at("tool")},{"effect",result.at("effect")}};
            if (result.contains("taskId")) {
                action["taskId"] = result.at("taskId");
                touched.insert(result.at("taskId").get<Id>());
            }
            for (const auto* key : {"workflowId","stageId"})
                if (result.contains(key)) action[key] = result.at(key);
            actions.push_back(std::move(action));
        }
    }
    if (route == "error" && !actions.empty()) {
        const bool ja = japanese(message.text);
        text += ja ? "\n\nこのメッセージで実行済みの操作:" : "\n\nAlready done for this message:";
        for (const auto& action : actions) {
            text += "\n- " + field(action,"effect") + ": ";
            if (action.contains("taskId")) text += requestName(load(action.at("taskId")));
            else if (action.contains("workflowId")) {
                auto workflow = query("SELECT request FROM workflows WHERE id=?", {action.at("workflowId")});
                workflow.next();
                text += bounded(workflow.string(0),160);
            }
        }
    }
    const auto status = effectStatus(actions);
    const auto webhook = message.routing.value("gitEvent",Json(nullptr));
    const Json linked = webhook.is_object() ? webhook.at("taskId") :
        touched.size() == 1 ? Json(*touched.begin()) : Json(nullptr);
    exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {linked,unixTime(),message.bot});
    const auto reply = last();
    Json data = {{"messageId",reply},{"replyToMessageId",message.id},{"text",redactor(message.bot).text(text)},{"route",route},
                 {"status",status},{"actions",actions}};
    if (suggestions.is_array() && !suggestions.empty()) data["suggestions"] = redactor(message.bot).payload(suggestions);
    if (!artifacts.empty()) data["artifacts"] = std::move(artifacts);
    if (!message.channelScope.empty() && channelMessageAuthorized(message.id,message.bot))
        queueChannelReply(message.id,reply,data);
    exec("UPDATE journal SET data=? WHERE id=?",{data,reply});
    exec("UPDATE journal SET task_id=?,data=json_set(data,'$.status',?) WHERE id=?",{linked,status,message.id});
    exec("UPDATE conversation_turns SET status='done',reply_id=?,reserved_attempt=NULL WHERE message_id=?",
         {reply,message.id});
}

void Store::Impl::pauseTurn(const Message& message) {
    const bool ja = japanese(message.text);
    const auto webhook = message.routing.value("gitEvent",Json(nullptr));
    exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {webhook.is_object() ? webhook.at("taskId") : Json(nullptr),unixTime(),message.bot});
    const auto reply = last();
    const Json data{{"messageId",reply},{"replyToMessageId",message.id},{"route","model_budget"},{"status","needs_you"},
                    {"text",ja ? "モデル呼び出しの上限に達しました。続けるには追加の1回を承認してください。"
                               : "The model call limit is exhausted. Approve one more model call to continue."}};
    exec("UPDATE journal SET data=? WHERE id=?",{data,reply});
    if (!message.channelScope.empty()) queueChannelReply(message.id,reply,data);
    exec("UPDATE journal SET data=json_set(data,'$.status','needs_you') WHERE id=?",{message.id});
    exec("UPDATE conversation_turns SET status='paused',pause_id=? WHERE message_id=?",{reply,message.id});
}

bool Store::Impl::authorizeTurn(const Message& message) {
    if (!turnPending(message.id)) return false;
    if (message.routing.contains("gitEvent")) {
        if (canReceiveGitWebhook(message.bot,message.routing.at("gitEvent"))) return true;
        exec("UPDATE journal SET data=json_set(data,'$.status','access_revoked') WHERE id=?",{message.id});
        exec("UPDATE conversation_turns SET status='done',reserved_attempt=NULL WHERE message_id=?",{message.id});
        return false;
    }
    if (message.channelScope.empty()) {
        if (Access(*this).permitsPerson(message.actor,message.bot,Role::decide)) return true;
        finishTurn(message,"You no longer have permission to message this Bot.",nullptr,"access");
    } else {
        if (channelMessageAuthorized(message.id,message.bot)) return true;
        finishTurn(message,"This channel connection is no longer authorized.",nullptr,"channel_access");
    }
    return false;
}

bool Store::Impl::runTurn(Id id, Model& model, T3& t3, const Runtime::SoulLoader& soul,
                          const std::function<Seconds()>& clock, const BotTools& settings, GitHost* gitHost) {
    Message message;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!turnPending(id)) return true;
        message = loadTurnMessage(id);
        exec("INSERT OR IGNORE INTO conversation_turns(message_id,status) VALUES(?,'running')",{id});
        transaction.commit();
    }
    const auto webhook = message.routing.value("gitEvent",Json(nullptr));
    const bool notification = webhook.is_object();
    const bool web = message.channelScope.empty() && !notification;
    auto tools = notification ? std::vector<std::string>{"reply"} : turnTools;
    if (gitHost && notification) {
        tools.push_back("git_pr_status");
        if (!field(webhook,"headSha").empty()) tools.push_back("git_commit_status");
        if (webhook.at("jobId") != 0) tools.push_back("git_read_ci_logs");
    } else if (gitHost && web) tools.insert(tools.end(),gitTools().begin(),gitTools().end());
    if (web) {
        tools.insert(tools.end(),webTools.begin(),webTools.end());
        tools.insert(tools.end(),botToolNames().begin(),botToolNames().end());
    }
    if (!message.channelScope.empty()) tools.insert(tools.end(),channelTools.begin(),channelTools.end());
    const auto available = [&] {
        auto offered = tools;
        auto machines = Json::array();
        if (!notification) {
            for (const auto& [machineId,machine] : localMachines)
                if (permitsLocalMachineOwner(machine->owner(),message.bot)) machines.push_back(machineId);
            if (!machines.empty()) offered.push_back("request_local_machine_connection");
        }
        return std::pair{std::move(offered),std::move(machines)};
    };
    const bool ja = japanese(message.text);
    const auto finish = [&](std::string text, std::string_view route) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (authorizeTurn(message)) finishTurn(message,std::move(text),nullptr,route);
        transaction.commit();
    };
    const auto failure = ja ? "このメッセージを処理できませんでした。もう一度送ってください。"
                            : "I couldn't process this message. Please send it again.";
    for (int step = 1; step <= maxSteps; ++step) {
        if (!message.channelScope.empty()) deliverChannels();
        std::vector<ModelCall> calls;
        bool saveCalls = false, botEffects = false, sessionEffect = false;
        bool hasWebEvidence = false;
        {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!authorizeTurn(message)) { transaction.commit(); return true; }
            auto row = query("SELECT decision,result,json_type(effect) FROM conversation_steps WHERE message_id=? AND step=?",
                             {id,step});
            if (row.next()) {
                if (!row.string(1).empty()) continue;
                botEffects = row.string(2) == "array";
                sessionEffect = row.string(2) == "object";
                if (!row.string(0).empty()) {
                    const auto [offered,machines] = available();
                    for (const auto& saved : row.json(0)) {
                        Decision call{saved.at("tool"),saved.at("args"),saved.at("reason")};
                        if (auto rejection = saved.value("rejection",std::string{}); !rejection.empty())
                            calls.push_back({std::move(call),std::move(rejection)});
                        else {
                            auto checked = checkModelCall(message.bot,std::move(call),offered,notification,
                                                          !saved.value("argumentsChecked",false));
                            saveCalls |= !checked.rejection.empty();
                            calls.push_back(std::move(checked));
                        }
                    }
                }
            }
            hasWebEvidence = query(R"SQL(
SELECT 1 FROM conversation_steps s, json_each(s.decision) call
WHERE s.message_id=? AND s.step<?
  AND json_extract(call.value,'$.tool') IN ('web_search','web_fetch')
  AND json_extract(s.result,'$[' || call.key || '].untrustedData') = 1
LIMIT 1
)SQL",{id,step}).next();
            transaction.commit();
        }
        if (calls.empty()) {
            const auto now = clock();
            BotNotes botNotes;
            try {
                if (!notification) botNotes = loadNotes(message.bot,soul,now);
            } catch (const std::exception& error) {
                const auto diagnostic = Json::parse(Json(error.what()).dump(
                    -1,' ',false,Json::error_handler_t::replace)).get<std::string>();
                finish(ja ? "このBotのノートを読み込めませんでした (" + diagnostic + ")。ファイルを修正してから、もう一度送ってください。"
                          : "I couldn't read this Bot's notes (" + diagnostic + "). Fix the file and send your message again.",
                       "error");
                return true;
            }
            ModelInput input;
            Id attempt = 0;
            {
                std::lock_guard lock(mutex);
                Transaction transaction(db);
                if (!authorizeTurn(message)) { transaction.commit(); return true; }
                std::vector<ModelImage> images;
                auto [offered,machines] = available();
                const auto context = turnInput(message,step,now,std::move(machines),images);
                input = {.task = context.at("task"),.event = context.at("event"),.history = context.at("history"),
                         .notes = botNotes,.tools = std::move(offered),.feedback = {},.config = message.routing.at("config"),
                         .images = std::move(images)};
                attempt = takeReservedConversationCall(id);
                if (!attempt) attempt = reserveModelCall(messageModelBudget(id),"conversation_turn",id,0,0);
                if (!attempt) pauseTurn(message);
                else exec("INSERT INTO conversation_steps(message_id,step,attempt_id) VALUES(?,?,?)"
                          " ON CONFLICT(message_id,step) DO UPDATE SET attempt_id=excluded.attempt_id",{id,step,attempt});
                transaction.commit();
            }
            if (!attempt) return true;
            try {
                authorizeSessionHistory(message.bot,message.channelScope,input.event.at("turn"),t3);
                authorizeBotToolHistory(message.bot,id,input);
                calls = callModel(message.bot,model,input,attempt).calls;
                if (calls.empty() || calls.size() > maxToolCalls)
                    throw ModelError("Invalid Flock call count",false);
                if (calls.size() > 1 && !std::ranges::all_of(calls,[](const auto& call) { return isBotTool(call.tool); }))
                    throw ModelError("Parallel calls must be independent Bot tools",false);
            } catch (const ModelError& error) {
                if (error.transient) return false;
                finish(failure,"error");
                return true;
            } catch (const std::exception&) {
                finish(failure,"error");
                return true;
            }
            saveCalls = true;
        }
        if (saveCalls) {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            auto saved = Json::array();
            if (turnPending(id) && !authorizeTurn(message)) { transaction.commit(); return true; }
            for (const auto& call : calls) {
                Json entry{{"tool",call.tool},{"args",call.args},{"reason",call.reason},{"argumentsChecked",true}};
                if (!call.rejection.empty()) entry["rejection"] = call.rejection;
                saved.push_back(std::move(entry));
            }
            exec("UPDATE conversation_steps SET decision=? WHERE message_id=? AND step=? AND result IS NULL",{saved,id,step});
            transaction.commit();
        }

        const auto& decision = calls.front();
        if (botEffects || isBotTool(decision.tool)) {
            if (!runBotTools({message.bot,id,settings,hasWebEvidence},step,calls)) return true;
            continue;
        }
        if (!decision.rejection.empty() && !sessionEffect) {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!authorizeTurn(message)) { transaction.commit(); return true; }
            auto result = toolFailure(decision.rejection);
            if (isGitTool(decision.tool) && gitConversationEffectStarted(id,step)) {
                result["delivery"] = "unknown";
                result["error"] = "The host outcome is unknown. Check the target before retrying.";
            }
            saveTurnResults(id,step,Json::array({result}));
            transaction.commit();
            continue;
        }
        if (!sessionEffect && hasWebEvidence && !allowedAfterWebEvidence(decision)) {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!authorizeTurn(message)) { transaction.commit(); return true; }
            saveTurnResults(id,step,Json::array({webEvidenceActionDenied()}));
            transaction.commit();
            continue;
        }

        if (decision.tool == "message_session" || decision.tool == "interrupt_session") {
            Json effect;
            {
                std::lock_guard lock(mutex);
                Transaction transaction(db);
                if (!authorizeTurn(message)) { transaction.commit(); return true; }
                auto saved = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND effect IS NOT NULL",
                                   {id,step});
                if (saved.next()) {
                    saveTurnResults(id,step,Json::array({Json{{"ok",false},{"taskId",saved.json(0).at("taskId")},
                        {"delivery","unknown"},{"error","Flock restarted while sending this, so T3 may have received it. "
                                                       "Check the Session before sending it again"}}}));
                } else {
                    try {
                        effect = prepareSessionEffect(message,step,decision);
                        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",{effect,id,step});
                    } catch (const ToolError& error) {
                        auto failed = error.details;
                        failed["ok"] = false;
                        failed["error"] = error.what();
                        saveTurnResults(id,step,Json::array({redactor(message.bot).record(failed)}));
                    }
                }
                transaction.commit();
            }
            if (effect.is_null()) continue;
            const auto outcome = redactor(message.bot).record(performSessionEffect(t3,effect));
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (!query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",{id,step}).next())
                return true;
            if (outcome.value("ok",false)) {
                Json entry = {{"messageId",id},{"step",step},{"run",outcome.value("run",effect.at("run"))}};
                if (effect.contains("text")) entry["text"] = effect.at("text");
                journal(effect.at("taskId"),effect.at("kind") == "steer" ? "instruction_steered" : "interrupt_requested",entry);
            }
            saveTurnResults(id,step,Json::array({outcome}));
            transaction.commit();
            continue;
        }

        Json result;
        if (isGitTool(decision.tool)) {
            {
                std::lock_guard lock(mutex);
                Transaction transaction(db);
                if (!authorizeTurn(message)) { transaction.commit(); return true; }
                transaction.commit();
            }
            result = gitConversationTool(gitHost,message,step,decision);
        } else if (decision.tool == "flock_session_search" || decision.tool == "flock_session_events") {
            result = sessionReadTool(message.bot,message.channelScope,decision,t3);
        } else if (decision.tool == "check_environment_readiness" && message.channelScope.empty()) {
            try {
                result = readiness(message.bot,message.routing,decision.args,t3);
                result["ok"] = true;
            } catch (const Conflict& error) { result = {{"ok",false},{"error",error.what()}}; }
        }
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!authorizeTurn(message)) { transaction.commit(); return true; }
        if (decision.tool == "reply") {
            try {
                const auto& args = decision.args;
                if (!args.is_object()) throw ToolError("Tool arguments must be an object");
                auto reply = text(args,"text",8000,true);
                Json suggestions = Json::array();
                if (const auto list = args.find("suggestions"); list != args.end() && !list->is_null()) {
                    if (!list->is_array() || list->size() > 4) throw ToolError("suggestions must be up to 4 strings");
                    for (const auto& item : *list) {
                        if (!item.is_string() || !hasText(item.get<std::string>()) || item.get<std::string>().size() > 200)
                            throw ToolError("Each suggestion must be a nonempty string up to 200 bytes");
                        if (std::ranges::find(suggestions,item) != suggestions.end())
                            throw ToolError("Each suggestion must be distinct so the user's choice is unambiguous");
                        suggestions.push_back(item);
                    }
                }
                saveTurnResults(id,step,Json::array({Json{{"ok",true}}}));
                finishTurn(message,std::move(reply),suggestions,"reply");
                transaction.commit();
                return true;
            } catch (const ToolError& error) {
                result = error.details;
                result["ok"] = false;
                result["error"] = error.what();
            }
        } else if (result.is_null()) {
            exec("SAVEPOINT turn_tool");
            try {
                result = applyTurnTool(message,step,decision,clock());
                exec("RELEASE turn_tool");
            } catch (const std::exception& error) {
                exec("ROLLBACK TO turn_tool");
                exec("RELEASE turn_tool");
                if (const auto* failed = dynamic_cast<const ToolError*>(&error)) result = failed->details;
                else result = Json::object();
                result["ok"] = false;
                result["error"] = error.what();
            }
        }
        saveTurnResults(id,step,Json::array({redactor(message.bot).record(result)}));
        transaction.commit();
    }
    finish(ja ? "このメッセージの処理が長くなりすぎたため中断しました。必要なら、もう一度送ってください。"
              : "I stopped because handling this message took too many steps. Send it again if needed.",
           "error");
    return true;
}

Json Store::pausedConversations(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT j.data FROM conversation_turns t JOIN journal j ON j.id=t.pause_id
WHERE j.conversation_bot_id=? AND t.status='paused' ORDER BY j.id DESC
)SQL",{bot});
    auto result = Json::array();
    while (rows.next()) result.push_back(rows.json(0));
    return result;
}

void Store::Impl::approveMessageModelCall(Id message, Id waitMessageId) {
    const bool toolPending = query("SELECT 1 FROM conversation_steps WHERE message_id=?"
                                   " AND result IS NULL AND json_type(effect)='array'",{message}).next();
    const auto attempt = reserveExtraModelCall(messageModelBudget(message),toolPending ? "bot_tool" : "conversation_turn",message,0,0);
    exec("UPDATE conversation_turns SET status='running',pause_id=NULL,reserved_attempt=? WHERE message_id=?",
             {attempt,message});
    exec("UPDATE journal SET data=json_remove(data,'$.status') WHERE id=?",{message});
    exec("UPDATE journal SET data=json_set(data,'$.status','done') WHERE id=?",{waitMessageId});
}
}
