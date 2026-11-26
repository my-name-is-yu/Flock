#include "../src/tool_batch.hpp"
#include "flock/git_host.hpp"
#include "flock/workspace_tools.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace flockv0;
using namespace flockv0::tool_batch;

namespace {
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Decision call(std::string_view tool, Json args = Json::object()) {
    return {std::string(tool), std::move(args), {}};
}

void classification() {
    using enum CallClass;
    const auto expect = [](CallClass expected, std::initializer_list<std::string_view> tools) {
        for (const auto tool : tools) check(classify(call(tool)) == expected, "Unexpected class for " + std::string(tool));
    };
    expect(parallel_read, {"read_result",        "find_tasks",          "flock_session_search",
                           "read_tool_result",   "workflow_status",     "expand_context",
                           "list_secrets",       "list_connections",    "check_environment_readiness",
                           "list_schedules",     "workspace_read_file", "workspace_search",
                           "web_search",         "web_fetch",           "mcp_discover",
                           "mcp_read_resource",  "skill_list",          "skill_read",
                           "inspect_attachment", "git_find_repos"});
    expect(gated_read, {"flock_session_events", "read_channel_attachment", "read_memory", "memory_history", "mcp_call_tool",
                        "git_repo_info", "git_pr_status", "git_commit_status", "git_read_ci_logs"});
    expect(exclusive, {"start_task",
                       "send_to_task",
                       "change_task",
                       "answer_task_question",
                       "record_task_blocker",
                       "withdraw_task_question",
                       "cancel_task",
                       "message_session",
                       "invite_bot",
                       "leave_session",
                       "interrupt_session",
                       "start_workflow",
                       "instruct_workflow_stage",
                       "stop_workflow_stage",
                       "stop_workflow",
                       "compact_conversation",
                       "continue_fresh_session",
                       "request_secret_input",
                       "request_bot_change",
                       "write_memory",
                       "revert_memory_edit",
                       "create_schedule",
                       "remove_schedule",
                       "create_reminder",
                       "create_trigger",
                       "channel_edit_message",
                       "channel_react",
                       "channel_send_attachment",
                       "publish_artifact",
                       "request_local_machine_connection",
                       "workspace_write_file",
                       "git_open_pr",
                       "git_update_pr",
                       "git_request_review",
                       "git_submit_review",
                       "git_comment",
                       "unknown_tool"});
    expect(terminal, {"reply"});

    for (const auto action : {"list", "read", "list_comments"})
        check(classify(call("git_manage_issue", {{"action", action}})) == gated_read, "Git issue read must be gated");
    for (const auto action : {"create", "update", "invalid"})
        check(classify(call("git_manage_issue", {{"action", action}})) == exclusive, "Git issue write must be exclusive");
    check(classify(call("git_manage_issue")) == exclusive, "Missing Git action must default to exclusive");
}

void batchAdmission() {
    for (const auto& tool : botToolNames()) check(batchable(tool), "Incorrect Bot batch admission for " + tool);
    for (const auto tool :
         {"flock_session_search", "check_environment_readiness", "flock_session_events", "find_tasks", "read_result", "workflow_status",
          "list_schedules", "list_secrets", "list_connections", "read_tool_result", "read_memory", "memory_history"}) {
        const auto classification = classify(call(tool));
        check(batchable(tool) && (classification == CallClass::parallel_read || classification == CallClass::gated_read),
              "Incorrect conversation read batch admission for " + std::string(tool));
    }
    for (const auto& tool : gitTools()) check(!batchable(tool), "Git tool admitted to batching");
    for (const auto tool : {"expand_context", "message_session", "reply", "unknown_tool"})
        check(!batchable(tool), "Unexpected batch admission for " + std::string(tool));
}

using Waves = std::vector<std::vector<std::size_t>>;

Waves waveSlots(const std::vector<Decision>& calls) {
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < calls.size(); ++i) candidates.push_back({i, calls[i]});
    Waves slots;
    for (const auto& wave : planWaves(candidates)) {
        auto& indices = slots.emplace_back();
        for (const auto& call : wave) indices.push_back(call.slot);
    }
    return slots;
}

void waves() {
    check(planWaves({}).empty(), "Empty batch produced a wave");
    check(waveSlots({call("flock_session_search"), call("check_environment_readiness")}) == Waves{{0, 1}},
          "Independent conversation reads must share one wave");
    check(waveSlots({call("workspace_write_file", {{"path", "src/./Readme"}}), call("workspace_read_file", {{"path", "src/README"}}),
                     call("workspace_read_file", {{"path", "src/README"}}), call("workspace_write_file", {{"path", "src/readme"}}),
                     call("workspace_read_file", {{"path", "src/other"}}), call("workspace_search", {{"query", "marker"}}),
                     call("check_environment_readiness"), call("workspace_read_file", {{"path", "src/readme"}}),
                     call("workspace_write_file", {{"path", "other"}})}) == Waves{{0, 4, 6}, {1, 2}, {3}, {5, 7}, {8}},
          "Each workspace call must follow every earlier conflict and use the earliest eligible wave");
    check(waveSlots({call("mcp_discover", {{"server", "a"}}), call("mcp_call_tool", {{"server", "a"}}),
                     call("mcp_call_tool", {{"server", "b"}}), call("mcp_read_resource", {{"server", "a"}}),
                     call("mcp_discover", {{"server", "b"}})}) == Waves{{0, 2}, {1, 4}, {3}},
          "MCP conflicts must order calls on the same server");
}

void sparseSlots() {
    const auto write = call("workspace_write_file", {{"path", "ordered.txt"}});
    const auto independent = call("flock_session_search");
    const auto read = call("workspace_read_file", {{"path", "ordered.txt"}});
    const Candidate calls[]{{1, write}, {3, independent}, {5, read}};
    const auto waves = planWaves(calls);
    check(waves.size() == 2 && waves[0].size() == 2 && waves[1].size() == 1, "Saved or rejected siblings changed wave boundaries");
    check(waves[0][0].slot == 1 && waves[0][1].slot == 3 && waves[1][0].slot == 5 &&
              waves[1][0].predecessors == std::vector<std::size_t>{1},
          "The plan lost original call slots or dependencies");
}

void approvalKeys() {
    check(turnCallKey(42, 3) == "turn/42/3", "Single-call approval key changed");
    check(turnCallKey(42, 3, 0) == "turn/42/3/0", "First indexed approval key changed");
    check(turnCallKey(42, 3, 2) == "turn/42/3/2", "Indexed approval keys must distinguish siblings");
}
}

int main() {
    try {
        classification();
        batchAdmission();
        waves();
        sparseSlots();
        approvalKeys();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
