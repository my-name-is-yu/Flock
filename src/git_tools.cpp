#include "flock/git_host.hpp"

#include <algorithm>
#include <stdexcept>

namespace flockv0 {
namespace {
struct Choice {
    const char* value;
    std::string_view operation = {};
};
struct Param {
    const char* name;
    const char* type;
    const char* description;
    bool required = true;
    std::vector<Choice> choices = {};
};
struct Tool {
    const char* description;
    std::string_view operation;
    std::vector<Param> params;
    const char* notificationArgument = nullptr;
    const char* notificationField = nullptr;
    bool taskOnly = false;
};

const std::vector<std::pair<std::string_view,Tool>>& specs() {
    static const std::vector<std::pair<std::string_view,Tool>> tools{
        {"git_find_repos",{"Find repositories with a repository:read grant on the connected host.",
            "read",{{"query","string","Optional case-insensitive substring of owner/repo.",false}}}},
        {"git_read_result",{"Recover a saved Git read for this task. Read IDs are listed in event.gitReadRefs.",
            "read",{{"readId","integer","Exact saved Git read ID."}},nullptr,nullptr,true}},
        {"git_repo_info",{"Read repository details using repository:read.","read",{}}},
        {"git_open_pr",{"Open or recover the task's PR using repository:pr and repository:read on a separate head repository.",
            "pr",{{"baseRef","string","Exact base branch."},{"headRepo","string","Head owner/repo. Omit for the base repository.",false},
             {"headRef","string","Exact head branch."},{"title","string","Pull request title."},
             {"body","string","Pull request description.",false},{"draft","boolean","Open as a draft.",false},
             {"newIntent","boolean","Set only when explicitly asked to create a replacement after the previous PR closed.",false}}}},
        {"git_update_pr",{"Update title, body, base branch or open/closed state using repository:pr.",
            "pr",{{"prNumber","integer","Exact PR number."},{"title","string","New title.",false},
             {"body","string","New description.",false},{"baseRef","string","New base branch.",false},
             {"state","string","New PR state.",false,{{"open"},{"closed"}}}}}},
        {"git_pr_status",{"Read PR state and head commit using repository:read.",
            "read",{{"prNumber","integer","Exact PR number."}},"prNumber"}},
        {"git_commit_status",{"Read commit statuses and checks using repository:read.",
            "read",{{"sha","string","Full commit SHA."}},"sha","headSha"}},
        {"git_read_ci_logs",{"Read a bounded CI job log using repository:read. Check truncated before drawing conclusions.",
            "read",{{"jobId","integer","Exact CI job ID, for example from a webhook."},
             {"maxBytes","integer","Byte limit, 1 to 16384. Defaults to 8192.",false}},"jobId"}},
        {"git_request_review",{"Request reviewers on the task's PR using repository:pr.",
            "pr",{{"prNumber","integer","Exact PR number."},{"reviewers","array","Git host usernames."}}}},
        {"git_submit_review",{"Submit a review using repository:review.",
            "review",{{"prNumber","integer","Exact PR number."},{"verdict","string","Review verdict.",true,{{"approve"},{"request_changes"},{"comment"}}},
             {"body","string","Review text. Required for comment and request_changes.",false}}}},
        {"git_comment",{"Create or edit a PR or issue comment. To edit or retry an edit, pass its exact commentId.",
            {},{{"target","string","Kind of object to comment on.",true,{{"pr","pr"},{"issue","issues"}}},
             {"number","integer","Exact PR or issue number."},{"body","string","Comment text."},
             {"commentId","integer","Exact existing comment ID. Omit only to create a new comment.",false}}}},
        {"git_manage_issue",{"List/read issues and comments using repository:read. Create/update using repository:issues. Use git_comment to write comments.",
            {},{{"action","string","Issue action.",true,{{"list","read"},{"read","read"},{"list_comments","read"},
                                                             {"create","issues"},{"update","issues"}}},
             {"issueNumber","integer","Required for read, list_comments and update.",false},
             {"title","string","Required to create. Optional to update.",false},
             {"body","string","Issue description for create or update.",false},
             {"state","string","State filter for list or new state for update.",false,{{"open"},{"closed"},{"all"}}},
             {"labels","array","Labels. For update, replaces the current list.",false},
             {"assignees","array","Usernames. For update, replaces the current list.",false},
             {"page","integer","Start at 1 and use nextPage to continue, even when items is empty.",false}}}},
    };
    return tools;
}

const Tool* findSpec(std::string_view name) {
    const auto& tools = specs();
    const auto tool = std::ranges::find_if(tools,[&](const auto& entry) { return entry.first == name; });
    return tool != tools.end() ? &tool->second : nullptr;
}

const Json* notificationTarget(const Tool& spec, const Json& event) {
    if (spec.operation != "read" || !spec.notificationArgument) return nullptr;
    const auto target = event.find(spec.notificationField ? spec.notificationField : spec.notificationArgument);
    if (target == event.end()) return nullptr;
    if (target->is_string() ? *target == "" : !target->is_number_integer() || *target <= 0) return nullptr;
    return &*target;
}
}

const std::vector<std::string>& gitTools(bool conversation) {
    const auto names = [](bool conversation) {
        std::vector<std::string> result;
        for (const auto& [name,spec] : specs())
            if (!conversation || !spec.taskOnly) result.emplace_back(name);
        std::ranges::sort(result);
        return result;
    };
    static const auto conversationNames = names(true);
    static const auto taskNames = names(false);
    return conversation ? conversationNames : taskNames;
}
bool isGitTool(std::string_view tool) { return findSpec(tool) != nullptr; }

std::string_view gitOperation(const Decision& call) {
    const auto* spec = findSpec(call.tool);
    if (!spec) return {};
    if (!spec->operation.empty()) return spec->operation;
    for (const auto& param : spec->params) {
        const auto argument = call.args.find(param.name);
        if (argument == call.args.end()) continue;
        for (const auto& choice : param.choices)
            if (!choice.operation.empty() && *argument == choice.value) return choice.operation;
    }
    return {};
}

void appendGitNotificationTools(const Json& event, std::vector<std::string>& tools) {
    for (const auto& [name,spec] : specs())
        if (notificationTarget(spec,event)) tools.emplace_back(name);
}

bool gitNotificationToolAllowed(const Decision& call, const Json& event) {
    const auto* spec = findSpec(call.tool);
    const auto* target = spec ? notificationTarget(*spec,event) : nullptr;
    if (!target) return false;
    const auto argument = call.args.find(spec->notificationArgument);
    return argument != call.args.end() && *argument == *target;
}

Json gitToolSchema(std::string_view name, bool conversation) {
    const auto* spec = findSpec(name);
    if (!spec) throw std::out_of_range("Unknown Git tool");
    Json properties{{"host",{{"type","string"},{"description","Connected Git host, for example github.com."}}},
                    {"reason",{{"type","string"},{"description","Evidence and reason for the action."}}}};
    Json required = {"host","reason"};
    if (name != "git_find_repos" && name != "git_read_result") {
        properties["repo"] = {{"type","string"},{"description","Exact owner/repo from the repository grant."}};
        required.push_back("repo");
        if (conversation)
            properties["taskId"] = {{"type","integer"},{"minimum",1},{"description","Task owned by this Bot. Required for mutations."}};
    }
    const auto taskIdPosition = required.size();
    bool mayRead = spec->operation == "read";
    for (const auto& param : spec->params) {
        Json property{{"type",param.type},{"description",param.description}};
        if (property["type"] == "integer") property["minimum"] = 1;
        if (property["type"] == "array") property["items"] = {{"type","string"}};
        for (const auto& choice : param.choices) {
            property["enum"].push_back(choice.value);
            mayRead |= choice.operation == "read";
        }
        properties[param.name] = std::move(property);
        if (param.required) required.push_back(param.name);
    }
    if (properties.contains("taskId") && !mayRead) required.insert(required.begin() + taskIdPosition,"taskId");
    if (name == "git_read_ci_logs") properties["maxBytes"]["maximum"] = 16384;
    if (name == "git_manage_issue") properties["page"]["maximum"] = 10000;
    Json parameters{{"type","object"},{"properties",std::move(properties)},
                    {"required",std::move(required)},{"additionalProperties",false}};
    if (name == "git_manage_issue" && conversation) {
        parameters["oneOf"] = Json::array({
            {{"properties",{{"action",{{"enum",{"list","read","list_comments"}}}}}}},
            {{"properties",{{"action",{{"enum",{"create","update"}}}}}},{"required",{"taskId"}}}
        });
    }
    return {{"type","function"},{"name",name},{"description",spec->description},{"strict",false},
            {"parameters",std::move(parameters)}};
}
}
