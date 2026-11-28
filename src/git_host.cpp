#include "conversation_turns.hpp"
#include "flock/bot_lifecycle.hpp"
#include "git_storage.hpp"
#include "conversation_storage.hpp"
#include "flock/git_host.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/decision.hpp"
#include "flock/task.hpp"
#include "flock/run_ref.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "run_ref_storage.hpp"
#include "flock/policy.hpp"
#include "policy_approvals.hpp"
#include "policy_origin.hpp"
#include "bot_lifecycle_storage.hpp"
#include "secret_storage.hpp"
#include "task_records.hpp"
#include "workspace_internal.hpp"

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
constexpr std::string_view slugChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
bool slug(std::string_view value) {
    return !value.empty() && value.size() <= 100 && value != "." && value != ".." && value.find_first_not_of(slugChars) == value.npos;
}

bool fullCommitSha(std::string_view sha) {
    return (sha.size() == 40 || sha.size() == 64) && sha.find_first_not_of("0123456789abcdefABCDEF") == sha.npos;
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

std::string_view validate(const Decision& decision) {
    const auto schema = gitToolSchema(decision.tool).at("parameters");
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
    if (tool == "git_commit_status" && !fullCommitSha(field(args, "sha"))) throw Conflict("Expected a full commit SHA");
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

Json GitStorage::gitTool(GitHost& host, std::string_view bot, std::string_view environment, std::string_view project,
                         const Decision& decision, std::string_view invocation, Id turn) {
    Id taskId = 0;
    const auto op = validate(decision);
    const auto& args = decision.args;
    const auto hostId = hostName(field(args, "host"));
    if (hostId != host.name()) throw Conflict("Git host is not connected");
    if (args.contains("taskId")) taskId = positiveId(args.at("taskId"));
    if (op != "read" && !taskId) throw Conflict("Select an owned task for the Git mutation");
    GitContext context{taskId, std::string(bot), std::string(environment), std::string(project), {}};
    {
        std::lock_guard lock(database.mutex);
        if (botLifecycle.botPaused(bot)) throw BotPaused{};
        if (taskId) {
            const auto task = taskRecords.load(taskId);
            if (task.botId != bot) throw Conflict("No task with this ID");
            if (op != "read" && (task.state == TaskState::cancelled || task.state == TaskState::cancelling))
                throw Conflict("The task is cancelled");
            context = taskGitContext(task);
        }
        secretStorage.sharedRedactor().requirePublic(args);
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
        return result;
    }
    const auto repo = GitRepository::parse(hostId, field(args, "repo"));
    if (!permits(repo, op)) throw Conflict("Repository access denied");
    const bool selectingPr = decision.tool == "git_open_pr" && args.contains("prNumber");
    if (!selectingPr) {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        auto origin = policyApprovals.policyOrigin(conversationTurns.loadTurnMessage(turn));
        if (taskId) {
            const auto task = taskRecords.load(taskId);
            origin.task = taskId;
            origin.revision = task.currentRevision.value_or(0);
            origin.generation = task.generation;
        }
        const auto action = *parseAction("repository." + std::string(op));
        const auto payload = Json::array({decision.tool, decision.args});
        const auto gate =
            policyApprovals.approveAction(origin, {action, repo.resource()}, std::string(invocation), payload, policyApprovals.now());
        transaction.commit();
        if (gate == ApprovalGate::waiting) return {{"policyWaiting", true}};
        if (gate != ApprovalGate::allow) return toolFailure(approvalError(gate));
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
                                invocation, args.value("newIntent", false),
                                selectingPr ? std::optional{args.at("prNumber").get<Id>()} : std::nullopt, decision.reason);
        result["ok"] = result.at("status") == "created";
        return result;
    }
    const bool submittedReview = decision.tool == "git_submit_review";
    const bool watchPr = decision.tool == "git_update_pr" || decision.tool == "git_request_review";
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
        database.writeJournal(taskId, "git_mutation",
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
    if (result.value("ok", false) && watchPr) {
        const auto pr = args.at("prNumber").get<Id>();
        const auto url = "https://" + repo.resource() + "/pull/" + std::to_string(pr);
        result["watchAttached"] = watchGitPr(taskRecords.load(taskId), runJson(context.run), eventCursor, repo, pr, url,
                                             field(result.at("data"), "headSha")) == GitWatchResult::attached;
    }
    database.exec("UPDATE git_mutations SET result=?1,dedupe=CASE WHEN json_extract(?1,'$.status')='failed' THEN NULL ELSE dedupe END"
                  " WHERE task_id=?2 AND invocation=?3",
                  {result, taskId, key});
    database.writeJournal(taskId, "git_mutation",
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
        return gitTool(*host, message.bot, field(message.routing, "environmentId"), field(message.routing, "projectId"), call, key,
                       message.id);
    } catch (const StorageError&) {
        throw;
    } catch (const std::exception& error) {
        return {{"ok", false}, {"error", secretStorage.sharedRedactor().text(error.what())}};
    }
}
}
