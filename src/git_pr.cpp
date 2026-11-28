#include "git_storage.hpp"
#include "flock/bot_lifecycle.hpp"
#include "flock/git_host.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "run_ref_storage.hpp"
#include "flock/policy.hpp"
#include "bot_lifecycle_storage.hpp"
#include "secret_storage.hpp"
#include "task_records.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
Json prResult(std::string_view operationId, std::string_view status, Id number = 0, std::string_view url = {}) {
    Json result{{"operationId", operationId}, {"status", status}};
    if (number) result.update({{"prNumber", number}, {"url", url}});
    return result;
}

bool verifiedPr(const GitPullRequest& pr, const GitOpenPr& request) {
    return pr.target == request.target && pr.operationId == request.operationId && pr.number > 0 &&
           pr.url == "https://" + pr.target.base.resource() + "/pull/" + std::to_string(pr.number);
}

bool terminalPrOperation(std::string_view status) {
    return status == "created" || status == "failed";
}
}

bool GitStorage::gitPrWatchUnavailable(const GitRepository& repo, Id pr, Id task) const {
    return gitPrMutationsInFlight.contains({repo.host, repo.repo, pr}) ||
           database
               .query("SELECT 1 FROM git_pr_watches WHERE (host,repo,pr_number)=(?,?,?) AND task_id<>?", {repo.host, repo.repo, pr, task})
               .next();
}

GitStorage::GitWatchResult GitStorage::watchGitPr(const TaskData& task, const Json& run, Id deliveryCursor, const GitRepository& repo,
                                                  Id pr, std::string_view url, std::string_view headSha) {
    using enum GitWatchResult;
    if (gitPrWatchUnavailable(repo, pr, task.id)) return unavailable;
    if (terminal(task) || task.state == TaskState::cancelling) return taskEnded;
    const auto priorObservation = database
                                      .query(R"SQL(
SELECT 1 FROM git_pr_watches WHERE (host,repo,pr_number)=(?1,?2,?3)
UNION ALL
SELECT 1 FROM git_pr_heads WHERE (host,repo,pr_number)=(?1,?2,?3) AND event_cursor<=?4
LIMIT 1
)SQL",
                                             {repo.host, repo.repo, pr, deliveryCursor})
                                      .next();
    std::optional<std::string> priorHead;
    {
        auto observedHead = database.query(R"SQL(
SELECT event_cursor,head_sha FROM git_pr_heads
WHERE (host,repo,pr_number)=(?1,?2,?3) AND event_cursor<=?4
ORDER BY event_cursor DESC LIMIT 1
)SQL",
                                           {repo.host, repo.repo, pr, deliveryCursor});
        if (observedHead.next() && observedHead.number(0) < deliveryCursor) priorHead = observedHead.string(1);
    }
    if (priorHead) {
        database.exec(R"SQL(
INSERT INTO git_pr_heads VALUES(?,?,?,?,?,0) ON CONFLICT(host,repo,pr_number,event_cursor)
 DO UPDATE SET head_sha=excluded.head_sha WHERE synchronized=0
)SQL",
                      {repo.host, repo.repo, pr, deliveryCursor, *priorHead});
    }
    const auto headObservedAt = priorObservation ? latestGitWebhookId() : deliveryCursor;
    database.exec(R"SQL(
INSERT INTO git_pr_watches VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(host,repo,pr_number)
 DO UPDATE SET url=excluded.url
)SQL",
                  {repo.host, repo.repo, pr, task.id, task.botId, run, url, deliveryCursor});
    if (!headSha.empty()) {
        database.exec(R"SQL(
INSERT INTO git_pr_heads VALUES(?,?,?,?,?,0) ON CONFLICT(host,repo,pr_number,event_cursor)
 DO UPDATE SET head_sha=excluded.head_sha WHERE synchronized=0
)SQL",
                      {repo.host, repo.repo, pr, headObservedAt, headSha});
    }
    gitWebhooksDrainedAt.reset();
    return attached;
}

Json GitStorage::openGitPr(GitHost& host, const GitContext& context, const GitOpenPr& input, std::string_view invocation, bool newIntent,
                           std::optional<Id> prNumber, std::string_view reason) {
    auto request = input;
    const auto& [base, head, baseRef, headRef] = request.target;
    const auto intent = std::string(invocation) + (newIntent ? "/new" : "");
    bool create;
    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (botLifecycle.botPaused(context.botId)) throw BotPaused{};
        auto operation = database.query(R"SQL(
SELECT operation_id,task_id,bot_id,status,pr_number,url FROM git_pr_operations
WHERE host=?1 AND base_repo=?2 AND base_ref=?3 AND head_repo=?4 AND head_ref=?5
AND (status IN ('pending','ambiguous') OR (task_id=?6 AND (invocation=?7 OR (status='created' AND NOT ?8))))
ORDER BY (task_id=?6 AND invocation=?7) DESC,status IN ('pending','ambiguous') DESC,rowid DESC LIMIT 1
)SQL",
                                        {base.host, base.repo, baseRef, head.repo, headRef, context.taskId, intent, int{newIntent}});
        create = !operation.next();
        if (create) {
            if (prNumber) throw Conflict("The PR is not a verified candidate for this operation");
            if (permissions.forbidden(context.botId, Action::repository_pr, base.resource())) throw Conflict("Repository access denied");
            request.operationId = actionId();
            database.exec(
                "INSERT INTO git_pr_operations(operation_id,host,base_repo,base_ref,head_repo,head_ref,task_id,bot_id,run,invocation,event_cursor,status) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,"
                "(SELECT coalesce(max(id),0) FROM git_webhook_events),'pending')",
                {request.operationId, base.host, base.repo, baseRef, head.repo, headRef, context.taskId, context.botId,
                 runJson(context.run), intent});
            auto pending = prResult(request.operationId, "pending");
            pending["reason"] = reason;
            database.writeJournal(context.taskId, JournalKind::git_mutation,
                                  secretStorage.sharedRedactor().record(gitJournal(context, "git_open_pr", base, pending)));
        } else {
            if (operation.number(1) != context.taskId || operation.string(2) != context.botId)
                throw Conflict("This PR target belongs to another task");
            request.operationId = operation.string(0);
            if (!prNumber && terminalPrOperation(operation.string(3)))
                return prResult(request.operationId, operation.string(3), operation.number(4), operation.string(5));
        }
        transaction.commit();
    }

    std::map<Id, GitPullRequest> matches;
    bool rejected = false;
    try {
        if (create) {
            auto pr = host.createPullRequest(context, request);
            if (verifiedPr(pr, request)) matches.emplace(pr.number, std::move(pr));
        } else if (!prNumber) {
            for (auto& pr : host.findPullRequests(context, request.target))
                if (verifiedPr(pr, request)) matches.emplace(pr.number, std::move(pr));
        }
    } catch (const GitRejected&) {
        rejected = create;
    } catch (const std::exception&) {
    }

    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    auto saved = database.query("SELECT status,pr_number,url,run,event_cursor,candidates FROM git_pr_operations WHERE operation_id=?",
                                {request.operationId});
    if (!saved.next()) throw Conflict("PR operation is missing");
    const auto previous = saved.string(0);
    const auto offered = prNumber ? saved.json(5) : Json::array();
    const auto selected =
        prNumber ? std::ranges::find_if(offered, [&](const Json& pr) { return pr.at("prNumber") == *prNumber; }) : offered.end();
    if (prNumber) {
        if (permissions.forbidden(context.botId, Action::repository_pr, base.resource()) ||
            (head != base && !permissions.allows(context.botId, Action::repository_read, head.resource())))
            throw Conflict("Repository access denied");
        if (selected == offered.end() || (terminalPrOperation(previous) && *prNumber != saved.number(1)))
            throw Conflict("The PR is not a verified candidate for this operation");
    }
    if (terminalPrOperation(previous)) return prResult(request.operationId, previous, saved.number(1), saved.string(2));

    const auto run = saved.json(3);
    const auto cursor = saved.number(4);
    const auto task = taskRecords.load(context.taskId);
    const auto scrub = secretStorage.sharedRedactor();
    std::string_view state = rejected ? "failed" : "ambiguous";
    Id number = 0;
    std::string url;
    auto candidates = Json::array();
    if (selected != offered.end()) {
        number = selected->at("prNumber").get<Id>();
        url = field(*selected, "url");
        if (watchGitPr(task, run, cursor, base, number, url, field(*selected, "headSha")) == GitWatchResult::unavailable)
            throw Conflict("The PR already belongs to another task");
        state = "created";
        candidates.push_back(*selected);
    } else if (matches.size() == 1) {
        const auto& pr = matches.begin()->second;
        if (watchGitPr(task, run, cursor, base, pr.number, pr.url, pr.headSha) != GitWatchResult::unavailable) {
            state = "created";
            number = pr.number;
            url = pr.url;
        }
    }

    auto result = prResult(request.operationId, state, number, url);
    if (state == "ambiguous") {
        for (const auto& [id, pr] : matches) {
            if (gitPrWatchUnavailable(base, id, context.taskId)) continue;
            candidates.push_back({{"prNumber", id}, {"url", pr.url}, {"headSha", pr.headSha}});
        }
        result["candidates"] = candidates;
    }
    database.exec("UPDATE git_pr_operations SET status=?,pr_number=?,url=?,candidates=? WHERE operation_id=?",
                  {state, number ? Json(number) : Json(nullptr), number ? Json(url) : Json(nullptr), candidates, request.operationId});
    if (state != previous) {
        auto entry = gitJournal(context, "git_open_pr", base, result);
        entry["run"] = run;
        entry["recovered"] = state == "created" && !create;
        database.writeJournal(context.taskId, JournalKind::git_mutation, scrub.record(std::move(entry)));
    }
    transaction.commit();
    return scrub.record(result, SecretRedactor::Fields::external);
}

Json GitStorage::associateGitPr(GitHost& host, const GitContext& context, const GitPrTarget& target, Id number, std::string_view reason) {
    if (context.run.threadId.empty()) throw Conflict("Select the task whose T3 Session created the PR");
    const auto& base = target.base;
    const auto& head = target.head;
    const auto url = "https://" + base.resource() + "/pull/" + std::to_string(number);
    Id cursor;
    {
        std::lock_guard lock(database.mutex);
        Transaction transaction(database.db);
        if (botLifecycle.botPaused(context.botId)) throw BotPaused{};
        if (permissions.forbidden(context.botId, Action::repository_pr, base.resource()) ||
            !permissions.allows(context.botId, Action::repository_read, base.resource()) ||
            (head != base && !permissions.allows(context.botId, Action::repository_read, head.resource())))
            throw Conflict("Repository access denied");
        const auto task = taskRecords.load(context.taskId);
        const auto& run = task.activeRun ? task.activeRun : task.lastRun;
        if (task.botId != context.botId || !run || run->ref != context.run || run->assignment.revisionId != task.currentRevision)
            throw Conflict("The task's Session changed before the PR could be associated");
        if (terminal(task) || task.state == TaskState::cancelling) throw Conflict("The task already ended");
        if (gitPrWatchUnavailable(base, number, task.id)) throw Conflict("The PR already belongs to another task");
        auto watch =
            database.query("SELECT task_id,url FROM git_pr_watches WHERE (host,repo,pr_number)=(?,?,?)", {base.host, base.repo, number});
        if (watch.next()) {
            if (watch.number(0) != task.id || watch.string(1) != url) throw Conflict("The PR already belongs to another task");
            transaction.commit();
            return {{"ok", true}, {"status", "associated"}, {"prNumber", number}, {"url", url}, {"reused", true}};
        }
        auto operation = database.query(R"SQL(
SELECT 1 FROM git_pr_operations WHERE host=? AND base_repo=? AND base_ref=? AND head_repo=? AND head_ref=?
 AND task_id=? AND bot_id=? AND pr_number=? AND status='created' LIMIT 1
)SQL",
                                        {base.host, base.repo, target.baseRef, head.repo, target.headRef, task.id, task.botId, number});
        if (!operation.next()) {
            transaction.commit();
            return {{"ok", false},
                    {"status", "unassociated"},
                    {"prNumber", number},
                    {"url", url},
                    {"error", "No trusted task-to-PR link exists; this PR was left unassociated."}};
        }
        cursor = latestGitWebhookId();
        transaction.commit();
    }
    const auto pr = host.readPullRequest(context, base, number);
    if (!pr || pr->number != number || pr->target != target || pr->url != url || pr->headSha.empty())
        throw Conflict("The open PR does not match the selected repository and branches");

    std::lock_guard lock(database.mutex);
    Transaction transaction(database.db);
    if (botLifecycle.botPaused(context.botId)) throw BotPaused{};
    if (permissions.forbidden(context.botId, Action::repository_pr, base.resource()) ||
        !permissions.allows(context.botId, Action::repository_read, base.resource()) ||
        (head != base && !permissions.allows(context.botId, Action::repository_read, head.resource())))
        throw Conflict("Repository access denied");
    const auto task = taskRecords.load(context.taskId);
    const auto& run = task.activeRun ? task.activeRun : task.lastRun;
    if (task.botId != context.botId || !run || run->ref != context.run || run->assignment.revisionId != task.currentRevision)
        throw Conflict("The task's Session changed before the PR could be associated");
    if (terminal(task) || task.state == TaskState::cancelling) throw Conflict("The task already ended");
    if (gitPrWatchUnavailable(base, number, task.id)) throw Conflict("The PR already belongs to another task");
    auto operation = database.query(R"SQL(
SELECT 1 FROM git_pr_operations WHERE host=? AND base_repo=? AND base_ref=? AND head_repo=? AND head_ref=?
 AND task_id=? AND bot_id=? AND pr_number=? AND status='created' LIMIT 1
)SQL",
                                    {base.host, base.repo, target.baseRef, head.repo, target.headRef, task.id, task.botId, number});
    if (!operation.next()) {
        transaction.commit();
        return {{"ok", false},
                {"status", "unassociated"},
                {"prNumber", number},
                {"url", url},
                {"error", "No trusted task-to-PR link exists; this PR was left unassociated."}};
    }

    const auto scrub = secretStorage.sharedRedactor();
    Json result{{"ok", true}, {"status", "associated"}, {"prNumber", number}, {"url", url}};
    if (!database.query("SELECT 1 FROM git_pr_watches WHERE (host,repo,pr_number)=(?,?,?)", {base.host, base.repo, number}).next()) {
        watchGitPr(task, runJson(context.run), cursor, base, number, url, pr->headSha);
        auto entry = gitJournal(context, "git_open_pr", base, result);
        entry["result"]["reason"] = reason;
        database.writeJournal(task.id, JournalKind::git_mutation, scrub.record(std::move(entry)));
    }
    transaction.commit();
    return scrub.record(result, SecretRedactor::Fields::external);
}
}
