#include "storage.hpp"
#include "model_calls.hpp"
#include "session_read_storage.hpp"
#include "policy_approvals.hpp"
#include "bot_settings_storage.hpp"
#include "conversation_call.hpp"
#include "database.hpp"
#include "conversation_tools.hpp"
#include "tool_call_result.hpp"
#include "conversation_storage.hpp"
#include "tool_batch.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/decision.hpp"

#include <algorithm>
#include <cstddef>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
bool allowedAfterWebEvidence(const Decision& call) {
    if (tool_batch::conversationRead(call.tool) != tool_batch::ConversationRead::none || gitOperation(call) == "read") return true;
    constexpr std::string_view readOnly[]{"reply",         "web_search",        "web_fetch",  "workspace_read_file", "workspace_search",
                                          "mcp_discover",  "mcp_read_resource", "skill_list", "skill_read",          "inspect_attachment",
                                          "expand_context"};
    return std::ranges::find(readOnly, call.tool) != std::end(readOnly);
}

Json webEvidenceActionDenied() {
    return toolFailure("After reading web evidence, this turn can only read and reply. "
                       "Ask the user to request the action in a new message.");
}

void ConversationTools::saveTurnResults(Id message, int step, const Json& results) {
    database.exec("UPDATE conversation_steps SET result=?,effect=NULL WHERE message_id=? AND step=? AND result IS NULL",
                  {results, message, step});
}

void ConversationTools::discardPendingResults(Id message) {
    std::erase_if(unpersistedToolResults, [message](const auto& entry) { return entry.first.first == message; });
}

namespace {
void recoverSlot(Json& result, const Decision& call) {
    if (!result.contains("inFlight")) return;
    using enum tool_batch::CallClass;
    if (tool_batch::conversationRead(call.tool) != tool_batch::ConversationRead::none || tool_batch::classify(call) == parallel_read)
        result = nullptr;
    else
        result = {{"ok", false}, {"delivery", "unknown"}, {"error", "Flock restarted during this tool call. Its outcome is unknown."}};
}

enum class ProviderState { available, routine, unavailable, failed };

ProviderState providerState(const BotTools& settings, const Decision& call) {
    if (!call.tool.starts_with("mcp_")) return ProviderState::available;
    const auto server = field(call.args, "server");
    try {
        if (!settings.mcp->hasServer(server)) return ProviderState::unavailable;
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception&) {
        return ProviderState::failed;
    }
    return call.tool == "mcp_call_tool" && settings.mcp->readOnlyHint(server, field(call.args, "name")) ? ProviderState::routine
                                                                                                        : ProviderState::available;
}
}

class ConversationTools::ToolBatch {
public:
    ToolBatch(ConversationTools& store, const conversation::Message& message, const ToolContext& tools, int step,
              const std::vector<ModelCall>& calls, const ConversationCallContext& ctx)
        : store_(store), message_(message), tools_(tools), step_(step), calls_(calls), ctx_(ctx), blocked_(calls.size()) {}

    bool run() {
        std::vector<tool_batch::Candidate> pending;
        {
            std::lock_guard lock(store_.database.mutex);
            flushResults();
            Transaction transaction(store_.database.db);
            if (!authorize()) {
                transaction.commit();
                return false;
            }
            auto row = store_.database.query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                                             {message_.id, step_});
            if (!row.next()) return true;
            const auto saved = row.string(0);
            auto results = saved.empty() ? Json(std::vector<Json>(calls_.size())) : Json::parse(saved);
            for (std::size_t slot = 0; slot < calls_.size(); ++slot) {
                const auto& call = calls_[slot];
                auto& result = results.at(slot);
                recoverSlot(result, call);
                if (!result.is_null()) continue;
                result = admissionFailure(call);
                if (result.is_null()) pending.push_back({slot, call});
            }
            store_.database.exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?", {results, message_.id, step_});
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
    ConversationTools& store_;
    const conversation::Message& message_;
    const ToolContext& tools_;
    int step_;
    const std::vector<ModelCall>& calls_;
    const ConversationCallContext& ctx_;
    std::vector<bool> blocked_;
    bool approvalWaiting_ = false;

    bool authorize() { return !ctx_.shutdown.stop_requested() && store_.conversationTurns.authorizeTurn(message_); }

    bool storeRead(std::size_t slot) const {
        return tool_batch::conversationRead(calls_[slot].tool) == tool_batch::ConversationRead::store;
    }

    Json admissionFailure(const ModelCall& call) {
        if (!call.rejection.empty()) return toolFailure(call.rejection);
        if (tools_.hasWebEvidence && !allowedAfterWebEvidence(call)) return webEvidenceActionDenied();
        const bool standaloneBotCall = calls_.size() == 1 && isBotTool(call.tool);
        if (!standaloneBotCall && !tool_batch::batchable(call.tool)) {
            auto result = toolFailure(tool_batch::classify(call) == tool_batch::CallClass::terminal
                                          ? "Reply after reviewing these results."
                                          : "Run this action on its own after reviewing these results.");
            result["notRun"] = true;
            return result;
        }
        try {
            if (isBotTool(call.tool)) store_.validateBotTool(tools_, call);
            return nullptr;
        } catch (const Conflict& error) {
            return toolFailure(store_.secretStorage.sharedRedactor().text(error.what()));
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception&) {
            return toolFailure("Bot tool target is unavailable");
        }
    }

    std::optional<std::vector<std::size_t>> reserveWave(const std::vector<tool_batch::PlannedCall>& wave) {
        std::vector<std::pair<std::size_t, ProviderState>> eligible;
        for (const auto& [slot, predecessors] : wave) {
            if (std::ranges::any_of(predecessors, [&](auto earlier) { return blocked_[earlier]; })) {
                blocked_[slot] = true;
                continue;
            }
            if (ctx_.shutdown.stop_requested()) return std::nullopt;
            eligible.emplace_back(slot, providerState(tools_.settings, calls_[slot]));
        }

        std::lock_guard lock(store_.database.mutex);
        Transaction transaction(store_.database.db);
        if (!authorize()) {
            transaction.commit();
            return std::nullopt;
        }
        const auto origin = store_.policyApprovals.policyOrigin(message_);
        const auto budget = store_.modelCalls.messageModelBudget(message_.id);
        std::vector<std::size_t> reserved;
        for (const auto& [slot, provider] : eligible) {
            if (isBotTool(calls_[slot].tool) && !reserveBotCall(slot, provider, origin, budget)) continue;
            if (!storeRead(slot)) saveSlot(slot, {{"inFlight", true}});
            reserved.push_back(slot);
        }
        transaction.commit();
        return reserved;
    }

    bool reserveBotCall(std::size_t slot, ProviderState provider, const PolicyOrigin& origin, const ModelBudget& budget) {
        const auto& call = calls_[slot];
        auto resource = store_.authorizedBotToolScope(tools_.bot, call);
        if (!resource) {
            saveSlot(slot, toolFailure("Bot tool access denied"));
            return false;
        }
        if (provider == ProviderState::unavailable || provider == ProviderState::failed) {
            saveSlot(slot,
                     toolFailure(provider == ProviderState::unavailable ? "MCP server is unavailable" : "Bot tool target is unavailable"));
            return false;
        }
        if (provider == ProviderState::routine) resource->routine = true;
        const auto key = callKey(slot);
        const auto payload = Json::array({call.tool, call.args});
        const auto now = store_.policyApprovals.now();
        const auto gate = store_.policyApprovals.approveAction(origin, *resource, key, payload, now, false, ApprovalWait::defer);
        if (gate == ApprovalGate::waiting) {
            approvalWaiting_ = true;
            blocked_[slot] = true;
            return false;
        }
        if (gate != ApprovalGate::allow) {
            saveSlot(slot, toolFailure(approvalError(gate)));
            return false;
        }
        if (!store_.modelCalls.takeReservedConversationCall(message_.id) &&
            !store_.modelCalls.reserveModelCall(budget, "bot_tool", message_.id, 0, 0)) {
            blocked_[slot] = true;
            return false;
        }
        store_.policyApprovals.approveAction(origin, *resource, key, payload, now);
        return true;
    }

    std::string callKey(std::size_t slot) const {
        // Single conversation reads can resume approvals created before they used ToolBatch.
        if (calls_.size() == 1 && tool_batch::conversationRead(calls_[slot].tool) != tool_batch::ConversationRead::none)
            return tool_batch::turnCallKey(message_.id, step_);
        return tool_batch::turnCallKey(message_.id, step_, slot);
    }

    void executeWave(const std::vector<std::size_t>& reserved) {
        std::vector<std::future<void>> workers;
        workers.reserve(reserved.size());
        for (const auto slot : reserved) {
            if (storeRead(slot)) continue;
            try {
                workers.push_back(std::async(std::launch::async, [this, slot] { recordResult(slot, executeCall(slot)); }));
            } catch (const std::system_error&) {
                recordResult(slot, CompletedToolCall{toolFailure("Could not start the tool call")});
            }
        }
        for (const auto slot : reserved)
            if (storeRead(slot)) executeStoreRead(slot);
        for (auto& worker : workers) worker.get();
    }

    void executeStoreRead(std::size_t slot) {
        std::lock_guard lock(store_.database.mutex);
        Transaction transaction(store_.database.db);
        if (authorize()) {
            auto result = store_.conversationTurns.applyLocalConversationCall(message_, step_, calls_[slot], callKey(slot), ctx_.clock(),
                                                                              ApprovalWait::defer, nullptr, LinkedTaskDestinations{});
            if (result)
                saveSlot(slot, store_.secretStorage.redactor(message_.bot).record(std::move(*result)));
            else
                approvalWaiting_ = true;
        }
        transaction.commit();
    }

    ToolCallResult executeCall(std::size_t slot) {
        const auto& call = calls_[slot];
        const bool bot = isBotTool(call.tool);
        try {
            {
                std::lock_guard lock(store_.database.mutex);
                Transaction transaction(store_.database.db);
                const bool allowed = authorize();
                transaction.commit();
                if (!allowed) return CompletedToolCall{toolFailure("This message has closed.")};
            }
            if (bot) {
                auto result = store_.executeBotTool(tools_, call);
                if (!result.contains("ok")) result["ok"] = true;
                return CompletedToolCall{std::move(result)};
            }
            using enum tool_batch::ConversationRead;
            switch (tool_batch::conversationRead(call.tool)) {
            case session: return store_.sessionReadStorage.sessionReadTool(message_, call, ctx_.t3, callKey(slot));
            case environment_readiness: {
                if (!message_.channelScope.empty()) return CompletedToolCall{toolFailure("Use Flock Web for this action")};
                auto result = store_.botSettingsStorage.readiness(message_.bot, message_.routing, call.args, ctx_.t3);
                result["ok"] = true;
                return CompletedToolCall{std::move(result)};
            }
            default: throw std::logic_error("Unsupported network conversation read");
            }
        } catch (const ToolOutcomeUnknown& error) {
            return CompletedToolCall{{{"ok", false}, {"error", error.what()}, {"delivery", "unknown"}}};
        } catch (const Conflict& error) {
            return CompletedToolCall{toolFailure(error.what())};
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception&) {
            return CompletedToolCall{toolFailure(bot ? "Bot tool execution failed" : "Conversation read failed")};
        }
    }

    void saveSlot(std::size_t slot, const Json& result) {
        const auto encoded = result.dump(-1, ' ', false, Json::error_handler_t::replace);
        store_.database.exec("UPDATE conversation_steps SET effect=json_set(effect,?,json(?))"
                             " WHERE message_id=? AND step=? AND result IS NULL",
                             {"$[" + std::to_string(slot) + "]", encoded, message_.id, step_});
    }

    void recordResult(std::size_t slot, ToolCallResult execution) {
        std::lock_guard lock(store_.database.mutex);
        if (!execution) approvalWaiting_ = true;
        store_.unpersistedToolResults[{message_.id, step_}].insert_or_assign(slot, std::move(execution));
        flushResults();
    }

    void flushResults() {
        auto& unpersisted = store_.unpersistedToolResults;
        const auto batch = unpersisted.find({message_.id, step_});
        if (batch == unpersisted.end()) return;
        auto& slots = batch->second;
        for (auto it = slots.begin(); it != slots.end();) {
            const auto& tool = calls_.at(it->first).tool;
            const auto scrub = !isBotTool(tool) || tool == "inspect_attachment" ? store_.secretStorage.redactor(message_.bot)
                                                                                : store_.secretStorage.sharedRedactor();
            const auto& result = it->second;
            Transaction transaction(store_.database.db);
            saveSlot(it->first, result ? scrub.record(result->value) : Json(nullptr));
            if (result && result->consumeApproval) store_.policyApprovals.consumePolicyApproval(callKey(it->first));
            transaction.commit();
            it = slots.erase(it);
        }
        unpersisted.erase(batch);
    }

    bool finish() {
        std::lock_guard lock(store_.database.mutex);
        Transaction transaction(store_.database.db);
        if (!authorize()) {
            transaction.commit();
            return false;
        }
        auto row = store_.database.query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=?", {message_.id, step_});
        if (!row.next()) throw Conflict("Tool batch step is missing");
        const auto results = row.json(0);
        store_.modelCalls.releaseModelCall(store_.modelCalls.takeReservedConversationCall(message_.id));
        const bool complete = std::ranges::none_of(results, [](const auto& result) { return result.is_null(); });
        if (complete) {
            store_.saveTurnResults(message_.id, step_, results);
        } else if (approvalWaiting_) {
            store_.database.exec("UPDATE conversation_turns SET status='policy_waiting' WHERE message_id=?"
                                 " AND EXISTS (SELECT 1 FROM policy_approvals WHERE turn=? AND status='pending')",
                                 {message_.id, message_.id});
        } else {
            store_.modelCalls.pauseTurn(message_);
        }
        transaction.commit();
        return complete;
    }
};

bool ConversationTools::runToolBatch(const conversation::Message& message, const ToolContext& tools, int step,
                                     const std::vector<ModelCall>& calls, const ConversationCallContext& ctx) {
    return ToolBatch(*this, message, tools, step, calls, ctx).run();
}
}
