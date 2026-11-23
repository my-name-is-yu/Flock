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

namespace {
void recoverSlot(Json& result, const Decision& call) {
    if (!result.contains("inFlight")) return;
    if (tool_batch::classify(call) == tool_batch::CallClass::parallel_read) result = nullptr;
    else result = {{"ok",false},{"delivery","unknown"},
                   {"error","Flock restarted during this tool call. Its outcome is unknown."}};
}

enum class ProviderState { available, routine, unavailable, failed };

ProviderState providerState(const BotTools& settings, const Decision& call) {
    if (!call.tool.starts_with("mcp_")) return ProviderState::available;
    const auto server = field(call.args,"server");
    try {
        if (!settings.mcp->hasServer(server)) return ProviderState::unavailable;
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception&) {
        return ProviderState::failed;
    }
    return call.tool == "mcp_call_tool" && settings.mcp->readOnlyHint(server,field(call.args,"name"))
        ? ProviderState::routine : ProviderState::available;
}
}

class Store::Impl::ToolBatch {
public:
    ToolBatch(Impl& store, const conversation::Message& message, const ToolContext& tools, int step,
              const std::vector<ModelCall>& calls, T3& t3, std::stop_token shutdown)
        : store_(store), message_(message), tools_(tools), step_(step), calls_(calls), t3_(t3), shutdown_(shutdown),
          blocked_(calls.size()) {}

    bool run() {
        std::vector<tool_batch::Candidate> pending;
        {
            std::lock_guard lock(store_.mutex);
            flushResults();
            Transaction transaction(store_.db);
            if (!authorize()) { transaction.commit(); return false; }
            auto row = store_.query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                                    {message_.id,step_});
            if (!row.next()) return true;
            const auto saved = row.string(0);
            auto results = saved.empty() ? Json(std::vector<Json>(calls_.size())) : Json::parse(saved);
            for (std::size_t slot = 0; slot < calls_.size(); ++slot) {
                const auto& call = calls_[slot];
                auto& result = results.at(slot);
                recoverSlot(result,call);
                if (!result.is_null()) continue;
                result = admissionFailure(call);
                if (result.is_null()) pending.push_back({slot,call});
            }
            store_.exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",{results,message_.id,step_});
            transaction.commit();
        }

        for (const auto& wave : tool_batch::planWaves(pending)) {
            const auto reserved = reserveWave(wave);
            if (!reserved) return false;
            executeWave(*reserved);
        }
        return finish();
    }

private:
    Impl& store_;
    const conversation::Message& message_;
    const ToolContext& tools_;
    int step_;
    const std::vector<ModelCall>& calls_;
    T3& t3_;
    std::stop_token shutdown_;
    std::vector<bool> blocked_;
    bool approvalWaiting_ = false;

    bool authorize() {
        return !shutdown_.stop_requested() && store_.authorizeTurn(message_);
    }

    Json admissionFailure(const ModelCall& call) {
        if (!call.rejection.empty()) return toolFailure(call.rejection);
        if (tools_.hasWebEvidence && !allowedAfterWebEvidence(call)) return webEvidenceActionDenied();
        const bool standaloneBotCall = calls_.size() == 1 && isBotTool(call.tool);
        if (!standaloneBotCall && !tool_batch::batchable(call.tool)) {
            auto result = toolFailure(tool_batch::classify(call) == tool_batch::CallClass::terminal ?
                "Reply after reviewing these results." : "Run this action on its own after reviewing these results.");
            result["notRun"] = true;
            return result;
        }
        try {
            if (isBotTool(call.tool)) store_.validateBotTool(tools_,call);
            return nullptr;
        } catch (const Conflict& error) {
            return toolFailure(store_.sharedRedactor().text(error.what()));
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception&) {
            return toolFailure("Bot tool target is unavailable");
        }
    }

    std::optional<std::vector<std::size_t>> reserveWave(const std::vector<tool_batch::PlannedCall>& wave) {
        std::vector<std::pair<std::size_t,ProviderState>> eligible;
        for (const auto& [slot,predecessors] : wave) {
            if (std::ranges::any_of(predecessors,[&](auto earlier) { return blocked_[earlier]; })) {
                blocked_[slot] = true;
                continue;
            }
            if (shutdown_.stop_requested()) return std::nullopt;
            eligible.emplace_back(slot,providerState(tools_.settings,calls_[slot]));
        }

        std::lock_guard lock(store_.mutex);
        Transaction transaction(store_.db);
        if (!authorize()) { transaction.commit(); return std::nullopt; }
        const auto origin = store_.policyOrigin(message_);
        const auto budget = store_.messageModelBudget(message_.id);
        std::vector<std::size_t> reserved;
        for (const auto& [slot,provider] : eligible) {
            if (isBotTool(calls_[slot].tool) && !reserveBotCall(slot,provider,origin,budget)) continue;
            saveSlot(slot,{{"inFlight",true}});
            reserved.push_back(slot);
        }
        transaction.commit();
        return reserved;
    }

    bool reserveBotCall(std::size_t slot, ProviderState provider, const PolicyOrigin& origin, std::string_view budget) {
        const auto& call = calls_[slot];
        auto resource = store_.authorizedBotToolScope(tools_.bot,call);
        if (!resource) {
            saveSlot(slot,toolFailure("Bot tool access denied"));
            return false;
        }
        if (provider == ProviderState::unavailable || provider == ProviderState::failed) {
            saveSlot(slot,toolFailure(provider == ProviderState::unavailable ?
                "MCP server is unavailable" : "Bot tool target is unavailable"));
            return false;
        }
        if (provider == ProviderState::routine) resource->routine = true;
        const auto key = callKey(slot);
        const auto payload = Json::array({call.tool,call.args});
        const auto now = store_.policyClock();
        const auto gate = store_.approveAction(origin,*resource,key,payload,now,false,ApprovalWait::defer);
        if (gate == ApprovalGate::waiting) {
            approvalWaiting_ = true;
            blocked_[slot] = true;
            return false;
        }
        if (gate != ApprovalGate::allow) {
            saveSlot(slot,toolFailure(approvalError(gate)));
            return false;
        }
        if (!store_.takeReservedConversationCall(message_.id) &&
            !store_.reserveModelCall(budget,"bot_tool",message_.id,0,0)) {
            blocked_[slot] = true;
            return false;
        }
        store_.approveAction(origin,*resource,key,payload,now);
        return true;
    }

    std::string callKey(std::size_t slot) const {
        return tool_batch::turnCallKey(message_.id,step_,slot);
    }

    void executeWave(const std::vector<std::size_t>& reserved) {
        std::vector<std::future<void>> workers;
        workers.reserve(reserved.size());
        for (const auto slot : reserved) {
            try {
                workers.push_back(std::async(std::launch::async,[this,slot] {
                    recordResult(slot,executeCall(slot));
                }));
            } catch (const std::system_error&) {
                recordResult(slot,toolFailure("Could not start the tool call"));
            }
        }
        for (auto& worker : workers) worker.get();
    }

    Json executeCall(std::size_t slot) {
        const auto& call = calls_[slot];
        const bool bot = isBotTool(call.tool);
        try {
            if (!bot) return store_.executeConversationRead(message_,call,t3_,callKey(slot));
            auto result = store_.executeBotTool(tools_,call);
            if (!result.contains("ok")) result["ok"] = true;
            return result;
        } catch (const ToolOutcomeUnknown& error) {
            return {{"ok",false},{"error",error.what()},{"delivery","unknown"}};
        } catch (const Conflict& error) {
            return toolFailure(error.what());
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception&) {
            return toolFailure(bot ? "Bot tool execution failed" : "Conversation read failed");
        }
    }

    void saveSlot(std::size_t slot, const Json& result) {
        const auto encoded = result.dump(-1,' ',false,Json::error_handler_t::replace);
        store_.exec("UPDATE conversation_steps SET effect=json_set(effect,?,json(?))"
                    " WHERE message_id=? AND step=? AND result IS NULL",
                    {"$[" + std::to_string(slot) + "]",encoded,message_.id,step_});
    }

    void recordResult(std::size_t slot, Json result) {
        std::lock_guard lock(store_.mutex);
        store_.unpersistedToolResults[{message_.id,step_}].insert_or_assign(slot,std::move(result));
        flushResults();
    }

    void flushResults() {
        auto& unpersisted = store_.unpersistedToolResults;
        const auto batch = unpersisted.find({message_.id,step_});
        if (batch == unpersisted.end()) return;
        auto& slots = batch->second;
        for (auto it = slots.begin(); it != slots.end();) {
            const auto& tool = calls_.at(it->first).tool;
            const auto scrub = !isBotTool(tool) || tool == "inspect_attachment" ?
                store_.redactor(message_.bot) : store_.sharedRedactor();
            saveSlot(it->first,scrub.record(it->second));
            it = slots.erase(it);
        }
        unpersisted.erase(batch);
    }

    bool finish() {
        std::lock_guard lock(store_.mutex);
        Transaction transaction(store_.db);
        if (!store_.authorizeTurn(message_)) { transaction.commit(); return false; }
        auto row = store_.query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=?",{message_.id,step_});
        if (!row.next()) throw Conflict("Tool batch step is missing");
        const auto results = row.json(0);
        store_.releaseModelCall(store_.takeReservedConversationCall(message_.id));
        const bool complete = std::ranges::none_of(results,[](const auto& result) { return result.is_null(); });
        if (complete) {
            store_.saveTurnResults(message_.id,step_,results);
        } else if (approvalWaiting_) {
            store_.exec("UPDATE conversation_turns SET status='policy_waiting' WHERE message_id=?"
                        " AND EXISTS (SELECT 1 FROM policy_approvals WHERE turn=? AND status='pending')",
                        {message_.id,message_.id});
        } else {
            store_.pauseTurn(message_);
        }
        transaction.commit();
        return complete;
    }
};

bool Store::Impl::runToolBatch(const conversation::Message& message, const ToolContext& tools, int step,
                               const std::vector<ModelCall>& calls, T3& t3, std::stop_token shutdown) {
    return ToolBatch(*this,message,tools,step,calls,t3,shutdown).run();
}
}
