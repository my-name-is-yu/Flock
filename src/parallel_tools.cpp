#include "storage.hpp"
#include "conversation_storage.hpp"
#include "tool_batch.hpp"

#include <algorithm>
#include <future>

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

bool Store::Impl::runToolBatch(const conversation::Message& message, const ToolContext& ctx, int step,
                               const std::vector<ModelCall>& calls, T3& t3, std::stop_token shutdown) {
    const auto id = message.id;
    const auto checkpoint = std::pair{id,step};
    const auto saveSlot = [&](std::size_t i, const Json& result) {
        const auto encoded = result.dump(-1,' ',false,Json::error_handler_t::replace);
        exec("UPDATE conversation_steps SET effect=json_set(effect,?,json(?))"
             " WHERE message_id=? AND step=? AND result IS NULL",
             {"$[" + std::to_string(i) + "]",encoded,id,step});
    };
    const auto flushResults = [&] {
        const auto entry = toolBatchResults.find(checkpoint);
        if (entry == toolBatchResults.end()) return;
        auto& slots = entry->second;
        for (auto it = slots.begin(); it != slots.end();) {
            const auto& tool = calls.at(it->first).tool;
            const auto scrub = !isBotTool(tool) || tool == "inspect_attachment" ? redactor(message.bot) : sharedRedactor();
            saveSlot(it->first,scrub.record(it->second));
            it = slots.erase(it);
        }
        toolBatchResults.erase(entry);
    };
    const auto persist = [&](std::size_t i, Json result) {
        std::lock_guard lock(mutex);
        // Remember completed calls if SQLite cannot save their results on this tick.
        toolBatchResults[checkpoint].insert_or_assign(i,std::move(result));
        flushResults();
    };

    std::vector<Decision> runnable;
    std::vector<std::size_t> slots;
    {
        std::lock_guard lock(mutex);
        flushResults();
        Transaction transaction(db);
        if (shutdown.stop_requested()) return false;
        if (!authorizeTurn(message)) { transaction.commit(); return false; }
        auto previous = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                              {id,step});
        if (!previous.next()) return true;
        const auto saved = previous.string(0);
        auto results = saved.empty() ? Json(std::vector<Json>(calls.size())) : Json::parse(saved);
        const auto scrub = sharedRedactor();
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const auto& call = calls[i];
            auto& result = results.at(i);
            if (result.contains("inFlight")) {
                if (tool_batch::classify(call) == tool_batch::CallClass::parallel_read) result = nullptr;
                else result = {{"ok",false},{"delivery","unknown"},
                               {"error","Flock restarted during this tool call. Its outcome is unknown."}};
            }
            if (!result.is_null()) continue;
            if (!call.rejection.empty()) {
                result = toolFailure(call.rejection);
            } else if (ctx.hasWebEvidence && !allowedAfterWebEvidence(call)) {
                result = webEvidenceActionDenied();
            } else if (!tool_batch::batchable(call.tool) ||
                       (call.tool == "check_environment_readiness" && !message.channelScope.empty())) {
                result = toolFailure(tool_batch::classify(call) == tool_batch::CallClass::terminal ?
                    "Reply after reviewing these results." : "Run this action on its own after reviewing these results.");
                result["notRun"] = true;
            } else {
                try {
                    if (isBotTool(call.tool)) validateBotTool(ctx,call);
                } catch (const Conflict& error) {
                    result = toolFailure(scrub.text(error.what()));
                } catch (const StorageError&) {
                    throw;
                } catch (const std::exception&) {
                    result = toolFailure("Bot tool target is unavailable");
                }
                if (result.is_null()) {
                    runnable.push_back(call);
                    slots.push_back(i);
                }
            }
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",{results,id,step});
        transaction.commit();
    }

    bool waitingForApproval = false;
    std::vector<std::size_t> blocked;
    for (const auto& wave : tool_batch::planWaves(runnable)) {
        std::vector<std::size_t> reserved;
        {
            std::lock_guard lock(mutex);
            Transaction transaction(db);
            if (shutdown.stop_requested()) return false;
            if (!authorizeTurn(message)) { transaction.commit(); return false; }
            const auto origin = policyOrigin(message);
            const auto budget = messageModelBudget(id);
            for (const auto index : wave) {
                if (std::ranges::any_of(blocked,[&](const auto earlier) {
                    return tool_batch::validatedCallsConflict(runnable[earlier],runnable[index]);
                })) {
                    blocked.push_back(index);
                    continue;
                }
                const auto i = slots[index];
                const auto& call = calls[i];
                if (isBotTool(call.tool)) {
                    auto resource = authorizedBotToolScope(ctx.bot,call);
                    if (!resource) {
                        saveSlot(i,toolFailure("Bot tool access denied"));
                        continue;
                    }
                    if (resource->action == Action::mcp_invoke)
                        resource->routine = ctx.settings.mcp->readOnlyHint(resource->target,field(call.args,"name"));
                    const auto key = tool_batch::turnCallKey(id,step,i);
                    const auto payload = Json::array({call.tool,call.args});
                    const auto now = policyClock();
                    // Keep the turn runnable until independent calls in all waves have settled.
                    const auto gate = approveAction(origin,*resource,key,payload,now,false,ApprovalWait::defer);
                    if (gate == ApprovalGate::waiting) {
                        waitingForApproval = true;
                        blocked.push_back(index);
                        continue;
                    }
                    if (gate != ApprovalGate::allow) {
                        saveSlot(i,toolFailure(approvalError(gate)));
                        continue;
                    }
                    if (!takeReservedConversationCall(id) && !reserveModelCall(budget,"bot_tool",id,0,0)) {
                        blocked.push_back(index);
                        continue;
                    }
                    approveAction(origin,*resource,key,payload,now);
                }
                saveSlot(i,{{"inFlight",true}});
                reserved.push_back(i);
            }
            transaction.commit();
        }

        std::vector<std::future<void>> workers;
        workers.reserve(reserved.size());
        for (const auto i : reserved) {
            try {
                workers.push_back(std::async(std::launch::async,[&,i] {
                    const auto& call = calls[i];
                    Json result;
                    try {
                        if (isBotTool(call.tool)) {
                            result = executeBotTool(ctx,call);
                            if (!result.contains("ok")) result["ok"] = true;
                        } else {
                            result = executeConversationRead(message,call,t3,tool_batch::turnCallKey(id,step,i));
                        }
                    } catch (const ToolOutcomeUnknown& error) {
                        result = {{"ok",false},{"error",error.what()},{"delivery","unknown"}};
                    } catch (const Conflict& error) {
                        result = toolFailure(error.what());
                    } catch (const StorageError&) {
                        throw;
                    } catch (const std::exception&) {
                        result = toolFailure(isBotTool(call.tool) ? "Bot tool execution failed" : "Conversation read failed");
                    }
                    persist(i,std::move(result));
                }));
            } catch (const std::system_error&) { persist(i,toolFailure("Could not start the tool call")); }
        }
        for (auto& worker : workers) worker.get();
    }

    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto row = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=?",{id,step});
    if (!row.next()) throw Conflict("Tool batch step is missing");
    const auto results = row.json(0);
    if (!authorizeTurn(message)) { transaction.commit(); return false; }
    releaseModelCall(takeReservedConversationCall(id));
    const bool pending = std::ranges::any_of(results,[](const auto& result) { return result.is_null(); });
    if (!pending) {
        saveTurnResults(id,step,results);
    } else if (waitingForApproval) {
        exec("UPDATE conversation_turns SET status='policy_waiting' WHERE message_id=?"
             " AND EXISTS (SELECT 1 FROM policy_approvals WHERE turn=? AND status='pending')",{id,id});
    } else {
        pauseTurn(message);
    }
    transaction.commit();
    return !pending;
}
}
