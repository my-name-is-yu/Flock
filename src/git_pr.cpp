#include "storage.hpp"

#include <map>

namespace flockv0 {
namespace {
Json prResult(std::string_view operationId, std::string_view status, Id number = 0, std::string_view url = {}) {
    Json result{{"operationId",operationId},{"status",status}};
    if (number) result.update({{"prNumber",number},{"url",url}});
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

void Store::Impl::initializeGitSchema() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS git_pr_operations(
 operation_id TEXT PRIMARY KEY NOT NULL, host TEXT NOT NULL, base_repo TEXT NOT NULL,
 base_ref TEXT NOT NULL, head_repo TEXT NOT NULL, head_ref TEXT NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL, invocation TEXT NOT NULL,
 event_cursor INTEGER NOT NULL, status TEXT NOT NULL CHECK(status IN ('pending','created','ambiguous','failed','rejected')),
 pr_number INTEGER, url TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS unresolved_git_pr_target
 ON git_pr_operations(host,base_repo,base_ref,head_repo,head_ref) WHERE status IN ('pending','ambiguous','rejected');
CREATE INDEX IF NOT EXISTS git_pr_history ON git_pr_operations(task_id,host,base_repo,base_ref,head_repo,head_ref);
CREATE TABLE IF NOT EXISTS git_pr_watches(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL,
 task_id INTEGER NOT NULL REFERENCES tasks(id), bot_id TEXT NOT NULL, run TEXT NOT NULL,
 url TEXT NOT NULL, event_cursor INTEGER NOT NULL,
 PRIMARY KEY(host,repo,pr_number));
CREATE TABLE IF NOT EXISTS git_pr_heads(
 host TEXT NOT NULL, repo TEXT NOT NULL, pr_number INTEGER NOT NULL, event_cursor INTEGER NOT NULL,
 head_sha TEXT NOT NULL, synchronized INTEGER NOT NULL CHECK(synchronized IN (0,1)),
 PRIMARY KEY(host,repo,pr_number,event_cursor));
CREATE TABLE IF NOT EXISTS git_mutations(
 invocation TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id), dedupe TEXT, result TEXT,
 PRIMARY KEY(task_id,invocation), UNIQUE(task_id,dedupe));
CREATE TABLE IF NOT EXISTS git_task_reads(
 id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL REFERENCES tasks(id),
 invocation TEXT NOT NULL, tool TEXT NOT NULL, target TEXT NOT NULL, result TEXT NOT NULL,
 UNIQUE(task_id,invocation));
CREATE INDEX IF NOT EXISTS git_task_reads_by_task ON git_task_reads(task_id,id);
CREATE TABLE IF NOT EXISTS git_webhook_events(
 id INTEGER PRIMARY KEY, host TEXT NOT NULL, event_id TEXT NOT NULL, repo TEXT NOT NULL,
 data TEXT NOT NULL, payload_digest TEXT, UNIQUE(host,event_id), UNIQUE(host,payload_digest));
CREATE TABLE IF NOT EXISTS git_webhook_deliveries(
 event INTEGER NOT NULL REFERENCES git_webhook_events(id) ON DELETE CASCADE, pr_number INTEGER NOT NULL,
 PRIMARY KEY(event,pr_number));
CREATE INDEX IF NOT EXISTS git_events_repo ON git_webhook_events(host,repo);
)SQL";
    if (sqlite3_exec(db,schema,nullptr,nullptr,nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}

bool Store::Impl::gitPrOwnedByAnotherTask(const GitRepository& repo, Id pr, Id task) const {
    return query(R"SQL(
SELECT 1 FROM git_pr_watches w WHERE (w.host,w.repo,w.pr_number)=(?,?,?) AND w.task_id<>?
AND NOT EXISTS (SELECT 1 FROM deleted_bots d WHERE d.bot=w.bot_id)
)SQL",{repo.host,repo.repo,pr,task}).next();
}

bool Store::Impl::watchGitPr(Id task, std::string_view bot, const Json& run, Id eventCursor, const GitPullRequest& pr) {
    const auto& repo = pr.target.base;
    auto prior = query("SELECT task_id,bot_id,event_cursor FROM git_pr_watches WHERE (host,repo,pr_number)=(?,?,?)",
                       {repo.host,repo.repo,pr.number});
    if (prior.next() && (prior.number(0) != task || prior.string(1) != bot)) {
        if (!Policy(*this).botDeleted(prior.string(1))) return false;
        // Keep the original attribution boundary and start the new owner's delivery at the handoff.
        exec("INSERT INTO git_pr_watch_origins VALUES(?,?,?,?) ON CONFLICT DO NOTHING",
             {repo.host,repo.repo,pr.number,prior.number(2)});
        auto events = query("SELECT coalesce(max(id),0) FROM git_webhook_events");
        events.next();
        eventCursor = events.number(0);
        exec(R"SQL(
UPDATE git_pr_watches SET task_id=?,bot_id=?,run=?,url=?,event_cursor=? WHERE (host,repo,pr_number)=(?,?,?)
)SQL",{task,bot,run,pr.url,eventCursor,repo.host,repo.repo,pr.number});
    } else {
        exec(R"SQL(
INSERT INTO git_pr_watches VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(host,repo,pr_number)
 DO UPDATE SET url=excluded.url
)SQL",{repo.host,repo.repo,pr.number,task,bot,run,pr.url,eventCursor});
    }
    if (!pr.headSha.empty()) {
        exec(R"SQL(
INSERT INTO git_pr_heads VALUES(?,?,?,?,?,0) ON CONFLICT(host,repo,pr_number,event_cursor)
 DO UPDATE SET head_sha=excluded.head_sha WHERE synchronized=0
)SQL",{repo.host,repo.repo,pr.number,eventCursor,pr.headSha});
    }
    gitWebhooksDrainedAt.reset();
    return true;
}

Json Store::Impl::openGitPr(GitHost& host, const GitContext& context, const GitOpenPr& input,
                               std::string_view invocation, bool newIntent) {
    auto request = input;
    const auto& [base,head,baseRef,headRef] = request.target;
    const auto intent = std::string(invocation) + (newIntent ? "/new" : "");
    bool create;
    {
        std::lock_guard lock(mutex);
        Transaction transaction(db);
        auto operation = query(R"SQL(
SELECT operation_id,task_id,bot_id,status,pr_number,url FROM git_pr_operations
WHERE host=?1 AND base_repo=?2 AND base_ref=?3 AND head_repo=?4 AND head_ref=?5
AND (status IN ('pending','ambiguous','rejected') OR (task_id=?6 AND (invocation=?7 OR (status='created' AND NOT ?8))))
ORDER BY (task_id=?6 AND invocation=?7) DESC,status IN ('pending','ambiguous','rejected') DESC,rowid DESC LIMIT 1
)SQL",{base.host,base.repo,baseRef,head.repo,headRef,context.taskId,intent,int{newIntent}});
        create = !operation.next();
        if (create) {
            if (Policy(*this).forbidden(context.botId,Action::repository_pr,base.resource()))
                throw Conflict("Repository access denied");
            request.operationId = actionId();
            exec("INSERT INTO git_pr_operations VALUES(?,?,?,?,?,?,?,?,?,?,"
                 "(SELECT coalesce(max(id),0) FROM git_webhook_events),'pending',NULL,NULL)",
                 {request.operationId,base.host,base.repo,baseRef,head.repo,headRef,context.taskId,context.botId,
                  runJson(context.run),intent});
            journal(context.taskId,"git_mutation",sharedRedactor().record(
                gitJournal(context,"git_open_pr",base,prResult(request.operationId,"pending"))));
        } else {
            if (operation.number(1) != context.taskId || operation.string(2) != context.botId)
                throw Conflict("This PR target belongs to another task");
            request.operationId = operation.string(0);
            if (terminalPrOperation(operation.string(3)))
                return prResult(request.operationId,operation.string(3),operation.number(4),operation.string(5));
        }
        transaction.commit();
    }

    std::map<Id,GitPullRequest> matches;
    bool rejected = false;
    try {
        if (create) {
            auto pr = host.createPullRequest(context,request);
            if (verifiedPr(pr,request)) matches.emplace(pr.number,std::move(pr));
        } else {
            for (auto& pr : host.findPullRequests(context,request.target))
                if (verifiedPr(pr,request)) matches.emplace(pr.number,std::move(pr));
        }
    } catch (const GitRejected&) {
        rejected = create;
    } catch (const std::exception&) {
    }

    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto saved = query("SELECT status,pr_number,url,run,event_cursor FROM git_pr_operations WHERE operation_id=?",{request.operationId});
    if (!saved.next()) throw Conflict("PR operation is missing");
    const auto previous = saved.string(0);
    if (terminalPrOperation(previous)) return prResult(request.operationId,previous,saved.number(1),saved.string(2));

    const auto run = saved.json(3);
    const auto task = load(context.taskId);
    const auto scrub = sharedRedactor();
    std::string_view state = rejected ? "failed" : "ambiguous";
    Id number = 0;
    std::string url;
    if (matches.size() == 1) {
        const auto& pr = matches.begin()->second;
        if (watchGitPr(context.taskId,context.botId,run,saved.number(4),pr)) {
            state = "created";
            number = pr.number;
            url = pr.url;
            if (pr.awaitingReview) gitInboxItem(task,"awaiting/" + url,url,"Pull request awaiting review",request.title);
        }
    }
    const auto result = prResult(request.operationId,state,number,url);
    exec("UPDATE git_pr_operations SET status=?,pr_number=?,url=? WHERE operation_id=?",
         {state,number ? Json(number) : Json(nullptr),number ? Json(url) : Json(nullptr),request.operationId});

    if (state == "ambiguous") {
        auto candidates = Json::array();
        for (const auto& [id,pr] : matches) {
            if (gitPrOwnedByAnotherTask(base,id,context.taskId)) continue;
            candidates.push_back({{"prNumber",id},{"url",pr.url},{"headSha",pr.headSha},{"awaitingReview",pr.awaitingReview}});
        }
        auto pending = query("SELECT id,candidates FROM pr_review_items WHERE operation_id=? AND status='pending'",
                             {request.operationId});
        const bool hasPending = pending.next();
        if (!hasPending || pending.json(1) != candidates) {
            if (hasPending) exec("UPDATE pr_review_items SET status='superseded' WHERE id=?",{pending.number(0)});
            insertPrReview(task,"https://" + base.resource() + "/pulls","Reconcile pull request creation",
                "Verify the original PR for " + head.resource() + ":" + headRef + " into " + baseRef +
                ". Choose a verified candidate, reject this operation, or retry with the same branches to check the host again.",
                {},request.operationId,candidates);
        }
    } else {
        exec("UPDATE pr_review_items SET status=? WHERE operation_id=? AND status='pending'",
             {state == "created" ? "linked" : "superseded",request.operationId});
    }
    if (state != previous) {
        auto entry = gitJournal(context,"git_open_pr",base,result);
        entry["run"] = run;
        entry["recovered"] = state == "created" && !create;
        journal(context.taskId,"git_mutation",scrub.record(std::move(entry)));
    }
    transaction.commit();
    return scrub.record(result,SecretRedactor::Fields::external);
}

void Store::Impl::decideGitPr(const InboxTarget& item, std::string_view decision, const Json& data, const Json& task) {
    auto row = query(R"SQL(
SELECT o.operation_id,o.host,o.base_repo,o.head_repo,o.base_ref,o.head_ref,o.run,i.candidates,o.event_cursor
FROM pr_review_items i JOIN git_pr_operations o ON o.operation_id=i.operation_id
WHERE i.id=? AND o.task_id=? AND o.bot_id=? AND o.status='ambiguous'
)SQL",{item.itemId,item.taskId,item.botId});
    if (!row.next()) throw DecisionUnavailable{};
    const auto operation = row.string(0);
    const auto base = GitRepository::parse(row.string(1),row.string(2));
    const auto head = GitRepository::parse(row.string(1),row.string(3));
    const auto run = row.json(6);
    const GitContext context{item.taskId,item.botId,field(task,"environmentId"),field(task,"projectId"),runRef(run)};
    Id number = 0;
    std::string url;
    const bool link = decision == "link_pr";
    if (link) {
        const Policy policy(*this);
        if (policy.forbidden(item.botId,Action::repository_pr,base.resource()) ||
            (head != base && !policy.allows(item.botId,Action::repository_read,head.resource())))
            throw DecisionDenied{};
        const auto candidates = row.json(7);
        const auto selected = data.find("prNumber");
        if (selected == data.end() || !selected->is_number_integer()) throw std::invalid_argument("Choose a verified PR");
        const auto candidate = std::find_if(candidates.begin(),candidates.end(),[&](const Json& pr) {
            return pr.at("prNumber") == *selected;
        });
        if (candidate == candidates.end()) throw Conflict("The PR is not a verified candidate for this operation");
        number = candidate->at("prNumber").get<Id>();
        url = field(*candidate,"url");
        if (!watchGitPr(item.taskId,item.botId,run,row.number(8),
                       {{base,head,row.string(4),row.string(5)},number,url,operation,field(*candidate,"headSha"),
                        candidate->at("awaitingReview").get<bool>()}))
            throw Conflict("The PR already belongs to another task");
        if (candidate->at("awaitingReview") == true)
            gitInboxItem(task,"awaiting/" + url,url,"Pull request awaiting review","Review the linked pull request.");
    }
    const auto status = link ? "created" : "rejected";
    exec("UPDATE git_pr_operations SET status=?,pr_number=?,url=? WHERE operation_id=?",
         {status,link ? Json(number) : Json(nullptr),link ? Json(url) : Json(nullptr),operation});
    exec("UPDATE pr_review_items SET status=? WHERE id=?",{link ? "linked" : "rejected",item.itemId});
    journal(item.taskId,"git_mutation",sharedRedactor().record(
        gitJournal(context,"git_open_pr",base,prResult(operation,status,number,url))));
}
}
