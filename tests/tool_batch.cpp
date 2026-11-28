#include "../src/tool_batch.hpp"
#include "support.hpp"
#include "../src/model_calls.hpp"
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
using flockv0::test::check;

Decision call(std::string_view tool, Json args = Json::object()) {
    return {std::string(tool), std::move(args), {}};
}

void classification() {
    using enum CallClass;
    const auto expect = [](CallClass expected, std::initializer_list<std::string_view> tools) {
        for (const auto tool : tools) check(classify(call(tool)) == expected, "Unexpected class for " + std::string(tool));
    };
    expect(parallel_read,
           {"read_result", "find_tasks", "flock_session_search", "read_tool_result", "expand_context", "list_secrets", "list_connections",
            "check_environment_readiness", "list_schedules", "workspace_read_file", "workspace_search", "web_search", "web_fetch",
            "mcp_discover", "mcp_read_resource", "skill_list", "skill_read", "inspect_attachment", "git_find_repos"});
    expect(gated_read, {"flock_session_events", "read_channel_attachment", "read_memory", "memory_history", "git_repo_info",
                        "git_pr_status", "git_commit_status", "git_read_ci_logs"});
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
                       "mcp_call_tool",
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
    for (const auto& tool : botToolNames())
        check(batchRead(call(tool)) == (tool != "workspace_write_file" && tool != "mcp_call_tool"),
              "Incorrect Bot batch admission for " + tool);
    for (const auto tool : {"flock_session_search", "check_environment_readiness", "flock_session_events", "find_tasks", "read_result",
                            "list_schedules", "list_secrets", "list_connections", "read_tool_result", "read_memory", "memory_history"}) {
        const auto classification = classify(call(tool));
        check(batchRead(call(tool)) && (classification == CallClass::parallel_read || classification == CallClass::gated_read),
              "Incorrect conversation read batch admission for " + std::string(tool));
    }
    for (const auto& tool : gitTools()) check(!batchRead(call(tool)), "Git tool admitted to batching");
    for (const auto tool : {"expand_context", "message_session", "reply", "unknown_tool"})
        check(!batchRead(call(tool)), "Unexpected batch admission for " + std::string(tool));
}

using Steps = std::vector<StepCallIndexes>;

Steps plannedSteps(std::initializer_list<Decision> calls) {
    std::vector<ModelCall> checked;
    for (const auto& call : calls) checked.push_back({call, {}});
    return planSteps(checked);
}

void steps() {
    check(planSteps({}).empty(), "Empty batch produced steps");
    check(plannedSteps({call("flock_session_search"), call("check_environment_readiness")}) == Steps{{0, 1}},
          "Independent conversation reads must share a step");
    check(plannedSteps({call("workspace_write_file", {{"path", "src/./Readme"}}), call("workspace_read_file", {{"path", "src/README"}}),
                        call("workspace_read_file", {{"path", "src/README"}}), call("workspace_write_file", {{"path", "src/readme"}}),
                        call("workspace_read_file", {{"path", "src/other"}}), call("workspace_search", {{"query", "marker"}}),
                        call("check_environment_readiness"), call("workspace_read_file", {{"path", "src/readme"}}),
                        call("workspace_write_file", {{"path", "other"}})}) == Steps{{4, 6}, {0}, {1, 2}, {3}, {5, 7}, {8}},
          "Workspace reads must follow earlier conflicts while independent reads share the first step");
    check(plannedSteps({call("mcp_discover", {{"server", "a"}}), call("mcp_call_tool", {{"server", "a"}}),
                        call("mcp_call_tool", {{"server", "b"}}), call("mcp_read_resource", {{"server", "a"}}),
                        call("mcp_discover", {{"server", "b"}}), call("mcp_read_resource", {{"server", "c"}})}) ==
              Steps{{0, 5}, {1}, {2}, {3, 4}},
          "MCP calls must keep their order, and later reads on their servers must wait");
    check(plannedSteps({call("workspace_write_file"), call("workspace_read_file", {{"path", "ordered.txt"}}), call("reply")}) ==
              Steps{{0}, {1, 2}},
          "Invalid writes must reach validation before dependent reads, and replies must wait for model review");
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
        steps();
        approvalKeys();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
