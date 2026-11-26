#include "wait_storage.hpp"
#include "task_records.hpp"
#include "secret_storage.hpp"
#include "model_calls.hpp"
#include "journal_storage.hpp"
#include "bot_lifecycle_storage.hpp"
#include "conversation_storage.hpp"
#include "flock/limits.hpp"
#include "journal_conversation.hpp"
#include "task_storage.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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
    return reason.size() <= limits::model::maxDecisionReasonBytes && conversation_storage::hasText(reason);
}

bool modelArgumentsContainProtected(const SecretRedactor& scrub, std::string_view tool, const Json& args) {
    if (!args.is_object()) return scrub.containsProtected(args);
    for (const auto& [key, value] : args.items()) {
        const bool protocolKey = redactsProse(tool, key) || (tool == "reply" && key == "suggestions");
        if ((!protocolKey && scrub.containsProtected(key)) || scrub.containsProtected(value)) return true;
    }
    return false;
}

Id initialModelCallLimit(const Json& config) {
    const auto limit = config.find("modelCallLimit");
    constexpr Id defaultLimit = 20, maxLimit = std::numeric_limits<int>::max();
    if (limit == config.end()) return defaultLimit;
    if (!limit->is_number_integer() || limit->get<long double>() < 1 || limit->get<long double>() > maxLimit)
        throw Conflict("modelCallLimit must be a positive integer up to " + std::to_string(maxLimit));
    return limit->get<Id>();
}

ModelBudget ModelCalls::messageModelBudget(Id messageId) const {
    for (;;) {
        auto row = database.query(R"SQL(
SELECT m.routing,i.source_message_id FROM messages m
LEFT JOIN session_invitations i ON i.message_id=m.id WHERE m.id=?
)SQL",
                                  {messageId});
        if (!row.next()) throw Conflict("The originating message is missing");
        const auto routing = row.json(0);
        const auto wake = routing.find("sessionWake");
        const auto source = wake == routing.end() ? row.number(1) : wake->at("sourceMessageId").get<Id>();
        if (!source) return {"message/" + std::to_string(messageId), initialModelCallLimit(routing.at("config"))};
        if (source >= messageId) throw Conflict("Invalid model budget ancestry");
        messageId = source;
    }
}

ModelBudget ModelCalls::taskModelBudget(Id taskId) const {
    static const auto sql = R"SQL(
WITH RECURSIVE ancestry(id) AS (
  SELECT ? UNION ALL
  SELECT p.sender_task FROM peer_exchanges p JOIN ancestry a ON a.id=p.recipient_task
  WHERE p.sender_task<a.id
)
SELECT )SQL" + taskProjection() +
                            " FROM ancestry a JOIN tasks t USING(id) ORDER BY t.id LIMIT 1";
    auto ancestor = database.query(sql.c_str(), {taskId});
    if (!ancestor.next()) throw Conflict("The originating task is missing");
    const auto root = taskRecords.readTask(ancestor);
    if (root.sourceMessageId) return messageModelBudget(*root.sourceMessageId);
    return {"task/" + std::to_string(root.id), initialModelCallLimit(root.config)};
}

Id ModelCalls::reserveModelCall(const ModelBudget& budget, std::string_view purpose, Id messageId, Id taskId, Id eventId) {
    database.exec("INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(?,?)", {budget.id, budget.initialLimit});
    database.exec("UPDATE model_budgets SET attempts=attempts+1 WHERE id=? AND attempts<call_limit", {budget.id});
    if (!sqlite3_changes(database.db)) return 0;
    database.exec("INSERT INTO model_attempts(budget_id,purpose,time,message_id,task_id,event_id) VALUES(?,?,?,?,?,?)",
                  {budget.id, purpose, unixTime(), messageId ? Json(messageId) : Json(nullptr), taskId ? Json(taskId) : Json(nullptr),
                   eventId ? Json(eventId) : Json(nullptr)});
    return database.last();
}

Id ModelCalls::takeReservedConversationCall(Id message) {
    auto reserved = database.query("SELECT reserved_attempt FROM conversation_turns WHERE message_id=?"
                                   " AND reserved_attempt IS NOT NULL",
                                   {message});
    if (!reserved.next()) return 0;
    const auto id = reserved.number(0);
    database.exec("UPDATE conversation_turns SET reserved_attempt=NULL WHERE message_id=?", {message});
    return id;
}

Id ModelCalls::takeReservedTaskCall(Id event) {
    auto reserved = database.query("SELECT reserved_attempt FROM events WHERE id=? AND reserved_attempt IS NOT NULL", {event});
    if (!reserved.next()) return 0;
    const auto id = reserved.number(0);
    database.exec("UPDATE events SET reserved_attempt=NULL WHERE id=?", {event});
    return id;
}

void ModelCalls::releaseModelCall(Id attempt) {
    if (!attempt) return;
    std::string budget;
    {
        auto removed = database.query("DELETE FROM model_attempts WHERE id=? RETURNING budget_id", {attempt});
        if (!removed.next()) return;
        budget = removed.string(0);
        removed.next();
    }
    database.exec("UPDATE model_budgets SET attempts=attempts-1 WHERE id=?", {budget});
}

Id ModelCalls::approveModelBudget(const Person& actor, const ModelBudget& budget, std::string_view purpose, Id messageId, Id taskId,
                                  Id eventId) {
    database.exec("UPDATE model_budgets SET call_limit=call_limit+? WHERE id=? AND call_limit<=?",
                  {budget.initialLimit, budget.id, std::numeric_limits<Id>::max() - budget.initialLimit});
    if (!sqlite3_changes(database.db)) throw Conflict("Cannot increase this model call limit");
    const auto attempt = reserveModelCall(budget, purpose, messageId, taskId, eventId);
    if (!attempt) throw Conflict("The approved model call could not be reserved");
    resumeSharedModelBudgetWaits(actor, budget.id, messageId, taskId);
    return attempt;
}

void ModelCalls::resumeSharedModelBudgetWaits(const Person& actor, std::string_view budget, Id approvedMessage, Id approvedTask) {
    if (!database.query("SELECT 1 FROM model_budgets WHERE id=? AND attempts<call_limit", {budget}).next()) return;
    const auto policy = Policy(remaining);
    std::vector<Id> tasks;
    auto waits = database.query(R"SQL(
SELECT w.task_id,t.bot_id FROM waits w CROSS JOIN tasks t ON t.id=w.task_id
WHERE w.resolution IS NULL AND w.task_id<>? AND w.reason='model_budget' AND t.state='waiting'
)SQL",
                                {approvedTask});
    while (waits.next()) {
        const auto task = waits.number(0);
        if (policy.allows(actor, Action::bot_decide, waits.string(1)) && taskModelBudget(task).id == budget) tasks.push_back(task);
    }
    for (const auto id : tasks) {
        auto task = taskRecords.load(id);
        if (!waitStorage.waitIsActionable(task)) continue;
        waitStorage.clearWait(task, "approve");
        task.state = TaskState::deciding;
        taskRecords.save(task);
    }

    struct PausedMessage {
        std::string bot;
        Id message, pause;
    };
    std::vector<PausedMessage> messages;
    auto paused = database.query(R"SQL(
SELECT t.message_id,t.pause_id,m.bot_id FROM conversation_turns t JOIN messages m ON m.id=t.message_id
WHERE t.pause_id IS NOT NULL AND t.status='paused' AND t.message_id<>? ORDER BY t.pause_id
)SQL",
                                 {approvedMessage});
    while (paused.next()) {
        const auto message = paused.number(0);
        auto bot = paused.string(2);
        if (policy.allows(actor, Action::bot_decide, bot) && messageModelBudget(message).id == budget)
            messages.push_back({std::move(bot), message, paused.number(1)});
    }
    for (const auto& [bot, message, pause] : messages) resumeMessageModelCall(bot, message, pause, 0);
}

SecretRedactor ModelCalls::modelCallRedactor(std::string_view bot, std::string_view tool, const Json& args, bool gitNotification) const {
    if (gitNotification || isGitTool(tool) || (sharedMemoryTool(tool) && args.contains("docId"))) return secretStorage.sharedRedactor();
    return secretStorage.redactor(bot);
}

ModelCall ModelCalls::checkModelCall(std::string_view bot, Decision call, const std::vector<std::string>& offered, bool gitNotification) {
    std::string rejection;
    if (std::ranges::find(offered, call.tool) == offered.end()) {
        rejection = "Tool unavailable for this turn";
        if (secretStorage.redactor(bot).containsProtected(call.tool)) call.tool.clear();
    } else if (sharedMemoryTool(call.tool) && call.args.contains("docId") &&
               Policy(remaining).forbidden(bot,
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

CheckedModelTurn ModelCalls::callModel(std::string_view bot, Model& model, const ModelInput& input) {
    Json revision;
    const bool conversation = input.event.contains("turn") && input.event.value("kind", "") == "conversation_turn";
    const auto scrub = secretStorage.redactor(bot, conversation ? &revision : nullptr);
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
            std::lock_guard lock(database.mutex);
            if (botLifecycle.botPaused(bot)) throw BotPaused{};
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

void ModelCalls::pauseTurn(const conversation::Message& message) {
    const auto task = message.principalSource == conversation::PrincipalSource::gitWebhook
                          ? std::optional{message.routing.at("gitEvent").at("taskId").get<Id>()}
                          : std::nullopt;
    const auto text = modelBudgetPauseText(message.text);
    const auto reply = journalStorage.journalConversationReply(task, message.bot,
                                                               {.replyToMessageId = message.id,
                                                                .text = std::string(text),
                                                                .status = ConversationReplyStatus::needs_you,
                                                                .route = ConversationReplyRoute::model_budget});
    if (!message.channelScope.empty()) channelStorage.queueChannelReply(message.id, reply, text, Json::array());
    database.exec("UPDATE conversation_turns SET status='paused',pause_id=? WHERE message_id=?", {reply, message.id});
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
        turn["allowance"] = p_->modelCalls.messageModelBudget(rows.number(1)).initialLimit;
        result.push_back(std::move(turn));
    }
    return result;
}

void ModelCalls::approveMessageModelCall(const Person& actor, std::string_view bot, Id message, Id waitMessageId) {
    auto slots = database.query(R"SQL(
SELECT json_extract(s.decision,'$[' || slot.key || '].tool')
FROM conversation_steps s, json_each(s.effect) slot
WHERE s.message_id=? AND s.result IS NULL AND json_type(s.effect)='array' AND slot.type='null'
)SQL",
                                {message});
    bool toolPending = false;
    while (!toolPending && slots.next()) toolPending = isBotTool(slots.string(0));
    const auto attempt =
        approveModelBudget(actor, messageModelBudget(message), toolPending ? "bot_tool" : "conversation_turn", message, 0, 0);
    resumeMessageModelCall(bot, message, waitMessageId, attempt);
}

void ModelCalls::resumeMessageModelCall(std::string_view bot, Id message, Id waitMessageId, Id attempt) {
    database.exec("UPDATE conversation_turns SET status='running',pause_id=NULL,reserved_attempt=? WHERE message_id=?",
                  {attempt ? Json(attempt) : Json(nullptr), message});
    journalStorage.journalConversationBudgetApproved(message, waitMessageId, bot);
}
}
