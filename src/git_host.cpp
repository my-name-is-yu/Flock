#include "storage.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace flockv0 {
namespace {
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
    if (!args.is_object() || args.dump().size() > 20000) throw Conflict("Invalid Git tool arguments");
    for (const auto& key : schema.at("required"))
        if (key != "reason" && !args.contains(key.get<std::string>())) throw Conflict("Missing Git tool argument: " + key.get<std::string>());
    for (const auto& [key,value] : args.items()) {
        const auto& properties = schema.at("properties");
        if (key == "reason" || !properties.contains(key)) throw Conflict("Unknown Git tool argument: " + key);
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
    auto ref = field(args,key);
    if (ref.empty() || ref.find_first_of(" \r\n\t~^:?*[\\") != ref.npos || ref.find("..") != ref.npos ||
        ref.starts_with('/') || ref.ends_with('/') || ref.ends_with('.') || ref.find("//") != ref.npos ||
        ref.starts_with('-') || ref.starts_with("refs/") || ref.find("@{") != ref.npos || ref == "@")
        throw Conflict("Expected an exact branch name");
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

Json prResult(std::string_view operationId, std::string_view status, Id number = 0, std::string_view url = {}) {
    Json result{{"operationId",operationId},{"status",status}};
    if (number) result.update({{"prNumber",number},{"url",url}});
    if (status == "ambiguous") result["message"] = "PR creation is unresolved. A reconciliation item is in the Decision Inbox.";
    return result;
}

bool verifiedPr(const GitPullRequest& pr, const GitOpenPr& request) {
    return pr.target == request.target && pr.operationId == request.operationId && pr.number > 0 &&
        pr.url.starts_with("https://" + pr.target.base.resource() + "/pull/") &&
        pr.url == "https://" + pr.target.base.resource() + "/pull/" + std::to_string(pr.number);
}

Json gitJournal(const GitContext& context, std::string_view tool, const GitRepository& repo, Json result) {
    return {{"tool",tool},{"host",repo.host},{"repo",repo.repo},{"run",runJson(context.run)},
            {"environmentId",context.environmentId},{"result",std::move(result)}};
}
}

GitRepository GitRepository::parse(std::string_view host, std::string_view repo) {
    const auto slash = repo.find('/');
    if (slash == repo.npos || !slug(repo.substr(0,slash)) || !slug(repo.substr(slash+1)))
        throw Conflict("Expected a Git repository in owner/repo form");
    return {hostName(host),lower(repo)};
}

void Store::Impl::migrateGit() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS git_pr_operations(
 operation_id TEXT PRIMARY KEY NOT NULL, host TEXT NOT NULL, base_repo TEXT NOT NULL,
 base_ref TEXT NOT NULL, head_repo TEXT NOT NULL, head_ref TEXT NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL,
 status TEXT NOT NULL CHECK(status IN ('pending','created','ambiguous','failed')),
 pr_number INTEGER, url TEXT, UNIQUE(host,base_repo,base_ref,head_repo,head_ref));
CREATE TABLE IF NOT EXISTS git_pr_watches(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL,
 url TEXT NOT NULL, head_sha TEXT NOT NULL,
 PRIMARY KEY(host,repo,pr_number));
CREATE INDEX IF NOT EXISTS git_watch_head ON git_pr_watches(host,repo,head_sha);
CREATE TABLE IF NOT EXISTS git_mutations(
 invocation TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id), result TEXT,
 PRIMARY KEY(task_id,invocation));
CREATE TABLE IF NOT EXISTS git_inbox_items(
 task_id INTEGER NOT NULL REFERENCES tasks(id), dedupe TEXT NOT NULL,
 inbox_id INTEGER NOT NULL UNIQUE REFERENCES pr_review_items(id),
 resolved INTEGER NOT NULL DEFAULT 0 CHECK(resolved IN (0,1)), PRIMARY KEY(task_id,dedupe));
CREATE TABLE IF NOT EXISTS git_webhook_events(
 id INTEGER PRIMARY KEY, host TEXT NOT NULL, event_id TEXT NOT NULL, repo TEXT NOT NULL,
 data TEXT NOT NULL, received_at INTEGER NOT NULL, UNIQUE(host,event_id));
CREATE TABLE IF NOT EXISTS git_webhook_deliveries(
 event INTEGER NOT NULL REFERENCES git_webhook_events(id) ON DELETE CASCADE, pr_number INTEGER NOT NULL,
 PRIMARY KEY(event,pr_number));
CREATE INDEX IF NOT EXISTS git_events_repo ON git_webhook_events(host,repo);
CREATE INDEX IF NOT EXISTS journal_pending_git_turn ON journal(conversation_bot_id,id)
 WHERE kind='git_webhook' AND json_extract(data,'$.status') IS NULL;
)SQL";
    if (sqlite3_exec(db,schema,nullptr,nullptr,nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}

Json Store::Impl::openGitPr(GitHost& host, const GitContext& context, const GitOpenPr& input) {
    auto request = input;
    const auto& [base,head,baseRef,headRef] = request.target;
    const Json key = {base.host,base.repo,baseRef,head.repo,headRef};
    bool create;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        auto operation = query("SELECT operation_id,task_id,bot_id,status,pr_number,url,run FROM git_pr_operations"
                               " WHERE host=? AND base_repo=? AND base_ref=? AND head_repo=? AND head_ref=?",key);
        create = !operation.next();
        if (create) {
            request.operationId = actionId();
            exec("INSERT INTO git_pr_operations VALUES(?,?,?,?,?,?,?,?,?, 'pending',NULL,NULL)",
                 {request.operationId,base.host,base.repo,baseRef,head.repo,headRef,context.taskId,context.botId,runJson(context.run)});
            journal(context.taskId,"git_mutation",gitJournal(context,"git_open_pr",base,prResult(request.operationId,"pending")));
        } else {
            if (operation.number(1) != context.taskId || operation.string(2) != context.botId)
                throw Conflict("This PR target belongs to another task");
            request.operationId = operation.string(0);
            if (operation.string(3) == "created" || operation.string(3) == "failed")
                return prResult(request.operationId,operation.string(3),operation.number(4),operation.string(5));
        }
        transaction.commit();
    }

    std::optional<GitPullRequest> match;
    std::string_view state = "ambiguous";
    try {
        if (create) {
            auto pr = host.createPullRequest(context,request);
            if (verifiedPr(pr,request)) match = std::move(pr);
        } else {
            for (auto& pr : host.findPullRequests(context,request.target)) {
                if (!verifiedPr(pr,request)) continue;
                if (match) {
                    match.reset();
                    break;
                }
                match = std::move(pr);
            }
        }
    } catch (const GitRejected&) {
        if (create) state = "failed";
    } catch (const std::exception&) {
    }

    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto saved = query("SELECT status,pr_number,url,run FROM git_pr_operations WHERE operation_id=?",{request.operationId});
    if (!saved.next()) throw Conflict("PR operation is missing");
    if (saved.string(0) == "created" || saved.string(0) == "failed")
        return prResult(request.operationId,saved.string(0),saved.number(1),saved.string(2));
    const auto run = saved.json(3);
    const auto task = load(context.taskId);
    const auto scrub = redactor(context.botId);
    if (match) {
        exec("INSERT OR IGNORE INTO git_pr_watches VALUES(?,?,?,?,?,?,?,?)",
             {base.host,base.repo,match->number,context.taskId,context.botId,run,match->url,match->headSha});
        if (query("SELECT 1 FROM git_pr_watches WHERE host=? AND repo=? AND pr_number=? AND task_id=? AND bot_id=?",
                  {base.host,base.repo,match->number,context.taskId,context.botId}).next()) state = "created";
        else match.reset();
    }
    const auto result = prResult(request.operationId,state,match ? match->number : 0,match ? match->url : "");
    exec("UPDATE git_pr_operations SET status=?,pr_number=?,url=? WHERE operation_id=?",
         {state,match ? Json(match->number) : Json(nullptr),match ? Json(match->url) : Json(nullptr),request.operationId});
    const auto inboxKey = "reconcile/" + request.operationId;
    if (state == "ambiguous") {
        gitInboxItem(task,inboxKey,"https://" + base.resource() + "/pulls","Reconcile pull request creation",
                     "Verify the original PR for " + head.resource() + ":" + headRef + " into " + baseRef +
                     ". Operation " + request.operationId + " remains unresolved. Retrying git_open_pr checks the host again.");
    }
    if (match) {
        exec("UPDATE git_inbox_items SET resolved=1 WHERE task_id=? AND dedupe=?",{context.taskId,inboxKey});
        if (match->awaitingReview) gitInboxItem(task,"awaiting/" + base.resource() + "/" + std::to_string(match->number),
                                               match->url,"Pull request awaiting review",request.title);
    }
    if (state != saved.string(0)) {
        auto entry = gitJournal(context,"git_open_pr",base,result);
        entry["run"] = run;
        entry["recovered"] = match.has_value() && !create;
        journal(context.taskId,"git_mutation",scrub.record(std::move(entry)));
    }
    transaction.commit();
    return result;
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
            const auto task = ownTask(taskId,bot);
            if (generation && task.at("generation") != *generation) throw RequestChanged{};
            context.environmentId = field(task,"environmentId");
            context.projectId = field(task,"projectId");
            const auto active = task.value("activeRun",Json(nullptr));
            context.run = runRef(active.is_null() ? task.value("lastRun",Json(nullptr)) : active);
        }
        redactor(bot).requirePublic(args);
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
            while (rows.next()) {
                try { candidates.push_back(GitRepository::parse(hostId,rows.string(0).substr(hostId.size()+1))); }
                catch (const Conflict&) {}
            }
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
        return {{"ok",true},{"host",hostId},{"repos",repos},{"truncated",truncated}};
    }
    const auto repo = GitRepository::parse(hostId,field(args,"repo"));
    if (!permits(repo,op)) throw Conflict("Repository access denied");
    if (decision.tool == "git_open_pr") {
        const auto head = args.contains("headRepo") ? GitRepository::parse(hostId,field(args,"headRepo")) : repo;
        if (head != repo && !permits(head,"read")) throw Conflict("Head repository read access denied");
        auto result = openGitPr(host,context,{{repo,head,branch(args,"baseRef"),branch(args,"headRef")},{},
                                     field(args,"title"),field(args,"body"),args.value("draft",false)});
        result["ok"] = result.at("status") == "created";
        return result;
    }
    if (decision.tool == "git_update_pr" && args.contains("baseRef")) branch(args,"baseRef");
    Json result;
    if (decision.tool == "git_read_ci_logs") {
        const auto maxBytes = args.value("maxBytes",std::size_t{8192});
        const auto log = host.readCiLogs(context,repo,args.at("jobId"),maxBytes);
        std::lock_guard lock(mutex);
        const auto sanitized = redactor(bot).text(log.text);
        const auto excerpt = bounded(sanitized,maxBytes);
        return {{"ok",true},{"data",{{"text",excerpt},{"truncated",log.truncated || excerpt.size() < sanitized.size()}}}};
    } else if (op == "read") {
        result = host.call(context,repo,decision);
    } else {
        auto target = args;
        target.erase("taskId");
        target["host"] = repo.host;
        target["repo"] = repo.repo;
        auto audit = target;
        audit.erase("body");
        const bool createComment = decision.tool == "git_comment" && !target.contains("commentId");
        const auto key = Json::array({createComment ? "comment" : invocation,decision.tool,target}).dump();
        {
            std::lock_guard lock(mutex);
            Transaction tx(db);
            auto previous = query("SELECT result FROM git_mutations WHERE task_id=? AND invocation=?",{taskId,key});
            if (previous.next()) {
                if (const auto stored = previous.string(0); !stored.empty()) return Json::parse(stored);
                return {{"ok",false},{"status","unknown"},{"error","The host outcome is unknown. Check the target before retrying."}};
            }
            exec("INSERT INTO git_mutations(task_id,invocation) VALUES(?,?)",{taskId,key});
            journal(taskId,"git_mutation",gitJournal(context,decision.tool,repo,
                {{"status","pending"},{"target",audit}}));
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
        const auto scrub = redactor(bot);
        result = scrub.record(std::move(result),SecretRedactor::Fields::external);
        exec("UPDATE git_mutations SET result=? WHERE task_id=? AND invocation=?",{result,taskId,key});
        journal(taskId,"git_mutation",gitJournal(context,decision.tool,repo,{{"target",audit},{"outcome",result}}));
        if (result.value("ok",false) && (decision.tool == "git_request_review" ||
                                        decision.tool == "git_submit_review" || decision.tool == "git_update_pr")) {
            const auto pr = args.at("prNumber").get<Id>();
            const auto url = "https://" + repo.resource() + "/pull/" + std::to_string(pr);
            if (decision.tool == "git_update_pr") {
                const auto sha = field(result.at("data"),"headSha");
                exec("INSERT OR IGNORE INTO git_pr_watches VALUES(?,?,?,?,?,?,?,?)",
                     {repo.host,repo.repo,pr,taskId,bot,runJson(context.run),url,sha});
            } else {
                const bool requested = decision.tool == "git_request_review";
                gitInboxItem(load(taskId),"review/" + actionId(),url,
                    requested ? "Pull request awaiting review" : "Pull request review submitted",
                    requested ? "Review requested on the Git host." : field(args,"verdict"));
            }
        }
        tx.commit();
        return result;
    }
    std::lock_guard lock(mutex);
    return {{"ok",true},{"data",redactor(bot).record(std::move(result),SecretRedactor::Fields::external)}};
}
}
