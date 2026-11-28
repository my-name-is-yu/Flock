#include "tool_batch.hpp"
#include "flock/types.hpp"
#include "flock/decision.hpp"
#include "flock/git_host.hpp"
#include "model_calls.hpp"
#include "workspace_internal.hpp"
#include "flock/json.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0::tool_batch {
namespace {
struct ReadSpec {
    std::string_view tool;
    ConversationRead path;
    CallClass classification = CallClass::parallel_read;
};
constexpr ReadSpec conversationBatchReads[]{{"flock_session_search", ConversationRead::session},
                                            {"check_environment_readiness", ConversationRead::environment_readiness},
                                            {"flock_session_events", ConversationRead::session, CallClass::gated_read},
                                            {"find_tasks", ConversationRead::store},
                                            {"read_result", ConversationRead::store},
                                            {"list_schedules", ConversationRead::store},
                                            {"list_secrets", ConversationRead::store},
                                            {"list_connections", ConversationRead::store},
                                            {"read_tool_result", ConversationRead::store},
                                            {"read_memory", ConversationRead::store, CallClass::gated_read},
                                            {"memory_history", ConversationRead::store, CallClass::gated_read}};

const ReadSpec* conversationReadSpec(std::string_view tool) {
    const auto* read = std::ranges::find(conversationBatchReads, tool, &ReadSpec::tool);
    return read == std::end(conversationBatchReads) ? nullptr : read;
}

bool filenamesMayMatch(const Decision& a, const Decision& b) {
    const auto left = relativeToolPath(field(a.args, "path")).generic_string();
    const auto right = relativeToolPath(field(b.args, "path")).generic_string();
    const auto nonAscii = [](unsigned char c) { return c >= 128; };
    if (std::ranges::any_of(left, nonAscii) || std::ranges::any_of(right, nonAscii)) return true;
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return std::ranges::equal(left, right, [&](char m, char n) { return fold(m) == fold(n); });
}
bool callsConflict(const Decision& a, const Decision& b) {
    if (a.tool.starts_with("workspace_") && b.tool.starts_with("workspace_")) {
        if (a.tool == "workspace_write_file" || b.tool == "workspace_write_file") {
            if (a.tool == "workspace_search" || b.tool == "workspace_search") return true;
            try {
                return filenamesMayMatch(a, b);
            } catch (const Conflict&) {
                // Leave invalid arguments to the tool's normal validation.
                return true;
            }
        }
    }
    return (a.tool == "mcp_call_tool" || b.tool == "mcp_call_tool") && a.args.contains("server") && b.args.contains("server") &&
           a.args["server"] == b.args["server"];
}
}

CallClass classify(const Decision& call) {
    const auto& tool = call.tool;
    if (tool == "reply") return CallClass::terminal;
    if (gitOperation(call) == "read") return tool == "git_find_repos" ? CallClass::parallel_read : CallClass::gated_read;
    if (const auto* read = conversationReadSpec(tool)) return read->classification;

    constexpr std::string_view gated[]{"read_channel_attachment"};
    if (std::ranges::find(gated, tool) != std::end(gated)) return CallClass::gated_read;

    constexpr std::string_view reads[]{"expand_context", "workspace_read_file", "workspace_search", "web_search", "web_fetch",
                                       "mcp_discover",   "mcp_read_resource",   "skill_list",       "skill_read", "inspect_attachment"};
    if (std::ranges::find(reads, tool) != std::end(reads)) return CallClass::parallel_read;

    return CallClass::exclusive;
}

ConversationRead conversationRead(std::string_view tool) {
    const auto* read = conversationReadSpec(tool);
    return read ? read->path : ConversationRead::none;
}

bool batchRead(const Decision& call) {
    return conversationReadSpec(call.tool) || (isBotTool(call.tool) && classify(call) == CallClass::parallel_read);
}

std::vector<StepCallIndexes> planSteps(std::span<const ModelCall> calls) {
    StepCallIndexes independentReads;
    StepCallIndexes replies;
    std::vector<StepCallIndexes> steps;
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto& call = calls[i];
        if (call.tool == "reply") {
            replies.push_back(i);
        } else if (!batchRead(call)) {
            steps.push_back({i});
        } else if (!std::ranges::any_of(calls.first(i), [&](const auto& earlier) { return callsConflict(earlier, call); })) {
            independentReads.push_back(i);
        } else {
            if (!batchRead(calls[steps.back().front()])) steps.emplace_back();
            steps.back().push_back(i);
        }
    }
    if (!independentReads.empty()) steps.insert(steps.begin(), std::move(independentReads));

    // A reply bundled with reads is returned as notRun so the model can review their results first.
    const auto lastRead =
        std::ranges::find_if(steps.rbegin(), steps.rend(), [&](const auto& step) { return batchRead(calls[step.front()]); });
    if (lastRead != steps.rend())
        lastRead->insert(lastRead->end(), replies.begin(), replies.end());
    else
        for (const auto reply : replies) steps.push_back({reply});
    return steps;
}

std::string turnCallKey(Id message, int step, std::optional<std::size_t> index) {
    auto key = "turn/" + std::to_string(message) + "/" + std::to_string(step);
    if (index) key += "/" + std::to_string(*index);
    return key;
}
}
