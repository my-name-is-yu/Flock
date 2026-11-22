#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <future>
#include <utility>

namespace flockv0 {
namespace {
bool filenamesMayMatch(const Decision& a, const Decision& b) {
    const auto left = relativeToolPath(field(a.args,"path")).generic_string();
    const auto right = relativeToolPath(field(b.args,"path")).generic_string();
    const auto nonAscii = [](unsigned char c) { return c >= 128; };
    if (std::ranges::any_of(left,nonAscii) || std::ranges::any_of(right,nonAscii)) return true;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; };
    return std::ranges::equal(left,right,[&](char m, char n) { return fold(m) == fold(n); });
}

bool conflict(const Decision& a, const Decision& b) {
    if (a.tool.starts_with("workspace_") && b.tool.starts_with("workspace_")) {
        if (a.tool == "workspace_run_command" || b.tool == "workspace_run_command") return true;
        if (a.args.contains("commandId") && b.args.contains("commandId"))
            return a.args["commandId"] == b.args["commandId"] &&
                (a.tool == "workspace_stop_command" || b.tool == "workspace_stop_command");
        if (a.tool == "workspace_write_file" || b.tool == "workspace_write_file")
            return a.tool == "workspace_search" || b.tool == "workspace_search" ||
                (a.args.contains("path") && b.args.contains("path") &&
                 filenamesMayMatch(a,b));
    }
    return (a.tool == "mcp_call_tool" || b.tool == "mcp_call_tool") &&
           a.args.contains("server") && b.args.contains("server") && a.args["server"] == b.args["server"];
}
}

bool allowedAfterWebEvidence(const Decision& call) {
    if (call.tool == "git_manage_issue") {
        const auto action = field(call.args,"action");
        return action == "read" || action == "list" || action == "list_comments";
    }
    constexpr std::string_view readOnly[]{
        "reply","web_search","web_fetch","read_result","find_tasks","flock_session_search","flock_session_events",
        "list_grants","list_secrets","list_connections","check_environment_readiness","workspace_read_file",
        "workspace_search","workspace_command_status","mcp_discover","mcp_read_resource",
        "skill_list","skill_read","inspect_attachment","workflow_status","expand_context",
        "read_memory","memory_history","list_schedules","git_find_repos","git_repo_info",
        "git_pr_status","git_commit_status","git_read_ci_logs"
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

bool Store::Impl::runBotTools(const ToolContext& ctx, int step, const std::vector<ModelCall>& calls) {
    std::vector<std::size_t> reserved;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (!authorizeTurn(loadTurnMessage(ctx.messageId))) { transaction.commit(); return false; }
        auto previous = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                              {ctx.messageId,step});
        if (!previous.next()) return true;
        const auto saved = previous.string(0);
        Json results = saved.empty() ? Json(std::vector<Json>(calls.size())) : Json::parse(saved);
        const auto budget = messageModelBudget(ctx.messageId);
        auto approved = takeReservedConversationCall(ctx.messageId);
        const auto scrub = sharedRedactor();
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
                    if (conflict(calls[i],calls[index])) throw Conflict("Tool calls must be independent");
            } catch (const Conflict& error) {
                result = toolFailure(scrub.text(error.what()));
                continue;
            } catch (const std::exception&) {
                result = toolFailure("Bot tool target is unavailable");
                continue;
            }
            if (!std::exchange(approved,0) && !reserveModelCall(budget,"bot_tool",ctx.messageId,0,0)) continue;
            result = {{"inFlight",true}};
            reserved.push_back(i);
        }
        if (approved) releaseModelCall(budget,approved);
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",
             {results,ctx.messageId,step});
        transaction.commit();
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
    const bool pending = std::ranges::any_of(results,[](const auto& result) { return result.is_null(); });
    if (pending) {
        const auto message = loadTurnMessage(ctx.messageId);
        if (authorizeTurn(message)) pauseTurn(message);
    } else saveTurnResults(ctx.messageId,step,results);
    transaction.commit();
    return !pending;
}
}
