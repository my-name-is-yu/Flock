#include "storage.hpp"
#include "conversation_router.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
namespace {
using conversation::Context;
using conversation::Kind;
using conversation::Reply;
using conversation::Result;
using conversation::Route;

constexpr auto messageSize = 16000;
constexpr auto resultSize = 8192;

bool hasText(std::string_view value) {
    return value.find_first_not_of(" \r\n\t") != std::string_view::npos;
}

Json receiptRow(const Statement& row, std::string_view text, Id replyTo) {
    if (row.string(1) != text || row.number(2) != replyTo)
        throw Conflict("This client message ID already has different content");
    return {{"messageId",row.number(0)},{"status","received"}};
}

Id integer(const Json& data, std::string_view key) {
    const auto value = data.find(key);
    return value != data.end() && value->is_number_integer() ? value->get<Id>() : 0;
}

std::string_view routeName(Kind kind) {
    switch (kind) {
        case Kind::respond: return "respond";
        case Kind::status: return "status";
        case Kind::createTask: return "create_task";
        case Kind::appendInstruction: return "append_instruction";
        case Kind::reviseTask: return "revise_task";
        case Kind::answerWait: return "answer_wait";
        case Kind::grantOne: return "resolve_wait";
        case Kind::cancelTask: return "cancel_task";
        case Kind::clarify: return "clarify";
    }
    throw std::logic_error("Unknown conversation route");
}

bool japanese(std::string_view text) {
    return std::any_of(text.begin(),text.end(),[](unsigned char byte) {
        return byte == 0xe3 || (byte >= 0xe4 && byte <= 0xe9);
    });
}
}

void Store::Impl::migrateConversation() {
    Transaction transaction(db);
    bool journalHasBot = false;
    {
        auto columns = query("PRAGMA table_info(journal)");
        while (columns.next()) journalHasBot |= columns.string(1) == "bot_id";
    }
    const auto schema = [&](const char* sql) {
        if (sqlite3_exec(db,sql,nullptr,nullptr,nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    };
    if (!journalHasBot) {
        schema(R"SQL(
ALTER TABLE journal RENAME TO legacy_journal;
CREATE TABLE journal(
  id INTEGER PRIMARY KEY, task_id INTEGER REFERENCES tasks(id),
  time INTEGER NOT NULL, kind TEXT NOT NULL, data TEXT NOT NULL, bot_id TEXT,
  CHECK(task_id IS NOT NULL OR bot_id IS NOT NULL));
INSERT INTO journal(id,task_id,time,kind,data) SELECT id,task_id,time,kind,data FROM legacy_journal;
DROP TABLE legacy_journal;
CREATE INDEX journal_task ON journal(task_id,id);
)SQL");
    }
    schema(R"SQL(
CREATE INDEX IF NOT EXISTS journal_bot ON journal(bot_id,id) WHERE bot_id IS NOT NULL;
CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY REFERENCES journal(id), bot_id TEXT NOT NULL,
  client_message_id TEXT NOT NULL, sequence INTEGER NOT NULL,
  reply_to INTEGER, text TEXT NOT NULL, routing TEXT NOT NULL,
  UNIQUE(bot_id,client_message_id), UNIQUE(bot_id,sequence));
CREATE TABLE IF NOT EXISTS route_decisions(
  message_id INTEGER PRIMARY KEY REFERENCES messages(id),
  reply_id INTEGER NOT NULL UNIQUE REFERENCES journal(id),
  status TEXT NOT NULL, data TEXT NOT NULL, resolution_id INTEGER REFERENCES messages(id));
CREATE TABLE IF NOT EXISTS route_candidates(
  message_id INTEGER NOT NULL REFERENCES route_decisions(message_id),
  task_id INTEGER NOT NULL REFERENCES tasks(id),
  PRIMARY KEY(message_id,task_id));
CREATE INDEX IF NOT EXISTS route_candidates_task ON route_candidates(task_id,message_id);
CREATE TABLE IF NOT EXISTS task_revisions(
  id INTEGER PRIMARY KEY, task_id INTEGER NOT NULL REFERENCES tasks(id),
  number INTEGER NOT NULL, intent TEXT NOT NULL, done_when TEXT NOT NULL,
  source_message_id INTEGER REFERENCES messages(id),
  UNIQUE(task_id,number));
CREATE TABLE IF NOT EXISTS conversation_migrations(name TEXT PRIMARY KEY);
)SQL");
    {
        auto applied = query("SELECT 1 FROM conversation_migrations WHERE name='legacy_revisions'");
        if (applied.next()) {
            transaction.commit();
            return;
        }
    }
    exec(R"SQL(
INSERT INTO task_revisions(task_id,number,intent,done_when)
SELECT id,1,json_extract(data,'$.request'),json_extract(data,'$.doneWhen')
FROM tasks WHERE length(json_extract(data,'$.doneWhen'))>0
AND NOT EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=tasks.id)
)SQL");
    exec(R"SQL(
UPDATE tasks SET data=json_set(data,'$.currentRevision',(
  SELECT id FROM task_revisions WHERE task_id=tasks.id AND number=1))
WHERE json_extract(data,'$.currentRevision') IS NULL
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=tasks.id)
)SQL");
    exec(R"SQL(
UPDATE outbox SET data=json_set(data,'$.revisionId',(
  SELECT id FROM task_revisions WHERE task_id=outbox.task_id AND number=1))
WHERE json_extract(data,'$.revisionId') IS NULL
AND json_extract(data,'$.tool') IN ('delegate','follow_up')
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=outbox.task_id)
)SQL");
    exec(R"SQL(
UPDATE events SET data=json_set(data,'$.revisionId',(
  SELECT id FROM task_revisions WHERE task_id=events.task_id AND number=1))
WHERE json_extract(data,'$.revisionId') IS NULL AND json_extract(data,'$.kind')='result'
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=events.task_id)
)SQL");
    exec(R"SQL(
UPDATE journal SET data=json_set(data,'$.revisionId',(
  SELECT id FROM task_revisions WHERE task_id=journal.task_id AND number=1))
WHERE kind IN ('report','completed') AND json_extract(data,'$.revisionId') IS NULL
AND EXISTS (SELECT 1 FROM task_revisions r WHERE r.task_id=journal.task_id)
)SQL");
    exec("INSERT INTO conversation_migrations(name) VALUES('legacy_revisions')");
    transaction.commit();
}

Id Store::Impl::addRevision(Json& task, std::string_view intent, std::string_view doneWhen, Id source) {
    if (!hasText(intent) || !hasText(doneWhen)) throw Conflict("Task revision needs intent and completion criteria");
    if (source) {
        auto row = query("SELECT 1 FROM messages WHERE id=? AND bot_id=?", {source,task["botId"]});
        if (!row.next()) throw Conflict("Task revision must come from this Bot's conversation");
    }
    exec(R"SQL(
INSERT INTO task_revisions(task_id,number,intent,done_when,source_message_id)
SELECT ?,coalesce(max(number),0)+1,?,?,? FROM task_revisions WHERE task_id=?
)SQL", {task["id"],intent,doneWhen,source ? Json(source) : Json(nullptr),task["id"]});
    const auto revision = last();
    task["currentRevision"] = revision;
    task["intent"] = intent;
    task["doneWhen"] = doneWhen;
    journal(task["id"],"task_revision",{{"revisionId",revision},{"sourceMessageId",source}});
    return revision;
}

bool Store::Impl::completionHeld(const Json& task) const {
    auto row = query(R"SQL(
SELECT 1 WHERE EXISTS(
  SELECT 1 FROM messages m WHERE m.bot_id=? AND NOT EXISTS (
    SELECT 1 FROM route_decisions r WHERE r.message_id=m.id))
OR EXISTS(
  SELECT 1 FROM route_candidates c JOIN route_decisions r ON r.message_id=c.message_id
  WHERE c.task_id=? AND r.status='awaiting_resolution')
)SQL",{task["botId"],task["id"]});
    return row.next();
}

std::optional<Json> Store::receipt(std::string_view bot, std::string_view clientId,
                                   std::string_view text, Id replyTo) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query("SELECT id,text,reply_to FROM messages WHERE bot_id=? AND client_message_id=?",
                         {bot,clientId});
    if (!row.next()) return std::nullopt;
    return receiptRow(row,text,replyTo);
}

Json Store::receive(const Intake& routing, std::string_view clientId, std::string_view text, Id replyTo) {
    if (routing.botId.empty() || !hasText(clientId) || clientId.size() > 256 ||
        !hasText(text) || text.size() > messageSize || replyTo < 0 || !routing.config.is_object())
        throw Conflict("Bot, client message ID and nonempty text are required");
    Json receipt;
    {
        std::lock_guard lock(p_->mutex);
        Transaction transaction(p_->db);
        const auto existing = this->receipt(routing.botId,clientId,text,replyTo);
        if (existing) receipt = *existing;
        else {
            const auto now = unixTime();
            p_->exec("INSERT INTO journal(time,kind,data,bot_id) VALUES(?,'conversation_message','{}',?)",
                     {now,routing.botId});
            const auto id = p_->last();
            p_->exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing)
SELECT ?,?,?,coalesce(max(sequence),0)+1,?,?,? FROM messages WHERE bot_id=?
)SQL", {id,routing.botId,clientId,replyTo ? Json(replyTo) : Json(nullptr),text,
                Json{{"environmentId",routing.environmentId},{"projectId",routing.projectId},{"config",routing.config}},
                routing.botId});
            receipt = {{"messageId",id},{"status","received"}};
        }
        transaction.commit();
    }
    p_->routePending();
    return receipt;
}

void Store::Impl::routePending() {
    constexpr int maxMessagesPerDrain = 20;
    for (int i = 0; i < maxMessagesPerDrain; ++i) {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        Id id = 0;
        {
            auto next = query(R"SQL(
SELECT m.id FROM messages m WHERE NOT EXISTS(
  SELECT 1 FROM route_decisions r WHERE r.message_id=m.id)
ORDER BY m.id LIMIT 1
)SQL");
            if (next.next()) id = next.number(0);
        }
        if (!id) break;
        routeMessage(id);
        transaction.commit();
    }
}

namespace {
Result resultRow(const Statement& row) {
    const auto data = row.json(4);
    Result result;
    result.taskId = row.number(0);
    result.messageId = row.number(1);
    result.revision = integer(data,"revisionId");
    result.taskName = bounded(row.string(2),800);
    result.state = row.string(3);
    result.text = bounded(field(data,row.string(5) == "report" ? "report" : "text"),resultSize);
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
constexpr auto latestResultFilter = R"SQL(
AND j.kind!='cancelled'
AND t.state!='cancelled'
AND (j.kind!='report' OR coalesce(json_extract(j.data,'$.revisionId'),0)=
                        coalesce(json_extract(t.data,'$.currentRevision'),0))
AND (j.kind!='report' OR NOT EXISTS(
  SELECT 1 FROM journal newer WHERE newer.task_id=j.task_id AND newer.id>j.id AND newer.kind='report'))
)SQL";

Json sourceRef(const Result& result) {
    Json source = {{"taskId",result.taskId},{"messageId",result.messageId},
                   {"revisionId",result.revision},{"text",result.text}};
    if (!result.provenance.is_null()) source["run"] = result.provenance;
    return source;
}

std::string verifiedEnvironment(const Json& task, const Json& routing, const Route& route) {
    if (route.environment.empty()) return {};
    if (route.environment == field(task,"environmentId")) return route.environment;
    const auto& config = task["projectId"] == routing["projectId"] ? routing.at("config") : task.at("config");
    if (!config.is_object()) return {};
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    auto id = field(*aliases,route.environment);
    if (id.empty() && (route.environment == "クラウドMac" || route.environment == "cloud Mac"))
        id = field(*aliases,route.environment == "cloud Mac" ? "クラウドMac" : "cloud Mac");
    if (id.empty() && (route.environment == "ローカルMac" || route.environment == "local Mac"))
        id = field(*aliases,route.environment == "local Mac" ? "ローカルMac" : "local Mac");
    constexpr auto valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:.";
    if (id.empty() || id.size() > 1024 || id.find_first_not_of(valid) != std::string::npos) return {};
    return id;
}
}

Context Store::Impl::routeContext(std::string_view bot, Id id, std::string_view text,
                                  Id replyTo, const Json& routing) {
    Context context;
    const auto& config = routing.at("config");
    if (const auto aliases = config.find("conversationEnvironments"); aliases != config.end() && aliases->is_object()) {
        for (auto alias = aliases->begin(); alias != aliases->end(); ++alias)
            context.environments.push_back(alias.key());
    }
    {
        auto active = query(R"SQL(
SELECT id,state,generation,json_extract(data,'$.request'),json_extract(data,'$.intent'),
       json_extract(data,'$.doneWhen'),json_extract(data,'$.currentRevision'),
       json_extract(data,'$.replanning'),json_extract(data,'$.wait')
FROM tasks WHERE bot_id=? ORDER BY id DESC
)SQL",{bot});
        while (active.next()) {
            const auto wait = active.string(8).empty() ? Json(nullptr) : active.json(8);
            conversation::Task task;
            task.id = active.number(0);
            task.name = bounded(active.string(3),800);
            task.intent = bounded(active.string(4),1600);
            if (task.intent.empty()) task.intent = task.name;
            task.state = active.string(1);
            if (active.number(7) && task.state != "completed" &&
                task.state != "cancelled" && task.state != "cancelling")
                task.state = "replanning";
            task.generation = active.number(2);
            task.revision = active.number(6);
            task.doneWhen = bounded(active.string(5),4000);
            task.waitId = integer(wait,"id");
            task.waitReason = field(wait,"reason");
            if (task.waitReason == "user_question" || task.waitReason == "budget")
                task.waitText = bounded(field(wait,"text"),2000);
            if (task.waitReason == "user_question" && wait.contains("choices") && wait["choices"].is_array()) {
                for (const auto& choice : wait["choices"])
                    if (choice.is_string()) task.waitChoices.push_back(bounded(choice.get_ref<const std::string&>(),200));
            }
            context.tasks.push_back(std::move(task));
        }
    }
    const auto readClarification = [&](Id reply) {
        auto row = query(R"SQL(
SELECT r.data,m.text FROM route_decisions r JOIN messages m ON m.id=r.message_id
WHERE r.reply_id=? AND m.bot_id=? AND r.status='awaiting_resolution'
)SQL",{reply,bot});
        if (!row.next()) return false;
        context.reply.kind = Reply::Kind::clarification;
        context.reply.id = reply;
        context.reply.originalText = row.string(1);
        const auto data = row.json(0);
        for (const auto& candidate : data.at("candidates"))
            context.reply.candidates.push_back(candidate.at("taskId").get<Id>());
        return true;
    };
    const auto readResult = [&](Id sourceId, const Json& excerpt = nullptr) -> std::optional<Result> {
        const auto sql = std::string(resultQuery) + " AND j.id=?";
        auto row = query(sql.c_str(),{bot,sourceId});
        if (!row.next()) return std::nullopt;
        auto result = resultRow(row);
        if (!field(excerpt,"text").empty()) result.text = field(excerpt,"text");
        return result;
    };
    if (replyTo > 0) {
        context.reply.kind = Reply::Kind::invalid;
        if (!readClarification(replyTo)) {
            auto row = query(R"SQL(
SELECT j.kind,j.task_id,j.data FROM journal j JOIN tasks t ON t.id=j.task_id
WHERE t.bot_id=? AND j.id=? AND j.kind='waiting'
)SQL",{bot,replyTo});
            if (row.next()) {
                context.reply.kind = Reply::Kind::wait;
                context.reply.id = replyTo;
                context.reply.taskId = row.number(1);
                context.reply.waitId = integer(row.json(2),"id");
            } else {
                Id sourceId = replyTo;
                auto prior = query("SELECT data FROM journal WHERE bot_id=? AND id=? AND kind='conversation_reply'",
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
        if (conversation::usesResults(text)) {
            auto latest = query(R"SQL(
SELECT j.data FROM journal j LEFT JOIN tasks t ON t.id=j.task_id
WHERE (j.bot_id=? OR (j.bot_id IS NULL AND t.bot_id=?)) AND j.id<?
AND (j.kind IN ('completed','report','conversation_reply')
     OR (j.kind='waiting' AND json_extract(j.data,'$.reason')='user_question'))
ORDER BY j.id DESC LIMIT 1
)SQL",{bot,bot,id});
            if (latest.next()) {
                const auto data = latest.json(0);
                if (field(data,"route") == "respond" && data.contains("source")) {
                    const auto& source = data.at("source");
                    context.recentSource = readResult(integer(source,"messageId"),source);
                }
            }
        }
        auto rows = query(R"SQL(
SELECT r.reply_id,r.data FROM route_decisions r JOIN messages m ON m.id=r.message_id
WHERE m.bot_id=? AND r.status='awaiting_resolution' ORDER BY m.sequence DESC
)SQL",{bot});
        Id match = 0;
        while (rows.next()) {
            const auto data = rows.json(1);
            const auto& candidates = data.at("candidates");
            if (std::none_of(candidates.begin(),candidates.end(),[&](const Json& candidate) {
                const auto name = field(candidate,"request");
                return conversation::namesCandidate(text,name);
            })) continue;
            if (match) {
                context.reply.kind = Reply::Kind::invalid;
                match = 0;
                break;
            }
            match = rows.number(0);
        }
        if (match) readClarification(match);
    }
    const auto reference = context.reply.kind == Reply::Kind::clarification
        ? std::string_view(context.reply.originalText) : text;
    const bool gatherResults = (context.reply.kind == Reply::Kind::none ||
        context.reply.kind == Reply::Kind::clarification) &&
        conversation::usesResults(reference);
    if (gatherResults) {
        const auto rankedResultsSql = std::string(resultQuery) + latestResultFilter + R"SQL(
AND j.id<? ORDER BY (length(json_extract(t.data,'$.request'))>0 AND
  instr(lower(?),lower(json_extract(t.data,'$.request')))>0) DESC,
  coalesce((SELECT final.id FROM journal final WHERE final.task_id=j.task_id
    AND final.kind='completed' ORDER BY final.id DESC LIMIT 1),j.id) DESC,j.id DESC LIMIT 10)SQL";
        auto results = query(rankedResultsSql.c_str(),{bot,id,reference});
        std::size_t remaining = 24 * 1024;
        while (results.next()) {
            auto result = resultRow(results);
            result.text = bounded(result.text,remaining);
            remaining -= result.text.size();
            context.rankedResults.push_back(std::move(result));
        }
    }
    return context;
}

void Store::Impl::routeMessage(Id id) {
    auto message = query("SELECT bot_id,text,reply_to,routing,sequence FROM messages WHERE id=?",{id});
    if (!message.next()) throw std::logic_error("Missing conversation message");
    const auto bot = message.string(0);
    const auto text = message.string(1);
    const auto replyTo = message.number(2);
    const auto routing = message.json(3);
    const auto sequence = message.number(4);
    const bool ja = japanese(text);
    const auto context = routeContext(bot,id,text,replyTo,routing);

    const auto routes = conversation::route(text,context);
    Json segments = Json::array(), candidates = Json::array();
    std::set<Id> candidateIds;
    bool clarify = false;
    std::string response;
    Json replySource = nullptr;
    std::string status = "done";
    Id linkedTask = 0;
    for (const auto& route : routes) {
        Json segment = {{"number",segments.size()+1},{"route",routeName(route.kind)},
            {"text",route.segmentText},{"taskId",route.taskId},{"generation",route.generation},
            {"revisionId",route.revision},{"waitId",route.waitId}};
        if (route.source) {
            segment["source"] = sourceRef(*route.source);
            replySource = segment["source"];
        }
        if (!route.candidates.empty()) segment["candidates"] = route.candidates;
        candidateIds.insert(route.candidates.begin(),route.candidates.end());
        segments.push_back(std::move(segment));
        if (route.kind == Kind::clarify) {
            clarify = true;
            if (!response.empty()) response += "\n";
            response += route.text;
        }
    }
    std::set<Id> modificationTargets;
    const auto validateRouteTarget = [&](const Route& route) {
        if (route.kind == Kind::createTask &&
            (field(routing,"environmentId").empty() || field(routing,"projectId").empty())) {
            response = ja ? "Bot の実行環境とプロジェクトの設定が必要です。"
                          : "The Bot needs an execution environment and project before it can start work.";
            return false;
        }
        if (route.kind == Kind::createTask && route.source) {
            const auto original = load(route.source->taskId);
            if (!terminal(original) || field(original,"botId") != bot) {
                response = ja ? "参照した結果の作業が続いています。対象の作業を確認してください。"
                              : "The task behind that result is still open. Please confirm which work to continue.";
                candidateIds.insert(route.source->taskId);
                return false;
            }
        }
        if (!route.taskId || route.kind == Kind::status || route.kind == Kind::respond ||
            route.kind == Kind::createTask) return true;
        const auto task = load(route.taskId);
        if (field(task,"botId") != bot || terminal(task) || task["state"] == "cancelling" ||
            integer(task,"generation") != route.generation || integer(task,"currentRevision") != route.revision ||
            (route.waitId && integer(task["wait"],"id") != route.waitId) ||
            (route.kind == Kind::answerWait && (!hasText(route.text) || route.text.size() > 8000)) ||
            !modificationTargets.insert(route.taskId).second) {
            response = ja ? "作業または質問の状態が変わりました。対象と指示を確認してください。"
                          : "Please confirm the task and instruction. The task or question is no longer current.";
            candidateIds.insert(route.taskId);
            return false;
        }
        if (route.source && route.source->taskId == route.taskId && route.source->revision != route.revision) {
            response = ja ? "参照した結果の後に作業条件が変わりました。現在の条件で進める指示を教えてください。"
                          : "This task’s criteria changed after that result. Please specify what to do under the current criteria.";
            candidateIds.insert(route.taskId);
            return false;
        }
        if (!route.environment.empty() && verifiedEnvironment(task,routing,route).empty()) {
            response = ja ? "指定された実行環境を Bot の許可済み設定から特定できません。実行環境を確認してください。"
                          : "I couldn't match that execution environment to this Bot's configured destinations. Please confirm the environment.";
            candidateIds.insert(route.taskId);
            return false;
        }
        return true;
    };
    if (!clarify) {
        for (const auto& route : routes) {
            if (validateRouteTarget(route)) continue;
            clarify = true;
            break;
        }
    }

    if (!clarify) {
        std::string separator;
        for (std::size_t index = 0; index < routes.size(); ++index) {
            const auto& route = routes[index];
            auto& segment = segments[index];
            std::string line = route.text;
            switch (route.kind) {
                case Kind::createTask: {
                    auto request = route.intent.empty() ? route.text : route.intent;
                    if (!hasText(request) || request.size() > messageSize)
                        throw std::logic_error("Invalid routed Task request");
                    auto source = route.source;
                    if (source && !source->messageId) {
                        auto cancelled = query("SELECT id FROM journal WHERE task_id=? AND kind='cancelled'"
                                               " ORDER BY id DESC LIMIT 1",{source->taskId});
                        if (cancelled.next()) source->messageId = cancelled.number(0);
                    }
                    linkedTask = submit({bot,request,field(routing,"environmentId"),field(routing,"projectId"),
                                         routing.at("config"),{},0},id,source ? sourceRef(*source) : Json(nullptr));
                    if (source) {
                        segment["source"] = sourceRef(*source);
                        replySource = segment["source"];
                    }
                    segment["effectTaskId"] = linkedTask;
                    status = "working";
                    line = route.source ? (ja ? "元の作業を参照して、新しい作業として受け付けました。"
                                              : "I queued a new task linked to the earlier work.")
                                        : (ja ? "作業を受け付けました。" : "I queued the task.");
                    break;
                }
                case Kind::answerWait:
                    operate(route.taskId,route.waitId,"answer",{{"text",route.text}},true);
                    linkedTask = route.taskId;
                    status = "working";
                    line = ja ? "質問への回答を受け付けました。" : "I saved your answer to the question.";
                    break;
                case Kind::grantOne:
                    operate(route.taskId,route.waitId,"approve",Json::object(),true);
                    linkedTask = route.taskId;
                    status = "working";
                    segment["amount"] = 1;
                    line = ja ? "追加作業を1回分受け付けました。" : "I added one additional run for this task.";
                    break;
                case Kind::cancelTask:
                    operate(route.taskId,0,"cancel",Json::object(),true);
                    linkedTask = route.taskId;
                    status = load(linkedTask)["state"] == "cancelled" ? "done" : "stopping";
                    line = ja ? "停止を受け付けました。" : "I accepted the stop request.";
                    break;
                case Kind::appendInstruction:
                case Kind::reviseTask: {
                    auto task = load(route.taskId);
                    const auto oldIntent = field(task,"intent").empty() ? field(task,"request") : field(task,"intent");
                    const auto oldDone = field(task,"doneWhen");
                    const auto intent = route.intent.empty() ? oldIntent : route.intent;
                    const auto done = route.doneWhen.empty() ? oldDone : route.doneWhen;
                    auto revision = route.revision;
                    if (route.kind == Kind::reviseTask && !oldDone.empty() &&
                        (intent != oldIntent || done != oldDone)) revision = addRevision(task,intent,done,id);
                    else if (route.kind == Kind::reviseTask && oldDone.empty()) {
                        task["intent"] = intent;
                        if (!route.doneWhen.empty()) {
                            task["requestedDoneWhen"] = route.doneWhen;
                            task["criteriaSourceMessageId"] = id;
                        }
                    }
                    const auto environment = verifiedEnvironment(task,routing,route);
                    const auto conflicting = route.conflicting &&
                        (environment.empty() || environment != field(task,"environmentId") || task.value("replanning",false));
                    Json pending = {{"messageId",id},{"text",route.text},{"revisionId",revision},
                                    {"conflicting",conflicting}};
                    if (!environment.empty()) {
                        pending["environmentId"] = environment;
                        if (conflicting) task["pendingEnvironmentId"] = environment;
                    }
                    if (conflicting) {
                        pending["replaceKey"] = !environment.empty() ? "environment"
                            : !route.doneWhen.empty() ? "criteria"
                            : !route.intent.empty() ? "intent" : "target";
                    }
                    if (!task.contains("pendingInstructions")) task["pendingInstructions"] = Json::array();
                    task["pendingInstructions"].push_back(pending);
                    if (conflicting) task["replanning"] = true;
                    journal(route.taskId,"instruction_accepted",pending);
                    save(task);
                    linkedTask = route.taskId;
                    status = conflicting ? "stopping" : "working";
                    line = conflicting
                        ? (ja ? "変更を保存しました。前の実行が終了したことを確認してから切り替えます。"
                              : "I saved the change. I’ll switch once the previous work has stopped.")
                        : (ja ? "補足を保存しました。実行側に送れる段階で引き継ぎます。"
                              : "I saved the instruction. I’ll pass it on when the current work reaches a safe point.");
                    break;
                }
                case Kind::status:
                case Kind::respond:
                case Kind::clarify:
                    break;
            }
            if (route.clarification) {
                exec("UPDATE route_decisions SET status=?,resolution_id=? WHERE reply_id=? AND status='awaiting_resolution'",
                     {route.superseded ? "superseded" : "resolved",id,route.clarification});
            }
            if (!line.empty()) {
                response += separator + line;
                separator = "\n";
            }
        }
    } else {
        status = "needs_you";
        if (routes.size() > 1) for (const auto& route : routes) if (route.taskId) candidateIds.insert(route.taskId);
        for (const auto& task : context.tasks) if (candidateIds.contains(task.id))
            candidates.push_back({{"taskId",task.id},{"request",task.name}});
        if (response.empty()) response = ja ? "どの作業についての指示ですか？" : "Which task did you mean?";
    }
    if (response.empty()) response = ja ? "確認しました。" : "I’ve read your message.";

    const std::string_view route = clarify ? "clarify" : routes.size() == 1 ? routeName(routes.front().kind) : "compound";
    const auto replyStatus = clarify ? "awaiting_resolution" : "applied";
    exec("INSERT INTO journal(task_id,time,kind,data,bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {linkedTask ? Json(linkedTask) : Json(nullptr),unixTime(),bot});
    const auto reply = last();
    Json answer = {{"messageId",reply},{"replyToMessageId",id},{"text",response},{"route",route},{"status",status}};
    if (!candidates.empty()) answer["candidates"] = candidates;
    if (!replySource.is_null() && !clarify) answer["source"] = replySource;
    exec("UPDATE journal SET data=? WHERE id=?", {answer,reply});
    exec("UPDATE journal SET task_id=?,data=? WHERE id=?", {linkedTask ? Json(linkedTask) : Json(nullptr),
        Json{{"status",status}},id});
    exec("INSERT INTO route_decisions(message_id,reply_id,status,data) VALUES(?,?,?,?)",
         {id,reply,replyStatus,Json{{"sequence",sequence},{"segments",segments},{"candidates",candidates}}});
    for (const auto& candidate : candidates)
        exec("INSERT INTO route_candidates(message_id,task_id) VALUES(?,?)",{id,candidate.at("taskId")});
}

Json Store::clarifications(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT j.data FROM route_decisions r JOIN journal j ON j.id=r.reply_id
WHERE j.bot_id=? AND r.status='awaiting_resolution' ORDER BY j.id DESC
)SQL",{bot});
    auto result = Json::array();
    while (rows.next()) result.push_back(rows.json(0));
    return result;
}
}
