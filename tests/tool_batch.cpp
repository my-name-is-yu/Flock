#include "../src/tool_batch.hpp"
#include "flock/git_host.hpp"
#include "flock/workspace_tools.hpp"

#include <iostream>
#include <stdexcept>

using namespace flockv0;
using namespace flockv0::tool_batch;

namespace {
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Decision call(std::string_view tool, Json args = Json::object()) {
    return {std::string(tool),std::move(args),{}};
}

void classification() {
    using enum CallClass;
    const auto expect = [](CallClass expected, std::initializer_list<std::string_view> tools) {
        for (const auto tool : tools)
            check(classify(call(tool)) == expected, "Unexpected class for " + std::string(tool));
    };
    expect(parallel_read,{"read_result","find_tasks","flock_session_search","read_tool_result","workflow_status",
        "expand_context","list_secrets","list_connections","check_environment_readiness","list_schedules",
        "workspace_read_file","workspace_search","workspace_command_status","web_search","web_fetch",
        "mcp_discover","mcp_read_resource","skill_list","skill_read","inspect_attachment","git_find_repos"});
    expect(gated_read,{"flock_session_events","read_channel_attachment","read_memory","memory_history",
        "mcp_call_tool","git_repo_info","git_pr_status","git_commit_status","git_read_ci_logs"});
    expect(exclusive,{"start_task","send_to_task","change_task","answer_task_question","record_task_blocker",
        "withdraw_task_question","cancel_task","message_session","invite_bot","leave_session","interrupt_session",
        "start_workflow","instruct_workflow_stage","stop_workflow_stage","stop_workflow","compact_conversation",
        "continue_fresh_session","request_secret_input","update_bot_settings","request_bot_change","propose_memory_edit",
        "apply_memory_edit","revert_memory_edit","create_schedule","remove_schedule","create_reminder","create_trigger",
        "channel_edit_message","channel_react","channel_send_attachment","publish_artifact","request_local_machine_connection",
        "workspace_write_file","workspace_run_command","workspace_stop_command","git_open_pr","git_update_pr",
        "git_request_review","git_submit_review","git_comment","unknown_tool"});
    expect(terminal,{"reply"});

    for (const auto action : {"list","read","list_comments"})
        check(classify(call("git_manage_issue",{{"action",action}})) == gated_read,"Git issue read must be gated");
    for (const auto action : {"create","update","invalid"})
        check(classify(call("git_manage_issue",{{"action",action}})) == exclusive,"Git issue write must be exclusive");
    check(classify(call("git_manage_issue")) == exclusive,"Missing Git action must default to exclusive");
}

void batchAdmission() {
    for (const auto& tool : botToolNames()) check(batchable(tool), "Bot tool missing from batch admission");
    check(batchable("flock_session_search"), "Session search missing from batch admission");
    check(batchable("check_environment_readiness"), "Environment readiness missing from batch admission");
    for (const auto& tool : gitTools()) check(!batchable(tool), "Git tool admitted to batching");
    for (const auto tool : {"read_tool_result","flock_session_events","message_session","reply","unknown_tool"})
        check(!batchable(tool), "Unexpected batch admission for " + std::string(tool));
}

void waves() {
    using Waves = std::vector<std::vector<std::size_t>>;
    check(planWaves({}).empty(), "Empty batch produced a wave");
    check(planWaves({call("flock_session_search"),call("check_environment_readiness")}) == Waves{{0,1}},
          "Independent conversation reads must share one wave");
    check(planWaves({
        call("workspace_write_file",{{"path","src/./Readme"}}),
        call("workspace_read_file",{{"path","src/README"}}),
        call("workspace_read_file",{{"path","src/README"}}),
        call("workspace_write_file",{{"path","src/readme"}}),
        call("workspace_read_file",{{"path","src/other"}}),
        call("workspace_search",{{"query","marker"}}),
        call("check_environment_readiness"),
        call("workspace_read_file",{{"path","src/readme"}}),
        call("workspace_write_file",{{"path","other"}})
    }) == Waves{{0,4,6},{1,2},{3},{5,7},{8}},
          "Each workspace call must follow every earlier conflict and use the earliest eligible wave");
    check(planWaves({
        call("workspace_command_status",{{"commandId",1}}),
        call("workspace_stop_command",{{"commandId",1}}),
        call("workspace_command_status",{{"commandId",2}}),
        call("workspace_run_command"),
        call("workspace_command_status",{{"commandId",1}})
    }) == Waves{{0,2},{1},{3},{4}}, "Command control must preserve conflict order");
    check(planWaves({
        call("mcp_discover",{{"server","a"}}),
        call("mcp_call_tool",{{"server","a"}}),
        call("mcp_call_tool",{{"server","b"}}),
        call("mcp_read_resource",{{"server","a"}}),
        call("mcp_discover",{{"server","b"}})
    }) == Waves{{0,2},{1,4},{3}}, "MCP conflicts must order calls on the same server");
}

void approvalKeys() {
    check(turnCallKey(42,3) == "turn/42/3", "Single-call approval key changed");
    check(turnCallKey(42,3,0) == "turn/42/3/0", "First indexed approval key changed");
    check(turnCallKey(42,3,2) == "turn/42/3/2", "Indexed approval keys must distinguish siblings");
}
}

int main() {
    try {
        classification();
        batchAdmission();
        waves();
        approvalKeys();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
