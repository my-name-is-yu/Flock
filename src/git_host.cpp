#include "conversation_turns.hpp"
#include "git_storage.hpp"
#include "storage.hpp"
#include "conversation_storage.hpp"
#include "flock/git_host.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
constexpr std::size_t gitResultTextLimit = 2000;
constexpr std::size_t gitReadResultLimit = 24576;
constexpr Id retainedGitReads = 64;
constexpr std::string_view slugChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
bool slug(std::string_view value) {
    return !value.empty() && value.size() <= 100 && value != "." && value != ".." && value.find_first_not_of(slugChars) == value.npos;
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string hostName(std::string_view host) {
    if (!slug(host) || host.find('_') != host.npos) throw Conflict("Invalid Git host");
    return lower(host);
}

Id positiveId(const Json& value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<Id>::max()) ||
        value.get<Id>() < 1)
        throw Conflict("Expected a positive Git identifier");
    return value.get<Id>();
}

std::string_view validate(const Decision& decision, bool conversation) {
    const auto schema = gitToolSchema(decision.tool, conversation).at("parameters");
    const auto& args = decision.args;
    const auto& properties = schema.at("properties");
    if (!args.is_object() || args.dump().size() > 20000) throw Conflict("Invalid Git tool arguments");
    for (const auto& key : schema.at("required"))
        if (key != "reason" && !args.contains(key.get<std::string>()))
            throw Conflict("Missing Git tool argument: " + key.get<std::string>());
    for (const auto& [key, value] : args.items()) {
        if (key == "reason" || !properties.contains(key)) throw Conflict("Unknown Git tool argument");
        const auto& property = properties.at(key);
        const auto type = property.at("type");
        if (type == "integer") {
            const auto id = positiveId(value);
            if (property.contains("maximum") && id > property.at("maximum").get<Id>()) throw Conflict("Git tool limit exceeded");
        } else if (type == "array") {
            if (!value.is_array() || value.size() > 30 || !std::ranges::all_of(value, [](const Json& item) {
                    return item.is_string() && !item.get_ref<const std::string&>().empty() &&
                           item.get_ref<const std::string&>().size() <= 100;
                }))
                throw Conflict("Invalid Git tool list");
        } else if (type == "boolean") {
            if (!value.is_boolean()) throw Conflict("Expected a boolean Git argument");
        } else {
            if (!value.is_string()) throw Conflict("Expected a text Git argument");
            const auto& text = value.get_ref<const std::string&>();
            if (text.find('\0') != text.npos || text.size() > (key == "body" ? 12000 : 500)) throw Conflict("Invalid Git text argument");
            if (property.contains("enum") && std::ranges::find(property.at("enum"), value) == property.at("enum").end())
                throw Conflict("Invalid Git action value");
        }
    }
    const auto requireText = [&](const char* key) {
        if (field(args, key).find_first_not_of(" \r\n\t") == std::string::npos) throw Conflict(std::string(key) + " is required");
    };
    const auto op = gitOperation(decision);
    if (op.empty()) throw Conflict("Unknown Git operation");
    if (op != "read" && !validDecisionReason(decision.reason)) throw Conflict("Git changes require a brief reason");
    const auto& tool = decision.tool;
    if (tool == "git_open_pr" || args.contains("title")) requireText("title");
    if (tool == "git_comment" || (tool == "git_submit_review" && args.at("verdict") != "approve")) requireText("body");
    if (tool == "git_request_review" && args.at("reviewers").empty()) throw Conflict("Choose at least one reviewer");
    if (tool == "git_update_pr" && !args.contains("title") && !args.contains("body") && !args.contains("baseRef") &&
        !args.contains("state"))
        throw Conflict("Choose a PR field to update");
    if (tool == "git_commit_status") {
        const auto sha = field(args, "sha");
        if ((sha.size() != 40 && sha.size() != 64) || sha.find_first_not_of("0123456789abcdefABCDEF") != sha.npos)
            throw Conflict("Expected a full commit SHA");
    }
    if (tool != "git_manage_issue") return op;
    const auto action = field(args, "action");
    const bool hasNumber = action == "read" || action == "list_comments" || action == "update";
    if (hasNumber != args.contains("issueNumber") ||
        (op == "read" && (args.contains("title") || args.contains("body") || args.contains("labels") || args.contains("assignees"))) ||
        (args.contains("state") && action != "list" && (action != "update" || args.at("state") == "all")) ||
        (args.contains("page") && action != "list" && action != "list_comments"))
        throw Conflict("Issue arguments do not match the requested action");
    if (action == "create") requireText("title");
    if (action == "update" && !args.contains("title") && !args.contains("body") && !args.contains("state") && !args.contains("labels") &&
        !args.contains("assignees"))
        throw Conflict("Choose an issue field to update");
    return op;
}

std::string branch(const Json& args, const char* key) {
    const auto ref = field(args, key);
    if (ref.empty() || ref.find_first_of(" \r\n\t~^:?*[\\") != ref.npos || ref.find("..") != ref.npos || ref.starts_with('/') ||
        ref.ends_with('/') || ref.ends_with('.') || ref.find("//") != ref.npos || ref.starts_with('-') || ref.starts_with("refs/") ||
        ref.find("@{") != ref.npos || ref == "@" || std::ranges::any_of(ref, [](unsigned char c) { return c < 32 || c == 127; }))
        throw Conflict("Expected an exact branch name");
    auto rest = std::string_view(ref);
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto component = rest.substr(0, slash);
        if (component.starts_with('.') || component.ends_with(".lock")) throw Conflict("Expected an exact branch name");
        if (slash == rest.npos) break;
        rest.remove_prefix(slash + 1);
    }
    return ref;
}

bool repositoryAccess(const Policy& policy, GitHost& host, const GitContext& context, const GitRepository& repo, std::string_view op,
                      const Decision* storedRead = nullptr) {
    const auto action = parseAction("repository." + std::string(op));
    if (!action || policy.forbidden(context.botId, *action, repo.resource())) return false;
    const bool upstream = storedRead ? host.permitsStoredRead(context, repo, *storedRead) : host.permits(context, repo, op);
    return upstream && !policy.forbidden(context.botId, *action, repo.resource());
}

GitContext taskGitContext(const TaskData& task) {
    const auto& run = task.activeRun ? task.activeRun : task.lastRun;
    return {task.id, task.botId, task.environmentId, task.projectId, run ? run->ref : RunRef{}};
}

Json readTarget(const Decision& decision, std::string_view host, std::string_view repo = {}) {
    auto target = decision.args;
    target["host"] = host;
    if (!repo.empty()) target["repo"] = repo;
    if (target.contains("sha")) target["sha"] = lower(field(target, "sha"));
    if (decision.tool == "git_find_repos") target["query"] = lower(field(target, "query"));
    if (decision.tool == "git_read_ci_logs") target["maxBytes"] = target.value("maxBytes", Id{8192});
    if (decision.tool == "git_manage_issue") {
        if (target.at("action") == "list") target["state"] = target.value("state", "open");
        if (target.at("action") != "read") target["page"] = target.value("page", Id{1});
    }
    return target;
}

Json boundedReadResult(std::string_view tool, const Json& result) {
    const auto encoded = result.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (encoded.size() <= gitReadResultLimit) return Json::parse(encoded);
    if (tool == "git_read_ci_logs")
        return {{"ok", true}, {"data", {{"text", bounded(field(result.at("data"), "text"), 4000)}, {"truncated", true}}}};
    if (tool == "git_manage_issue" && result.contains("data")) {
        const auto& source = result.at("data");
        const auto items = source.find("items");
        const auto next = source.find("nextPage");
        if (items != source.end() && items->is_array() && items->size() <= 30 && next != source.end() &&
            (next->is_number_integer() || next->is_null())) {
            auto page = Json::array();
            for (const auto& item : *items) {
                const auto number = item.find("number"), id = item.find("id");
                if (number != item.end() && number->is_number_integer())
                    page.push_back({{"number", *number}, {"title", bounded(field(item, "title"), 96)}});
                else if (id != item.end() && id->is_number_integer()) {
                    auto author = bounded(field(item, "author"), 64);
                    const auto bodyLimit = std::min<std::size_t>(96, 120 - author.size());
                    page.push_back({{"id", *id}, {"author", std::move(author)}, {"body", bounded(field(item, "body"), bodyLimit)}});
                }
            }
            if (page.size() == items->size())
                return {{"ok", true}, {"truncated", true}, {"data", {{"items", std::move(page)}, {"nextPage", *next}}}};
        }
    }
    return {{"ok", result.at("ok")}, {"truncated", true}, {"data", {{"excerpt", bounded(encoded, 4096)}}}};
}

}

Json GitStorage::gitJournal(const GitContext& context, std::string_view tool, const GitRepository& repo, Json result) {
    return {{"tool", tool},
            {"host", repo.host},
            {"repo", repo.repo},
            {"run", runJson(context.run)},
            {"environmentId", context.environmentId},
            {"result", std::move(result)}};
}

GitRepository GitRepository::parse(std::string_view host, std::string_view repo) {
    const auto slash = repo.find('/');
    if (slash == repo.npos || !slug(repo.substr(0, slash)) || !slug(repo.substr(slash + 1)))
        throw Conflict("Expected a Git repository in owner/repo form");
    return {hostName(host), lower(repo)};
}

Json GitStorage::saveTaskGitRead(Id task, std::string_view invocation, std::string_view tool, const Json& target, const Json& result) {
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    const auto scrub = secretStorage.sharedRedactor();
    scrub.requirePublic(target);
    database.exec("INSERT INTO git_task_reads(task_id,invocation,tool,target,result) VALUES(?,?,?,?,?)"
                  " ON CONFLICT(task_id,invocation) DO NOTHING",
                  {task, invocation, tool, target, boundedReadResult(tool, scrub.payload(result))});
    auto row = database.query("SELECT id FROM git_task_reads WHERE task_id=? AND invocation=?", {task, invocation});
    if (!row.next()) throw Conflict("The saved Git read is missing");
    const auto id = row.number(0);
    database.exec("DELETE FROM git_task_reads WHERE task_id=? AND id < (SELECT id FROM git_task_reads WHERE task_id=?"
                  " ORDER BY id DESC LIMIT 1 OFFSET ?)",
                  {task, task, retainedGitReads - 1});
    tx.commit();
    return {{"readId", id}};
}

void GitStorage::projectTaskGitReads(GitHost& host, const TaskData& task, Json& event) {
    const auto context = taskGitContext(task);
    auto refs = Json::array();
    std::map<Id, std::pair<Decision, Json>> requested;
    {
        std::lock_guard lock(database.mutex);
        auto rows = database.query("SELECT id FROM git_task_reads WHERE task_id=? ORDER BY id", {context.taskId});
        while (rows.next()) refs.push_back(rows.number(0));
        if (event.contains("gitResults")) {
            for (const auto& entry : event.at("gitResults")) {
                if (!entry.contains("readId")) continue;
                const auto id = entry.at("readId").get<Id>();
                if (requested.contains(id)) continue;
                auto read = database.query("SELECT tool,target,result FROM git_task_reads WHERE task_id=? AND id=?", {context.taskId, id});
                if (read.next()) requested.emplace(id, std::pair{Decision{read.string(0), read.json(1), {}}, read.json(2)});
            }
        }
    }
    if (!refs.empty()) event["gitReadRefs"] = std::move(refs);
    if (!event.contains("gitResults")) return;

    const auto scrub = secretStorage.sharedRedactor();
    const Policy policy = permissions;
    std::set<Id> authorized;
    for (const auto& [id, read] : requested) {
        const auto& [source, result] = read;
        if (field(source.args, "host") != host.name()) continue;
        try {
            if (source.tool == "git_find_repos") {
                if (!std::ranges::all_of(result.at("repos"), [&](const Json& repo) {
                        return repositoryAccess(policy, host, context, {std::string(host.name()), repo.get<std::string>()}, "read",
                                                &source);
                    }))
                    continue;
            } else {
                const auto repo = GitRepository::parse(host.name(), field(source.args, "repo"));
                if (!repositoryAccess(policy, host, context, repo, "read", &source)) continue;
            }
            authorized.insert(id);
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception&) {
            continue;
        }
    }
    std::lock_guard lock(database.mutex);
    Transaction projection(database.db, Transaction::Mode::read);
    const auto localReadAllowed = [&](std::string_view repo) {
        return policy.allows(context.botId, Action::repository_read, GitRepository::parse(host.name(), repo).resource());
    };
    for (auto& entry : event.at("gitResults")) {
        if (!entry.contains("readId")) continue;
        const auto id = entry.at("readId").get<Id>();
        if (authorized.contains(id)) {
            const auto& [source, result] = requested.at(id);
            const bool localAllowed =
                source.tool == "git_find_repos"
                    ? std::ranges::all_of(result.at("repos"),
                                          [&](const Json& repo) { return localReadAllowed(repo.get_ref<const std::string&>()); })
                    : [&] {
                          const auto target = GitRepository::parse(host.name(), field(source.args, "repo")).resource();
                          if (policy.forbidden(context.botId, Action::repository_read, target)) return false;
                          return localReadAllowed(field(source.args, "repo")) || database
                                                                                     .query(R"SQL(
SELECT 1 FROM git_task_reads r JOIN policy_approvals a
 ON a.operation='git/' || r.task_id || '/' || r.invocation
WHERE r.task_id=? AND r.id=? AND a.task=r.task_id AND a.bot=?
 AND a.action='repository.read' AND a.target=? AND a.status='used'
)SQL",
                                                                                            {context.taskId, id, context.botId, target})
                                                                                     .next();
                      }();
            if (localAllowed) {
                entry.update({{"sourceTool", source.tool}, {"target", scrub.payload(source.args)}, {"result", scrub.payload(result)}});
                continue;
            }
        }
        entry["result"] = {{"ok", false}, {"error", "Saved Git read is unavailable or access was denied"}};
    }
    projection.commit();
}

class GitStorage::GitPrMutationClaim {
public:
    GitPrMutationClaim(GitStorage& store, std::multiset<GitPrKey>& pending) : store_(store), pending_(pending) {}
    GitPrMutationClaim(const GitPrMutationClaim&) = delete;
    GitPrMutationClaim& operator=(const GitPrMutationClaim&) = delete;
    ~GitPrMutationClaim() { release(); }

    void acquire(const GitRepository& repo, Id pr) { claim_ = pending_.emplace(repo.host, repo.repo, pr); }

    void release() {
        if (!claim_) return;
        std::lock_guard lock(store_.database.mutex);
        pending_.erase(*claim_);
        claim_.reset();
        store_.gitWebhooksDrainedAt.reset();
    }
private:
    GitStorage& store_;
    std::multiset<GitPrKey>& pending_;
    std::optional<std::multiset<GitPrKey>::iterator> claim_;
};

Json GitStorage::gitTool(GitHost& host, std::string_view bot, Id taskId, std::string_view environment, std::string_view project,
                         const Decision& decision, std::string_view invocation, std::optional<Id> generation, Id turn) {
    if (decision.tool == "git_read_result" && !generation) throw Conflict("Saved Git reads are available within their task");
    const auto op = validate(decision, !generation.has_value());
    const auto& args = decision.args;
    const auto hostId = hostName(field(args, "host"));
    if (hostId != host.name()) throw Conflict("Git host is not connected");
    if (!taskId && args.contains("taskId")) taskId = positiveId(args.at("taskId"));
    if (op != "read" && !taskId) throw Conflict("Select an owned task for the Git mutation");
    GitContext context{taskId, std::string(bot), std::string(environment), std::string(project), {}};
    {
        std::lock_guard lock(database.mutex);
        if (botLifecycle.botPaused(bot)) throw BotPaused{};
        if (taskId) {
            const auto task = taskRecords.load(taskId);
            if (task.botId != bot) throw Conflict("No task with this ID");
            if (generation && task.generation != *generation) throw RequestChanged{};
            if (op != "read" && (task.state == TaskState::cancelled || task.state == TaskState::cancelling))
                throw Conflict("The task is cancelled");
            context = taskGitContext(task);
        }
        secretStorage.sharedRedactor().requirePublic(args);
        if (decision.tool == "git_read_result") return {{"readId", args.at("readId")}};
        if (generation && op == "read") {
            auto previous = database.query("SELECT id FROM git_task_reads WHERE task_id=? AND invocation=?", {taskId, invocation});
            if (previous.next()) return {{"readId", previous.number(0)}};
        }
    }
    const Policy policy = permissions;
    const auto permits = [&](const GitRepository& repository, std::string_view required) {
        return repositoryAccess(policy, host, context, repository, required);
    };
    if (decision.tool == "git_find_repos") {
        const auto page = args.value("page", Id{1});
        const auto [candidates, more] = host.repositories(context, lower(field(args, "query")), page);
        Json repos = Json::array();
        bool truncated = more;
        for (const auto& candidate : candidates) {
            if (!permits(candidate, op) || !policy.allows(bot, Action::repository_read, candidate.resource())) continue;
            if (repos.size() == 100) {
                truncated = true;
                break;
            }
            repos.push_back(candidate.repo);
        }
        const auto result = secretStorage.sharedRedactor().record(
            {{"ok", true},
             {"host", hostId},
             {"repos", repos},
             {"truncated", truncated},
             {"nextPage", more && page < std::numeric_limits<Id>::max() ? Json(page + 1) : Json(nullptr)}},
            SecretRedactor::Fields::external);
        if (generation) return saveTaskGitRead(taskId, invocation, decision.tool, readTarget(decision, hostId), result);
        return result;
    }
    const auto repo = GitRepository::parse(hostId, field(args, "repo"));
    if (!permits(repo, op)) throw Conflict("Repository access denied");
    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        PolicyOrigin origin;
        if (turn) origin = policyApprovals.policyOrigin(conversationTurns.loadTurnMessage(turn));
        if (taskId) {
            const auto task = taskRecords.load(taskId);
            if (generation && task.generation != *generation) throw RequestChanged{};
            if (!turn)
                origin = policyApprovals.taskPolicyOrigin(task);
            else {
                origin.task = taskId;
                origin.revision = task.currentRevision.value_or(0);
                origin.generation = task.generation;
            }
        }
        if (turn || origin.task) {
            const auto action = *parseAction("repository." + std::string(op));
            const auto key = turn ? std::string(invocation) : "git/" + std::to_string(taskId) + "/" + std::string(invocation);
            const auto payload = Json::array({decision.tool, decision.args});
            const auto gate = policyApprovals.approveAction(origin, {action, repo.resource()}, key, payload, policyApprovals.now());
            if (gate == ApprovalGate::waiting && !turn) {
                Json resume{{"tool", decision.tool}, {"args", decision.args}, {"invocation", invocation}};
                if (op != "read") resume["reason"] = secretStorage.sharedRedactor().text(decision.reason);
                database.exec("UPDATE policy_approvals SET resume=? WHERE operation=?", {resume, key});
            }
            transaction.commit();
            if (gate == ApprovalGate::waiting) return {{"policyWaiting", true}};
            if (gate != ApprovalGate::allow) return toolFailure(approvalError(gate));
        } else
            transaction.commit();
    }
    if (decision.tool == "git_open_pr") {
        const auto head = args.contains("headRepo") ? GitRepository::parse(hostId, field(args, "headRepo")) : repo;
        if (head != repo && (!permits(head, "read") || !policy.allows(bot, Action::repository_read, head.resource())))
            throw Conflict("Head repository read access denied");
        auto result = openGitPr(host, context,
                                {{repo, head, branch(args, "baseRef"), branch(args, "headRef")},
                                 {},
                                 field(args, "title"),
                                 field(args, "body"),
                                 args.value("draft", false)},
                                invocation, args.value("newIntent", false), decision.reason);
        result["ok"] = result.at("status") == "created";
        return result;
    }
    const bool requestedReview = decision.tool == "git_request_review";
    const bool submittedReview = decision.tool == "git_submit_review";
    const bool watchPr = decision.tool == "git_update_pr" || requestedReview;
    if (watchPr && args.contains("baseRef")) branch(args, "baseRef");
    if (op == "read") {
        Json result;
        try {
            if (decision.tool == "git_read_ci_logs") {
                const auto maxBytes = args.value("maxBytes", std::size_t{8192});
                const auto log = host.readCiLogs(context, repo, args.at("jobId"), maxBytes);
                const auto sanitized = secretStorage.sharedRedactor().text(log.text, log.truncated);
                const auto utf8 = Json::parse(Json(sanitized).dump(-1, ' ', false, Json::error_handler_t::replace)).get<std::string>();
                const auto excerpt = bounded(utf8, maxBytes);
                result = {{"ok", true}, {"data", {{"text", excerpt}, {"truncated", log.truncated || excerpt.size() < utf8.size()}}}};
            } else {
                const auto data = host.call(context, repo, decision);
                result = {{"ok", true}, {"data", secretStorage.sharedRedactor().payload(data, false, gitResultTextLimit)}};
            }
        } catch (const StorageError&) {
            throw;
        } catch (const std::exception& error) {
            result = {{"ok", false}, {"error", bounded(secretStorage.sharedRedactor().text(error.what()), gitResultTextLimit)}};
        }
        if (generation && result.at("ok") == true)
            return saveTaskGitRead(taskId, invocation, decision.tool, readTarget(decision, repo.host, repo.repo), result);
        return result;
    }

    Json result;
    auto target = args;
    target.erase("taskId");
    target["host"] = repo.host;
    target["repo"] = repo.repo;
    auto audit = target;
    audit.erase("body");
    const bool createComment = decision.tool == "git_comment" && !target.contains("commentId");
    const bool createPrComment = createComment && target.at("target") == "pr";
    const auto key = Json::array({invocation, decision.tool, target}).dump();
    const Json dedupe = createComment ? Json(target.dump()) : Json(nullptr);
    const auto reviewKey = requestedReview ? "review/" + actionId() : std::string{};
    Id eventCursor = 0;
    GitPrMutationClaim watchMutation(*this, gitPrMutationsInFlight);
    GitPrMutationClaim feedbackMutation(*this, gitFeedbackMutationsInFlight);
    {
        std::lock_guard lock(database.mutex);
        Transaction tx(database.db);
        if (botLifecycle.botPaused(bot)) throw BotPaused{};
        auto previous = database.query("SELECT result FROM git_mutations WHERE task_id=? AND (invocation=? OR dedupe=?)"
                                       " ORDER BY invocation=? DESC LIMIT 1",
                                       {taskId, key, dedupe, key});
        if (previous.next()) {
            if (const auto stored = previous.string(0); !stored.empty()) return Json::parse(stored);
            return {{"ok", false}, {"status", "unknown"}, {"error", "The host outcome is unknown. Check the target before retrying."}};
        }
        if (watchPr) {
            const auto pr = args.at("prNumber").get<Id>();
            if (gitPrWatchUnavailable(repo, pr, taskId)) throw Conflict("This pull request belongs to another task");
            watchMutation.acquire(repo, pr);
            eventCursor = latestGitWebhookId();
        }
        if (submittedReview || createPrComment) feedbackMutation.acquire(repo, args.at(submittedReview ? "prNumber" : "number"));
        if (policy.forbidden(bot, *parseAction("repository." + std::string(op)), repo.resource()))
            throw Conflict("Repository access denied");
        database.exec("INSERT INTO git_mutations(task_id,invocation,dedupe) VALUES(?,?,?)", {taskId, key, dedupe});
        if (requestedReview) {
            // A webhook can be delivered before the Git host returns the result.
            database.exec(R"SQL(
INSERT INTO git_review_receipts
SELECT ?1,?2,?3,?4,?5,lower(value),?6 FROM json_each(?7) WHERE true
ON CONFLICT(inbox_key,reviewer) DO NOTHING
)SQL",
                          {reviewKey, taskId, repo.host, repo.repo, args.at("prNumber"), eventCursor, args.at("reviewers").dump()});
        }
        journalStorage.journal(taskId, "git_mutation",
                               secretStorage.sharedRedactor().record(gitJournal(
                                   context, decision.tool, repo, {{"status", "pending"}, {"target", audit}, {"reason", decision.reason}})));
        tx.commit();
    }
    try {
        result = {{"ok", true}, {"data", host.call(context, repo, decision)}};
    } catch (const GitRejected& error) {
        result = {{"ok", false}, {"status", "failed"}, {"error", error.what()}};
    } catch (const std::exception&) {
        result = {{"ok", false}, {"status", "unknown"}, {"error", "The host outcome is unknown. Check the target before retrying."}};
    }
    std::lock_guard lock(database.mutex);
    Transaction tx(database.db);
    watchMutation.release();
    feedbackMutation.release();
    const auto scrub = secretStorage.sharedRedactor();
    if (result.value("ok", false))
        result["data"] = scrub.payload(result.at("data"), false, gitResultTextLimit);
    else
        result["error"] = scrub.text(field(result, "error"));
    if (requestedReview && result.value("status", "") == "failed")
        database.exec("DELETE FROM git_review_receipts WHERE inbox_key=?", {reviewKey});
    if (result.value("ok", false) && (watchPr || submittedReview)) {
        const auto pr = args.at("prNumber").get<Id>();
        const auto url = "https://" + repo.resource() + "/pull/" + std::to_string(pr);
        if (watchPr) {
            const auto sha = field(result.at("data"), "headSha");
            result["watchAttached"] = watchGitPr(taskId, bot, runJson(context.run), eventCursor,
                                                 {{repo, repo, {}, {}}, pr, url, {}, sha, false}, GitWatchStart::mutationAdmission);
        }
        if (requestedReview && result.at("watchAttached") == true)
            gitInboxItem(taskRecords.load(taskId), reviewKey, url, "Pull request awaiting review", "Review requested on the Git host.");
        else if (submittedReview)
            gitInboxItem(taskRecords.load(taskId), "review/" + actionId(), url, "Pull request review submitted", field(args, "verdict"));
    }
    database.exec("UPDATE git_mutations SET result=?1,dedupe=CASE WHEN json_extract(?1,'$.status')='failed' THEN NULL ELSE dedupe END"
                  " WHERE task_id=?2 AND invocation=?3",
                  {result, taskId, key});
    journalStorage.journal(taskId, "git_mutation",
                           scrub.record(gitJournal(context, decision.tool, repo, {{"target", audit}, {"outcome", result}})));
    tx.commit();
    return result;
}

bool GitStorage::gitEffectStarted(std::string_view invocation, Id taskId) const {
    return database
        .query(R"SQL(
SELECT 1 FROM git_pr_operations WHERE (?1=0 OR task_id=?1) AND invocation IN (?2,?2 || '/new')
UNION ALL SELECT 1 FROM git_mutations WHERE (?1=0 OR task_id=?1) AND json_extract(invocation,'$[0]')=?2
LIMIT 1
)SQL",
               {taskId, invocation})
        .next();
}

Json GitStorage::gitConversationTool(GitHost* host, const conversation::Message& message, const Decision& decision, std::string_view key) {
    try {
        if (!host || !message.channelScope.empty()) throw Conflict("Git tools are available in Flock Web with a connected Git host");
        auto call = decision;
        if (message.routing.contains("gitEvent")) {
            const auto& webhook = message.routing.at("gitEvent");
            const auto owner = webhook.at("taskId").get<Id>();
            if ((call.args.contains("taskId") && call.args.at("taskId") != owner) || field(call.args, "host") != field(webhook, "host") ||
                field(call.args, "repo") != field(webhook, "repo") || !gitNotificationToolAllowed(call, webhook))
                throw Conflict("Git notification tools must use the watched target");
            call.args["taskId"] = owner;
        }
        return gitTool(*host, message.bot, 0, field(message.routing, "environmentId"), field(message.routing, "projectId"), call, key, {},
                       message.id);
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        return {{"ok", false}, {"error", secretStorage.sharedRedactor().text(error.what())}};
    }
}
}
