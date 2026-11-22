#include "storage.hpp"

#include <limits>

namespace flockv0 {
Id modelCallLimit(const Json& config) {
    const auto limit = config.find("modelCallLimit");
    if (limit == config.end()) return 20;
    if (!limit->is_number_integer() || limit->get<long double>() < 1 ||
        limit->get<long double>() > std::numeric_limits<int>::max())
        throw Conflict("modelCallLimit must be a positive integer up to 2147483647");
    return limit->get<Id>();
}

void Store::Impl::initializeModelBudgetSchema() {
    Transaction transaction(db);
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
    transaction.commit();
}

std::string Store::Impl::ensureModelBudget(std::string key, const Json& config) {
    exec("INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(?,?)",{key,modelCallLimit(config)});
    return key;
}

std::string Store::Impl::messageModelBudget(Id messageId) {
    auto row = query("SELECT routing FROM messages WHERE id=?",{messageId});
    if (!row.next()) throw Conflict("The originating message is missing");
    const auto routing = row.json(0);
    if (auto saved = field(routing,"modelBudgetId"); !saved.empty()) return saved;
    const auto budget = ensureModelBudget("message/" + std::to_string(messageId),routing.at("config"));
    exec("UPDATE messages SET routing=json_set(routing,'$.modelBudgetId',?) WHERE id=?",{budget,messageId});
    return budget;
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

void Store::Impl::releaseModelCall(std::string_view budget, Id attempt) {
    exec("DELETE FROM model_attempts WHERE id=? AND budget_id=?",{attempt,budget});
    if (sqlite3_changes(db)) exec("UPDATE model_budgets SET attempts=attempts-1 WHERE id=?",{budget});
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

ModelTurn Store::Impl::callModel(std::string_view bot, Model& model, const ModelInput& input, Id attemptId) {
    const auto scrub = redactor(bot);
    ModelTurn turn;
    try {
        auto safe = input;
        safe.task = scrub.record(input.task);
        safe.event = scrub.record(input.event);
        safe.history = scrub.record(input.history);
        safe.evidence = scrub.record(input.evidence);
        safe.config = scrub.record(input.config,SecretRedactor::Fields::external);
        safe.notes = {scrub.text(input.notes.soul),scrub.text(input.notes.memory)};
        safe.feedback = scrub.text(input.feedback);
        turn = model.decide(safe);
        for (auto& decision : turn.calls) {
            for (auto& [key,value] : decision.args.items()) value = scrub.payload(value);
            decision.reason = scrub.text(decision.reason);

            if (decision.args.contains("docId")) {
                const auto shared = sharedRedactor();
                const auto document = decision.args.at("docId");
                decision.args = shared.record(std::move(decision.args),SecretRedactor::Fields::external);
                if (decision.args.at("docId") != document) decision.args["docId"] = nullptr;
                decision.reason = shared.text(decision.reason);
            }
            if (isGitTool(decision.tool) || input.event.contains("gitEvent")) {
                const auto shared = sharedRedactor();
                decision.args = shared.payload(decision.args);
                decision.reason = shared.text(decision.reason);
            }
            if (std::find(input.tools.begin(),input.tools.end(),decision.tool)==input.tools.end())
                decision.tool = scrub.text(decision.tool);
        }
        turn.usage = scrub.payload(turn.usage);
    } catch (const ModelError& error) {
        auto usage = scrub.payload(error.usage);
        if (attemptId && !usage.empty()) {
            std::lock_guard lock(mutex);
            exec("UPDATE model_attempts SET usage=? WHERE id=?",{usage,attemptId});
        }
        throw ModelError(scrub.text(error.what()),error.transient,std::move(usage));
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        throw ModelError(scrub.text(error.what()),false);
    }
    if (attemptId && !turn.usage.empty()) {
        std::lock_guard lock(mutex);
        exec("UPDATE model_attempts SET usage=? WHERE id=?",{turn.usage,attemptId});
    }
    return turn;
}
}
