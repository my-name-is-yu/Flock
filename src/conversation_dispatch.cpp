#include "flock/bot_access.hpp"
#include "flock/bot_id.hpp"
#include "conversation_call.hpp"
#include "wait_storage.hpp"
#include "task_routing.hpp"
#include "task_records.hpp"
#include "session_read_storage.hpp"
#include "session_participation_storage.hpp"
#include "session_message_storage.hpp"
#include "secret_storage.hpp"
#include "policy_approvals.hpp"
#include "git_storage.hpp"
#include "conversation_tools.hpp"
#include "conversation_turns.hpp"
#include "channel_storage.hpp"
#include "conversation_storage.hpp"
#include "journal_conversation.hpp"
#include "tool_batch.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/decision.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "policy_origin.hpp"
#include "workspace_internal.hpp"
#include "flock/git_host.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
bool hasDestinationSelector(const Json& args) {
    return args.is_object() && (args.contains("destination") || args.contains("environmentId") || args.contains("projectId") ||
                                args.contains("gatewayEnvironmentId"));
}

using conversation::Message;
using conversation_storage::hasText;
using conversation_storage::text;
using conversation_storage::ToolError;

namespace {
Json toolErrorResult(const ToolError& error) {
    auto result = error.details;
    result["ok"] = false;
    result["error"] = error.what();
    return result;
}
}

void ConversationStorage::saveConversationCallResult(const Message& message, int step, Json result) {
    conversationTools.saveTurnResults(message.id, step, Json::array({secretStorage.redactor(message.bot).record(std::move(result))}));
}

CallOutcome ConversationStorage::executeConversationStep(const Message& message, int step, const std::vector<ModelCall>& calls,
                                                         std::string_view effectType, const BotTools& settings,
                                                         const ConversationCallContext& ctx) {
    using enum CallOutcome;
    const auto& decision = calls.front();
    const auto& tool = decision.tool;
    if (calls.size() == 1 && tool == "send_to_task" && decision.args.is_object() && decision.args.contains("taskId")) {
        bool destinationTask = false;
        try {
            std::lock_guard lock(database.mutex);
            Transaction transaction(database.db);
            destinationTask = !ownTask(conversation_storage::identifier(decision.args, "taskId", true), message).runResults().empty();
            transaction.commit();
        } catch (const ToolError&) {
        }
        if (destinationTask)
            return sessionMessageStorage.sendSessionMessage(message, step, decision, tool_batch::turnCallKey(message.id, step), ctx);
    }
    if (calls.size() == 1 && tool == "cancel_task" && hasDestinationSelector(decision.args))
        return sessionMessageStorage.cancelDestinationRun(message, step, decision, tool_batch::turnCallKey(message.id, step),
                                                          decision.rejection.empty(), ctx.shutdown);
    if (effectType == "array" || calls.size() > 1 || isBotTool(tool) ||
        tool_batch::conversationRead(tool) != tool_batch::ConversationRead::none) {
        const ToolContext tools{message.bot, message.id, settings};
        return conversationTools.runToolBatch(message, tools, step, calls, ctx) ? advance : stop;
    }
    const auto key = tool_batch::turnCallKey(message.id, step);
    const bool offered = decision.rejection.empty();
    if (offered || effectType == "object") {
        if (tool == "message_session" || tool == "fix_pull_request")
            return sessionMessageStorage.sendSessionMessage(message, step, decision, key, ctx);
        if (tool == "interrupt_session")
            return sessionMessageStorage.runSessionEffect(message, step, decision, key, offered, ctx.t3, ctx.shutdown);
    }

    if (!offered) {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (!authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        auto result = toolFailure(decision.rejection);
        if (isGitTool(tool) && gitStorage.gitEffectStarted(key)) {
            result["delivery"] = "unknown";
            result["error"] = "The host outcome is unknown. Check the target before retrying.";
        }
        conversationTools.saveTurnResults(message.id, step, Json::array({result}));
        transaction.commit();
        return advance;
    }

    if (tool == "invite_bot") {
        return sessionParticipationStorage.inviteSessionBot(message, step, decision, ctx.t3, ctx.intake, ctx.peers, ctx.shutdown) ? advance
                                                                                                                                  : stop;
    }
    if (tool == "leave_session") {
        return sessionParticipationStorage.leaveSession(sessionReadStorage, message, step, decision, ctx.t3, ctx.shutdown) ? advance : stop;
    }

    if (tool == "reply") return executeConversationReply(message, step, decision, key, ctx.clock);
    if (tool == "request_bot_change") return executeConversationBotChange(message, step, decision, key, ctx);

    if (!isGitTool(tool)) return executeLocalConversationCall(message, step, decision, key, ctx);

    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (ctx.shutdown.stop_requested()) return stop;
        const bool allowed = authorizeTurn(message);
        transaction.commit();
        if (!allowed) return stop;
    }

    auto result = gitStorage.gitConversationTool(ctx.gitHost, message, decision, key);
    if (result.value("policyWaiting", false)) return waiting;

    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    saveConversationCallResult(message, step, std::move(result));
    transaction.commit();
    return advance;
}

CallOutcome ConversationStorage::executeConversationBotChange(const Message& message, int step, const Decision& decision,
                                                              std::string_view key, const ConversationCallContext& ctx) {
    using enum CallOutcome;
    const auto id = message.id;
    Json result;
    {
        std::lock_guard lock(database.mutex);
        try {
            if (!message.channelScope.empty()) throw ToolError("Use Flock Web for this action");
            const auto& args = decision.args;
            const auto operation = toolText(args, "operation", 32);
            const auto target = toolText(args, "targetBotId", 128);
            const auto name = toolText(args, "name", 128);
            const auto actor = turnActor(message);
            const auto action = operation == "create_bot" ? Action::bot_create : Action::bot_rename;
            if ((operation != "create_bot" && operation != "rename_bot") || !validBotId(target) ||
                !permissions.allows(actor, operation == "create_bot" ? Action::bot_create : Action::bot_settings, target))
                throw ToolError("Bot change denied by policy");
            const auto payload = Json::array({decision.tool, args});
            {
                Transaction authorization(database.db);
                if (!authorizeTurn(message) || ctx.shutdown.stop_requested()) {
                    authorization.commit();
                    return stop;
                }
                const auto gate = policyApprovals.approveAction(policyApprovals.policyOrigin(message), {action, target}, key, payload,
                                                                ctx.clock(), false);
                authorization.commit();
                if (gate == ApprovalGate::waiting) return waiting;
                if (gate != ApprovalGate::allow) throw ToolError(std::string(approvalError(gate)));
            }
            constexpr int maxCreationAttempts = 4;
            auto retries = database.query("SELECT failures FROM conversation_retries WHERE message_id=?", {id});
            const bool exhausted = retries.next() && retries.number(0) >= maxCreationAttempts - 1;
            const auto changeId = botAccess.applyBotChange(message.bot, actor, operation, target, name, id, step, ctx.applyBotChange,
                                                           exhausted ? &ctx.reconcileBotCreation : nullptr);
            Transaction completed(database.db);
            policyApprovals.consumePolicyApproval(key);
            conversationTools.saveTurnResults(id, step, Json::array({Json{{"ok", true}, {"changeId", changeId}, {"status", "applied"}}}));
            completed.commit();
            return advance;
        } catch (const BotCreationUncertain&) {
            deferConversation(id, ctx.clock());
            return stop;
        } catch (const Conflict& error) {
            result = toolFailure(error.what());
        } catch (const ToolError& error) {
            result = toolFailure(error.what());
        }
    }
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    database.exec("DELETE FROM conversation_retries WHERE message_id=?", {id});
    saveConversationCallResult(message, step, std::move(result));
    transaction.commit();
    return advance;
}

CallOutcome ConversationStorage::executeConversationReply(const Message& message, int step, const Decision& decision, std::string_view key,
                                                          const std::function<Seconds()>& clock) {
    using enum CallOutcome;
    const auto id = message.id;
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    try {
        const auto& args = decision.args;
        if (!args.is_object()) throw ToolError("Tool arguments must be an object");
        auto reply = text(args, "text", 8000, true);
        const auto continuesQuestion = args.find("continuesQuestion");
        const conversation::QuestionAnchor* question = nullptr;
        if (continuesQuestion != args.end() && !continuesQuestion->is_null()) {
            if (!continuesQuestion->is_boolean()) throw ToolError("continuesQuestion must be a boolean");
            if (continuesQuestion->get<bool>()) {
                if (!message.question) throw ToolError("Use continuesQuestion only for a reply about the attached task question");
                const auto taskId = message.question->taskId;
                if (const auto error = questionAnswerError(taskRecords.load(taskId), message, waitStorage.currentWait(taskId)))
                    throw ToolError(error);
                question = &*message.question;
            }
        }
        Json suggestions = Json::array();
        if (const auto list = args.find("suggestions"); list != args.end() && !list->is_null()) {
            if (!list->is_array() || list->size() > 4) throw ToolError("suggestions must be up to 4 strings");
            for (const auto& item : *list) {
                if (!item.is_string() || !hasText(item.get<std::string>()) || item.get<std::string>().size() > 200)
                    throw ToolError("Each suggestion must be a nonempty string up to 200 bytes");
                if (std::ranges::find(suggestions, item) != suggestions.end())
                    throw ToolError("Each suggestion must be distinct so the user's choice is unambiguous");
                suggestions.push_back(item);
            }
        }
        if (const auto resource = channelStorage.channelToolPolicy(id, "reply")) {
            const auto gate = policyApprovals.approveAction(
                policyApprovals.policyOrigin(message), *resource, key,
                Json{{"text", reply}, {"suggestions", suggestions}, {"continuesQuestion", question != nullptr}}, clock());
            if (gate == ApprovalGate::waiting) {
                transaction.commit();
                return waiting;
            }
            if (gate != ApprovalGate::allow) throw ToolError(std::string(approvalError(gate)));
        }
        conversationTools.saveTurnResults(id, step, Json::array({Json{{"ok", true}}}));
        finishTurn(message, std::move(reply), suggestions, ConversationReplyRoute::reply, question);
        transaction.commit();
        return finished;
    } catch (const ToolError& error) {
        saveConversationCallResult(message, step, toolErrorResult(error));
    }
    transaction.commit();
    return advance;
}

std::optional<Resource> ConversationStorage::conversationCallResource(const Message& message, const Decision& decision) {
    const auto& tool = decision.tool;
    if (tool == "create_schedule" || tool == "create_reminder" || tool == "create_trigger" || tool == "remove_schedule")
        return Resource{Action::automation_change, message.bot};
    const bool shared = decision.args.contains("docId");
    if (tool == "remember_preference") return Resource{Action::private_memory_write, message.bot};
    if (tool == "propose_memory_edit" || tool == "apply_memory_edit" || tool == "revert_memory_edit")
        return shared ? Resource{Action::shared_memory_write, field(decision.args, "docId")}
                      : Resource{Action::private_memory_write, message.bot};
    if (!shared) return channelStorage.channelToolPolicy(message.id, tool);

    if (tool == "read_memory" || tool == "memory_history") return Resource{Action::shared_memory_read, field(decision.args, "docId")};
    return std::nullopt;
}

CallOutcome ConversationStorage::executeLocalConversationCall(const Message& message, int step, const Decision& decision,
                                                              std::string_view key, const ConversationCallContext& ctx) {
    using enum CallOutcome;
    Json result;
    std::optional<WorkSession> selectedSession;
    LinkedTaskDestinations linked;
    if (decision.tool == "start_task" && decision.args.is_object() && !field(decision.args, "sessionRef").empty()) {
        try {
            selectedSession = sessionReadStorage.resolveWorkSession(message, field(decision.args, "sessionRef"), ctx.t3);
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception&) {
            result = toolFailure("The selected T3 Session could not be verified. Search for it again.");
        }
    }
    if ((decision.tool == "start_task" || decision.tool == "change_task") && !selectedSession && result.is_null())
        linked = taskRouting.discoverLinkedTaskDestinations(ctx.t3, message.bot);
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    if (!result.is_null()) {
        saveConversationCallResult(message, step, std::move(result));
        transaction.commit();
        return advance;
    }
    if (ctx.shutdown.stop_requested()) return stop;
    auto completed = applyLocalConversationCall(message, step, decision, key, ctx.clock(), ApprovalWait::pause,
                                                selectedSession ? &*selectedSession : nullptr, linked);
    if (completed) saveConversationCallResult(message, step, std::move(*completed));
    transaction.commit();
    return completed ? advance : waiting;
}

std::optional<Json> ConversationStorage::applyLocalConversationCall(const Message& message, int step, const Decision& decision,
                                                                    std::string_view key, Seconds now, ApprovalWait waitMode,
                                                                    const WorkSession* selectedSession,
                                                                    const LinkedTaskDestinations& linked) {
    if (const auto resource = conversationCallResource(message, decision)) {
        if (resource->action == Action::private_memory_write &&
            !permissions.allows(turnActor(message), Action::bot_settings, resource->target))
            return toolFailure("Memory access denied");
        const auto gate = policyApprovals.approveAction(policyApprovals.policyOrigin(message), *resource, key,
                                                        Json::array({decision.tool, decision.args}), now, true, waitMode);
        if (gate == ApprovalGate::waiting) return std::nullopt;
        if (gate != ApprovalGate::allow) return toolFailure(approvalError(gate));
    }
    database.exec("SAVEPOINT turn_tool");
    try {
        auto result = applyTurnTool(message, step, decision, now, selectedSession, linked);
        database.exec("RELEASE turn_tool");
        return result;
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        database.exec("ROLLBACK TO turn_tool");
        database.exec("RELEASE turn_tool");
        const auto* failed = dynamic_cast<const ToolError*>(&error);
        return failed ? toolErrorResult(*failed) : toolFailure(error.what());
    }
}
}
