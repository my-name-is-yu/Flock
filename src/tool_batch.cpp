#include "tool_batch.hpp"
#include "storage.hpp"

#include <algorithm>

namespace flockv0::tool_batch {
namespace {
bool filenamesMayMatch(const Decision& a, const Decision& b) {
    const auto left = relativeToolPath(field(a.args,"path")).generic_string();
    const auto right = relativeToolPath(field(b.args,"path")).generic_string();
    const auto nonAscii = [](unsigned char c) { return c >= 128; };
    if (std::ranges::any_of(left,nonAscii) || std::ranges::any_of(right,nonAscii)) return true;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; };
    return std::ranges::equal(left,right,[&](char m, char n) { return fold(m) == fold(n); });
}
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

CallClass classify(const Decision& call) {
    const auto& tool = call.tool;
    if (tool == "reply") return CallClass::terminal;
    if (gitOperation(call) == "read")
        return tool == "git_find_repos" || tool == "git_read_result" ? CallClass::parallel_read : CallClass::gated_read;

    // MCP invocations have a per-call policy gate. Discovery and resource reads are ordinary reads.
    constexpr std::string_view gated[]{
        "flock_session_events","read_channel_attachment","read_memory","memory_history","mcp_call_tool"
    };
    if (std::ranges::find(gated,tool) != std::end(gated)) return CallClass::gated_read;

    constexpr std::string_view reads[]{
        "read_result","find_tasks","flock_session_search","read_tool_result","workflow_status","expand_context",
        "list_secrets","list_connections","check_environment_readiness","list_schedules","workspace_read_file",
        "workspace_search","workspace_command_status","web_search","web_fetch","mcp_discover","mcp_read_resource",
        "skill_list","skill_read","inspect_attachment"
    };
    if (std::ranges::find(reads,tool) != std::end(reads)) return CallClass::parallel_read;

    // Routing, mutations, workspace writes and command execution/control require a step of their own.
    // Unrecognized tools default to the same restriction.
    return CallClass::exclusive;
}

bool batchable(const Decision& call) {
    return isBotTool(call.tool) || call.tool == "flock_session_search" || call.tool == "check_environment_readiness";
}

std::vector<std::vector<std::size_t>> planWaves(const std::vector<Decision>& calls) {
    std::vector<std::vector<std::size_t>> waves;
    std::vector<std::size_t> waveForCall(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        auto& wave = waveForCall[i];
        for (std::size_t earlier = 0; earlier < i; ++earlier)
            if (conflict(calls[i],calls[earlier])) wave = std::max(wave,waveForCall[earlier] + 1);
        if (wave == waves.size()) waves.emplace_back();
        waves[wave].push_back(i);
    }
    return waves;
}

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index) {
    auto key = "turn/" + std::to_string(message) + "/" + std::to_string(step);
    if (index) key += "/" + std::to_string(*index);
    return key;
}
}
