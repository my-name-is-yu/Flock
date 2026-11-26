#include "storage.hpp"
#include "flock/types.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"

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
    if (status == "ambiguous") result["message"] = "PR creation is unresolved. A reconciliation item is in the Decision Inbox.";
    return result;
}

bool verifiedPr(const GitPullRequest& pr, const GitOpenPr& request) {
    return pr.target == request.target && pr.operationId == request.operationId && pr.number > 0 &&
           pr.url == "https://" + pr.target.base.resource() + "/pull/" + std::to_string(pr.number);
}

bool terminalPrOperation(std::string_view status) {
    return status == "created" || status == "failed" || status == "rejected";
}
}

bool Store::Impl::gitPrWatchUnavailable(const GitRepository& repo, Id pr, Id task) const {
    return gitPrMutationsInFlight.contains({repo.host, repo.repo, pr}) || query(R"SQL(
SELECT 1 FROM git_pr_watches w WHERE (w.host,w.repo,w.pr_number)=(?,?,?) AND w.task_id<>?
AND NOT EXISTS (SELECT 1 FROM deleted_bots d WHERE d.bot=w.bot_id)
)SQL",
                                                                                {repo.host, repo.repo, pr, task})
                                                                              .next();
}

bool Store::Impl::watchGitPr(Id task, std::string_view bot, const Json& run, Id deliveryCursor, const GitPullRequest& pr,
                             GitWatchStart start) {
    const auto& repo = pr.target.base;
    if (gitPrWatchUnavailable(repo, pr.number, task)) return false;
    auto prior =
        query("SELECT task_id,event_cursor FROM git_pr_watches WHERE (host,repo,pr_number)=(?,?,?)", {repo.host, repo.repo, pr.number});
    const bool hasPriorWatch = prior.next();
    const auto headObservedAt = hasPriorWatch ? latestGitWebhookId() : deliveryCursor;
    if (hasPriorWatch && prior.number(0) != task) {
        const auto previousDeliveryCursor = prior.number(1);
        exec("INSERT INTO git_pr_watch_origins VALUES(?,?,?,?) ON CONFLICT DO NOTHING",
             {repo.host, repo.repo, pr.number, previousDeliveryCursor});
        if (start == GitWatchStart::operationIntent) deliveryCursor = headObservedAt;
        exec(R"SQL(
UPDATE git_pr_watches SET task_id=?,bot_id=?,run=?,url=?,event_cursor=? WHERE (host,repo,pr_number)=(?,?,?)
)SQL",
             {task, bot, run, pr.url, deliveryCursor, repo.host, repo.repo, pr.number});
    } else {
        exec(R"SQL(
INSERT INTO git_pr_watches VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(host,repo,pr_number)
 DO UPDATE SET url=excluded.url
)SQL",
             {repo.host, repo.repo, pr.number, task, bot, run, pr.url, deliveryCursor});
    }
    if (!pr.headSha.empty()) {
        exec(R"SQL(
INSERT INTO git_pr_heads VALUES(?,?,?,?,?,0) ON CONFLICT(host,repo,pr_number,event_cursor)
 DO UPDATE SET head_sha=excluded.head_sha WHERE synchronized=0
)SQL",
             {repo.host, repo.repo, pr.number, headObservedAt, pr.headSha});
    }
    gitWebhooksDrainedAt.reset();
    return true;
}

Json Store::Impl::openGitPr(GitHost& host, const GitContext& context, const GitOpenPr& input, std::string_view invocation, bool newIntent,
                            std::string_view reason) {
    auto request = input;
    const auto& [base, head, baseRef, headRef] = request.target;
    const auto intent = std::string(invocation) + (newIntent ? "/new" : "");
    bool create;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        if (botPaused(context.botId)) throw BotPaused{};
        auto operation = query(R"SQL(
SELECT operation_id,task_id,bot_id,status,pr_number,url FROM git_pr_operations
WHERE host=?1 AND base_repo=?2 AND base_ref=?3 AND head_repo=?4 AND head_ref=?5
AND (status IN ('pending','ambiguous','rejected') OR (task_id=?6 AND (invocation=?7 OR (status='created' AND NOT ?8))))
ORDER BY (task_id=?6 AND invocation=?7) DESC,status IN ('pending','ambiguous','rejected') DESC,rowid DESC LIMIT 1
)SQL",
                               {base.host, base.repo, baseRef, head.repo, headRef, context.taskId, intent, int{newIntent}});
        create = !operation.next();
        if (create) {
            if (Policy(*this).forbidden(context.botId, Action::repository_pr, base.resource())) throw Conflict("Repository access denied");
            request.operationId = actionId();
            exec("INSERT INTO git_pr_operations VALUES(?,?,?,?,?,?,?,?,?,?,"
                 "(SELECT coalesce(max(id),0) FROM git_webhook_events),'pending',NULL,NULL)",
                 {request.operationId, base.host, base.repo, baseRef, head.repo, headRef, context.taskId, context.botId,
                  runJson(context.run), intent});
            auto pending = prResult(request.operationId, "pending");
            pending["reason"] = reason;
            journalStorage.journal(context.taskId, "git_mutation",
                                   secretStorage.sharedRedactor().record(gitJournal(context, "git_open_pr", base, pending)));
        } else {
            if (operation.number(1) != context.taskId || operation.string(2) != context.botId)
                throw Conflict("This PR target belongs to another task");
            request.operationId = operation.string(0);
            if (terminalPrOperation(operation.string(3)))
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
        } else {
            for (auto& pr : host.findPullRequests(context, request.target))
                if (verifiedPr(pr, request)) matches.emplace(pr.number, std::move(pr));
        }
    } catch (const GitRejected&) {
        rejected = create;
    } catch (const std::exception&) {
    }

    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto saved = query("SELECT status,pr_number,url,run,event_cursor FROM git_pr_operations WHERE operation_id=?", {request.operationId});
    if (!saved.next()) throw Conflict("PR operation is missing");
    const auto previous = saved.string(0);
    if (terminalPrOperation(previous)) return prResult(request.operationId, previous, saved.number(1), saved.string(2));

    const auto run = saved.json(3);
    const auto task = taskRecords.load(context.taskId);
    const auto scrub = secretStorage.sharedRedactor();
    std::string_view state = rejected ? "failed" : "ambiguous";
    Id number = 0;
    std::string url;
    if (matches.size() == 1) {
        const auto& pr = matches.begin()->second;
        if (watchGitPr(context.taskId, context.botId, run, saved.number(4), pr)) {
            state = "created";
            number = pr.number;
            url = pr.url;
            if (pr.awaitingReview) gitInboxItem(task, "awaiting/" + url, url, "Pull request awaiting review", request.title);
        }
    }
    const auto result = prResult(request.operationId, state, number, url);
    exec("UPDATE git_pr_operations SET status=?,pr_number=?,url=? WHERE operation_id=?",
         {state, number ? Json(number) : Json(nullptr), number ? Json(url) : Json(nullptr), request.operationId});

    if (state == "ambiguous") {
        auto candidates = Json::array();
        for (const auto& [id, pr] : matches) {
            if (gitPrWatchUnavailable(base, id, context.taskId)) continue;
            candidates.push_back({{"prNumber", id}, {"url", pr.url}, {"headSha", pr.headSha}, {"awaitingReview", pr.awaitingReview}});
        }
        auto pending = query("SELECT id,candidates FROM pr_review_items WHERE operation_id=? AND status='pending'", {request.operationId});
        const bool hasPending = pending.next();
        if (!hasPending || pending.json(1) != candidates) {
            if (hasPending) exec("UPDATE pr_review_items SET status='superseded' WHERE id=?", {pending.number(0)});
            insertPrReview(
                task, "https://" + base.resource() + "/pulls", "Reconcile pull request creation",
                "Verify the original PR for " + head.resource() + ":" + headRef + " into " + baseRef +
                    ". Choose a verified candidate, reject this operation, or retry with the same branches to check the host again.",
                {}, request.operationId, candidates);
        }
    } else {
        exec("UPDATE pr_review_items SET status=? WHERE operation_id=? AND status='pending'",
             {state == "created" ? "linked" : "superseded", request.operationId});
    }
    if (state != previous) {
        auto entry = gitJournal(context, "git_open_pr", base, result);
        entry["run"] = run;
        entry["recovered"] = state == "created" && !create;
        journalStorage.journal(context.taskId, "git_mutation", scrub.record(std::move(entry)));
    }
    transaction.commit();
    return scrub.record(result, SecretRedactor::Fields::external);
}

void Store::Impl::decideGitPr(const InboxTarget& item, std::string_view decision, const Json& data, const TaskData& task) {
    auto row = query(R"SQL(
SELECT o.operation_id,o.host,o.base_repo,o.head_repo,o.base_ref,o.head_ref,o.run,i.candidates,o.event_cursor
FROM pr_review_items i JOIN git_pr_operations o ON o.operation_id=i.operation_id
WHERE i.id=? AND o.task_id=? AND o.bot_id=? AND o.status='ambiguous'
)SQL",
                     {item.itemId, item.taskId, item.botId});
    if (!row.next()) throw DecisionUnavailable{};
    const auto operation = row.string(0);
    const auto base = GitRepository::parse(row.string(1), row.string(2));
    const auto head = GitRepository::parse(row.string(1), row.string(3));
    const auto run = row.json(6);
    const GitContext context{item.taskId, item.botId, task.environmentId, task.projectId, runRef(run)};
    Id number = 0;
    std::string url;
    const bool link = decision == "link_pr";
    if (link) {
        const Policy policy(*this);
        if (policy.forbidden(item.botId, Action::repository_pr, base.resource()) ||
            (head != base && !policy.allows(item.botId, Action::repository_read, head.resource())))
            throw DecisionDenied{};
        const auto candidates = row.json(7);
        const auto selected = data.find("prNumber");
        if (selected == data.end() || !selected->is_number_integer()) throw std::invalid_argument("Choose a verified PR");
        const auto candidate =
            std::find_if(candidates.begin(), candidates.end(), [&](const Json& pr) { return pr.at("prNumber") == *selected; });
        if (candidate == candidates.end()) throw Conflict("The PR is not a verified candidate for this operation");
        number = candidate->at("prNumber").get<Id>();
        url = field(*candidate, "url");
        if (!watchGitPr(item.taskId, item.botId, run, row.number(8),
                        {{base, head, row.string(4), row.string(5)},
                         number,
                         url,
                         operation,
                         field(*candidate, "headSha"),
                         candidate->at("awaitingReview").get<bool>()}))
            throw Conflict("The PR already belongs to another task");
        if (candidate->at("awaitingReview") == true)
            gitInboxItem(task, "awaiting/" + url, url, "Pull request awaiting review", "Review the linked pull request.");
    }
    const auto status = link ? "created" : "rejected";
    exec("UPDATE git_pr_operations SET status=?,pr_number=?,url=? WHERE operation_id=?",
         {status, link ? Json(number) : Json(nullptr), link ? Json(url) : Json(nullptr), operation});
    exec("UPDATE pr_review_items SET status=? WHERE id=?", {link ? "linked" : "rejected", item.itemId});
    journalStorage.journal(
        item.taskId, "git_mutation",
        secretStorage.sharedRedactor().record(gitJournal(context, "git_open_pr", base, prResult(operation, status, number, url))));
}
}
