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
  event_id INTEGER REFERENCES events(id), usage TEXT,
  route_step INTEGER, route_input TEXT, route_decision TEXT)
)SQL");
    exec("CREATE INDEX IF NOT EXISTS model_route_calls ON model_attempts(message_id,route_step,id)"
         " WHERE route_decision IS NOT NULL");
    transaction.commit();
}

std::string Store::Impl::ensureModelBudget(std::string key, const Json& config) {
    exec("INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(?,?)",{key,modelCallLimit(config)});
    return key;
}

std::string Store::Impl::messageModelBudget(Id messageId, Id clarificationReplyId) {
    auto row = query("SELECT routing FROM messages WHERE id=?",{messageId});
    if (!row.next()) throw Conflict("The originating message is missing");
    const auto routing = row.json(0);
    if (auto saved = field(routing,"modelBudgetId"); !saved.empty()) return saved;

    std::string budget;
    if (clarificationReplyId) {
        auto origin = query("SELECT message_id FROM route_decisions WHERE reply_id=? AND message_id<?",
                            {clarificationReplyId,messageId});
        if (origin.next()) budget = messageModelBudget(origin.number(0));
    }
    if (budget.empty()) budget = ensureModelBudget("message/" + std::to_string(messageId),routing.at("config"));
    exec("UPDATE messages SET routing=json_set(routing,'$.modelBudgetId',?) WHERE id=?",{budget,messageId});
    return budget;
}

void Store::Impl::inheritModelBudget(Id messageId, Id clarificationReplyId) {
    auto origin = query("SELECT message_id FROM route_decisions WHERE reply_id=? AND message_id<?",
                        {clarificationReplyId,messageId});
    if (!origin.next()) return;
    const auto originating = messageModelBudget(origin.number(0));
    auto row = query("SELECT routing FROM messages WHERE id=?",{messageId});
    if (!row.next()) return;
    const auto current = field(row.json(0),"modelBudgetId");
    if (current.empty() || current == originating) return;
    auto count = query("SELECT count(*) FROM model_attempts WHERE message_id=? AND budget_id=?",
                       {messageId,current});
    count.next();
    if (const auto moved = count.number(0)) {
        exec("UPDATE model_attempts SET budget_id=? WHERE message_id=? AND budget_id=?",
             {originating,messageId,current});
        exec("UPDATE model_budgets SET attempts=attempts-? WHERE id=?",{moved,current});
        exec("UPDATE model_budgets SET attempts=min(attempts+?,call_limit) WHERE id=?",
             {moved,originating});
    }
    exec("UPDATE messages SET routing=json_set(routing,'$.modelBudgetId',?) WHERE id=?",
         {originating,messageId});
}

std::string Store::Impl::taskModelBudget(const Json& task) {
    // Exchanges are retained after closing. Follow them back to the original task.
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

// Called in the same write transaction as the caller's state check or wait creation.
Id Store::Impl::reserveModelCall(std::string_view budget, std::string_view purpose,
                                Id messageId, Id taskId, Id eventId) {
    exec("UPDATE model_budgets SET attempts=attempts+1 WHERE id=? AND attempts<call_limit",{budget});
    if (!sqlite3_changes(db)) return 0;
    exec("INSERT INTO model_attempts(budget_id,purpose,time,message_id,task_id,event_id) VALUES(?,?,?,?,?,?)",
         {budget,purpose,unixTime(),messageId ? Json(messageId) : Json(nullptr),
          taskId ? Json(taskId) : Json(nullptr),eventId ? Json(eventId) : Json(nullptr)});
    return last();
}

void Store::Impl::grantModelCall(std::string_view budget) {
    exec("UPDATE model_budgets SET call_limit=call_limit+1 WHERE id=? AND call_limit<?",
         {budget,std::numeric_limits<Id>::max()});
    if (!sqlite3_changes(db)) throw Conflict("Cannot increase this model call limit");
}

std::optional<Decision> Store::Impl::savedRouteCall(Id messageId, unsigned step, const Json& input) const {
    auto row = query("SELECT route_decision FROM model_attempts WHERE message_id=? AND route_step=?"
                     " AND route_input=? AND route_decision IS NOT NULL ORDER BY id DESC LIMIT 1",
                     {messageId,step,input});
    if (!row.next()) return std::nullopt;
    const auto saved = row.json(0);
    return Decision{saved.at("tool"),saved.at("args"),saved.at("reason"),saved.at("usage")};
}

Decision Store::Impl::callModel(Model& model, const ModelInput& input, Id attemptId, unsigned routeStep,
                               const Json& routeInput) {
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
    std::lock_guard lock(mutex);
    const auto checkpoint = routeStep ? Json{{"tool",decision.tool},{"args",decision.args},
        {"reason",decision.reason},{"usage",decision.usage}} : Json(nullptr);
    exec("UPDATE model_attempts SET usage=?,route_step=?,route_input=?,route_decision=? WHERE id=?",
         {decision.usage.empty() ? Json(nullptr) : decision.usage,routeStep ? Json(routeStep) : Json(nullptr),
          routeInput,checkpoint,attemptId});
    return decision;
}
}
