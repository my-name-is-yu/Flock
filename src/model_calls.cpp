#include "flock/bot_lifecycle.hpp"
#include "database.hpp"
#include "secret_storage.hpp"
#include "model_calls.hpp"
#include "memory_learnings.hpp"
#include "bot_lifecycle_storage.hpp"
#include "conversation_storage.hpp"
#include "flock/limits.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/decision.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "flock/git_host.hpp"
#include "flock/workspace_tools.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {

bool repairableModelOutput(ModelErrorCode code) {
    return code == ModelErrorCode::invalid_tool_count || code == ModelErrorCode::invalid_tool_output ||
           code == ModelErrorCode::unavailable_tool || code == ModelErrorCode::invalid_tool_arguments;
}

namespace {
bool redactsProse(std::string_view tool, std::string_view key) {
    if (tool == "reply") return key == "text";
    return tool == "write_memory" && (key == "content" || key == "summary");
}

bool sharedMemoryTool(std::string_view tool) {
    return tool == "read_memory" || tool == "memory_history" || tool == "write_memory" || tool == "revert_memory_edit";
}

}

bool recordsTaskDecisionReason(std::string_view tool) {
    constexpr std::string_view recorded[]{"delegate", "follow_up", "complete", "ask_user", "answer_session_question"};
    return std::ranges::find(recorded, tool) != std::end(recorded);
}

bool validDecisionReason(std::string_view reason) {
    return reason.size() <= limits::model::maxDecisionReasonBytes && conversation_storage::hasText(reason);
}

bool modelArgumentsContainProtected(const SecretRedactor& scrub, std::string_view tool, const Json& args) {
    if (!args.is_object()) return scrub.containsProtected(args);
    for (const auto& [key, value] : args.items()) {
        const bool protocolKey =
            redactsProse(tool, key) || (tool == "reply" && key == "suggestions") || (tool == "complete" && key == "learnings");
        if ((!protocolKey && scrub.containsProtected(key)) || scrub.containsProtected(value)) return true;
    }
    return false;
}

std::string ModelCalls::messageModelScope(Id messageId) const {
    for (;;) {
        auto row = database.query(R"SQL(
SELECT json_extract(m.routing,'$.sessionWake.sourceMessageId'),i.source_message_id FROM messages m
LEFT JOIN session_invitations i ON i.message_id=m.id WHERE m.id=?
)SQL",
                                  {messageId});
        if (!row.next()) throw Conflict("The originating message is missing");
        const auto source = row.null(0) ? row.number(1) : row.number(0);
        if (!source) return "message/" + std::to_string(messageId);
        if (source >= messageId) throw Conflict("Invalid model call ancestry");
        messageId = source;
    }
}

void ModelCalls::recordBotTool(std::string_view scope, Id messageId) {
    recordAttempt(scope, "bot_tool", messageId, 0, 0);
}

Id ModelCalls::recordAttempt(std::string_view scope, std::string_view purpose, Id messageId, Id taskId, Id eventId) {
    database.exec("INSERT INTO model_attempts(budget_id,purpose,time,message_id,task_id,event_id) VALUES(?,?,?,?,?,?)",
                  {scope, purpose, unixTime(), messageId ? Json(messageId) : Json(nullptr), taskId ? Json(taskId) : Json(nullptr),
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

void ModelCalls::releaseModelCall(Id attempt) {
    if (attempt) database.exec("DELETE FROM model_attempts WHERE id=?", {attempt});
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
               permissions.forbidden(bot,
                                     (call.tool == "read_memory" || call.tool == "memory_history") ? Action::shared_memory_read
                                                                                                   : Action::shared_memory_write,
                                     field(call.args, "docId"))) {
        rejection = "Memory access denied";
    } else {
        const auto scrub = modelCallRedactor(bot, call.tool, call.args, gitNotification);
        if (call.args.is_object()) {
            if (call.tool == "complete") {
                const auto learnings = call.args.find("learnings");
                if (learnings != call.args.end()) {
                    auto safe = safeTaskLearnings(*learnings, scrub);
                    if (safe.empty())
                        call.args.erase(learnings);
                    else
                        *learnings = std::move(safe);
                }
            }
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
        safe.config = scrub.record(input.config, SecretRedactor::Fields::external);
        safe.notes.soul = scrub.text(input.notes.soul);
        safe.notes.memory = scrub.text(input.notes.memory);
        safe.feedback = scrub.text(input.feedback);
        if (scrub.containsProtectedText(safe.learningRunId)) safe.learningRunId.clear();
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
}
