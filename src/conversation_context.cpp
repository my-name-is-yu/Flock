#include "storage.hpp"
#include "conversation_router.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <tuple>

namespace flockv0 {
using namespace conversation;
using conversation_storage::hasText;
using conversation_storage::integer;
using conversation_storage::messageSize;
using conversation_storage::verifiedEnvironment;

namespace {
Result resultRow(const Statement& row) {
    const auto data = row.json(4);
    Result result;
    result.taskId = row.number(0);
    result.messageId = row.number(1);
    result.revision = integer(data,"revisionId");
    result.taskName = bounded(row.string(2),800);
    result.state = row.string(3);
    result.text = field(data,row.string(5) == "report" ? "report" : "text");
    if (row.string(5) == "report") {
        result.provenance = {{"environmentId",field(data,"environmentId")},
            {"threadId",data.contains("run") ? field(data["run"],"threadId") : ""},
            {"runId",field(data,"resultRunId")},{"itemId",field(data,"itemId")}};
    } else if (data.contains("source")) result.provenance = data.at("source");
    return result;
}

constexpr auto resultQuery = R"SQL(
SELECT t.id,j.id,json_extract(t.data,'$.request'),t.state,j.data,j.kind
FROM journal j JOIN tasks t ON t.id=j.task_id
WHERE t.bot_id=? AND j.kind IN ('completed','report','cancelled')
)SQL";

constexpr int activeTaskLimit = 12;
constexpr int terminalTaskLimit = 8;
constexpr int waitLimit = 12;
constexpr int recentTurnLimit = 8;
constexpr int namePageSize = 4;
constexpr int resultLimit = 8;

std::set<std::string> explicitNames(std::string_view text) {
    using namespace std::literals;
    constexpr std::array quotes{
        std::pair{"「"sv,"」"sv},std::pair{"『"sv,"』"sv},std::pair{"“"sv,"”"sv},
        std::pair{"\""sv,"\""sv},std::pair{"`"sv,"`"sv}};
    std::set<std::string> names;
    for (const auto& [left,right] : quotes) {
        for (std::size_t start = 0; names.size() < namePageSize;) {
            const auto open = text.find(left,start);
            if (open == text.npos) break;
            const auto end = text.find(right,open+left.size());
            if (end == text.npos) break;
            const auto title = text.substr(open+left.size(),end-open-left.size());
            if (title.size() <= 800 && hasText(title)) names.emplace(title);
            start = end+right.size();
        }
    }
    return names;
}
}

Json Store::Impl::captureConversationWaits(std::string_view bot) const {
    auto rows = query(R"SQL(
SELECT id,json_extract(data,'$.wait.id') FROM tasks
WHERE bot_id=? AND state='waiting' AND json_extract(data,'$.wait.reason') IN ('user_question','budget')
ORDER BY id DESC LIMIT ?
)SQL",{bot,waitLimit+1});
    auto items = Json::array();
    while (rows.next()) {
        if (items.size() == waitLimit) return {{"items",std::move(items)},{"more",true}};
        items.push_back({{"taskId",rows.number(0)},{"waitId",rows.number(1)}});
    }
    return {{"items",std::move(items)},{"more",false}};
}

Context Store::Impl::routeContext(std::string_view bot, Id id, std::string_view text,
                                  Id replyTo, const Json& routing) {
    Context context;
    const auto addDestination = [&](const std::string& label, const std::string& id) {
        if (label.empty() || id.empty() || context.destinationIds.contains(label)) return;
        context.destinationIds[label] = id;
        context.environments.push_back(label);
    };
    const auto initial = field(routing,"environmentId");
    addDestination(initial,initial);
    const auto& config = routing.at("config");
    if (const auto aliases = config.find("conversationEnvironments"); aliases != config.end() && aliases->is_object()) {
        for (auto alias = aliases->begin(); alias != aliases->end(); ++alias) {
            const auto canonical = verifiedEnvironment(routing,routing,alias.key());
            addDestination(alias.key(),canonical);
        }
        for (const auto& alias : *aliases)
            if (alias.is_string()) {
                const auto id = alias.get<std::string>();
                addDestination(id,verifiedEnvironment(routing,routing,id));
            }
    }

    constexpr auto taskQuery = R"SQL(
SELECT id,state,generation,json_extract(data,'$.request'),json_extract(data,'$.intent'),
       json_extract(data,'$.doneWhen'),json_extract(data,'$.currentRevision'),
       json_extract(data,'$.replanning'),json_extract(data,'$.wait'),
       coalesce(json_extract(data,'$.environmentId'),environment_id),project_id
FROM tasks WHERE bot_id=?
)SQL";
    std::set<Id> taskIds;
    const auto rememberTask = [&](const Statement& row) {
        const Id taskId = row.number(0);
        if (!taskIds.insert(taskId).second) return;
        const auto wait = row.string(8).empty() ? Json(nullptr) : row.json(8);
        conversation::Task task;
        task.id = taskId;
        task.fullName = row.string(3);
        task.name = bounded(task.fullName,800);
        task.intent = bounded(row.string(4),1000);
        if (task.intent.empty()) task.intent = task.name;
        task.state = row.string(1);
        if (row.number(7) && task.state != "completed" && task.state != "cancelled" && task.state != "cancelling")
            task.state = "replanning";
        task.generation = row.number(2);
        task.revision = row.number(6);
        task.doneWhen = bounded(row.string(5),1600);
        task.environmentId = bounded(row.string(9),256);
        task.projectId = bounded(row.string(10),256);
        task.waitId = integer(wait,"id");
        task.waitReason = field(wait,"reason");
        if (task.waitReason == "user_question" || task.waitReason == "budget")
            task.waitText = bounded(field(wait,"text"),1000);
        if (task.waitReason == "user_question" && wait.contains("choices") && wait["choices"].is_array()) {
            for (const auto& choice : wait["choices"]) {
                if (task.waitChoices.size() == 12) break;
                if (choice.is_string()) task.waitChoices.push_back(bounded(choice.get_ref<const std::string&>(),200));
            }
        }
        context.tasks.push_back(std::move(task));
    };
    const auto rememberTaskId = [&](Id taskId) {
        if (taskIds.contains(taskId)) return;
        const auto sql = std::string(taskQuery)+" AND id=?";
        auto row = query(sql.c_str(),{bot,taskId});
        if (row.next()) rememberTask(row);
    };
    const auto readTasks = [&](bool active, int limit) {
        const auto sql = std::string(taskQuery)
            +(active ? " AND state NOT IN ('completed','cancelled')" : " AND state IN ('completed','cancelled')")
            +" ORDER BY id DESC LIMIT ?";
        auto rows = query(sql.c_str(),{bot,limit+1});
        for (int i = 0; rows.next(); ++i) {
            if (i == limit) return true;
            rememberTask(rows);
        }
        return false;
    };
    context.moreTasks = readTasks(true,activeTaskLimit);
    readTasks(false,terminalTaskLimit);

    if (const auto waits = routing.find("waitsAtIntake"); waits != routing.end()) {
        context.moreWaits = waits->at("more").get<bool>();
        for (const auto& item : waits->at("items")) {
            const Id taskId = integer(item,"taskId"), waitId = integer(item,"waitId");
            rememberTaskId(taskId);
            const auto task = std::ranges::find(context.tasks,taskId,&conversation::Task::id);
            if (task != context.tasks.end() && task->waitId == waitId && waitId > 0) {
                task->waitAtIntake = true;
                continue;
            }
            auto prior = query(R"SQL(
SELECT w.data FROM waits w JOIN tasks t ON t.id=w.task_id WHERE w.id=? AND w.task_id=? AND t.bot_id=?
)SQL",{waitId,taskId,bot});
            if (!prior.next()) continue;
            const auto data = prior.json(0);
            context.expiredWaits.push_back({{"taskId",taskId},{"waitId",waitId},
                {"reason",field(data,"reason")},{"text",bounded(field(data,"text"),1000)}});
        }
        context.staleWaits = !context.expiredWaits.empty();
    }

    const auto readResult = [&](Id sourceId, const Json& excerpt = nullptr) -> std::optional<Result> {
        const auto sql = std::string(resultQuery)+" AND j.id=? AND j.id<?";
        auto row = query(sql.c_str(),{bot,sourceId,id});
        if (!row.next()) return std::nullopt;
        auto result = resultRow(row);
        if (!excerpt.is_null()) {
            if (integer(excerpt,"taskId") != result.taskId || integer(excerpt,"revisionId") != result.revision)
                return std::nullopt;
            const auto selected = field(excerpt,"text");
            if (!selected.empty()) {
                if (result.text.find(selected) == std::string::npos) return std::nullopt;
                result.text = selected;
                result.option = static_cast<int>(integer(excerpt,"option"));
            }
        }
        rememberTaskId(result.taskId);
        return result;
    };
    const auto savedTarget = [&](const Json& data) {
        const auto taskId = integer(data,"taskId");
        if (taskId) rememberTaskId(taskId);
        return Target{taskId,integer(data,"generation"),integer(data,"revisionId"),integer(data,"waitId")};
    };
    const auto savedRevision = [&](const Json& data) -> std::optional<RevisionDraft> {
        if (!data.is_object()) return std::nullopt;
        return RevisionDraft{savedTarget(data),field(data,"intent"),field(data,"doneWhen")};
    };
    const auto readDestinationPlan = [&](const Json& segments, const Reply& pending) {
        std::vector<Route> routes;
        for (const auto& segment : segments) {
            const auto kind = field(segment,"route");
            const auto target = savedTarget(segment);
            std::optional<Result> source;
            if (const auto ref = segment.find("source"); ref != segment.end()) {
                source = readResult(integer(*ref,"messageId"),*ref);
                if (!source) return std::vector<Route>{};
            }
            const auto instruction = field(segment,"instruction");
            const auto environment = field(segment,"environment");
            Action action;
            if (kind == "clarify")
                action = Clarify{pending.question,{},std::move(source),"destination",
                                 savedRevision(segment.value("revisionDraft",Json(nullptr))),
                                 segment.value("resume",false)};
            else if (kind == "create_task")
                action = CreateTask{instruction,std::move(source),environment,segment.value("resume",false)};
            else if (kind == "append_instruction") action = AppendInstruction{target,instruction,std::move(source)};
            else if (kind == "revise_task")
                action = ReviseTask{target,instruction,field(segment,"intent"),field(segment,"doneWhen"),
                                    environment,std::move(source)};
            else if (kind == "answer_wait") action = AnswerWait{target,field(segment,"answer")};
            else if (kind == "cancel_task") action = CancelTask{target};
            else if (kind == "respond") action = Respond{field(segment,"response"),std::move(source)};
            else if (kind == "status")
                action = Status{field(segment,"response"),target.taskId ? std::optional{target} : std::nullopt};
            else return std::vector<Route>{};
            routes.push_back({field(segment,"text"),std::move(action)});
        }
        return routes;
    };
    constexpr auto clarificationCandidates = R"SQL(
SELECT t.id,json_extract(t.data,'$.request')
FROM route_decisions r, json_each(r.data,'$.candidates') candidate
JOIN tasks t ON t.id=json_extract(candidate.value,'$.taskId') WHERE r.message_id=? LIMIT ?
)SQL";
    const auto readClarification = [&](Id reply) -> std::optional<Reply> {
        auto row = query(R"SQL(
SELECT m.id,m.text,json_extract(m.routing,'$.waitsAtIntake'),j.data,
       json_extract(r.data,'$.segments[0].text'),json_extract(r.data,'$.operation'),
       COALESCE(json_extract(r.data,'$.segments[0].revisionDraft'),'null'),
       json_extract(r.data,'$.segments[0].resume'),
       CASE WHEN json_extract(r.data,'$.destinationPlan') THEN json_extract(r.data,'$.segments') ELSE 'null' END
FROM route_decisions r JOIN messages m ON m.id=r.message_id JOIN journal j ON j.id=r.reply_id
WHERE r.reply_id=? AND m.bot_id=? AND m.id<? AND r.status='awaiting_resolution'
)SQL",{reply,bot,id});
        if (!row.next()) return std::nullopt;
        Reply pending;
        pending.kind = Reply::Kind::clarification;
        pending.id = reply;
        pending.originalText = row.string(1);
        pending.operation = row.string(5);
        if (!row.string(7).empty()) pending.resume = row.number(7) != 0;
        const auto data = row.json(3);
        pending.question = bounded(field(data,"text"),2000);
        pending.revisionDraft = savedRevision(row.json(6));
        if (const auto segments = row.json(8); segments.is_array()) {
            pending.destinationPlan = readDestinationPlan(segments,pending);
            for (const auto& segment : segments)
                if (segment.value("route",std::string{}) == "clarify" && segment.contains("resume"))
                    pending.resume = segment["resume"].get<bool>();
        }
        if (const auto source = data.find("source"); source != data.end()) {
            pending.result = readResult(integer(*source,"messageId"),*source);
            if (pending.result && pending.destinationPlan.empty()) pending.originalText = row.string(4);
        }
        if (const auto captured = row.string(2); !captured.empty()) {
            const auto waits = Json::parse(captured);
            const auto& items = waits.at("items");
            pending.incompleteWaitsAtIntake = waits.at("more").get<bool>() || items.size() > waitLimit;
            for (const auto& item : items) {
                if (pending.waitsAtIntake.size() == waitLimit) break;
                pending.waitsAtIntake.emplace_back(integer(item,"taskId"),integer(item,"waitId"));
            }
        }
        auto candidates = query(clarificationCandidates,{row.number(0),activeTaskLimit+waitLimit});
        while (candidates.next()) {
            const auto candidateId = candidates.number(0);
            rememberTaskId(candidateId);
            if (taskIds.contains(candidateId)) pending.candidates.push_back(candidateId);
        }
        return pending;
    };
    if (replyTo > 0) {
        context.reply.kind = Reply::Kind::invalid;
        if (auto pending = readClarification(replyTo)) context.reply = std::move(*pending);
        else if (replyTo < id) {
            auto row = query(R"SQL(
SELECT j.task_id,j.data FROM journal j JOIN tasks t ON t.id=j.task_id
WHERE t.bot_id=? AND j.id=? AND j.kind='waiting'
)SQL",{bot,replyTo});
            if (row.next()) {
                context.reply.kind = Reply::Kind::wait;
                context.reply.id = replyTo;
                context.reply.taskId = row.number(0);
                context.reply.waitId = integer(row.json(1),"id");
                rememberTaskId(context.reply.taskId);
            } else {
                Id sourceId = replyTo;
                auto prior = query("SELECT data FROM journal WHERE conversation_bot_id=? AND id=? AND kind='conversation_reply'",
                                   {bot,replyTo});
                Json priorSource = nullptr;
                if (prior.next()) {
                    const auto data = prior.json(0);
                    if (data.contains("source")) {
                        priorSource = data.at("source");
                        sourceId = integer(priorSource,"messageId");
                    }
                }
                if (auto result = readResult(sourceId,priorSource)) {
                    context.reply.kind = Reply::Kind::result;
                    context.reply.id = replyTo;
                    context.reply.result = std::move(result);
                }
            }
        }
    } else {
        auto rows = query(R"SQL(
SELECT r.reply_id,r.message_id FROM route_decisions r JOIN messages m ON m.id=r.message_id
WHERE r.status='awaiting_resolution' AND m.bot_id=? AND m.id<? ORDER BY r.message_id DESC
)SQL",{bot,id});
        Id match = 0, latest = 0;
        while (rows.next()) {
            if (latest) context.multiplePendingClarifications = true;
            else latest = rows.number(0);
            auto candidates = query(clarificationCandidates,{rows.number(1),activeTaskLimit+waitLimit});
            bool matches = false;
            while (!matches && candidates.next()) matches = conversation::namesCandidate(text,candidates.string(1));
            if (!matches) continue;
            if (match) {
                context.reply.kind = Reply::Kind::invalid;
                match = 0;
                break;
            }
            match = rows.number(0);
        }
        if (match) {
            if (auto pending = readClarification(match)) context.reply = std::move(*pending);
        } else if (latest && !context.multiplePendingClarifications)
            context.pendingClarification = readClarification(latest);
    }

    if (const auto candidate = integer(routing,"candidateTaskId")) {
        const bool offered = context.reply.kind == Reply::Kind::clarification &&
            std::ranges::find(context.reply.candidates,candidate) != context.reply.candidates.end();
        if (offered) context.selection = candidate;
        else {
            const auto anchor = context.reply.id ? context.reply.id : replyTo;
            context.reply = Reply{};
            context.reply.kind = Reply::Kind::invalid;
            context.reply.id = anchor;
        }
    }

    auto turns = query(R"SQL(
SELECT m.id,m.text,j.id,j.task_id,j.data FROM messages m
JOIN route_decisions r ON r.message_id=m.id JOIN journal j ON j.id=r.reply_id
WHERE m.bot_id=? AND m.sequence<(SELECT sequence FROM messages WHERE id=?)
ORDER BY m.sequence DESC LIMIT ?
)SQL",{bot,id,recentTurnLimit});
    while (turns.next()) {
        const auto reply = turns.json(4);
        const auto route = field(reply,"route");
        if (context.history.empty() && context.reply.kind == Reply::Kind::none && route == "respond" && reply.contains("source")) {
            const auto& source = reply.at("source");
            context.recentSource = readResult(integer(source,"messageId"),source);
        }
        context.history.push_back({{"messageId",turns.number(2)},{"role","bot"},{"taskId",turns.number(3)},
                                   {"text",bounded(field(reply,"text"),700)},{"route",route}});
        context.history.push_back({{"messageId",turns.number(0)},{"role","user"},{"text",bounded(turns.string(1),700)}});
    }
    std::reverse(context.history.begin(),context.history.end());

    auto names = explicitNames(text);
    if (context.reply.kind == Reply::Kind::clarification) {
        const auto earlierNames = explicitNames(context.reply.originalText);
        names.insert(earlierNames.begin(),earlierNames.end());
    }
    std::set<Id> namedTasks;
    for (const auto& name : names) {
        Id before = std::numeric_limits<Id>::max();
        const auto sql = std::string(taskQuery)
            +" AND lower(json_extract(data,'$.request'))=lower(?) AND id<? ORDER BY id DESC LIMIT ?";
        for (int page = 0; page < 2 && namedTasks.size() < resultLimit; ++page) {
            auto rows = query(sql.c_str(),{bot,name,before,namePageSize});
            int count = 0;
            while (count < namePageSize && namedTasks.size() < resultLimit && rows.next()) {
                before = rows.number(0);
                namedTasks.insert(before);
                rememberTask(rows);
                ++count;
            }
            if (count < namePageSize) break;
        }
    }

    const auto& reference = context.reply.result ? context.reply.result : context.recentSource;
    if (reference) {
        if (auto report = readResult(reference->messageId)) context.rankedResults.push_back(std::move(*report));
    }
    const auto resultFor = [&](const conversation::Task& task, std::string_view kind) {
        const auto sql = std::string(resultQuery)+" AND t.id=? AND j.kind=? AND j.id<? ORDER BY j.id DESC LIMIT 1";
        auto row = query(sql.c_str(),{bot,task.id,kind,id});
        if (!row.next()) return;
        auto result = resultRow(row);
        if (reference && result.messageId == reference->messageId) return;
        if (kind != "cancelled" && (result.revision != task.revision || result.text.empty())) return;
        context.rankedResults.push_back(std::move(result));
    };
    std::set<Id> primaryNamedResults;
    for (const auto& task : context.tasks) {
        const auto before = context.rankedResults.size();
        if (task.state == "cancelled") resultFor(task,"cancelled");
        else {
            if (task.state == "completed") resultFor(task,"completed");
            resultFor(task,"report");
        }
        if (namedTasks.contains(task.id) && context.rankedResults.size() > before)
            primaryNamedResults.insert(context.rankedResults[before].messageId);
    }
    const auto priority = [&](const Result& result) {
        if (reference && result.messageId == reference->messageId) return 3;
        if (primaryNamedResults.contains(result.messageId)) return 2;
        return namedTasks.contains(result.taskId) ? 1 : 0;
    };
    std::ranges::sort(context.rankedResults,[&](const Result& a,const Result& b) {
        if (priority(a) != priority(b)) return priority(a) > priority(b);
        return a.messageId > b.messageId;
    });
    if (context.rankedResults.size() > resultLimit) context.rankedResults.resize(resultLimit);
    return context;
}
}
