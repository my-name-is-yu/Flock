#include "tool_batch.hpp"
#include "storage.hpp"

#include <algorithm>

namespace flockv0::tool_batch {
namespace {
struct ReadSpec {
    std::string_view tool;
    ConversationRead path;
    CallClass classification = CallClass::parallel_read;
};
constexpr ReadSpec conversationBatchReads[]{
    {"flock_session_search",ConversationRead::session},
    {"check_environment_readiness",ConversationRead::environment_readiness},
    {"flock_session_events",ConversationRead::session,CallClass::gated_read},
    {"find_tasks",ConversationRead::store},
    {"read_result",ConversationRead::store},
    {"workflow_status",ConversationRead::store},
    {"list_schedules",ConversationRead::store},
    {"list_secrets",ConversationRead::store},
    {"list_connections",ConversationRead::store},
    {"read_tool_result",ConversationRead::store},
    {"read_memory",ConversationRead::store,CallClass::gated_read},
    {"memory_history",ConversationRead::store,CallClass::gated_read}
};

const ReadSpec* conversationReadSpec(std::string_view tool) {
    const auto* read = std::ranges::find(conversationBatchReads,tool,&ReadSpec::tool);
    return read == std::end(conversationBatchReads) ? nullptr : read;
}

bool filenamesMayMatch(const Decision& a, const Decision& b) {
    const auto left = relativeToolPath(field(a.args,"path")).generic_string();
    const auto right = relativeToolPath(field(b.args,"path")).generic_string();
    const auto nonAscii = [](unsigned char c) { return c >= 128; };
    if (std::ranges::any_of(left,nonAscii) || std::ranges::any_of(right,nonAscii)) return true;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; };
    return std::ranges::equal(left,right,[&](char m, char n) { return fold(m) == fold(n); });
}
bool validatedCallsConflict(const Decision& a, const Decision& b) {
    if (a.tool.starts_with("workspace_") && b.tool.starts_with("workspace_")) {
        if (a.tool == "workspace_write_file" || b.tool == "workspace_write_file")
            return a.tool == "workspace_search" || b.tool == "workspace_search" ||
                (a.args.contains("path") && b.args.contains("path") &&
                 filenamesMayMatch(a,b));
    }
    return (a.tool == "mcp_call_tool" || b.tool == "mcp_call_tool") &&
           a.args.contains("server") && b.args.contains("server") && a.args["server"] == b.args["server"];
}
}

CallClass classify(const Decision& call) {
    const auto& tool = call.tool;
    if (tool == "reply") return CallClass::terminal;
    if (gitOperation(call) == "read")
        return tool == "git_find_repos" || tool == "git_read_result" ? CallClass::parallel_read : CallClass::gated_read;
    if (const auto* read = conversationReadSpec(tool)) return read->classification;

    constexpr std::string_view gated[]{"read_channel_attachment","mcp_call_tool"};
    if (std::ranges::find(gated,tool) != std::end(gated)) return CallClass::gated_read;

    constexpr std::string_view reads[]{
        "expand_context","workspace_read_file",
        "workspace_search","web_search","web_fetch","mcp_discover","mcp_read_resource",
        "skill_list","skill_read","inspect_attachment"
    };
    if (std::ranges::find(reads,tool) != std::end(reads)) return CallClass::parallel_read;

    return CallClass::exclusive;
}

ConversationRead conversationRead(std::string_view tool) {
    const auto* read = conversationReadSpec(tool);
    return read ? read->path : ConversationRead::none;
}

bool batchable(std::string_view tool) {
    return isBotTool(tool) || conversationReadSpec(tool);
}

std::vector<std::vector<PlannedCall>> planWaves(std::span<const Candidate> validatedCalls) {
    std::vector<std::vector<PlannedCall>> waves;
    std::vector<std::size_t> waveForCall(validatedCalls.size());
    for (std::size_t i = 0; i < validatedCalls.size(); ++i) {
        auto& wave = waveForCall[i];
        const auto& candidate = validatedCalls[i];
        PlannedCall call{candidate.slot,{}};
        for (std::size_t earlier = 0; earlier < i; ++earlier) {
            const auto& predecessor = validatedCalls[earlier];
            if (!validatedCallsConflict(candidate.decision,predecessor.decision)) continue;
            wave = std::max(wave,waveForCall[earlier] + 1);
            call.predecessors.push_back(predecessor.slot);
        }
        if (wave == waves.size()) waves.emplace_back();
        waves[wave].push_back(std::move(call));
    }
    return waves;
}

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index) {
    auto key = "turn/" + std::to_string(message) + "/" + std::to_string(step);
    if (index) key += "/" + std::to_string(*index);
    return key;
}
}
