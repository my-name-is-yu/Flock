#include "storage.hpp"
#include "conversation_router.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <tuple>

namespace flockv0 {
struct conversation::Message {
    Id id, replyTo, sequence, watermark;
    std::string bot, text;
    Json routing;
};

namespace {
using namespace conversation;

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
CREATE INDEX IF NOT EXISTS journal_pending_route ON journal(bot_id,id)
WHERE kind='conversation_message' AND json_extract(data,'$.status') IS NULL;
CREATE INDEX IF NOT EXISTS journal_task_results ON journal(task_id,id DESC)
  WHERE kind IN ('completed','report','cancelled');
CREATE INDEX IF NOT EXISTS conversation_active_tasks ON tasks(bot_id,id DESC)
  WHERE state NOT IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS conversation_terminal_tasks ON tasks(bot_id,id DESC)
  WHERE state IN ('completed','cancelled');
CREATE INDEX IF NOT EXISTS conversation_waiting_tasks ON tasks(bot_id,id DESC)
  WHERE state='waiting' AND json_extract(data,'$.wait.reason') IN ('user_question','budget');
CREATE INDEX IF NOT EXISTS conversation_task_name ON tasks(bot_id,lower(json_extract(data,'$.request')),id DESC);
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
  SELECT 1 FROM journal j WHERE j.bot_id=? AND j.kind='conversation_message'
    AND json_extract(j.data,'$.status') IS NULL)
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
                Json{{"environmentId",routing.environmentId},{"projectId",routing.projectId},{"config",routing.config},
                     {"waitsAtIntake",p_->captureConversationWaits(routing.botId)}},
                routing.botId});
            receipt = {{"messageId",id},{"status","received"}};
        }
        transaction.commit();
    }
    return receipt;
}

std::vector<std::pair<std::string,Id>> Store::Impl::pendingConversations(unsigned limit) const {
    std::lock_guard lock(mutex);
    auto next = query(R"SQL(
SELECT j.bot_id,min(j.id) FROM journal j
WHERE j.kind='conversation_message' AND json_extract(j.data,'$.status') IS NULL
GROUP BY j.bot_id ORDER BY min(j.id) LIMIT ?
)SQL",{limit});
    std::vector<std::pair<std::string,Id>> result;
    while (next.next()) result.emplace_back(next.string(0),next.number(1));
    return result;
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

constexpr int activeTaskLimit = 12;
constexpr int terminalTaskLimit = 8;
constexpr int waitLimit = 12;
constexpr int recentTurnLimit = 8;
constexpr int clarificationLookback = 24;
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

Json sourceRef(const Result& result) {
    Json source = {{"taskId",result.taskId},{"messageId",result.messageId},
                   {"revisionId",result.revision},{"text",result.text}};
    if (!result.provenance.is_null()) source["run"] = result.provenance;
    return source;
}

std::string verifiedEnvironment(const Json& task, const Json& routing, std::string_view environment) {
    if (environment.empty()) return {};
    if (environment == field(task,"environmentId")) return std::string(environment);
    const auto& config = task["projectId"] == routing["projectId"] ? routing.at("config") : task.at("config");
    if (!config.is_object()) return {};
    const auto aliases = config.find("conversationEnvironments");
    if (aliases == config.end() || !aliases->is_object()) return {};
    const auto id = field(*aliases,environment);
    constexpr auto valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_:.";
    if (id.empty() || id.size() > 1024 || id.find_first_not_of(valid) != std::string::npos) return {};
    return id;
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
    const auto& config = routing.at("config");
    if (const auto aliases = config.find("conversationEnvironments"); aliases != config.end() && aliases->is_object()) {
        for (auto alias = aliases->begin(); alias != aliases->end() && context.environments.size() < 12; ++alias)
            if (alias->is_string() && alias.key().size() <= 120) context.environments.push_back(alias.key());
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
        task.name = bounded(row.string(3),800);
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

    constexpr auto clarificationCandidates = R"SQL(
SELECT json_extract(candidate.value,'$.taskId'),json_extract(candidate.value,'$.request')
FROM route_decisions r, json_each(r.data,'$.candidates') candidate WHERE r.message_id=? LIMIT ?
)SQL";
    const auto readClarification = [&](Id reply) {
        auto row = query(R"SQL(
SELECT m.id,m.text,json_extract(m.routing,'$.waitsAtIntake')
FROM route_decisions r JOIN messages m ON m.id=r.message_id
WHERE r.reply_id=? AND m.bot_id=? AND m.id<? AND r.status='awaiting_resolution'
)SQL",{reply,bot,id});
        if (!row.next()) return false;
        context.reply.kind = Reply::Kind::clarification;
        context.reply.id = reply;
        context.reply.originalText = row.string(1);
        if (const auto captured = row.string(2); !captured.empty()) {
            const auto waits = Json::parse(captured);
            const auto& items = waits.at("items");
            context.reply.incompleteWaitsAtIntake = waits.at("more").get<bool>() || items.size() > waitLimit;
            for (const auto& item : items) {
                if (context.reply.waitsAtIntake.size() == waitLimit) break;
                context.reply.waitsAtIntake.emplace_back(integer(item,"taskId"),integer(item,"waitId"));
            }
        }
        auto candidates = query(clarificationCandidates,{row.number(0),activeTaskLimit+waitLimit});
        while (candidates.next()) {
            const auto candidateId = candidates.number(0);
            rememberTaskId(candidateId);
            if (taskIds.contains(candidateId)) context.reply.candidates.push_back(candidateId);
        }
        return true;
    };
    const auto readResult = [&](Id sourceId, const Json& excerpt = nullptr) -> std::optional<Result> {
        const auto sql = std::string(resultQuery)+" AND j.id=? AND j.id<?";
        auto row = query(sql.c_str(),{bot,sourceId,id});
        if (!row.next()) return std::nullopt;
        auto result = resultRow(row);
        if (!field(excerpt,"text").empty()) result.text = bounded(field(excerpt,"text"),resultSize);
        rememberTaskId(result.taskId);
        return result;
    };
    if (replyTo > 0) {
        context.reply.kind = Reply::Kind::invalid;
        if (replyTo < id && !readClarification(replyTo)) {
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
        auto rows = query(R"SQL(
SELECT r.reply_id,r.message_id FROM (
  SELECT id FROM messages WHERE bot_id=? AND sequence<(SELECT sequence FROM messages WHERE id=?)
  ORDER BY sequence DESC LIMIT ?
) recent JOIN route_decisions r ON r.message_id=recent.id WHERE r.status='awaiting_resolution'
)SQL",{bot,id,clarificationLookback});
        Id match = 0;
        while (rows.next()) {
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
        if (match) readClarification(match);
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

    const auto resultFor = [&](const conversation::Task& task, std::string_view kind) {
        const auto sql = std::string(resultQuery)+" AND t.id=? AND j.kind=? AND j.id<? ORDER BY j.id DESC LIMIT 1";
        auto row = query(sql.c_str(),{bot,task.id,kind,id});
        if (!row.next()) return;
        auto result = resultRow(row);
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
        if (primaryNamedResults.contains(result.messageId)) return 2;
        return namedTasks.contains(result.taskId) ? 1 : 0;
    };
    std::ranges::sort(context.rankedResults,[&](const Result& a,const Result& b) {
        if (priority(a) != priority(b)) return priority(a) > priority(b);
        return a.messageId > b.messageId;
    });
    if (context.rankedResults.size() > resultLimit) context.rankedResults.resize(resultLimit);
    for (auto& result : context.rankedResults) result.text = bounded(result.text,3072);
    return context;
}

namespace {
template<class... F>
struct Visit : F... { using F::operator()...; };

std::string_view routeName(const Action& action) {
    return std::visit(Visit{
        [](const Respond&) { return "respond"; },
        [](const Status&) { return "status"; },
        [](const CreateTask&) { return "create_task"; },
        [](const AppendInstruction&) { return "append_instruction"; },
        [](const ReviseTask&) { return "revise_task"; },
        [](const AnswerWait&) { return "answer_wait"; },
        [](const GrantOne&) { return "resolve_wait"; },
        [](const CancelTask&) { return "cancel_task"; },
        [](const Clarify&) { return "clarify"; }
    },action);
}

const Target* targetOf(const Action& action) {
    return std::visit([](const auto& item) -> const Target* {
        if constexpr (requires { item.target.taskId; }) return &item.target;
        if constexpr (requires { item.target.has_value(); }) return item.target ? &*item.target : nullptr;
        return nullptr;
    },action);
}

const Result* sourceOf(const Action& action) {
    return std::visit([](const auto& item) -> const Result* {
        if constexpr (requires { item.source; }) return item.source ? &*item.source : nullptr;
        return nullptr;
    },action);
}

const conversation::Task* candidate(const Context& context, Id taskId) {
    const auto found = std::ranges::find(context.tasks,taskId,&conversation::Task::id);
    return found == context.tasks.end() ? nullptr : &*found;
}

bool sameSource(const Result& excerpt, const Result& result) {
    return std::tie(excerpt.taskId,excerpt.messageId,excerpt.revision,excerpt.provenance) ==
           std::tie(result.taskId,result.messageId,result.revision,result.provenance) &&
           result.text.find(excerpt.text) != std::string::npos;
}

bool knownSource(const Context& context, const Result& result) {
    return (context.reply.result && sameSource(result,*context.reply.result)) ||
           (context.recentSource && sameSource(result,*context.recentSource)) ||
           std::ranges::any_of(context.rankedResults,[&](const Result& entry) { return sameSource(result,entry); });
}

bool expiredWait(const Plan& plan, const Context& context) {
    return std::ranges::any_of(plan.routes,[&](const Route& route) {
        if (!std::holds_alternative<AnswerWait>(route.action) && !std::holds_alternative<GrantOne>(route.action))
            return false;
        const auto& target = *targetOf(route.action);
        const auto task = candidate(context,target.taskId);
        return !task || task->waitId != target.waitId;
    });
}

Plan routingQuestion(const std::string& text, std::string question) {
    return {{{text,Clarify{std::move(question),{}}}},std::nullopt};
}
}

bool Store::Impl::routeMessage(Id id, Model& model) {
    const auto readMessage = [&]() -> std::optional<Message> {
        auto row = query(R"SQL(
SELECT m.reply_to,m.sequence,m.bot_id,m.text,m.routing,
  (SELECT max(latest.sequence) FROM messages latest WHERE latest.bot_id=m.bot_id)
FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND j.kind='conversation_message' AND json_extract(j.data,'$.status') IS NULL
AND NOT EXISTS (
  SELECT 1 FROM journal prior WHERE prior.bot_id=m.bot_id AND prior.id<m.id
  AND prior.kind='conversation_message' AND json_extract(prior.data,'$.status') IS NULL)
)SQL",{id});
        if (!row.next()) return std::nullopt;
        return Message{id,row.number(0),row.number(1),row.number(5),row.string(2),row.string(3),row.json(4)};
    };
    bool waitExpired = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::optional<Message> message;
        Context context;
        {
            std::lock_guard lock(mutex);
            Transaction transaction(db,Transaction::Mode::read);
            message = readMessage();
            if (!message) return true;
            context = routeContext(message->bot,id,message->text,message->replyTo,message->routing);
            transaction.commit();
        }

        const bool ja = japanese(message->text);
        Plan plan;
        if (waitExpired) {
            plan = routingQuestion(message->text,ja ? "その質問はすでに終了しています。現在の質問を確認してください。"
                                                    : "That question has closed. Please check the current question.");
        } else {
            try {
                plan = conversation::decide(message->text,context,model,message->routing.at("config"));
            } catch (const std::exception&) {
                plan = routingQuestion(message->text,ja ? "このメッセージを判断できませんでした。ここに返信して再試行してください。"
                                                        : "I couldn't route this message. Reply here to try again.");
            }
        }

        std::lock_guard lock(mutex);
        Transaction transaction(db);
        const auto current = readMessage();
        if (!current) return true;
        const auto refreshed = routeContext(current->bot,id,current->text,current->replyTo,current->routing);
        if (current->sequence != message->sequence || current->watermark != message->watermark ||
            refreshed != context) {
            waitExpired |= expiredWait(plan,refreshed);
            continue;
        }
        applyRoutes(*current,refreshed,plan);
        transaction.commit();
        return true;
    }
    return false;
}

void Store::Impl::applyRoutes(const Message& message, const Context& context, const Plan& plan) {
    const auto& [id,replyTo,sequence,watermark,bot,text,routing] = message;
    const bool ja = japanese(text);
    const auto& routes = plan.routes;
    Json segments = Json::array(), candidates = Json::array();
    std::set<Id> candidateIds, modificationTargets;
    std::string response;
    Json replySource = nullptr;
    std::string status = "done";
    Id linkedTask = 0;
    const auto addLine = [&](std::string_view line) {
        if (line.empty()) return;
        if (!response.empty()) response += '\n';
        response += line;
    };
    const auto resolutionIsCurrent = [&] {
        return !plan.resolution || (context.reply.kind == Reply::Kind::clarification &&
                                    context.reply.id == plan.resolution->replyId);
    };
    const bool resolving = plan.resolution && plan.resolution->kind == Resolution::Kind::resolved;
    bool clarify = routes.empty() || !resolutionIsCurrent();
    for (const auto& route : routes) {
        Json segment = {{"number",segments.size()+1},{"route",routeName(route.action)},{"text",route.segment}};
        if (const auto target = targetOf(route.action)) {
            segment.update({{"taskId",target->taskId},{"generation",target->generation},
                            {"revisionId",target->revision},{"waitId",target->waitId}});
        }
        if (const auto source = sourceOf(route.action)) {
            segment["source"] = sourceRef(*source);
            replySource = segment["source"];
        }
        if (const auto question = std::get_if<Clarify>(&route.action)) {
            clarify = true;
            addLine(question->question);
            candidateIds.insert(question->candidates.begin(),question->candidates.end());
            segment["candidates"] = question->candidates;
        }
        segments.push_back(std::move(segment));
    }
    const auto validate = [&](const Route& route) {
        const auto& action = route.action;
        const bool readOnly = std::holds_alternative<Respond>(action) || std::holds_alternative<Status>(action);
        if (context.reply.kind == Reply::Kind::invalid && !readOnly) return false;
        const auto source = sourceOf(action);
        if (source && !knownSource(context,*source)) return false;
        if (const auto creation = std::get_if<CreateTask>(&action)) {
            if (!hasText(creation->request) || creation->request.size() > messageSize) return false;
            if (field(routing,"environmentId").empty() || field(routing,"projectId").empty()) {
                addLine(ja ? "Bot の実行環境とプロジェクトの設定が必要です。"
                           : "The Bot needs an execution environment and project before it can start work.");
                return false;
            }
            if (source && !terminal(load(source->taskId))) return false;
        }
        const auto target = targetOf(action);
        if (!target) return true;
        const auto view = candidate(context,target->taskId);
        if (!view) return false;
        const auto task = load(target->taskId);
        const auto& wait = task.at("wait");
        if (field(task,"botId") != bot || integer(task,"generation") != target->generation ||
            integer(task,"currentRevision") != target->revision || integer(wait,"id") != target->waitId)
            return false;
        if (std::holds_alternative<Status>(action)) return true;
        if (terminal(task) || task["state"] == "cancelling" || !modificationTargets.insert(target->taskId).second)
            return false;
        if (std::holds_alternative<CancelTask>(action) && !explicitCancellation(route.segment)) return false;
        if (source && (source->taskId != target->taskId || source->revision != target->revision))
            return false;
        if (const auto answer = std::get_if<AnswerWait>(&action)) {
            if (!hasText(answer->answer) || answer->answer.size() > 8000 || field(wait,"reason") != "user_question")
                return false;
        }
        if (std::holds_alternative<GrantOne>(action) && field(wait,"reason") != "budget") return false;
        if (std::holds_alternative<AnswerWait>(action) || std::holds_alternative<GrantOne>(action)) {
            const bool anchored = replyTo && context.reply.kind == Reply::Kind::wait &&
                                  context.reply.taskId == target->taskId && context.reply.waitId == target->waitId;
            if (task["state"] != "waiting" || !target->waitId ||
                (!anchored && (!view->waitAtIntake || context.staleWaits || context.moreWaits))) return false;
            const auto originalWait = std::pair{target->taskId,target->waitId};
            if (resolving && (context.reply.incompleteWaitsAtIntake ||
                std::ranges::find(context.reply.waitsAtIntake,originalWait) == context.reply.waitsAtIntake.end()))
                return false;
            if (std::holds_alternative<GrantOne>(action) && !explicitBudgetGrant(route.segment) &&
                !(anchored && budgetConfirmation(route.segment))) return false;
        }
        if (const auto revision = std::get_if<ReviseTask>(&action)) {
            if (!revision->environment.empty() && verifiedEnvironment(task,routing,revision->environment).empty()) {
                addLine(ja ? "指定された実行環境を Bot の設定から特定できません。"
                           : "I couldn't match that execution environment to this Bot's configured destinations.");
                return false;
            }
            for (const auto& value : {revision->intent,revision->doneWhen}) {
                const bool verbatim = route.segment.find(value) != std::string::npos ||
                                      (resolving && text.find(value) != std::string::npos);
                if (!value.empty() && (!hasText(value) || value.size() > 8000 || !verbatim))
                    return false;
            }
        }
        return true;
    };
    if (!clarify) {
        for (const auto& route : routes) {
            if (validate(route)) continue;
            clarify = true;
            if (const auto target = targetOf(route.action)) candidateIds.insert(target->taskId);
            break;
        }
    }

    const auto saveInstruction = [&](const Target& target, std::string_view instruction, const ReviseTask* change) {
        auto task = load(target.taskId);
        auto revision = target.revision;
        bool conflicting = change != nullptr;
        std::string environment;
        if (change) {
            const auto oldIntent = field(task,"intent").empty() ? field(task,"request") : field(task,"intent");
            const auto oldDone = field(task,"doneWhen");
            const auto intent = change->intent.empty() ? oldIntent : change->intent;
            const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
            if (!oldDone.empty() && (intent != oldIntent || done != oldDone)) revision = addRevision(task,intent,done,id);
            else if (oldDone.empty()) {
                task["intent"] = intent;
                if (!change->doneWhen.empty()) {
                    task["requestedDoneWhen"] = change->doneWhen;
                    task["criteriaSourceMessageId"] = id;
                }
            }
            environment = verifiedEnvironment(task,routing,change->environment);
            conflicting = !change->doneWhen.empty() || !change->intent.empty() || environment.empty() ||
                          environment != field(task,"environmentId") || task.value("replanning",false);
        }
        Json pending = {{"messageId",id},{"text",instruction},{"revisionId",revision},{"conflicting",conflicting}};
        if (!environment.empty()) {
            pending["environmentId"] = environment;
            if (conflicting) task["pendingEnvironmentId"] = environment;
        }
        if (conflicting) {
            pending["replaceKey"] = !environment.empty() ? "environment" : !change->doneWhen.empty() ? "criteria"
                                    : !change->intent.empty() ? "intent" : "target";
            task["replanning"] = true;
        }
        if (!task.contains("pendingInstructions")) task["pendingInstructions"] = Json::array();
        task["pendingInstructions"].push_back(pending);
        journal(target.taskId,"instruction_accepted",pending);
        save(task);
        linkedTask = target.taskId;
        status = conflicting ? "stopping" : "working";
        addLine(conflicting
            ? (ja ? "変更を保存しました。前の実行が終了したことを確認してから切り替えます。"
                  : "I saved the change. I’ll switch once the previous work has stopped.")
            : (ja ? "補足を保存しました。実行側に送れる段階で引き継ぎます。"
                  : "I saved the instruction. I’ll pass it on when the current work reaches a safe point."));
    };

    if (!clarify) {
        for (std::size_t index = 0; index < routes.size(); ++index) {
            std::visit(Visit{
                [&](const Respond& item) { addLine(item.text); },
                [&](const Status& item) { addLine(item.text); },
                [&](const CreateTask& item) {
                    linkedTask = submit({bot,item.request,field(routing,"environmentId"),field(routing,"projectId"),
                                         routing.at("config"),{},0},id,item.source ? sourceRef(*item.source) : Json(nullptr));
                    segments[index]["effectTaskId"] = linkedTask;
                    status = "working";
                    addLine(item.source ? (ja ? "元の作業を参照して、新しい作業として受け付けました。"
                                               : "I queued a new task linked to the earlier work.")
                                        : (ja ? "作業を受け付けました。" : "I queued the task."));
                },
                [&](const AppendInstruction& item) { saveInstruction(item.target,item.instruction,nullptr); },
                [&](const ReviseTask& item) { saveInstruction(item.target,item.instruction,&item); },
                [&](const AnswerWait& item) {
                    operate(item.target.taskId,item.target.waitId,"answer",{{"text",item.answer}},true);
                    linkedTask = item.target.taskId;
                    status = "working";
                    addLine(ja ? "質問への回答を受け付けました。" : "I saved your answer to the question.");
                },
                [&](const GrantOne& item) {
                    operate(item.target.taskId,item.target.waitId,"approve",Json::object(),true);
                    linkedTask = item.target.taskId;
                    status = "working";
                    segments[index]["amount"] = 1;
                    addLine(ja ? "追加作業を1回分受け付けました。" : "I added one additional run for this task.");
                },
                [&](const CancelTask& item) {
                    operate(item.target.taskId,0,"cancel",Json::object(),true);
                    linkedTask = item.target.taskId;
                    status = load(linkedTask)["state"] == "cancelled" ? "done" : "stopping";
                    addLine(ja ? "停止を受け付けました。" : "I accepted the stop request.");
                },
                [](const Clarify&) {}
            },routes[index].action);
        }
        if (plan.resolution) {
            exec("UPDATE route_decisions SET status=?,resolution_id=? WHERE reply_id=? AND status='awaiting_resolution'",
                 {plan.resolution->kind == Resolution::Kind::superseded ? "superseded" : "resolved",
                  id,plan.resolution->replyId});
        }
    } else {
        status = "needs_you";
        for (const auto& route : routes) {
            const auto target = targetOf(route.action);
            if (target && !std::holds_alternative<Status>(route.action)) candidateIds.insert(target->taskId);
            if (const auto source = sourceOf(route.action); source && !std::holds_alternative<Respond>(route.action))
                candidateIds.insert(source->taskId);
        }
        for (const auto& task : context.tasks) {
            if (candidateIds.contains(task.id) && task.state != "completed" && task.state != "cancelled")
                candidates.push_back({{"taskId",task.id},{"request",task.name}});
        }
        if (response.empty()) addLine(ja ? "対象の作業と指示を確認してください。"
                                         : "Please confirm the task and instruction.");
    }
    if (response.empty()) addLine(ja ? "確認しました。" : "I’ve read your message.");

    const std::string_view route = clarify ? "clarify" : routes.size() == 1 ? routeName(routes.front().action) : "compound";
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
         {id,reply,replyStatus,Json{{"sequence",sequence},{"inboxWatermark",watermark},
                                    {"segments",segments},{"candidates",candidates}}});
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
