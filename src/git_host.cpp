#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace flockv0 {
namespace {
constexpr std::size_t gitResultTextLimit = 2000;
constexpr std::string_view slugChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
bool slug(std::string_view value) {
    return !value.empty() && value.size() <= 100 && value != "." && value != ".." &&
           value.find_first_not_of(slugChars) == value.npos;
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result,result.begin(),[](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string hostName(std::string_view host) {
    if (!slug(host) || host.find('_') != host.npos) throw Conflict("Invalid Git host");
    return lower(host);
}

Id positiveId(const Json& value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<Id>::max()) ||
        value.get<Id>() < 1) throw Conflict("Expected a positive Git identifier");
    return value.get<Id>();
}

void validate(const Decision& decision, bool conversation) {
    const auto schema = gitToolSchema(decision.tool,conversation).at("parameters");
    const auto& args = decision.args;
    const auto& properties = schema.at("properties");
    if (!args.is_object() || args.dump().size() > 20000) throw Conflict("Invalid Git tool arguments");
    for (const auto& key : schema.at("required"))
        if (key != "reason" && !args.contains(key.get<std::string>())) throw Conflict("Missing Git tool argument: " + key.get<std::string>());
    for (const auto& [key,value] : args.items()) {
        if (key == "reason" || !properties.contains(key)) throw Conflict("Unknown Git tool argument");
        const auto& property = properties.at(key);
        const auto type = property.at("type");
        if (type == "integer") {
            const auto id = positiveId(value);
            if (property.contains("maximum") && id > property.at("maximum").get<Id>())
                throw Conflict("Git tool limit exceeded");
        } else if (type == "array") {
            if (!value.is_array() || value.size() > 30 || !std::ranges::all_of(value,[](const Json& item) {
                return item.is_string() && !item.get_ref<const std::string&>().empty() &&
                    item.get_ref<const std::string&>().size() <= 100;
            })) throw Conflict("Invalid Git tool list");
        } else if (type == "boolean") {
            if (!value.is_boolean()) throw Conflict("Expected a boolean Git argument");
        } else {
            if (!value.is_string()) throw Conflict("Expected a text Git argument");
            const auto& text = value.get_ref<const std::string&>();
            if (text.find('\0') != text.npos || text.size() > (key == "body" ? 12000 : 500))
                throw Conflict("Invalid Git text argument");
            if (property.contains("enum") && std::ranges::find(property.at("enum"),value) == property.at("enum").end())
                throw Conflict("Invalid Git action value");
        }
    }
    const auto requireText = [&](const char* key) {
        if (field(args,key).find_first_not_of(" \r\n\t") == std::string::npos)
            throw Conflict(std::string(key) + " is required");
    };
    const auto& tool = decision.tool;
    if (tool == "git_open_pr" || args.contains("title")) requireText("title");
    if (tool == "git_comment" || (tool == "git_submit_review" && args.at("verdict") != "approve")) requireText("body");
    if (tool == "git_request_review" && args.at("reviewers").empty()) throw Conflict("Choose at least one reviewer");
    if (tool == "git_update_pr" && !args.contains("title") && !args.contains("body") &&
        !args.contains("baseRef") && !args.contains("state")) throw Conflict("Choose a PR field to update");
    if (tool == "git_commit_status") {
        const auto sha = field(args,"sha");
        if ((sha.size() != 40 && sha.size() != 64) || sha.find_first_not_of("0123456789abcdefABCDEF") != sha.npos)
            throw Conflict("Expected a full commit SHA");
    }
    if (tool != "git_manage_issue") return;
    const auto action = field(args,"action");
    const bool writes = action == "create" || action == "update";
    const bool hasNumber = action == "read" || action == "list_comments" || action == "update";
    if (hasNumber != args.contains("issueNumber") ||
        (!writes && (args.contains("title") || args.contains("body") || args.contains("labels") || args.contains("assignees"))) ||
        (args.contains("state") && action != "list" && (action != "update" || args.at("state") == "all")) ||
        (args.contains("page") && action != "list" && action != "list_comments"))
        throw Conflict("Issue arguments do not match the requested action");
    if (action == "create") requireText("title");
    if (action == "update" && !args.contains("title") && !args.contains("body") && !args.contains("state") &&
        !args.contains("labels") && !args.contains("assignees")) throw Conflict("Choose an issue field to update");
}

std::string branch(const Json& args, const char* key) {
    const auto ref = field(args,key);
    if (ref.empty() || ref.find_first_of(" \r\n\t~^:?*[\\") != ref.npos || ref.find("..") != ref.npos ||
        ref.starts_with('/') || ref.ends_with('/') || ref.ends_with('.') || ref.find("//") != ref.npos ||
        ref.starts_with('-') || ref.starts_with("refs/") || ref.find("@{") != ref.npos || ref == "@" ||
        std::ranges::any_of(ref,[](unsigned char c) { return c < 32 || c == 127; }))
        throw Conflict("Expected an exact branch name");
    auto rest = std::string_view(ref);
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto component = rest.substr(0,slash);
        if (component.starts_with('.') || component.ends_with(".lock")) throw Conflict("Expected an exact branch name");
        if (slash == rest.npos) break;
        rest.remove_prefix(slash + 1);
    }
    return ref;
}

std::string_view operation(const Decision& decision) {
    const auto& tool = decision.tool;
    if (tool == "git_open_pr" || tool == "git_update_pr" || tool == "git_request_review") return "pr";
    if (tool == "git_submit_review") return "review";
    if (tool == "git_comment") return decision.args.at("target") == "pr" ? "pr" : "issues";
    if (tool == "git_manage_issue" && (decision.args.at("action") == "create" || decision.args.at("action") == "update"))
        return "issues";
    return "read";
}

}

Json gitJournal(const GitContext& context, std::string_view tool, const GitRepository& repo, Json result) {
    return {{"tool",tool},{"host",repo.host},{"repo",repo.repo},{"run",runJson(context.run)},
            {"environmentId",context.environmentId},{"result",std::move(result)}};
}

GitRepository GitRepository::parse(std::string_view host, std::string_view repo) {
    const auto slash = repo.find('/');
    if (slash == repo.npos || !slug(repo.substr(0,slash)) || !slug(repo.substr(slash+1)))
        throw Conflict("Expected a Git repository in owner/repo form");
    return {hostName(host),lower(repo)};
}

Json Store::Impl::gitTool(GitHost& host, std::string_view bot, Id taskId, const Json& routing,
                          const Decision& decision, std::string_view invocation, std::optional<Id> generation) {
    validate(decision,!generation.has_value());
    const auto& args = decision.args;
    const auto hostId = hostName(field(args,"host"));
    if (hostId != host.name()) throw Conflict("Git host is not connected");
    if (!taskId && args.contains("taskId")) taskId = positiveId(args.at("taskId"));
    const auto op = operation(decision);
    if (op != "read" && !taskId) throw Conflict("Select an owned task for the Git mutation");
    GitContext context{taskId,std::string(bot),field(routing,"environmentId"),field(routing,"projectId"),{}};
    {
        std::lock_guard lock(mutex);
        if (taskId) {
            const auto task = load(taskId);
            if (field(task,"botId") != bot) throw Conflict("No task with this ID");
            if (generation && task.at("generation") != *generation) throw RequestChanged{};
            if (op != "read" && (task.at("state") == "cancelled" || task.at("state") == "cancelling"))
                throw Conflict("The task is cancelled");
            context.environmentId = field(task,"environmentId");
            context.projectId = field(task,"projectId");
            const auto active = task.value("activeRun",Json(nullptr));
            context.run = runRef(active.is_null() ? task.value("lastRun",Json(nullptr)) : active);
        }
        sharedRedactor().requirePublic(args);
    }
    const Access access(*this);
    const auto permits = [&](const GitRepository& repository, std::string_view required) {
        const Scope scope{"repository",repository.resource(),std::string(required)};
        return access.permits(bot,scope,true) && access.permits(bot,scope,host.permits(context,repository,required));
    };
    if (decision.tool == "git_find_repos") {
        std::vector<GitRepository> candidates;
        {
            std::lock_guard lock(mutex);
            auto rows = query("SELECT resource FROM bot_grants WHERE bot=? AND kind='repository' AND op='read'"
                              " AND substr(resource,1,?)=? ORDER BY resource",{bot,hostId.size()+1,hostId + "/"});
            while (rows.next()) candidates.push_back({hostId,rows.string(0).substr(hostId.size()+1)});
        }
        Json repos = Json::array();
        const auto search = lower(field(args,"query"));
        bool truncated = false;
        for (const auto& candidate : candidates) {
            if (candidate.repo.find(search) == candidate.repo.npos || !permits(candidate,"read")) continue;
            if (repos.size() == 100) {
                truncated = true;
                break;
            }
            repos.push_back(candidate.repo);
        }
        return sharedRedactor().record({{"ok",true},{"host",hostId},{"repos",repos},{"truncated",truncated}},
                                    SecretRedactor::Fields::external);
    }
    const auto repo = GitRepository::parse(hostId,field(args,"repo"));
    if (!permits(repo,op)) throw Conflict("Repository access denied");
    if (decision.tool == "git_open_pr") {
        const auto head = args.contains("headRepo") ? GitRepository::parse(hostId,field(args,"headRepo")) : repo;
        if (head != repo && !permits(head,"read")) throw Conflict("Head repository read access denied");
        auto result = openGitPr(host,context,{{repo,head,branch(args,"baseRef"),branch(args,"headRef")},{},
                                     field(args,"title"),field(args,"body"),args.value("draft",false)},
                                invocation,args.value("newIntent",false));
        result["ok"] = result.at("status") == "created";
        return result;
    }
    const bool watchPr = decision.tool == "git_update_pr" || decision.tool == "git_request_review";
    if (watchPr) {
        if (args.contains("baseRef")) branch(args,"baseRef");
        std::lock_guard lock(mutex);
        if (query("SELECT 1 FROM git_pr_watches WHERE host=? AND repo=? AND pr_number=? AND task_id<>?",
                  {repo.host,repo.repo,args.at("prNumber"),taskId}).next())
            throw Conflict("This pull request belongs to another task");
    }
    if (op == "read") {
        try {
            const auto scrub = sharedRedactor();
            if (decision.tool == "git_read_ci_logs") {
                const auto maxBytes = args.value("maxBytes",std::size_t{8192});
                const auto log = host.readCiLogs(context,repo,args.at("jobId"),maxBytes);
                const auto sanitized = scrub.text(log.text,log.truncated);
                const auto utf8 = Json::parse(Json(sanitized).dump(-1,' ',false,Json::error_handler_t::replace)).get<std::string>();
                const auto excerpt = bounded(utf8,maxBytes);
                return {{"ok",true},{"data",{{"text",excerpt},{"truncated",log.truncated || excerpt.size() < utf8.size()}}}};
            }
            return {{"ok",true},{"data",scrub.payload(host.call(context,repo,decision),false,gitResultTextLimit)}};
        } catch (const std::exception& error) {
            return {{"ok",false},{"error",sharedRedactor().text(error.what())}};
        }
    }

    Json result;
    auto target = args;
    target.erase("taskId");
    target["host"] = repo.host;
    target["repo"] = repo.repo;
    auto audit = target;
    audit.erase("body");
    const bool createComment = decision.tool == "git_comment" && !target.contains("commentId");
    const auto key = Json::array({invocation,decision.tool,target}).dump();
    const Json dedupe = createComment ? Json(target.dump()) : Json(nullptr);
    Id eventCursor = 0;
    {
        std::lock_guard lock(mutex);
        Transaction tx(db);
        auto previous = query("SELECT result FROM git_mutations WHERE task_id=? AND (invocation=? OR dedupe=?)"
                              " ORDER BY invocation=? DESC LIMIT 1",{taskId,key,dedupe,key});
        if (previous.next()) {
            if (const auto stored = previous.string(0); !stored.empty()) return Json::parse(stored);
            return {{"ok",false},{"status","unknown"},{"error","The host outcome is unknown. Check the target before retrying."}};
        }
        if (watchPr) {
            auto events = query("SELECT coalesce(max(id),0) FROM git_webhook_events");
            events.next();
            eventCursor = events.number(0);
        }
        exec("INSERT INTO git_mutations(task_id,invocation,dedupe) VALUES(?,?,?)",{taskId,key,dedupe});
        journal(taskId,"git_mutation",sharedRedactor().record(gitJournal(context,decision.tool,repo,
            {{"status","pending"},{"target",audit}})));
        tx.commit();
    }
    try {
        result = {{"ok",true},{"data",host.call(context,repo,decision)}};
    } catch (const GitRejected& error) {
        result = {{"ok",false},{"status","failed"},{"error",error.what()}};
    } catch (const std::exception&) {
        result = {{"ok",false},{"status","unknown"},{"error","The host outcome is unknown. Check the target before retrying."}};
    }
    std::lock_guard lock(mutex);
    Transaction tx(db);
    const auto scrub = sharedRedactor();
    if (result.value("ok",false)) result["data"] = scrub.payload(result.at("data"),false,gitResultTextLimit);
    else result["error"] = scrub.text(field(result,"error"));
    if (result.value("ok",false) && (watchPr || decision.tool == "git_submit_review")) {
        const auto pr = args.at("prNumber").get<Id>();
        const auto url = "https://" + repo.resource() + "/pull/" + std::to_string(pr);
        const bool requested = decision.tool == "git_request_review";
        if (watchPr) {
            const auto sha = field(result.at("data"),"headSha");
            result["watchAttached"] = watchGitPr(taskId,bot,runJson(context.run),eventCursor,{{repo,repo,{},{}},pr,url,{},sha,false});
        }
        if ((requested && result.at("watchAttached") == true) || decision.tool == "git_submit_review")
            gitInboxItem(load(taskId),"review/" + actionId(),url,
                requested ? "Pull request awaiting review" : "Pull request review submitted",
                requested ? "Review requested on the Git host." : field(args,"verdict"));
    }
    exec("UPDATE git_mutations SET result=?1,dedupe=CASE WHEN json_extract(?1,'$.status')='failed' THEN NULL ELSE dedupe END"
         " WHERE task_id=?2 AND invocation=?3",{result,taskId,key});
    journal(taskId,"git_mutation",scrub.record(gitJournal(context,decision.tool,repo,{{"target",audit},{"outcome",result}})));
    tx.commit();
    return result;
}

bool Store::Impl::gitConversationEffectStarted(Id message, int step) const {
    const auto invocation = "turn/" + std::to_string(message) + "/" + std::to_string(step);
    return query(R"SQL(
SELECT 1 FROM git_pr_operations WHERE invocation IN (?,?)
UNION ALL SELECT 1 FROM git_mutations WHERE json_extract(invocation,'$[0]')=?
LIMIT 1
)SQL",{invocation,invocation + "/new",invocation}).next();
}

Json Store::Impl::gitConversationTool(GitHost* host, const conversation::Message& message, int step, const Decision& decision) {
    try {
        if (!host || !message.channelScope.empty()) throw Conflict("Git tools are available in Flock Web with a connected Git host");
        auto call = decision;
        if (message.routing.contains("gitEvent")) {
            const auto& webhook = message.routing.at("gitEvent");
            const auto owner = webhook.at("taskId").get<Id>();
            if ((call.args.contains("taskId") && call.args.at("taskId") != owner) ||
                field(call.args,"host") != field(webhook,"host") || field(call.args,"repo") != field(webhook,"repo") ||
                (call.tool == "git_pr_status" && call.args.at("prNumber") != webhook.at("prNumber")) ||
                (call.tool == "git_commit_status" && field(call.args,"sha") != field(webhook,"headSha")) ||
                (call.tool == "git_read_ci_logs" && call.args.at("jobId") != webhook.at("jobId")))
                throw Conflict("Git notification tools must use the watched target");
            call.args["taskId"] = owner;
        }
        return gitTool(*host,message.bot,0,message.routing,call,
                       "turn/" + std::to_string(message.id) + "/" + std::to_string(step));
    } catch (const std::exception& error) {
        return {{"ok",false},{"error",sharedRedactor().text(error.what())}};
    }
}
}
