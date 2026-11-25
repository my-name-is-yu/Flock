#include "conversation_storage.hpp"

#include <algorithm>
#include <limits>

namespace flockv0 {
namespace {
bool redactsProse(std::string_view tool, std::string_view key) {
    if (tool == "reply") return key == "text";
    if (tool == "apply_memory_edit") return key == "content";
    if (tool == "remember_preference") return key == "content" || key == "summary";
    if (tool == "propose_memory_edit") return key == "content" || key == "rationale";
    return false;
}

bool sharedMemoryTool(std::string_view tool) {
    return tool == "read_memory" || tool == "memory_history" || tool == "propose_memory_edit" || tool == "apply_memory_edit" ||
           tool == "revert_memory_edit";
}

std::string_view modelBudgetPauseText(std::string_view request) {
    return conversation_storage::japanese(request) ? "この依頼に使えるステップの上限に達しました。"
                                                   : "I’ve used the steps available for this request.";
}
}

bool recordsTaskDecisionReason(std::string_view tool) {
    constexpr std::string_view recorded[]{"delegate",         "follow_up",   "complete", "ask_user", "answer_session_question",
                                          "send_bot_message", "reply_to_bot"};
    return std::ranges::find(recorded, tool) != std::end(recorded);
}

bool validDecisionReason(std::string_view reason) {
    return reason.size() <= decisionReasonLimit && conversation_storage::hasText(reason);
}

bool modelArgumentsContainProtected(const SecretRedactor& scrub, std::string_view tool, const Json& args) {
    if (!args.is_object()) return scrub.containsProtected(args);
    for (const auto& [key, value] : args.items()) {
        const bool protocolKey = redactsProse(tool, key) || (tool == "reply" && key == "suggestions");
        if ((!protocolKey && scrub.containsProtected(key)) || scrub.containsProtected(value)) return true;
    }
    return false;
}

Id modelCallLimit(const Json& config) {
    const auto limit = config.find("modelCallLimit");
    if (limit == config.end()) return 20;
    if (!limit->is_number_integer() || limit->get<long double>() < 1 || limit->get<long double>() > std::numeric_limits<int>::max())
        throw Conflict("modelCallLimit must be a positive integer up to 2147483647");
    return limit->get<Id>();
}

ModelBudget Store::Impl::messageModelBudget(Id messageId) const {
    for (;;) {
        auto row = query(R"SQL(
SELECT m.routing,i.source_message_id FROM messages m
LEFT JOIN session_invitations i ON i.message_id=m.id WHERE m.id=?
)SQL",
                         {messageId});
        if (!row.next()) throw Conflict("The originating message is missing");
        const auto routing = row.json(0);
        const auto wake = routing.find("sessionWake");
        const auto source = wake == routing.end() ? row.number(1) : wake->at("sourceMessageId").get<Id>();
        if (!source) return {"message/" + std::to_string(messageId), modelCallLimit(routing.at("config"))};
        if (source >= messageId) throw Conflict("Invalid model budget ancestry");
        messageId = source;
    }
}

ModelBudget Store::Impl::taskModelBudget(Id taskId) const {
    auto ancestor = query(R"SQL(
WITH RECURSIVE ancestry(id) AS (
  SELECT ? UNION ALL
  SELECT p.sender_task FROM peer_exchanges p JOIN ancestry a ON a.id=p.recipient_task
  WHERE p.sender_task<a.id
)
SELECT t.id,t.data FROM ancestry a JOIN tasks t USING(id) ORDER BY t.id LIMIT 1
)SQL",
                          {taskId});
    if (!ancestor.next()) throw Conflict("The originating task is missing");
    const auto root = ancestor.json(1);
    if (const auto source = root.value("sourceMessageId", Id{0}); source) return messageModelBudget(source);
    return {"task/" + std::to_string(ancestor.number(0)), modelCallLimit(root.at("config"))};
}

Id Store::Impl::reserveModelCall(const ModelBudget& budget, std::string_view purpose, Id messageId, Id taskId, Id eventId) {
    exec("INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(?,?)", {budget.id, budget.initialLimit});
    exec("UPDATE model_budgets SET attempts=attempts+1 WHERE id=? AND attempts<call_limit", {budget.id});
    if (!sqlite3_changes(db)) return 0;
    exec("INSERT INTO model_attempts(budget_id,purpose,time,message_id,task_id,event_id) VALUES(?,?,?,?,?,?)",
         {budget.id, purpose, unixTime(), messageId ? Json(messageId) : Json(nullptr), taskId ? Json(taskId) : Json(nullptr),
          eventId ? Json(eventId) : Json(nullptr)});
    return last();
}

Id Store::Impl::takeReservedConversationCall(Id message) {
    auto reserved = query("SELECT reserved_attempt FROM conversation_turns WHERE message_id=?"
                          " AND reserved_attempt IS NOT NULL",
                          {message});
    if (!reserved.next()) return 0;
    const auto id = reserved.number(0);
    exec("UPDATE conversation_turns SET reserved_attempt=NULL WHERE message_id=?", {message});
    return id;
}

Id Store::Impl::takeReservedTaskCall(Id event) {
    auto reserved = query("SELECT reserved_attempt FROM events WHERE id=? AND reserved_attempt IS NOT NULL", {event});
    if (!reserved.next()) return 0;
    const auto id = reserved.number(0);
    exec("UPDATE events SET reserved_attempt=NULL WHERE id=?", {event});
    return id;
}

void Store::Impl::releaseModelCall(Id attempt) {
    if (!attempt) return;
    std::string budget;
    {
        auto removed = query("DELETE FROM model_attempts WHERE id=? RETURNING budget_id", {attempt});
        if (!removed.next()) return;
        budget = removed.string(0);
        removed.next();
    }
    exec("UPDATE model_budgets SET attempts=attempts-1 WHERE id=?", {budget});
}

Id Store::Impl::approveModelBudget(const Person& actor, const ModelBudget& budget, std::string_view purpose, Id messageId, Id taskId,
                                   Id eventId) {
    exec("UPDATE model_budgets SET call_limit=call_limit+? WHERE id=? AND call_limit<=?",
         {budget.initialLimit, budget.id, std::numeric_limits<Id>::max() - budget.initialLimit});
    if (!sqlite3_changes(db)) throw Conflict("Cannot increase this model call limit");
    const auto attempt = reserveModelCall(budget, purpose, messageId, taskId, eventId);
    if (!attempt) throw Conflict("The approved model call could not be reserved");
    resumeSharedModelBudgetWaits(actor, budget.id, messageId, taskId);
    return attempt;
}

void Store::Impl::resumeSharedModelBudgetWaits(const Person& actor, std::string_view budget, Id approvedMessage, Id approvedTask) {
    if (!query("SELECT 1 FROM model_budgets WHERE id=? AND attempts<call_limit", {budget}).next()) return;
    const auto policy = Policy(*this);
    std::vector<Id> tasks;
    auto waits = query(R"SQL(
SELECT w.task_id,t.bot_id FROM waits w CROSS JOIN tasks t ON t.id=w.task_id
WHERE w.resolution IS NULL AND w.task_id<>? AND json_extract(w.data,'$.reason')='model_budget' AND t.state='waiting'
)SQL",
                       {approvedTask});
    while (waits.next()) {
        const auto task = waits.number(0);
        if (policy.allows(actor, Action::bot_decide, waits.string(1)) && taskModelBudget(task).id == budget) tasks.push_back(task);
    }
    for (const auto id : tasks) {
        auto task = load(id);
        if (!waitIsActionable(task)) continue;
        clearWait(task, "approve");
        task["state"] = "deciding";
        save(task);
    }

    std::vector<std::pair<Id, Id>> messages;
    auto paused = query(R"SQL(
SELECT t.message_id,t.pause_id,m.bot_id FROM conversation_turns t JOIN messages m ON m.id=t.message_id
WHERE t.pause_id IS NOT NULL AND t.status='paused' AND t.message_id<>? ORDER BY t.pause_id
)SQL",
                        {approvedMessage});
    while (paused.next()) {
        const auto message = paused.number(0);
        if (policy.allows(actor, Action::bot_decide, paused.string(2)) && messageModelBudget(message).id == budget)
            messages.emplace_back(message, paused.number(1));
    }
    for (const auto& [message, pause] : messages) resumeMessageModelCall(message, pause, 0);
}

SecretRedactor Store::Impl::modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args, bool gitNotification) const {
    if (gitNotification || isGitTool(tool) || (sharedMemoryTool(tool) && args.contains("docId"))) return sharedRedactor();
    return redactor(bot);
}

ModelCall Store::Impl::checkModelCall(std::string_view bot, Decision call, const std::vector<std::string>& offered, bool gitNotification) {
    std::string rejection;
    if (std::ranges::find(offered, call.tool) == offered.end()) {
        rejection = "Tool unavailable for this turn";
        if (redactor(bot).containsProtected(call.tool)) call.tool.clear();
    } else if (sharedMemoryTool(call.tool) && call.args.contains("docId") &&
               Policy(*this).forbidden(bot,
                                       (call.tool == "read_memory" || call.tool == "memory_history") ? Action::shared_memory_read
                                                                                                     : Action::shared_memory_write,
                                       field(call.args, "docId"))) {
        rejection = "Memory access denied";
    } else {
        const auto scrub = modelCallRedactor(bot, call.tool, call.args, gitNotification);
        if (call.args.is_object()) {
            for (auto& [key, value] : call.args.items())
                if (value.is_string() && redactsProse(call.tool, key)) value = scrub.text(value.get_ref<const std::string&>());
        }
        if (modelArgumentsContainProtected(scrub, call.tool, call.args))
            rejection = "Tool arguments contain protected material. Use a secret reference where supported.";
        if (rejection.empty()) {
            const auto operation = gitOperation(call);
            if (recordsTaskDecisionReason(call.tool) || (!operation.empty() && operation != "read"))
                call.reason = scrub.text(call.reason);
            else
                call.reason.clear();
        }
    }
    if (!rejection.empty()) {
        call.args = Json::object();
        call.reason.clear();
    }
    return {std::move(call), std::move(rejection)};
}

CheckedModelTurn Store::Impl::callModel(std::string_view bot, Model& model, const ModelInput& input) {
    Json revision;
    const bool conversation = input.event.contains("turn") && input.event.value("kind", "") == "conversation_turn";
    const auto scrub = redactor(bot, conversation ? &revision : nullptr);
    CheckedModelTurn checked;
    try {
        auto safe = input;
        safe.task = scrub.record(input.task);
        safe.event = scrub.record(input.event);
        safe.history = scrub.record(input.history);
        safe.evidence = scrub.record(input.evidence);
        safe.config = scrub.record(input.config, SecretRedactor::Fields::external);
        safe.notes = {scrub.text(input.notes.soul), scrub.text(input.notes.memory)};
        safe.feedback = scrub.text(input.feedback);
        if (conversation) conversation_storage::projectToolHistory(safe.event.at("turn"), revision);
        {
            std::lock_guard lock(mutex);
            if (botPaused(bot)) throw BotPaused{};
        }
        auto turn = model.decide(safe);
        checked.calls.reserve(turn.calls.size());
        for (std::size_t i = 0; i < turn.calls.size(); ++i) {
            auto& call = turn.calls[i];
            const auto rejected = turn.rejections.find(i);
            if (rejected != turn.rejections.end()) {
                call.args = Json::object();
                call.reason.clear();
            }
            auto result = checkModelCall(bot, std::move(call), input.tools, input.event.contains("gitEvent"));
            if (rejected != turn.rejections.end() && result.rejection.empty()) result.rejection = scrub.text(rejected->second);
            checked.calls.push_back(std::move(result));
        }
        checked.usage = scrub.payload(turn.usage);
    } catch (const ModelError& error) {
        throw ModelError(scrub.text(error.what()), error.transient, scrub.payload(error.usage), error.code);
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        throw ModelError(scrub.text(error.what()), false);
    }
    return checked;
}

void Store::Impl::pauseTurn(const conversation::Message& message) {
    const Json task =
        message.principalSource == conversation::PrincipalSource::gitWebhook ? message.routing.at("gitEvent").at("taskId") : Json(nullptr);
    exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {task, unixTime(), message.bot});
    const auto reply = last();
    const Json data{{"messageId", reply},
                    {"replyToMessageId", message.id},
                    {"route", "model_budget"},
                    {"status", "needs_you"},
                    {"text", modelBudgetPauseText(message.text)}};
    exec("UPDATE journal SET data=? WHERE id=?", {data, reply});
    if (!message.channelScope.empty()) queueChannelReply(message.id, reply, data);
    exec("UPDATE conversation_turns SET status='paused',pause_id=? WHERE message_id=?", {reply, message.id});
}

Json Store::pausedConversations(std::string_view bot) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
-- Seek the populated pause IDs instead of visiting all journal entries for the Bot.
SELECT j.data,t.message_id,m.text FROM conversation_turns t CROSS JOIN journal j ON j.id=t.pause_id
CROSS JOIN messages m ON m.id=t.message_id
WHERE t.pause_id IS NOT NULL AND t.status='paused' AND j.conversation_bot_id=? ORDER BY t.pause_id DESC
)SQL",
                          {bot});
    auto result = Json::array();
    while (rows.next()) {
        auto turn = rows.json(0);
        turn["text"] = modelBudgetPauseText(rows.string(2));
        turn["allowance"] = p_->messageModelBudget(rows.number(1)).initialLimit;
        result.push_back(std::move(turn));
    }
    return result;
}

void Store::Impl::approveMessageModelCall(const Person& actor, Id message, Id waitMessageId) {
    auto slots = query(R"SQL(
SELECT json_extract(s.decision,'$[' || slot.key || '].tool')
FROM conversation_steps s, json_each(s.effect) slot
WHERE s.message_id=? AND s.result IS NULL AND json_type(s.effect)='array' AND slot.type='null'
)SQL",
                       {message});
    bool toolPending = false;
    while (!toolPending && slots.next()) toolPending = isBotTool(slots.string(0));
    const auto attempt =
        approveModelBudget(actor, messageModelBudget(message), toolPending ? "bot_tool" : "conversation_turn", message, 0, 0);
    resumeMessageModelCall(message, waitMessageId, attempt);
}

void Store::Impl::resumeMessageModelCall(Id message, Id waitMessageId, Id attempt) {
    exec("UPDATE conversation_turns SET status='running',pause_id=NULL,reserved_attempt=? WHERE message_id=?",
         {attempt ? Json(attempt) : Json(nullptr), message});
    exec("UPDATE journal SET data=json_set(data,'$.status','done') WHERE id=?", {waitMessageId});
    exec(R"SQL(
INSERT INTO journal(time,kind,data,conversation_bot_id)
SELECT ?,'conversation_budget_approved',json_object('messageId',id,'waitMessageId',?),bot_id
FROM messages WHERE id=?
)SQL",
         {unixTime(), waitMessageId, message});
}
}
