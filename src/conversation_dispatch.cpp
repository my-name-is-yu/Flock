#include "flock/bot_access.hpp"
#include "turn_results.hpp"
#include "conversation_messages.hpp"
#include "flock/bot_id.hpp"
#include "flock/avatar_style.hpp"
#include "flock/intake.hpp"
#include "flock/limits.hpp"
#include "conversation_call.hpp"
#include "bot_lifecycle_storage.hpp"
#include "bot_setup_storage.hpp"
#include "wait_storage.hpp"
#include "task_routing.hpp"
#include "task_records.hpp"
#include "session_read_storage.hpp"
#include "session_participation_storage.hpp"
#include "session_message_storage.hpp"
#include "secret_storage.hpp"
#include "memory_storage.hpp"
#include "policy_approvals.hpp"
#include "git_storage.hpp"
#include "conversation_tools.hpp"
#include "conversation_turns.hpp"
#include "turn_runner.hpp"
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

std::optional<BotAvatarChoice> requestedAvatar(const Json& args) {
    const auto avatar = args.find("avatar");
    if (avatar == args.end()) return std::nullopt;
    const auto style = validatedAvatarStyle(args);
    const auto& kind = style.at("kind").get_ref<const std::string&>();
    if (kind == "upload") throw ToolError("Avatar kind must be shape or icon; upload images from Flock Web");
    BotAvatarChoice choice{kind, style.at(kind == "shape" ? "example" : "icon").get<std::string>(), std::nullopt};
    if (avatar->contains("color")) choice.color = style.at("color").get<std::string>();
    return choice;
}
}

void TurnRunner::saveConversationCallResult(const Message& message, int step, Json result) {
    turnResults.save(message.id, step, Json::array({secretStorage.redactor(message.bot).record(std::move(result))}));
}

CallOutcome TurnRunner::executeConversationStep(const Message& message, int step, const std::vector<ModelCall>& calls,
                                                std::string_view effectType, const BotTools& settings, const ConversationCallContext& ctx) {
    using enum CallOutcome;
    const auto& decision = calls.front();
    const auto& tool = decision.tool;
    const auto key = tool_batch::turnCallKey(message.id, step);
    if (calls.size() == 2 && tool == botSetupTool && calls[1].tool == "reply" && decision.rejection.empty() && calls[1].rejection.empty())
        return executeBotSetup(message, step, decision, key, ctx, &calls[1]);
    if (calls.size() == 1 && tool == "send_to_task" && decision.args.is_object() && decision.args.contains("taskId")) {
        bool destinationTask = false;
        try {
            std::lock_guard lock(database.mutex);
            Transaction transaction(database.db);
            destinationTask =
                !conversationTurns.ownTask(conversation_storage::identifier(decision.args, "taskId", true), message).runResults().empty();
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
        const auto readStore = [&](const Decision& call, std::string_view key) {
            return applyLocalConversationCall(message, step, call, key, ctx.clock(), ApprovalWait::defer, nullptr, TaskDestinations{},
                                              settings);
        };
        return conversationTools.runToolBatch(message, tools, step, calls, ctx, readStore) ? advance : stop;
    }
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
        if (!conversationTurns.authorizeTurn(message)) {
            transaction.commit();
            return stop;
        }
        auto result = toolFailure(decision.rejection);
        if (isGitTool(tool) && gitStorage.gitEffectStarted(key)) {
            result["delivery"] = "unknown";
            result["error"] = "The host outcome is unknown. Check the target before retrying.";
        }
        turnResults.save(message.id, step, Json::array({result}));
        transaction.commit();
        return advance;
    }

    if (tool == "invite_bot") {
        return sessionParticipationStorage.inviteSessionBot(message, step, decision, ctx.t3, ctx.intake, ctx.peers, ctx.shutdown) ? advance
                                                                                                                                  : stop;
    }
    if (tool == "message_bot") return executeMessageBot(message, step, decision, ctx);
    if (tool == "leave_session") {
        return sessionParticipationStorage.leaveSession(sessionReadStorage, message, step, decision, ctx.t3, ctx.shutdown) ? advance : stop;
    }

    if (tool == "reply") return executeConversationReply(message, step, decision, key, ctx.clock);
    if (tool == botSetupTool) return executeBotSetup(message, step, decision, key, ctx);
    if (tool == "request_bot_change") return executeConversationBotChange(message, step, decision, key, ctx);

    if (!isGitTool(tool)) return executeLocalConversationCall(message, step, decision, key, ctx, settings);

    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (ctx.shutdown.stop_requested()) return stop;
        const bool allowed = conversationTurns.authorizeTurn(message);
        transaction.commit();
        if (!allowed) return stop;
    }

    auto result = gitStorage.gitConversationTool(ctx.gitHost, message, decision, key);
    if (result.value("policyWaiting", false)) return waiting;

    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!conversationTurns.authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    saveConversationCallResult(message, step, std::move(result));
    transaction.commit();
    return advance;
}

CallOutcome TurnRunner::executeMessageBot(const Message& message, int step, const Decision& decision, const ConversationCallContext& ctx) {
    using enum CallOutcome;
    // Each hop from the person's message counts, so two Bots cannot keep messaging each other.
    constexpr Id maxDepth = 6;
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (ctx.shutdown.stop_requested()) return stop;
    if (!conversationTurns.authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    Json result;
    try {
        const auto& args = decision.args;
        if (!args.is_object() || args.size() != 2) throw Conflict("Invalid message_bot arguments");
        const auto bot = text(args, "botId", 128, true);
        const auto body = text(args, "text", limits::messages::maxBodyBytes, true);
        if (!validBotId(bot) || body.find('\0') != body.npos) throw Conflict("Invalid message_bot arguments");
        if (message.principalSource != conversation::PrincipalSource::web &&
            message.principalSource != conversation::PrincipalSource::trusted)
            throw Conflict("Message other Bots from a Flock Web conversation");
        if (bot == message.bot) throw Conflict("A Bot cannot message itself");
        const auto actor = conversationMessages.actor(message);
        if (!permissions.allows(actor, Action::bot_decide, message.bot) || !permissions.allows(actor, Action::bot_decide, bot))
            throw Conflict("Bot access is required");
        if (!ctx.peers || std::ranges::none_of(ctx.peers(message.bot), [&](const PeerBot& peer) { return peer.intake.botId == bot; }))
            throw Conflict("The destination Bot is unavailable or not allowed");
        if (botLifecycle.botPaused(bot)) throw Conflict("The destination Bot is paused");
        const auto previous = message.routing.find("botMessage");
        const Id depth = (previous == message.routing.end() ? 0 : previous->at("depth").get<Id>()) + 1;
        if (depth > maxDepth)
            throw Conflict("Bots can message each other at most " + std::to_string(maxDepth) +
                           " times after the person's message. Reply to the person instead.");
        const auto key = "bot-message:" + std::to_string(message.id) + ":" + std::to_string(step);
        auto sent = database.query("SELECT id FROM messages WHERE bot_id=? AND person_id=? AND client_message_id=?", {bot, actor.id, key});
        if (sent.next()) {
            result = {{"ok", true}, {"effect", "sent"}, {"messageId", sent.number(0)}, {"depth", depth}};
        } else {
            if (!ctx.intake) throw Conflict("Bot intake is unavailable");
            const auto routing = ctx.intake(bot, body);
            if (routing.botId != bot) throw Conflict("The destination Bot is unavailable");
            const ConversationBotMessageEntry origin{message.bot, message.routing.at("config").value("botDisplayName", message.bot), depth};
            const auto id =
                conversationTurns.enqueueConversation(actor, routing, message.principalSource, key, body, 0, nullptr, nullptr, &origin);
            result = {{"ok", true}, {"effect", "sent"}, {"messageId", id}, {"depth", depth}};
        }
    } catch (const Conflict& error) {
        result = toolFailure(error.what());
    } catch (const ToolError& error) {
        result = toolErrorResult(error);
    }
    saveConversationCallResult(message, step, std::move(result));
    transaction.commit();
    return advance;
}

CallOutcome TurnRunner::executeBotSetup(const Message& message, int step, const Decision& decision, std::string_view key,
                                        const ConversationCallContext& ctx, const Decision* reply) {
    using enum CallOutcome;
    const auto catalog = botSetup.modelCatalog(decision.args);
    std::lock_guard lock(database.mutex);
    Json result;
    try {
        Transaction authorization(database.db);
        if (!conversationTurns.authorizeTurn(message) || ctx.shutdown.stop_requested()) {
            authorization.commit();
            return stop;
        }
        if (!allowsTurnTool(message, decision.tool)) throw Conflict("This action needs a message from the person");
        if (auto applied = botSetup.recorded(key)) {
            authorization.commit();
            result = std::move(*applied);
        } else {
            botSetup.validate(message.bot, decision.args);
            if (!permissions.allows(conversationMessages.actor(message), Action::bot_settings, message.bot))
                throw Conflict("You cannot change this Bot’s settings");
            const auto gate = policyApprovals.approveAction(policyApprovals.policyOrigin(message), {Action::bot_settings, message.bot}, key,
                                                            decision.args, ctx.clock(), false);
            authorization.commit();
            if (gate == ApprovalGate::waiting) return waiting;
            if (gate != ApprovalGate::allow) throw Conflict(std::string(approvalError(gate)));
            result = botSetup.apply(message.bot, key, decision.args, catalog, ctx.clock(), 0, message.id);
        }
    } catch (const Conflict& error) {
        result = toolFailure(error.what());
    }
    if (reply && result.value("ok", false)) return executeConversationReply(message, step, *reply, key, ctx.clock, &result);

    Transaction transaction(database.db);
    policyApprovals.consumePolicyApproval(key);
    if (reply)
        turnResults.save(message.id, step, Json::array({result, toolFailure("The Bot setup change did not apply")}));
    else
        saveConversationCallResult(message, step, std::move(result));
    transaction.commit();
    return advance;
}

CallOutcome TurnRunner::executeConversationBotChange(const Message& message, int step, const Decision& decision, std::string_view key,
                                                     const ConversationCallContext& ctx) {
    using enum CallOutcome;
    const auto id = message.id;
    Json result;
    {
        std::lock_guard lock(database.mutex);
        try {
            if (!allowsTurnTool(message, decision.tool)) throw ToolError("This action needs a message from the person");
            if (!message.channelScope.empty()) throw ToolError("Use Flock Web for this action");
            const auto& args = decision.args;
            const auto operation = toolText(args, "operation", 32);
            const auto target = toolText(args, "targetBotId", 128);
            const auto name = toolText(args, "name", 128);
            const auto actor = conversationMessages.actor(message);
            if (operation != "create_bot" || !validBotId(target) || !permissions.allows(actor, Action::bot_create, target))
                throw ToolError("Bot change denied by policy");
            auto avatar = requestedAvatar(args);
            const auto payload = Json::array({decision.tool, args});
            {
                Transaction authorization(database.db);
                if (!conversationTurns.authorizeTurn(message) || ctx.shutdown.stop_requested()) {
                    authorization.commit();
                    return stop;
                }
                const auto gate = policyApprovals.approveAction(policyApprovals.policyOrigin(message), {Action::bot_create, target}, key,
                                                                payload, ctx.clock(), false);
                authorization.commit();
                if (gate == ApprovalGate::waiting) return waiting;
                if (gate != ApprovalGate::allow) throw ToolError(std::string(approvalError(gate)));
            }
            constexpr int maxCreationAttempts = 4;
            auto retries = database.query("SELECT failures FROM conversation_retries WHERE message_id=?", {id});
            const bool exhausted = retries.next() && retries.number(0) >= maxCreationAttempts - 1;
            const auto changeId = botAccess.applyBotChange(message.bot, actor, target, name, std::move(avatar), id, step,
                                                           ctx.applyBotChange, exhausted ? &ctx.reconcileBotCreation : nullptr);
            Transaction completed(database.db);
            policyApprovals.consumePolicyApproval(key);
            turnResults.save(id, step, Json::array({Json{{"ok", true}, {"changeId", changeId}, {"status", "applied"}}}));
            completed.commit();
            return advance;
        } catch (const BotCreationUncertain&) {
            conversationTurns.deferConversation(id, ctx.clock());
            return stop;
        } catch (const Conflict& error) {
            result = toolFailure(error.what());
        } catch (const ToolError& error) {
            result = toolFailure(error.what());
        }
    }
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!conversationTurns.authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    database.exec("DELETE FROM conversation_retries WHERE message_id=?", {id});
    saveConversationCallResult(message, step, std::move(result));
    transaction.commit();
    return advance;
}

CallOutcome TurnRunner::executeConversationReply(const Message& message, int step, const Decision& decision, std::string_view key,
                                                 const std::function<Seconds()>& clock, const Json* setup) {
    using enum CallOutcome;
    const auto id = message.id;
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!conversationTurns.authorizeTurn(message)) {
        transaction.commit();
        return stop;
    }
    const auto saveReplyResult = [&](Json result) {
        if (setup) {
            policyApprovals.consumePolicyApproval(key);
            turnResults.save(id, step, Json::array({*setup, result}));
        } else {
            saveConversationCallResult(message, step, std::move(result));
        }
    };
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
                if (const auto error =
                        conversationTurns.questionAnswerError(taskRecords.load(taskId), message, waitStorage.currentWait(taskId)))
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
        saveReplyResult({{"ok", true}});
        conversationTurns.finishTurn(message, std::move(reply), suggestions, ConversationReplyRoute::reply, question);
        transaction.commit();
        return finished;
    } catch (const ToolError& error) {
        saveReplyResult(toolErrorResult(error));
    }
    transaction.commit();
    return advance;
}

std::optional<Resource> TurnRunner::conversationCallResource(const Message& message, const Decision& decision) {
    const auto& tool = decision.tool;
    if (tool == "create_schedule" || tool == "create_reminder" || tool == "create_trigger" || tool == "remove_schedule")
        return Resource{Action::automation_change, message.bot};
    const bool shared = decision.args.contains("docId");
    if (tool == "write_memory" || tool == "revert_memory_edit")
        return shared ? Resource{Action::shared_memory_write, field(decision.args, "docId")}
                      : Resource{Action::private_memory_write, message.bot};
    if (!shared) return channelStorage.channelToolPolicy(message.id, tool);

    if (tool == "read_memory" || tool == "memory_history") return Resource{Action::shared_memory_read, field(decision.args, "docId")};
    return std::nullopt;
}

CallOutcome TurnRunner::executeLocalConversationCall(const Message& message, int step, const Decision& decision, std::string_view key,
                                                     const ConversationCallContext& ctx, const BotTools& settings) {
    using enum CallOutcome;
    Json result;
    std::optional<WorkSession> selectedSession;
    TaskDestinations linked;
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
        linked = taskRouting.discoverTaskDestinations(ctx.t3, message,
                                                      decision.tool == "start_task" || decision.args.contains("projectId") ||
                                                          decision.args.contains("environments") || decision.args.contains("commit"),
                                                      decision.tool == "change_task" ? &decision.args : nullptr);
    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (!conversationTurns.authorizeTurn(message)) {
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
                                                selectedSession ? &*selectedSession : nullptr, linked, settings);
    if (completed) saveConversationCallResult(message, step, std::move(*completed));
    transaction.commit();
    return completed ? advance : waiting;
}

std::optional<Json> TurnRunner::applyLocalConversationCall(const Message& message, int step, const Decision& decision, std::string_view key,
                                                           Seconds now, ApprovalWait waitMode, const WorkSession* selectedSession,
                                                           const TaskDestinations& linked, const BotTools& settings) {
    if (!allowsTurnTool(message, decision.tool)) return toolFailure("This action needs a message from the person");
    if (const auto resource = conversationCallResource(message, decision)) {
        using enum Action;
        const bool writesMemory = resource->action == private_memory_write || resource->action == shared_memory_write;
        if (writesMemory && !memoryStorage.canEditFrom(message)) return toolFailure("Memory access denied");
        if (resource->action == private_memory_write && decision.tool == "write_memory") {
            if (!permissions.allows(message.bot, private_memory_write, message.bot)) return toolFailure("The action was denied by policy.");
        } else {
            const auto gate = policyApprovals.approveAction(policyApprovals.policyOrigin(message), *resource, key,
                                                            Json::array({decision.tool, decision.args}), now, true, waitMode);
            if (gate == ApprovalGate::waiting) return std::nullopt;
            if (gate != ApprovalGate::allow) return toolFailure(approvalError(gate));
        }
    }
    database.exec("SAVEPOINT turn_tool");
    try {
        auto result = applyTurnTool(message, step, decision, now, selectedSession, linked, settings);
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
