#include "storage.hpp"

#include <future>

namespace flockv0 {
namespace {
bool workspaceMutation(const Decision& call) {
    return call.tool == "workspace_write_file" || call.tool == "workspace_run_command";
}

bool conflict(const Decision& a, const Decision& b) {
    if (a.tool.starts_with("workspace_") && b.tool.starts_with("workspace_")) {
        if (a.tool == "workspace_run_command" || b.tool == "workspace_run_command") return true;
        if (a.args.contains("commandId") && b.args.contains("commandId"))
            return a.args["commandId"] == b.args["commandId"] &&
                (a.tool == "workspace_stop_command" || b.tool == "workspace_stop_command");
        if (workspaceMutation(a) || workspaceMutation(b))
            return a.tool == "workspace_search" || b.tool == "workspace_search" ||
                (a.args.contains("path") && b.args.contains("path") &&
                 relativeToolPath(field(a.args,"path")) == relativeToolPath(field(b.args,"path")));
    }
    return (a.tool == "mcp_call_tool" || b.tool == "mcp_call_tool") &&
           a.args.contains("server") && b.args.contains("server") && a.args["server"] == b.args["server"];
}
}

void Store::Impl::saveTurnResults(Id message, int step, const Json& results) {
    exec("UPDATE conversation_steps SET result=? WHERE message_id=? AND step=? AND result IS NULL",
         {results,message,step});
}

void Store::Impl::runBotTools(const ToolContext& ctx, int step, const std::vector<Decision>& calls) {
    auto results = Json::array();
    std::vector<std::size_t> reserved;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        auto previous = query("SELECT effect FROM conversation_steps WHERE message_id=? AND step=? AND result IS NULL",
                              {ctx.messageId,step});
        if (!previous.next()) return;
        const auto saved = previous.string(0);
        if (!saved.empty()) {
            results = Json::parse(saved).at("pendingResults");
            for (auto& result : results)
                if (result.is_null()) result = {{"ok",false},{"delivery","unknown"},
                    {"error","Flock restarted during this tool call. Its outcome is unknown."}};
            saveTurnResults(ctx.messageId,step,results);
            transaction.commit();
            return;
        }
        const auto budget = messageModelBudget(ctx.messageId);
        for (std::size_t i = 0; i < calls.size(); ++i) {
            try {
                validateBotTool(ctx,calls[i]);
                for (const auto index : reserved)
                    if (conflict(calls[i],calls[index])) throw Conflict("Tool calls must be independent");
            } catch (const Conflict& error) {
                results.push_back(toolFailure(error.what()));
                continue;
            } catch (const std::exception&) {
                results.push_back(toolFailure("Bot tool target is unavailable"));
                continue;
            }
            if (!reserveModelCall(budget,"bot_tool",ctx.messageId,0,0)) {
                auto rejected = toolFailure("The turn's action budget is exhausted");
                rejected["budgetExhausted"] = true;
                results.push_back(std::move(rejected));
                continue;
            }
            reserved.push_back(i);
            results.push_back(nullptr);
        }
        exec("UPDATE conversation_steps SET effect=? WHERE message_id=? AND step=?",
             {Json{{"pendingResults",redactor(ctx.bot).record(results)}},ctx.messageId,step});
        transaction.commit();
    }

    std::vector<std::pair<std::size_t,std::future<Json>>> workers;
    for (const auto i : reserved) {
        try {
            workers.emplace_back(i,std::async(std::launch::async,[this,&ctx,&call = calls[i]]() -> Json {
                try {
                    auto result = executeBotTool(ctx,call);
                    if (!result.contains("ok")) result["ok"] = true;
                    return result;
                } catch (const ToolOutcomeUnknown& error) {
                    return {{"ok",false},{"error",error.what()},{"delivery","unknown"}};
                } catch (const Conflict& error) { return toolFailure(error.what()); }
                catch (const std::exception&) { return toolFailure("Bot tool execution failed"); }
            }));
        } catch (const std::system_error&) { results[i] = toolFailure("Could not start the Bot tool"); }
    }
    for (auto& [i,worker] : workers) results[i] = worker.get();
    std::lock_guard lock(mutex);
    auto safe = redactor(ctx.bot).record(std::move(results));
    for (auto& result : safe) {
        if (!result.contains("stdout")) continue;
        auto& output = result.at("stdout").get_ref<std::string&>();
        auto& error = result.at("stderr").get_ref<std::string&>();
        if (output.size() + error.size() <= workspaceOutputLimit) continue;
        output = bounded(output,workspaceOutputLimit);
        error = bounded(error,workspaceOutputLimit-output.size());
        result["truncated"] = true;
    }
    saveTurnResults(ctx.messageId,step,safe);
}
}
