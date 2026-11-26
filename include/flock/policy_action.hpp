#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
enum class Action {
    bot_view,
    bot_talk,
    bot_decide,
    bot_settings,
    bot_create,
    bot_rename,
    rules_manage,
    repository_read,
    repository_write,
    repository_pr,
    repository_review,
    repository_issues,
    secret_use,
    session_read,
    mcp_discover,
    mcp_invoke,
    mcp_read_resource,
    environment_inspect,
    environment_configure,
    environment_launch,
    shared_memory_read,
    shared_memory_write,
    private_memory_write,
    workspace_read,
    workspace_write,
    web_search,
    web_fetch,
    skill_list,
    skill_read,
    attachment_inspect,
    channel_use,
    channel_read,
    channel_post,
    t3_send,
    automation_change,
    email_send,
    destructive_write
};
enum class PolicyResult { allow, ask, deny };
enum class RuleKind { allow, ask, never };
std::optional<RuleKind> parseRuleKind(std::string_view);
std::string_view actionName(Action);
std::optional<Action> parseAction(std::string_view);

struct Resource {
    Action action;
    std::string target;
    // Verified exception to the category default, still subject to rules.
    bool routine = false;
    static Resource sessionRead(std::string_view environment, std::string_view thread);
};
}
