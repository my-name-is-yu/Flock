#include "storage.hpp"
#include "conversation_storage.hpp"
#include "tool_batch.hpp"

#include <algorithm>
#include <future>
#include <utility>

namespace flockv0 {
bool allowedAfterWebEvidence(const Decision& call) {
    if (gitOperation(call) == "read") return true;
    constexpr std::string_view readOnly[]{
        "reply","web_search","web_fetch","read_result","find_tasks","flock_session_search","flock_session_events",
        "list_secrets","list_connections","check_environment_readiness","workspace_read_file",
        "workspace_search","workspace_command_status","mcp_discover","mcp_read_resource",
        "skill_list","skill_read","inspect_attachment","workflow_status","expand_context","read_tool_result",
        "read_memory","memory_history","list_schedules"
    };
    return std::ranges::find(readOnly,call.tool) != std::end(readOnly);
}

Json webEvidenceActionDenied() {
    return toolFailure("After reading web evidence, this turn can only read and reply. "
                       "Ask the user to request the action in a new message.");
}

void Store::Impl::saveTurnResults(Id message, int step, const Json& results) {
    exec("UPDATE conversation_steps SET result=?,effect=NULL WHERE message_id=? AND step=? AND result IS NULL",
         {results,message,step});
}

bool Store::Impl::runBotTools(const ToolContext& ctx, int step, const std::vector<ModelCall>& calls,
                             std::stop_token shutdown) {
    std::vector<std::size_t> reserved;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(loadTurnMessage(ctx.messageId))) { transaction.commit(); return false; }
        auto previous = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                              {ctx.messageId,step});
        if (!previous.next()) return true;
        const auto saved = previous.string(0);
        Json results = saved.empty() ? Json(std::vector<Json>(calls.size())) : Json::parse(saved);
        const auto budget = messageModelBudget(ctx.messageId);
        auto approved = takeReservedConversationCall(ctx.messageId);
        const auto scrub = sharedRedactor();
        const auto origin = policyOrigin(loadTurnMessage(ctx.messageId));
        bool waiting = false;
        for (std::size_t i = 0; i < calls.size(); ++i) {
            auto& result = results.at(i);
            if (result.contains("inFlight")) {
                result = {{"ok",false},{"delivery","unknown"},
                          {"error","Flock restarted during this tool call. Its outcome is unknown."}};
            }
            if (!result.is_null()) continue;
            if (!calls[i].rejection.empty()) {
                result = toolFailure(calls[i].rejection);
                continue;
            }
            if (ctx.hasWebEvidence && !allowedAfterWebEvidence(calls[i])) {
                result = webEvidenceActionDenied();
                continue;
            }
            try {
                validateBotTool(ctx,calls[i]);
                for (const auto index : reserved)
                    if (tool_batch::conflict(calls[i],calls[index])) throw Conflict("Tool calls must be independent");
            } catch (const Conflict& error) {
                result = toolFailure(scrub.text(error.what()));
                continue;
            } catch (const StorageError&) {
                throw;
            } catch (const std::exception&) {
                result = toolFailure("Bot tool target is unavailable");
                continue;
            }
            auto resource = authorizedBotToolScope(ctx.bot,calls[i]);
            if (resource->action == Action::mcp_invoke)
                resource->routine = ctx.settings.mcp->readOnlyHint(resource->target,field(calls[i].args,"name"));
            const auto key = tool_batch::turnCallKey(ctx.messageId,step,i);
            const auto payload = Json::array({calls[i].tool,calls[i].args});
            const auto now = policyClock();
            const auto gate = approveAction(origin,*resource,key,payload,now,false);
            if (gate != ApprovalGate::allow) {
                if (gate == ApprovalGate::waiting) waiting = true;
                else result = toolFailure(approvalError(gate));
                continue;
            }
            if (!std::exchange(approved,0) && !reserveModelCall(budget,"bot_tool",ctx.messageId,0,0)) continue;
            approveAction(origin,*resource,key,payload,now);
            result = {{"inFlight",true}};
            reserved.push_back(i);
        }
        releaseModelCall(approved);
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",
             {results,ctx.messageId,step});
        transaction.commit();
        if (waiting && reserved.empty()) return false;
    }

    const auto persist = [&](std::size_t i, Json result) {
        std::lock_guard lock(mutex);
        const auto scrub = calls[i].tool == "inspect_attachment" ? redactor(ctx.bot) : sharedRedactor();
        auto safe = scrub.record(std::move(result));
        const auto encoded = safe.dump(-1,' ',false,Json::error_handler_t::replace);
        exec("UPDATE conversation_steps SET effect=json_set(effect,?,json(?)) WHERE message_id=? AND step=?",
             {"$[" + std::to_string(i) + "]",encoded,ctx.messageId,step});
    };
    std::vector<std::future<void>> workers;
    workers.reserve(reserved.size());
    for (const auto i : reserved) {
        try {
            workers.push_back(std::async(std::launch::async,[&,i] {
                Json result;
                try {
                    result = executeBotTool(ctx,calls[i]);
                    if (!result.contains("ok")) result["ok"] = true;
                } catch (const ToolOutcomeUnknown& error) {
                    result = {{"ok",false},{"error",error.what()},{"delivery","unknown"}};
                } catch (const Conflict& error) {
                    result = toolFailure(error.what());
                } catch (const StorageError&) {
                    throw;
                } catch (const std::exception&) {
                    result = toolFailure("Bot tool execution failed");
                }
                persist(i,std::move(result));
            }));
        } catch (const std::system_error&) { persist(i,toolFailure("Could not start the Bot tool")); }
    }
    for (auto& worker : workers) worker.get();
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto row = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=?",{ctx.messageId,step});
    if (!row.next()) throw Conflict("Bot tool step is missing");
    const auto results = row.json(0);
    if (query("SELECT 1 FROM conversation_turns WHERE message_id=? AND status='policy_waiting'",{ctx.messageId}).next()) {
        transaction.commit();
        return false;
    }
    const bool pending = std::ranges::any_of(results,[](const auto& result) { return result.is_null(); });
    if (pending) {
        const auto message = loadTurnMessage(ctx.messageId);
        if (authorizeTurn(message)) pauseTurn(message);
    } else saveTurnResults(ctx.messageId,step,results);
    transaction.commit();
    return !pending;
}
}
