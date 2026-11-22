#include "flock/git_host.hpp"

#include <algorithm>
#include <map>

namespace flockv0 {
namespace {
struct Param {
    const char* name;
    const char* type;
    const char* description;
    bool required = true;
    std::vector<const char*> choices = {};
};
struct Tool {
    const char* description;
    std::vector<Param> params;
};

const std::map<std::string_view,Tool>& specs() {
    static const std::map<std::string_view,Tool> tools{
        {"git_find_repos",{"Find repositories with a repository:read grant on the connected host.",
            {{"query","string","Optional case-insensitive substring of owner/repo.",false}}}},
        {"git_repo_info",{"Read repository details using repository:read.",{}}},
        {"git_open_pr",{"Open or recover the task's PR using repository:pr and repository:read on a separate head repository.",
            {{"baseRef","string","Exact base branch."},{"headRepo","string","Head owner/repo. Omit for the base repository.",false},
             {"headRef","string","Exact head branch."},{"title","string","Pull request title."},
             {"body","string","Pull request description.",false},{"draft","boolean","Open as a draft.",false}}}},
        {"git_update_pr",{"Update title, body, base branch or open/closed state using repository:pr.",
            {{"prNumber","integer","Exact PR number."},{"title","string","New title.",false},
             {"body","string","New description.",false},{"baseRef","string","New base branch.",false},
             {"state","string","New PR state.",false,{"open","closed"}}}}},
        {"git_pr_status",{"Read PR state and head commit using repository:read.",
            {{"prNumber","integer","Exact PR number."}}}},
        {"git_commit_status",{"Read commit statuses and checks using repository:read.",
            {{"sha","string","Full commit SHA."}}}},
        {"git_read_ci_logs",{"Read a bounded CI job log using repository:read. Check truncated before drawing conclusions.",
            {{"jobId","integer","Exact CI job ID, for example from a webhook."},
             {"maxBytes","integer","Byte limit, 1 to 16384. Defaults to 8192.",false}}}},
        {"git_request_review",{"Request reviewers on the task's PR using repository:pr.",
            {{"prNumber","integer","Exact PR number."},{"reviewers","array","Git host usernames."}}}},
        {"git_submit_review",{"Submit a review using repository:review.",
            {{"prNumber","integer","Exact PR number."},{"verdict","string","Review verdict.",true,{"approve","request_changes","comment"}},
             {"body","string","Review text. Required for comment and request_changes.",false}}}},
        {"git_comment",{"Create or edit a PR or issue comment. To edit or retry an edit, pass its exact commentId.",
            {{"target","string","Kind of object to comment on.",true,{"pr","issue"}},
             {"number","integer","Exact PR or issue number."},{"body","string","Comment text."},
             {"commentId","integer","Exact existing comment ID. Omit only to create a new comment.",false}}}},
        {"git_manage_issue",{"List/read issues and comments using repository:read. Create/update using repository:issues. Use git_comment to write comments.",
            {{"action","string","Issue action.",true,{"list","read","list_comments","create","update"}},
             {"issueNumber","integer","Required for read, list_comments and update.",false},
             {"title","string","Required to create. Optional to update.",false},
             {"body","string","Issue description for create or update.",false},
             {"state","string","State filter for list or new state for update.",false,{"open","closed","all"}},
             {"labels","array","Labels. For update, replaces the current list.",false},
             {"assignees","array","Usernames. For update, replaces the current list.",false},
             {"page","integer","Page of up to 30 items for list or list_comments.",false}}}},
    };
    return tools;
}
}

const std::vector<std::string>& gitTools() {
    static const auto names = [] {
        std::vector<std::string> result;
        for (const auto& [name,spec] : specs()) result.emplace_back(name);
        return result;
    }();
    return names;
}
bool isGitTool(std::string_view tool) { return specs().contains(tool); }

Json gitToolSchema(std::string_view name, bool conversation) {
    const auto& spec = specs().at(name);
    Json properties{{"host",{{"type","string"},{"description","Connected Git host, for example github.com."}}},
                    {"reason",{{"type","string"},{"description","Evidence and reason for the action."}}}};
    Json required = {"host","reason"};
    if (name != "git_find_repos") {
        properties["repo"] = {{"type","string"},{"description","Exact owner/repo from the repository grant."}};
        required.push_back("repo");
        if (conversation) {
            properties["taskId"] = {{"type","integer"},{"minimum",1},{"description","Task owned by this Bot. Required for mutations."}};
            if (name == "git_open_pr" || name == "git_update_pr" || name == "git_request_review" ||
                name == "git_submit_review" || name == "git_comment") required.push_back("taskId");
        }
    }
    for (const auto& param : spec.params) {
        Json property{{"type",param.type},{"description",param.description}};
        if (property["type"] == "integer") property["minimum"] = 1;
        if (property["type"] == "array") property["items"] = {{"type","string"}};
        if (param.choices.size()) property["enum"] = param.choices;
        properties[param.name] = std::move(property);
        if (param.required) required.push_back(param.name);
    }
    if (name == "git_read_ci_logs") properties["maxBytes"]["maximum"] = 16384;
    return {{"type","function"},{"name",name},{"description",spec.description},{"strict",false},
        {"parameters",{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}}};
}
}
