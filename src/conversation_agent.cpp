#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
using conversation_storage::hasText;
using conversation_storage::integer;
using conversation_storage::messageSize;
using conversation_storage::verifiedEnvironment;

struct conversation::Message {
    Id id = 0, replyTo = 0;
    std::string bot, text;
    Json routing;
};

namespace {
using conversation::Message;

constexpr int activeTaskLimit = 12;
constexpr int terminalTaskLimit = 8;
constexpr int historyLimit = 24;
constexpr int findLimit = 10;
constexpr int maxSteps = 24;
constexpr std::size_t historyBytes = 24000;
constexpr std::size_t resultChunk = 6000;
constexpr std::size_t sourceSize = 8192;

const std::vector<std::string> turnTools{"start_task","send_to_task","change_task","answer_task_question",
                                         "cancel_task","message_session","interrupt_session","read_result",
                                         "find_tasks","reply"};

bool japanese(std::string_view text) {
    return std::any_of(text.begin(),text.end(),[](unsigned char byte) {
        return byte == 0xe3 || (byte >= 0xe4 && byte <= 0xe9);
    });
}

struct ToolError : std::runtime_error {
    Json details;
    explicit ToolError(const std::string& message, Json extra = Json::object())
        : std::runtime_error(message), details(std::move(extra)) {}
};

std::string text(const Json& args, std::string_view key, std::size_t limit, bool required) {
    const auto value = args.find(key);
    if (value == args.end() || value->is_null()) {
        if (required) throw ToolError(std::string(key) + " is required");
        return {};
    }
    if (!value->is_string()) throw ToolError(std::string(key) + " must be a string");
    auto result = value->get<std::string>();
    if (!hasText(result)) {
        if (required) throw ToolError(std::string(key) + " must not be empty");
        return {};
    }
    if (result.size() > limit)
        throw ToolError(std::string(key) + " exceeds " + std::to_string(limit) + " bytes");
    return result;
}

Id identifier(const Json& args, std::string_view key, bool required, long double minimum = 1) {
    const auto value = args.find(key);
    if (value == args.end() || value->is_null()) {
        if (required) throw ToolError(std::string(key) + " is required");
        return 0;
    }
    if (!value->is_number_integer() || value->get<long double>() < minimum)
        throw ToolError(std::string(key) + (minimum > 0 ? " must be a positive integer" : " must be a non-negative integer"));
    return value->get<Id>();
}

Json waitView(const Json& wait, Id messageId) {
    if (!wait.is_object()) return nullptr;
    Json view = {{"waitId",wait.value("id",Id{0})},{"reason",field(wait,"reason")},
                 {"text",bounded(field(wait,"text"),1000)}};
    if (const auto choices = wait.find("choices"); choices != wait.end() && choices->is_array()) {
        auto& list = view["choices"] = Json::array();
        for (const auto& choice : *choices)
            if (choice.is_string() && list.size() < 12) list.push_back(bounded(choice.get<std::string>(),200));
    }
    view["answerable"] = field(wait,"reason") == "user_question" && integer(wait,"messageId") < messageId;
    return view;
}

Json taskState(const Json& task, Id messageId) {
    Json view = {{"generation",task.at("generation")},{"revisionId",task.value("currentRevision",Id{0})},
                 {"state",task.at("state")},{"doneWhen",bounded(field(task,"doneWhen"),1600)},
                 {"replanning",task.value("replanning",false)},{"wait",waitView(task.value("wait",Json(nullptr)),messageId)}};
    if (const auto intent = field(task,"intent"); !intent.empty()) view["intent"] = bounded(intent,1000);
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

std::optional<Message> Store::Impl::turnMessage(Id id) const {
    auto row = query(R"SQL(
SELECT m.reply_to,m.bot_id,m.text,m.routing
FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND j.kind='conversation_message' AND json_extract(j.data,'$.status') IS NULL
AND NOT EXISTS (
  SELECT 1 FROM journal prior WHERE prior.conversation_bot_id=m.bot_id AND prior.id<m.id
  AND prior.kind='conversation_message' AND json_extract(prior.data,'$.status') IS NULL)
)SQL",{id});
    if (!row.next()) return std::nullopt;
    return Message{id,row.number(0),row.string(1),row.string(2),row.json(3)};
}

Json Store::Impl::ownTask(Id taskId, std::string_view bot) const {
    try {
        auto task = load(taskId);
        if (field(task,"botId") == bot) return task;
    } catch (const Conflict&) {}
    throw ToolError("No task with taskId " + std::to_string(taskId));
}

Json Store::Impl::resultReference(Id messageId, std::string_view bot) const {
    auto row = query(R"SQL(
SELECT t.id,t.state,j.kind,j.data,json_extract(t.data,'$.request'),coalesce(json_extract(t.data,'$.currentRevision'),0)
FROM journal j JOIN tasks t ON t.id=j.task_id
WHERE j.id=? AND t.bot_id=? AND j.kind IN ('report','completed','cancelled')
)SQL",{messageId,bot});
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
    return result;
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

Json Store::Impl::turnInput(const Message& message, int step) const {
    Json tasks = Json::array();
    std::set<Id> listed;
    const auto latestResult = [&](Id taskId) -> Json {
        auto row = query(R"SQL(
SELECT id,kind,data FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled') AND id<?
ORDER BY id DESC LIMIT 1
)SQL",{taskId,message.id});
        if (!row.next()) return nullptr;
        const auto kind = row.string(1);
        const auto data = row.json(2);
        return {{"messageId",row.number(0)},{"kind",kind},{"revisionId",integer(data,"revisionId")},
                {"excerpt",bounded(field(data,kind == "report" ? "report" : "text"),600)}};
    };
    const auto addTask = [&](Id taskId) {
        if (!listed.insert(taskId).second) return;
        const auto task = load(taskId);
        Json view = {{"taskId",taskId},{"request",bounded(field(task,"request"),800)},
                     {"environmentId",field(task,"environmentId")}};
        view.update(taskState(task,message.id));
        if (task.contains("sourceTaskId")) view["sourceTaskId"] = task.at("sourceTaskId");
        if (auto result = latestResult(taskId); !result.is_null()) view["latestResult"] = std::move(result);
        tasks.push_back(std::move(view));
    };
    bool moreTasks = false;
    {
        auto rows = query("SELECT id FROM tasks WHERE bot_id=? AND state NOT IN ('completed','cancelled')"
                          " ORDER BY id DESC LIMIT ?",{message.bot,activeTaskLimit + 1});
        for (int i = 0; rows.next(); ++i) {
            if (i == activeTaskLimit) { moreTasks = true; break; }
            addTask(rows.number(0));
        }
    }
    {
        auto rows = query("SELECT id FROM tasks WHERE bot_id=? AND state IN ('completed','cancelled')"
                          " ORDER BY id DESC LIMIT ?",{message.bot,terminalTaskLimit});
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
                    {"current",current.is_object() && integer(current,"id") == integer(data,"id")}});
                addTask(taskId);
            } else if (kind == "report" || kind == "completed" || kind == "cancelled") {
                replyTo.update({{"kind","result"},{"taskId",taskId},
                    {"text",bounded(field(data,kind == "report" ? "report" : "text"),2000)}});
                addTask(taskId);
            } else if (kind == "conversation_reply") {
                replyTo.update({{"kind","bot_message"},{"text",bounded(field(data,"text"),2000)}});
                auto original = query("SELECT id,text FROM messages WHERE id=? AND bot_id=?",
                                      {integer(data,"replyToMessageId"),message.bot});
                if (original.next())
                    replyTo["inReplyTo"] = {{"messageId",original.number(0)},{"text",bounded(original.string(1),messageSize)}};
            } else if (kind == "conversation_message") {
                replyTo.update({{"kind","user_message"},{"text",bounded(row.string(3),2000)}});
            } else {
                replyTo.update({{"kind",kind},{"text",bounded(field(data,"text"),2000)}});
            }
        } else replyTo = {{"messageId",message.replyTo},{"kind","unavailable"}};
    }

    Json history = Json::array();
    {
        auto rows = query(R"SQL(
SELECT j.id,j.task_id,j.kind,j.data,m.text FROM journal j
LEFT JOIN tasks t ON t.id=j.task_id LEFT JOIN messages m ON m.id=j.id
WHERE j.id<? AND (
  (j.conversation_bot_id=? AND j.kind IN ('conversation_message','conversation_reply')) OR
  (j.conversation_bot_id IS NULL AND t.bot_id=? AND j.kind IN ('waiting','report','completed','cancelled')))
ORDER BY j.id DESC LIMIT ?
)SQL",{message.id,message.bot,message.bot,historyLimit});
        std::size_t remaining = historyBytes;
        while (rows.next() && remaining > 0) {
            const auto kind = rows.string(2);
            const auto data = rows.json(3);
            Json entry = {{"messageId",rows.number(0)},{"kind",kind}};
            if (rows.number(1)) entry["taskId"] = rows.number(1);
            std::string body;
            if (kind == "conversation_message") {
                entry["role"] = "user";
                body = rows.string(4);
            } else if (kind == "conversation_reply") {
                entry["role"] = "bot";
                body = field(data,"text");
                if (data.contains("actions")) entry["actions"] = data.at("actions");
            } else {
                entry["role"] = "task";
                body = field(data,kind == "report" ? "report" : "text");
                if (kind == "waiting") {
                    if (field(data,"reason") != "user_question") continue;
                    entry["waitId"] = integer(data,"id");
                }
            }
            const std::size_t limit = entry["role"] == "user" ? 4000 : entry["role"] == "bot" ? 1500 : 700;
            body = bounded(body,std::min(limit,remaining));
            remaining -= body.size();
            entry["text"] = std::move(body);
            history.push_back(std::move(entry));
        }
        std::reverse(history.begin(),history.end());
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

    Json turn = Json::array();
    auto steps = query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND step<?"
                       " AND result IS NOT NULL ORDER BY step",{message.id,step});
    while (steps.next()) {
        const auto decision = steps.json(0);
        turn.push_back({{"tool",decision.at("tool")},{"args",decision.at("args")},{"result",steps.json(1)}});
    }
    return {{"task",{{"tasks",std::move(tasks)},{"moreTasks",moreTasks},{"environments",std::move(environments)}}},
            {"event",{{"kind","conversation_turn"},{"messageId",message.id},{"text",message.text},
                      {"replyTo",std::move(replyTo)},{"turn",std::move(turn)}}},
            {"history",std::move(history)}};
}

Json Store::Impl::applyTurnTool(const Message& message, int step, const Decision& decision) {
    const auto& args = decision.args;
    if (!args.is_object()) throw ToolError("Tool arguments must be an object");
    const auto& tool = decision.tool;
    const auto& bot = message.bot;
    const auto& routing = message.routing;
    const auto expectGeneration = [&](const Json& task) {
        const auto expected = identifier(args,"generation",true,0);
        if (expected != task.at("generation").get<Id>())
            throw ToolError("stale",{{"applied",0},{"taskId",task.at("id")},{"expected",{{"generation",expected}}},
                                     {"current",taskState(task,message.id)}});
    };
    const auto active = [&](const Json& task) {
        if (terminal(task) || task.at("state") == "cancelling")
            throw ToolError("The task is " + task.at("state").get<std::string>(),
                            {{"taskId",task.at("id")},{"current",taskState(task,message.id)}});
    };
    const auto source = [&](const Json* target) -> Json {
        const auto sourceId = identifier(args,"sourceResultMessageId",false);
        if (!sourceId) return nullptr;
        const auto result = resultReference(sourceId,bot);
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
        const auto task = ownTask(identifier(args,"taskId",true),bot);
        expectGeneration(task);
        active(task);
        const auto instruction = text(args,"instruction",messageSize,true);
        const auto result = source(&task);
        std::optional<TaskChange> change;
        if (tool == "change_task") {
            const auto revision = identifier(args,"revisionId",true,0);
            if (revision != task.value("currentRevision",Id{0}))
                throw ToolError("stale",{{"applied",0},{"taskId",task.at("id")},
                    {"expected",{{"revisionId",revision}}},{"current",taskState(task,message.id)}});
            change = TaskChange{text(args,"intent",8000,false),text(args,"doneWhen",8000,false),
                                text(args,"environment",256,false)};
            if (change->intent.empty() && change->doneWhen.empty() && change->environment.empty())
                throw ToolError("change_task needs intent, doneWhen, or environment; use send_to_task for other instructions");
            if (!change->environment.empty() && verifiedEnvironment(task,routing,change->environment).empty())
                throw ToolError("Unknown environment " + change->environment + "; use one of task.environments");
        }
        const bool conflicting = acceptInstruction(task.at("id"),task.value("currentRevision",Id{0}),message.id,step,
            instruction,result.is_null() ? Json(nullptr) : sourceRef(result),routing,change ? &*change : nullptr);
        return {{"ok",true},{"effect",conflicting ? "replanning" : "accepted"},{"taskId",task.at("id")}};
    }
    if (tool == "answer_task_question") {
        const auto task = ownTask(identifier(args,"taskId",true),bot);
        const auto waitId = identifier(args,"waitId",true);
        const auto answer = text(args,"text",8000,true);
        if (message.replyTo) {
            auto anchor = query("SELECT task_id,json_extract(data,'$.id') FROM journal WHERE id=? AND kind='waiting'",
                                {message.replyTo});
            if (!anchor.next() || anchor.number(0) != task.at("id").get<Id>() || anchor.number(1) != waitId)
                throw ToolError("This message replies to something else, so it can only answer the question it replies to");
        }
        if (task.value("replanning",false))
            throw ToolError("The task is replanning, so this question will close. Its Bot asks again if it still needs to know",
                            {{"taskId",task.at("id")},{"current",taskState(task,message.id)}});
        const auto& wait = task.at("wait");
        if (task.at("state") != "waiting" || !wait.is_object() || integer(wait,"id") != waitId)
            throw ToolError("That question is no longer open",{{"taskId",task.at("id")},{"current",taskState(task,message.id)}});
        if (field(wait,"reason") != "user_question")
            throw ToolError("Only user_question waits can be answered; approvals use their own controls");
        if (integer(wait,"messageId") >= message.id)
            throw ToolError("That question opened after this message was sent, so this message cannot answer it");
        if (wait.value("run",Json(nullptr)) != task.value("activeRun",Json(nullptr)) ||
            integer(wait,"eventId") != task.value("eventId",Id{0}))
            throw ToolError("That question is no longer current");
        operate(task.at("id"),waitId,"answer",{{"text",answer},{"messageId",message.id}},true);
        return {{"ok",true},{"effect","answered"},{"taskId",task.at("id")}};
    }
    if (tool == "cancel_task") {
        const auto task = ownTask(identifier(args,"taskId",true),bot);
        expectGeneration(task);
        active(task);
        operate(task.at("id"),0,"cancel",Json::object(),true);
        const auto state = load(task.at("id")).at("state");
        return {{"ok",true},{"effect",state == "cancelled" ? "cancelled" : "cancelling"},{"taskId",task.at("id")}};
    }
    if (tool == "read_result") {
        const auto result = resultReference(identifier(args,"messageId",true),bot);
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
SELECT id FROM tasks WHERE bot_id=? AND (?='' OR instr(lower(json_extract(data,'$.request')),lower(?))>0)
AND (?=0 OR id<?) ORDER BY id DESC LIMIT ?
)SQL",{bot,search,search,before,before,findLimit + 1});
        Json found = Json::array();
        Id last = 0;
        bool more = false;
        while (rows.next()) {
            if (static_cast<int>(found.size()) == findLimit) { more = true; break; }
            last = rows.number(0);
            const auto task = load(last);
            Json view = {{"taskId",last},{"request",bounded(field(task,"request"),300)},
                         {"environmentId",field(task,"environmentId")}};
            view.update(taskState(task,message.id));
            auto result = query("SELECT id,kind FROM journal WHERE task_id=? AND kind IN ('report','completed','cancelled')"
                                " AND id<? ORDER BY id DESC LIMIT 1",{last,message.id});
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
    const auto task = ownTask(identifier(args,"taskId",true),message.bot);
    const auto expected = identifier(args,"generation",true,0);
    if (expected != task.at("generation").get<Id>())
        throw ToolError("stale",{{"applied",0},{"taskId",task.at("id")},{"expected",{{"generation",expected}}},
                                 {"current",taskState(task,message.id)}});
    if (terminal(task) || task.at("state") == "cancelling")
        throw ToolError("The task is " + task.at("state").get<std::string>(),{{"taskId",task.at("id")}});
    const auto& run = task.at("activeRun");
    if (!run.is_object() || field(run,"runId").empty())
        throw ToolError("The task has no running Session. Use send_to_task to pass it on at the next safe point",
                        {{"taskId",task.at("id")},{"current",taskState(task,message.id)}});
    Json effect = {{"kind",decision.tool == "message_session" ? "steer" : "interrupt"},{"taskId",task.at("id")},
                   {"environmentId",field(run,"environmentId").empty() ? field(task,"environmentId") : field(run,"environmentId")},
                   {"projectId",field(task,"projectId")},{"config",task.at("config")},{"run",run},
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
    auto steps = query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND result IS NOT NULL"
                       " ORDER BY step",{message.id});
    while (steps.next()) {
        const auto result = steps.json(1);
        if (!result.value("ok",false) || !result.contains("effect")) continue;
        actions.push_back({{"tool",steps.json(0).at("tool")},{"taskId",result.at("taskId")},{"effect",result.at("effect")}});
        touched.insert(result.at("taskId").get<Id>());
    }
    if (route == "error" && !actions.empty()) {
        const bool ja = japanese(message.text);
        text += ja ? "\n\nこのメッセージで実行済みの操作:" : "\n\nAlready done for this message:";
        for (const auto& action : actions) {
            const auto task = load(action.at("taskId"));
            text += "\n- " + field(action,"effect") + ": " + requestName(task);
        }
    }
    const auto status = effectStatus(actions);
    const Json linked = touched.size() == 1 ? Json(*touched.begin()) : Json(nullptr);
    exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {linked,unixTime(),message.bot});
    const auto reply = last();
    Json data = {{"messageId",reply},{"replyToMessageId",message.id},{"text",text},{"route",route},
                 {"status",status},{"actions",actions}};
    if (suggestions.is_array() && !suggestions.empty()) data["suggestions"] = suggestions;
    exec("UPDATE journal SET data=? WHERE id=?",{data,reply});
    exec("UPDATE journal SET task_id=?,data=? WHERE id=?",{linked,Json{{"status",status}},message.id});
    exec("UPDATE conversation_turns SET status='done',reply_id=?,reserved_attempt=NULL WHERE message_id=?",
         {reply,message.id});
}

void Store::Impl::pauseTurn(const Message& message) {
    const bool ja = japanese(message.text);
    exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_reply','{}',?)",
         {unixTime(),message.bot});
    const auto reply = last();
    exec("UPDATE journal SET data=? WHERE id=?",
         {Json{{"messageId",reply},{"replyToMessageId",message.id},{"route","model_budget"},{"status","needs_you"},
               {"text",ja ? "モデル呼び出しの上限に達しました。続けるには追加の1回を承認してください。"
                          : "The model call limit is exhausted. Approve one more model call to continue."}},reply});
    exec("UPDATE journal SET data=json_object('status','needs_you') WHERE id=?",{message.id});
    exec("UPDATE conversation_turns SET status='paused',pause_id=? WHERE message_id=?",{reply,message.id});
}

bool Store::Impl::runTurn(Id id, Model& model, T3& t3, const Runtime::NotesLoader& notes) {
    std::optional<Message> message;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        message = turnMessage(id);
        if (message) exec("INSERT OR IGNORE INTO conversation_turns(message_id,status) VALUES(?,'running')",{id});
        transaction.commit();
    }
    if (!message) return true;
    const bool ja = japanese(message->text);
    const auto finish = [&](std::string text, std::string_view route) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!turnMessage(id)) return;
        finishTurn(*message,std::move(text),nullptr,route);
        transaction.commit();
    };
    BotNotes botNotes;
    try {
        botNotes = notes(message->bot);
    } catch (const std::exception& error) {
        const auto diagnostic = Json::parse(Json(error.what()).dump(
            -1,' ',false,Json::error_handler_t::replace)).get<std::string>();
        finish(ja ? "このBotのノートを読み込めませんでした (" + diagnostic + ")。ファイルを修正してから、もう一度送ってください。"
                  : "I couldn't read this Bot's notes (" + diagnostic + "). Fix the file and send your message again.",
               "error");
        return true;
    }
    const auto failure = ja ? "このメッセージを処理できませんでした。もう一度送ってください。"
                            : "I couldn't process this message. Please send it again.";
    for (int step = 1; step <= maxSteps; ++step) {
        std::optional<Decision> decision;
        {
            std::lock_guard lock(mutex);
            Transaction transaction(db,Transaction::Mode::read);
            if (!turnMessage(id)) return true;
            auto row = query("SELECT decision,result FROM conversation_steps WHERE message_id=? AND step=?",{id,step});
            if (row.next()) {
                if (!row.string(1).empty()) continue;
                if (!row.string(0).empty()) {
                    const auto saved = row.json(0);
                    decision = Decision{saved.at("tool"),saved.at("args"),saved.value("reason",""),saved.value("usage",Json::object())};
                }
            }
            transaction.commit();
        }
        if (!decision) {
            ModelInput input;
            Id attempt = 0;
            {
                std::lock_guard lock(mutex);
                Transaction transaction(db);
                if (!turnMessage(id)) return true;
                const auto context = turnInput(*message,step);
                input = {.task = context.at("task"),.event = context.at("event"),.history = context.at("history"),
                         .notes = botNotes,.tools = turnTools,.feedback = {},.config = message->routing.at("config")};
                auto reserved = query("SELECT reserved_attempt FROM conversation_turns WHERE message_id=?"
                                      " AND reserved_attempt IS NOT NULL",{id});
                if (reserved.next()) {
                    attempt = reserved.number(0);
                    exec("UPDATE conversation_turns SET reserved_attempt=NULL WHERE message_id=?",{id});
                } else attempt = reserveModelCall(messageModelBudget(id),"conversation_turn",id,0,0);
                if (!attempt) pauseTurn(*message);
                else exec("INSERT INTO conversation_steps(message_id,step,attempt_id) VALUES(?,?,?)"
                          " ON CONFLICT(message_id,step) DO UPDATE SET attempt_id=excluded.attempt_id",{id,step,attempt});
                transaction.commit();
            }
            if (!attempt) return true;
            try {
                decision = callModel(model,input,attempt);
            } catch (const ModelError& error) {
                if (error.transient) return false;
                finish(failure,"error");
                return true;
            } catch (const std::exception&) {
                finish(failure,"error");
                return true;
            }
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            exec("UPDATE conversation_steps SET decision=? WHERE message_id=? AND step=? AND result IS NULL",
                 {Json{{"tool",decision->tool},{"args",decision->args},{"reason",decision->reason},
                       {"usage",decision->usage}},id,step});
            transaction.commit();
        }

        if (decision->tool == "message_session" || decision->tool == "interrupt_session") {
            Json effect;
            {
                std::lock_guard lock(mutex);
                Transaction transaction(db);
                const auto current = turnMessage(id);
                if (!current) return true;
                auto saved = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND effect IS NOT NULL",
                                   {id,step});
                if (saved.next()) {
                    // The earlier attempt may have reached T3 before the restart. Report it instead of sending again.
                    exec("UPDATE conversation_steps SET result=? WHERE message_id=? AND step=? AND result IS NULL",
                         {Json{{"ok",false},{"taskId",saved.json(0).at("taskId")},{"delivery","unknown"},
                               {"error","Flock restarted while sending this, so T3 may have received it. "
                                        "Check the Session before sending it again"}},id,step});
                } else {
                    try {
                        effect = prepareSessionEffect(*current,step,*decision);
                        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",{effect,id,step});
                    } catch (const ToolError& error) {
                        auto failed = error.details;
                        failed["ok"] = false;
                        failed["error"] = error.what();
                        exec("UPDATE conversation_steps SET result=? WHERE message_id=? AND step=?",{failed,id,step});
                    }
                }
                transaction.commit();
            }
            if (effect.is_null()) continue;
            const auto outcome = performSessionEffect(t3,effect);
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            // Record what T3 did even if an older turn became runnable meanwhile.
            if (!query("SELECT 1 FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",{id,step}).next())
                return true;
            if (outcome.value("ok",false)) {
                Json entry = {{"messageId",id},{"step",step},{"run",outcome.value("run",effect.at("run"))}};
                if (effect.contains("text")) entry["text"] = effect.at("text");
                journal(effect.at("taskId"),effect.at("kind") == "steer" ? "instruction_steered" : "interrupt_requested",entry);
            }
            exec("UPDATE conversation_steps SET result=? WHERE message_id=? AND step=?",{outcome,id,step});
            transaction.commit();
            continue;
        }

        std::lock_guard lock(mutex);
        Transaction transaction(db);
        const auto current = turnMessage(id);
        if (!current) return true;
        Json result;
        if (decision->tool == "reply") {
            try {
                const auto& args = decision->args;
                if (!args.is_object()) throw ToolError("Tool arguments must be an object");
                auto reply = text(args,"text",8000,true);
                Json suggestions = Json::array();
                if (const auto list = args.find("suggestions"); list != args.end() && !list->is_null()) {
                    if (!list->is_array() || list->size() > 4) throw ToolError("suggestions must be up to 4 strings");
                    for (const auto& item : *list) {
                        if (!item.is_string() || !hasText(item.get<std::string>()) || item.get<std::string>().size() > 200)
                            throw ToolError("Each suggestion must be a nonempty string up to 200 bytes");
                        suggestions.push_back(item);
                    }
                }
                exec("UPDATE conversation_steps SET result=? WHERE message_id=? AND step=?",
                     {Json{{"ok",true}},id,step});
                finishTurn(*current,std::move(reply),suggestions,"reply");
                transaction.commit();
                return true;
            } catch (const ToolError& error) {
                result = error.details;
                result["ok"] = false;
                result["error"] = error.what();
            }
        } else {
            exec("SAVEPOINT turn_tool");
            try {
                result = applyTurnTool(*current,step,*decision);
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
        exec("UPDATE conversation_steps SET result=? WHERE message_id=? AND step=?",{result,id,step});
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

void Store::approveMessageModelCall(std::string_view botId, Id waitMessageId) {
    std::lock_guard lock(p_->mutex);
    Transaction transaction(p_->db);
    auto paused = p_->query(R"SQL(
SELECT t.message_id FROM conversation_turns t JOIN messages m ON m.id=t.message_id
WHERE m.bot_id=? AND t.pause_id=? AND t.status='paused'
)SQL",{botId,waitMessageId});
    if (!paused.next()) throw Conflict("Waiting item is no longer current");
    const auto message = paused.number(0);
    const auto budget = p_->messageModelBudget(message);
    p_->grantModelCall(budget);
    const auto attempt = p_->reserveModelCall(budget,"conversation_turn",message,0,0);
    if (!attempt) throw Conflict("The approved model call could not be reserved");
    p_->exec("UPDATE conversation_turns SET status='running',pause_id=NULL,reserved_attempt=? WHERE message_id=?",
             {attempt,message});
    p_->exec("UPDATE journal SET data='{}' WHERE id=?",{message});
    p_->exec("UPDATE journal SET data=json_set(data,'$.status','done') WHERE id=?",{waitMessageId});
    transaction.commit();
}
}
