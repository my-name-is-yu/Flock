#include "conversation_storage.hpp"

#include <algorithm>
#include <limits>

namespace flockv0 {
namespace {
bool redactsProse(std::string_view tool, std::string_view key) {
    if (tool == "reply") return key == "text";
    if (tool == "apply_memory_edit") return key == "content";
    if (tool == "propose_memory_edit") return key == "content" || key == "rationale";
    return false;
}

bool sharedMemoryTool(std::string_view tool) {
    return tool == "read_memory" || tool == "memory_history" || tool == "propose_memory_edit" ||
           tool == "apply_memory_edit" || tool == "revert_memory_edit";
}
}

bool modelArgumentsContainProtected(const SecretRedactor& scrub, std::string_view tool, const Json& args) {
    if (!args.is_object()) return scrub.containsProtected(args);
    for (const auto& [key,value] : args.items()) {
        const bool protocolKey = redactsProse(tool,key) || (tool == "reply" && key == "suggestions");
        if ((!protocolKey && scrub.containsProtected(key)) || scrub.containsProtected(value)) return true;
    }
    return false;
}

Id modelCallLimit(const Json& config) {
    const auto limit = config.find("modelCallLimit");
    if (limit == config.end()) return 20;
    if (!limit->is_number_integer() || limit->get<long double>() < 1 ||
        limit->get<long double>() > std::numeric_limits<int>::max())
        throw Conflict("modelCallLimit must be a positive integer up to 2147483647");
    return limit->get<Id>();
}

void Store::Impl::initializeModelBudgetSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS model_budgets(
  id TEXT PRIMARY KEY, call_limit INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
  CHECK(attempts>=0 AND attempts<=call_limit))
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS model_attempts(
  id INTEGER PRIMARY KEY, budget_id TEXT NOT NULL REFERENCES model_budgets(id),
  purpose TEXT NOT NULL, time INTEGER NOT NULL,
  message_id INTEGER REFERENCES messages(id), task_id INTEGER REFERENCES tasks(id),
  event_id INTEGER REFERENCES events(id), usage TEXT)
)SQL");
}

std::string Store::Impl::ensureModelBudget(std::string key, const Json& config) {
    exec("INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(?,?)",{key,modelCallLimit(config)});
    return key;
}

std::string Store::Impl::messageModelBudget(Id messageId) {
    for (;;) {
        auto row = query(R"SQL(
SELECT m.routing,i.source_message_id FROM messages m
LEFT JOIN session_invitations i ON i.message_id=m.id WHERE m.id=?
)SQL",{messageId});
        if (!row.next()) throw Conflict("The originating message is missing");
        const auto routing = row.json(0);
        const auto wake = routing.find("sessionWake");
        const auto source = wake == routing.end() ? row.number(1) : wake->at("sourceMessageId").get<Id>();
        if (!source) return ensureModelBudget("message/" + std::to_string(messageId),routing.at("config"));
        if (source >= messageId) throw Conflict("Invalid model budget ancestry");
        messageId = source;
    }
}

std::string Store::Impl::taskModelBudget(const Json& task) {
    auto ancestor = query(R"SQL(
WITH RECURSIVE ancestry(id) AS (
  SELECT ? UNION ALL
  SELECT p.sender_task FROM peer_exchanges p JOIN ancestry a ON a.id=p.recipient_task
  WHERE p.sender_task<a.id
)
SELECT t.id,t.data FROM ancestry a JOIN tasks t USING(id) ORDER BY t.id LIMIT 1
)SQL",{task.at("id")});
    if (!ancestor.next()) throw Conflict("The originating task is missing");
    const auto root = ancestor.json(1);
    if (const auto source = root.value("sourceMessageId",Id{0}); source) return messageModelBudget(source);
    return ensureModelBudget("task/" + std::to_string(ancestor.number(0)),root.at("config"));
}

Id Store::Impl::reserveModelCall(std::string_view budget, std::string_view purpose,
                                Id messageId, Id taskId, Id eventId) {
    exec("UPDATE model_budgets SET attempts=attempts+1 WHERE id=? AND attempts<call_limit",{budget});
    if (!sqlite3_changes(db)) return 0;
    exec("INSERT INTO model_attempts(budget_id,purpose,time,message_id,task_id,event_id) VALUES(?,?,?,?,?,?)",
         {budget,purpose,unixTime(),messageId ? Json(messageId) : Json(nullptr),
          taskId ? Json(taskId) : Json(nullptr),eventId ? Json(eventId) : Json(nullptr)});
    return last();
}

Id Store::Impl::takeReservedConversationCall(Id message) {
    auto reserved = query("SELECT reserved_attempt FROM conversation_turns WHERE message_id=?"
                          " AND reserved_attempt IS NOT NULL",{message});
    if (!reserved.next()) return 0;
    const auto id = reserved.number(0);
    exec("UPDATE conversation_turns SET reserved_attempt=NULL WHERE message_id=?",{message});
    return id;
}

Id Store::Impl::takeReservedTaskCall(Id event) {
    auto reserved = query("SELECT reserved_attempt FROM events WHERE id=? AND reserved_attempt IS NOT NULL",{event});
    if (!reserved.next()) return 0;
    const auto id = reserved.number(0);
    exec("UPDATE events SET reserved_attempt=NULL WHERE id=?",{event});
    return id;
}

void Store::Impl::releaseModelCall(Id attempt) {
    if (!attempt) return;
    std::string budget;
    {
        auto removed = query("DELETE FROM model_attempts WHERE id=? RETURNING budget_id",{attempt});
        if (!removed.next()) return;
        budget = removed.string(0);
        removed.next();
    }
    exec("UPDATE model_budgets SET attempts=attempts-1 WHERE id=?",{budget});
}

Id Store::Impl::reserveExtraModelCall(std::string_view budget, std::string_view purpose,
                                     Id messageId, Id taskId, Id eventId) {
    exec("UPDATE model_budgets SET call_limit=call_limit+1 WHERE id=? AND call_limit<?",
         {budget,std::numeric_limits<Id>::max()});
    if (!sqlite3_changes(db)) throw Conflict("Cannot increase this model call limit");
    const auto attempt = reserveModelCall(budget,purpose,messageId,taskId,eventId);
    if (!attempt) throw Conflict("The approved model call could not be reserved");
    return attempt;
}

SecretRedactor Store::Impl::modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args,
                                              bool gitNotification) const {
    if (gitNotification || isGitTool(tool) || (sharedMemoryTool(tool) && args.contains("docId")))
        return sharedRedactor();
    return redactor(bot);
}

ModelCall Store::Impl::checkModelCall(std::string_view bot, Decision call, const std::vector<std::string>& offered,
                                      bool gitNotification) {
    std::string rejection;
    if (std::ranges::find(offered,call.tool) == offered.end()) {
        rejection = "Tool unavailable for this turn";
        if (redactor(bot).containsProtected(call.tool)) call.tool.clear();
    } else if (sharedMemoryTool(call.tool) && call.args.contains("docId") &&
               Policy(*this).forbidden(bot,(call.tool == "read_memory" || call.tool == "memory_history")
                   ? Action::shared_memory_read : Action::shared_memory_write,field(call.args,"docId"))) {
        rejection = "Memory access denied";
    } else {
        const auto scrub = modelCallRedactor(bot,call.tool,call.args,gitNotification);
        if (call.args.is_object()) {
            for (auto& [key,value] : call.args.items())
                if (value.is_string() && redactsProse(call.tool,key))
                    value = scrub.text(value.get_ref<const std::string&>());
        }
        if (modelArgumentsContainProtected(scrub,call.tool,call.args))
            rejection = "Tool arguments contain protected material. Use a secret reference where supported.";
        if (rejection.empty()) call.reason = scrub.text(call.reason);
    }
    if (!rejection.empty()) {
        call.args = Json::object();
        call.reason.clear();
    }
    return {std::move(call),std::move(rejection)};
}

CheckedModelTurn Store::Impl::callModel(std::string_view bot, Model& model, const ModelInput& input) {
    Json revision;
    const bool conversation = input.event.contains("turn") && input.event.value("kind","") == "conversation_turn";
    const auto scrub = redactor(bot,conversation ? &revision : nullptr);
    CheckedModelTurn checked;
    try {
        auto safe = input;
        safe.task = scrub.record(input.task);
        safe.event = scrub.record(input.event);
        safe.history = scrub.record(input.history);
        safe.evidence = scrub.record(input.evidence);
        safe.config = scrub.record(input.config,SecretRedactor::Fields::external);
        safe.notes = {scrub.text(input.notes.soul),scrub.text(input.notes.memory)};
        safe.feedback = scrub.text(input.feedback);
        if (conversation) conversation_storage::projectToolHistory(safe.event.at("turn"),revision);
        auto turn = model.decide(safe);
        checked.calls.reserve(turn.calls.size());
        for (auto& call : turn.calls)
            checked.calls.push_back(checkModelCall(bot,std::move(call),input.tools,input.event.contains("gitEvent")));
        checked.usage = scrub.payload(turn.usage);
    } catch (const ModelError& error) {
        throw ModelError(scrub.text(error.what()),error.transient,scrub.payload(error.usage),error.code);
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        throw ModelError(scrub.text(error.what()),false);
    }
    return checked;
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
    exec("UPDATE journal SET data=json_set(data,'$.status','done') WHERE id=?",{waitMessageId});
    exec(R"SQL(
INSERT INTO journal(time,kind,data,conversation_bot_id)
SELECT ?,'conversation_budget_approved',json_object('messageId',id,'waitMessageId',?),bot_id
FROM messages WHERE id=?
)SQL",{unixTime(),waitMessageId,message});
}
}
