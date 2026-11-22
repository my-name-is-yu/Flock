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

void Store::Impl::migrateModelBudget() {
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
  event_id INTEGER REFERENCES events(id), reserved INTEGER NOT NULL DEFAULT 0, usage TEXT)
)SQL");
    exec("CREATE INDEX IF NOT EXISTS reserved_model_attempts ON model_attempts(budget_id,purpose) WHERE reserved=1");
    exec(R"SQL(
CREATE TRIGGER IF NOT EXISTS inherit_peer_budget AFTER INSERT ON peer_exchanges BEGIN
  UPDATE tasks SET budget_id=(SELECT budget_id FROM tasks WHERE id=NEW.sender_task)
  WHERE id=NEW.recipient_task;
END
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
    auto row = query("SELECT budget_id FROM tasks WHERE id=?",{task.at("id")});
    if (!row.next() || row.string(0).empty()) throw Conflict("The task has no model budget");
    return row.string(0);
}

Id Store::Impl::reserveModelCall(std::string_view budget, std::string_view purpose,
                                Id messageId, Id taskId, Id eventId) {
    auto reservation = query("SELECT id FROM model_attempts WHERE reserved=1 AND budget_id=? AND purpose=?"
                             " AND message_id IS ? AND task_id IS ? AND event_id IS ?",
                             {budget,purpose,messageId ? Json(messageId) : Json(nullptr),
                              taskId ? Json(taskId) : Json(nullptr),eventId ? Json(eventId) : Json(nullptr)});
    if (reservation.next()) {
        const auto attempt = reservation.number(0);
        exec("UPDATE model_attempts SET reserved=0 WHERE id=?",{attempt});
        return attempt;
    }
    return allocateModelAttempt(budget,purpose,messageId,taskId,eventId,false);
}

Id Store::Impl::allocateModelAttempt(std::string_view budget, std::string_view purpose,
                                     Id messageId, Id taskId, Id eventId, bool reserved) {
    exec("UPDATE model_budgets SET attempts=attempts+1 WHERE id=? AND attempts<call_limit",{budget});
    if (!sqlite3_changes(db)) return 0;
    exec("INSERT INTO model_attempts(budget_id,purpose,time,message_id,task_id,event_id,reserved) VALUES(?,?,?,?,?,?,?)",
         {budget,purpose,unixTime(),messageId ? Json(messageId) : Json(nullptr),
          taskId ? Json(taskId) : Json(nullptr),eventId ? Json(eventId) : Json(nullptr),reserved ? 1 : 0});
    return last();
}

void Store::Impl::approveModelCall(std::string_view budget, std::string_view purpose, Id message, Id task, Id event) {
    exec("UPDATE model_budgets SET call_limit=call_limit+1 WHERE id=? AND call_limit<?",
         {budget,std::numeric_limits<Id>::max()});
    if (!sqlite3_changes(db)) throw Conflict("Cannot increase this model call limit");
    const auto attempt = allocateModelAttempt(budget,purpose,message,task,event,true);
    if (!attempt) throw Conflict("The approved model call could not be reserved");
}

Decision Store::Impl::callModel(Model& model, const ModelInput& input, Id attemptId) {
    Decision decision;
    try {
        decision = model.decide(input);
    } catch (const ModelError& error) {
        if (!error.usage.empty()) {
            std::lock_guard lock(mutex);
            exec("UPDATE model_attempts SET usage=? WHERE id=?",{error.usage,attemptId});
        }
        throw;
    }
    if (!decision.usage.empty()) {
        std::lock_guard lock(mutex);
        exec("UPDATE model_attempts SET usage=? WHERE id=?",{decision.usage,attemptId});
    }
    return decision;
}
}
