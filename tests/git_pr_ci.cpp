#include "fake_git.hpp"
#include "model.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <sqlite3.h>

using namespace git_fixture;

namespace {
class Script final : public Model {
public:
    std::map<std::string,Decision> calls;
    Json results = Json::object();
    unsigned notifications = 0;
    bool openAfterSession = false;
    bool checkOriginatingSession = false;
    std::function<void()> onNotification;

    ModelTurn decide(const ModelInput& input) override {
        auto request = app::responseRequest(input);
        auto next = choose(input);
        next.args["reason"] = "Use the verified fixture evidence";
        const Json call{{"type","function_call"},{"namespace","flock"},{"status","completed"},
                        {"name",next.tool},{"arguments",next.args.dump()}};
        return app::responseDecision(Json::array({call}),request);
    }

private:
    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "intake")
            return {"delegate",{{"instruction","Push change"},{"doneWhen","Open a PR for the pushed branch"}}, {}};
        if (event.at("kind") == "result") {
            const auto steps = event.value("gitResults",Json::array());
            if (openAfterSession && steps.empty()) return {"git_open_pr",openArgs(),{}};
            if (!steps.empty()) {
                results["session-pr"] = steps.at(0).at("result");
                check(results["session-pr"].value("ok",false),"task PR open failed: " + results["session-pr"].dump());
            }
            return {"complete",{{"report","Change prepared and checked"}},{}};
        }
        const auto& steps = event.at("turn");
        if (event.contains("gitEvent")) {
            const auto& gitEvent = event.at("gitEvent");
            if (steps.empty()) {
                ++notifications;
                if (onNotification) onNotification();
                check(input.task.empty() && input.history.empty(),"Notification received unrelated task context");
                if (checkOriginatingSession)
                    check(gitEvent.at("run").at("threadId") == "thread-project","Webhook lost its originating Session");
                check(std::ranges::find(input.tools,"git_open_pr") == input.tools.end(),"Webhook supplied mutation tools");
                if (gitEvent.at("jobId") == 81)
                    return {"git_read_ci_logs",{{"host",gitEvent.at("host")},{"repo",gitEvent.at("repo")},
                        {"jobId",gitEvent.at("jobId")},{"maxBytes",32}},{}};
                return {"reply",{{"text","Git notification received."}},{}};
            }
            results["log"] = steps.at(0).at("result");
            const auto& log = results.at("log").at("data");
            check(log.at("truncated") == true && log.at("text").get<std::string>().size() == 32,
                  "CI excerpt is not explicitly bounded");
            return {"reply",{{"text","CI failed. " + log.at("text").get<std::string>()}},{}};
        }
        const auto name = event.at("text").get<std::string>();
        if (steps.empty()) return calls.at(name);
        results[name] = steps.at(0).at("result");
        return {"reply",{{"text","Git tool result recorded"}},{}};
    }
};

Json invoke(Store& store, Runtime& runtime, Script& model, std::string id, std::string tool, Json args) {
    model.calls.insert_or_assign(id,Decision{std::move(tool),std::move(args),{}});
    store.receive(localOwner,intake(),id,id);
    runtime.runConversations();
    if (!model.results.contains(id)) throw std::runtime_error("No result for " + id);
    return model.results.at(id);
}

Json persistence(const std::filesystem::path& db, std::string_view operationId) {
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2(db.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr) == SQLITE_OK,"could not read fixture database");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(raw,sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(raw,
        "SELECT json_object('status',status,'refs',json_array(host,base_repo,base_ref,head_repo,head_ref),"
        "'taskId',task_id,'botId',bot_id,'prNumber',pr_number) FROM git_pr_operations WHERE operation_id=?",
        -1,&stmt,nullptr) == SQLITE_OK,"could not query PR operations");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(stmt,sqlite3_finalize);
    sqlite3_bind_text(stmt,1,operationId.data(),static_cast<int>(operationId.size()),SQLITE_TRANSIENT);
    check(sqlite3_step(stmt) == SQLITE_ROW,"PR operation was missing before the host call");
    return Json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt,0)));
}

std::string watchedPrUrl(const std::filesystem::path& db) {
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2(db.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr) == SQLITE_OK,"could not read fixture database");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(raw,sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(raw,"SELECT url FROM git_pr_watches",-1,&stmt,nullptr) == SQLITE_OK,"could not query PR watches");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(stmt,sqlite3_finalize);
    check(sqlite3_step(stmt) == SQLITE_ROW,"no pull request is watched");
    std::string url = reinterpret_cast<const char*>(sqlite3_column_text(stmt,0));
    check(sqlite3_step(stmt) == SQLITE_DONE,"more than one pull request is watched");
    return url;
}

void freshPrReviewSchema() {
    Directory directory;
    const auto path = directory.path/"fresh.db";
    Store store(path);
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2(path.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr) == SQLITE_OK,"could not open fresh database");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(raw,sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(raw,R"SQL(
SELECT (SELECT count(*) FROM pragma_table_info('pr_review_items')
        WHERE name IN ('operation_id','git_key','candidates'))=3
   AND EXISTS (SELECT 1 FROM pragma_index_list('pr_review_items')
               WHERE name='pending_pr_reconciliation' AND "unique"=1 AND partial=1)
   AND (SELECT group_concat(name) FROM pragma_index_info('pending_pr_reconciliation'))='operation_id'
)SQL",-1,&stmt,nullptr) == SQLITE_OK,"could not query fresh PR review schema");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(stmt,sqlite3_finalize);
    check(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt,0) == 1,
          "Fresh DB is missing PR review columns or the partial unique reconciliation index");
}

void runner() {
    Directory directory;
    const auto path = directory.path/"state.db";
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Script script;
    script.openAfterSession = true;
    script.checkOriginatingSession = true;
    Id task, other;
    {
        Store store(path);
        grant(store,"owner","environment","sandbox","launch");
        grant(store,"owner","repository",repoGrant,"pr");
        grant(store,"owner","repository",repoGrant,"read");
        task = store.submit(intake());
        other = store.submit(intake("other","other-project"));
        Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(store.task(task).at("state") == "completed" && host->createCalls == 1,"Session-to-PR flow did not complete: " + store.history(task).dump());
        check(script.results.at("session-pr").at("prNumber") == 1,"task did not receive the PR number");
        check(historyCount(store,task,"git_mutation") == 2,"PR operation history must include intent and result");
        check(historyCount(store,task,"report") == 1,"Session result missing");
        check(store.inbox(localOwner,"owner").at(0).at("prTitle") == "Pull request awaiting review","PR awaiting review missing");
        check(store.receiveGitWebhook({hostName,"ci-before-restart",repoName,"ci","completed",{1},sha,"test job failed",81}),
              "CI event was rejected before restart");
        check(historyCount(store,task,"git_webhook") == 0,"intake ran delivery synchronously");
    }
    {
        Store store(path);
        const auto otherHistory = store.history(other);
        Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
        check(!store.receiveGitWebhook({hostName,"ci-before-restart","org/other","ci","completed",{2},sha,"changed duplicate",81}),
              "same host and delivery ID inserted twice");
        check(store.receiveGitWebhook({hostName,"wrong-pr",repoName,"ci","completed",{2},sha,"wrong PR",81}),
              "unmatched webhook was not recorded");
        check(store.receiveGitWebhook({hostName,"wrong-repo","org/other","ci","completed",{1},sha,"wrong repo",81}),
              "unmatched repository event was not recorded");
        runtime.runConversations();
        check(script.notifications == 1 && host->logCalls == 1 && historyCount(store,task,"git_webhook") == 1,
              "CI event did not wake exactly the persistent PR owner");
        check(store.history(other) == otherHistory && store.conversation("other").dump().find("CI failed") == std::string::npos,
              "unrelated task was woken");
        check(store.history(task).dump().find("CI failed. linker: missing symbol") != std::string::npos,
              "CI report is missing from the owning task");
        check(store.receiveGitWebhook({hostName,"review",repoName,"review","submitted",{1},sha,"Approved",0}),"review rejected");
        check(store.receiveGitWebhook({hostName,"scan",repoName,"code_scan","appeared",{},sha,"Scan result",0}),"scan rejected");
        runtime.runConversations();
        runtime.runConversations();
        const auto inbox = store.inbox(localOwner,"owner");
        check(inbox.size() == 3 && inbox.dump().find("Pull request review submitted") != std::string::npos &&
              inbox.dump().find("Code scanning result") != std::string::npos,"Git inbox decisions missing");
        const auto review = target(inbox.at(0));
        store.decideInbox(localOwner,review,"request_changes");
        check(store.inbox(localOwner,"owner").size() == 2 &&
              store.decisionHistory(localOwner,"owner").at(0).at("decision") == "request_changes",
              "Git inbox decision did not update and record");
        check(script.notifications == 3 && historyCount(store,task,"git_webhook") == 3,"webhooks were delivered more than once");

        const auto sibling = store.submit(intake());
        auto args = openArgs("sibling-change");
        args["taskId"] = sibling;
        check(invoke(store,runtime,script,"watch-sibling","git_open_pr",args).at("prNumber") == 2,
              "Could not create a second watch at the same commit");
        check(store.receiveGitWebhook({hostName,"shared-commit",repoName,"ci","completed",{},sha,"Shared SHA",81}),
              "CI event with a shared SHA was not saved");
        runtime.runConversations();
        check(script.notifications == 3 && historyCount(store,sibling,"git_webhook") == 0,
              "A SHA-only CI event woke a task without an unambiguous watch");
    }
}

void webhookHeadPeriods() {
    Directory directory;
    Store store(directory.path/"state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    grant(store,"owner","repository",repoGrant,"pr");
    grant(store,"owner","repository",repoGrant,"read");
    const auto first = store.submit(intake());
    auto args = openArgs();
    args["taskId"] = first;
    check(invoke(store,runtime,script,"watch-first","git_open_pr",args).at("prNumber") == 1,"could not create first watch");

    const std::string newSha(40,'b');
    check(store.receiveGitWebhook({hostName,"original-job",repoName,"ci","completed",{},sha,"Original head",0}),
          "could not enqueue original job");
    check(store.receiveGitWebhook({hostName,"early-job",repoName,"ci","completed",{},newSha,"New head",0}),
          "could not enqueue early job");
    runtime.runConversations();
    check(historyCount(store,first,"git_webhook") == 1,"Early job was routed before synchronization");
    check(store.receiveGitWebhook({hostName,"queued-old-job",repoName,"ci","completed",{},sha,"Queued old job",0}),
          "could not enqueue a job before synchronization");
    check(store.receiveGitWebhook({hostName,"first-sync",repoName,"pull_request","synchronize",{1},newSha,"New head",0}),
          "could not enqueue synchronization");

    const auto second = store.submit(intake());
    args = openArgs("other-change");
    args["taskId"] = second;
    check(invoke(store,runtime,script,"watch-second","git_open_pr",args).at("prNumber") == 2,"could not create second watch");
    check(historyCount(store,first,"git_webhook") == 4,"The old or early job was lost across synchronization");
    check(store.receiveGitWebhook({hostName,"reused-head-job",repoName,"ci","completed",{},sha,"Reused head",0}),
          "could not enqueue job on reused head");
    runtime.runConversations();
    check(historyCount(store,first,"git_webhook") == 4 && historyCount(store,second,"git_webhook") == 1,
          "A superseded head prevented the current watch from receiving a SHA-only job");
    check(store.receiveGitWebhook({hostName,"return-to-head",repoName,"pull_request","synchronize",{1},sha,"Original head",0}),
          "could not enqueue return to original head");
    runtime.runConversations();
    check(historyCount(store,first,"git_webhook") == 5 && historyCount(store,second,"git_webhook") == 1,
          "A SHA-only delivery was routed again when another watch moved to the same head");
}

void reviewAfterTaskProgress() {
    Directory directory;
    Store store(directory.path/"state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    grant(store,"owner","environment","sandbox","launch");
    grant(store,"owner","repository",repoGrant,"pr");
    const auto task = store.submit(intake());
    for (int i = 0; i < 4 && store.task(task).value("currentRevision",Id{0}) == 0; ++i) runtime.tick();
    check(store.task(task).value("currentRevision",Id{0}) > 0,"task has no initial revision");

    auto args = openArgs();
    args["taskId"] = task;
    check(invoke(store,runtime,script,"open-review-pr","git_open_pr",args).at("status") == "created",
          "fixture did not open a PR awaiting review");
    const auto gitReview = target(store.inbox(localOwner,"owner").at(0));
    const auto manualReview = store.requestPrReview(task,gitReview.targetRevision,gitReview.targetGeneration,
                                                  "https://github.com/org/flock/pull/1","Check changes","Verify tests");
    check(store.inbox(localOwner,"owner").size() == 2,"fixture did not create both reviews");

    check(invoke(store,runtime,script,"send-instruction","send_to_task",{{"taskId",task},
                 {"generation",gitReview.targetGeneration},{"instruction","Include the test results"}}).at("ok") == true,
          "fixture did not update the task within its revision");
    const auto inbox = store.inbox(localOwner,"owner");
    check(inbox.size() == 1,"manual review should expire after a Task generation change");
    const auto latest = target(inbox.at(0));
    check(latest.itemId == gitReview.itemId && latest.targetRevision == gitReview.targetRevision &&
          latest.targetGeneration > gitReview.targetGeneration,"Git review did not follow the Task within its revision");
    auto invalidManual = latest;
    invalidManual.itemId = manualReview;
    for (const auto& stale : {gitReview,invalidManual}) {
        try {
            store.decideInbox(localOwner,stale,"approve");
            throw std::runtime_error("A PR review decision with an invalid generation was accepted");
        } catch (const RequestChanged&) {}
    }
    store.decideInbox(localOwner,latest,"approve");
    check(store.inbox(localOwner,"owner").empty(),"Refreshed Git review decision did not close the item");
}

void reconciliationAfterTaskRevision() {
    Directory directory;
    const auto path = directory.path/"state.db";
    Store store(path);
    auto host = std::make_shared<FakeHost>();
    host->create = FakeHost::Create::lostResponse;
    Script script;
    Sessions sessions;
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    grant(store,"owner","environment","sandbox","launch");
    grant(store,"owner","repository",repoGrant,"pr");
    const auto task = store.submit(intake());
    for (int i = 0; i < 4 && store.task(task).value("currentRevision",Id{0}) == 0; ++i) runtime.tick();
    const auto before = store.task(task);
    check(before.value("currentRevision",Id{0}) > 0,"task has no initial revision");

    auto args = openArgs();
    args["taskId"] = task;
    const auto lost = invoke(store,runtime,script,"lose-pr-response","git_open_pr",args);
    check(lost.at("status") == "ambiguous","fixture did not create an ambiguous operation");
    auto extra = host->prs.front();
    extra.pr.number = 2;
    extra.pr.url = "https://github.com/org/flock/pull/2";
    host->prs.push_back(std::move(extra));
    check(invoke(store,runtime,script,"find-two-prs","git_open_pr",args).at("status") == "ambiguous",
          "fixture did not leave two verified candidates");
    const auto initial = store.inbox(localOwner,"owner").at(0);
    check(initial.at("candidates").size() == 2,"fixture failed to put candidates in the inbox");
    const auto changed = invoke(store,runtime,script,"revise-task","change_task",
        {{"taskId",task},{"generation",before.at("generation")},{"revisionId",before.at("currentRevision")},
         {"instruction","Update the success criteria"},{"doneWhen","Verify the new completion criteria"}});
    check(changed.at("ok") == true && store.task(task).at("replanning") == true,"fixture did not revise the active task");
    check(invoke(store,runtime,script,"retry-after-replan","git_open_pr",args).at("status") == "ambiguous",
          "fixture did not keep the PR creation unresolved");
    const auto latest = store.inbox(localOwner,"owner").at(0);
    check(latest.at("itemId") == initial.at("itemId") && latest.at("targetRevision") != initial.at("targetRevision") &&
          latest.at("targetRevision") == store.task(task).at("currentRevision") &&
          latest.at("targetGeneration") == store.task(task).at("generation") &&
          latest.at("candidates") == initial.at("candidates"),"Reconciliation lost its current target or verified candidates");
    try {
        store.decideInbox(localOwner,target(initial),"reject_pr");
        throw std::runtime_error("A stale reconciliation decision was accepted after task revision");
    } catch (const RequestChanged&) {}
    store.decideInbox(localOwner,target(latest),"link_pr",{{"prNumber",2}});
    const auto saved = persistence(path,lost.at("operationId").get<std::string>());
    check(saved.at("status") == "created" && saved.at("prNumber") == 2 && host->createCalls == 1,
          "The current reconciliation target could not link the original operation");
}

void recovery(FakeHost::State state, bool losesRequest, bool multipleMatches = false, bool wrongCorrelation = false) {
    Directory directory;
    const auto path = directory.path/"state.db";
    auto host = std::make_shared<FakeHost>();
    host->create = losesRequest ? FakeHost::Create::lostRequest : FakeHost::Create::lostResponse;
    host->state = state;
    Sessions sessions;
    Script script;
    Id task;
    std::string operationId;
    InboxTarget initial{};
    {
        Store store(path);
        task = store.submit(intake());
        grant(store,"owner","repository",repoGrant,"pr");
        Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
        host->inspect = [&](const GitContext& context, const GitOpenPr& request) {
            operationId = request.operationId;
            const auto pending = persistence(path,operationId);
            check(pending.at("status") == "pending" && pending.at("taskId") == context.taskId &&
                  pending.at("botId") == context.botId &&
                  pending.at("refs") == Json::array({hostName,repoName,"main",repoName,"change"}),
                  "create fired before durable operation intent");
        };
        auto args = openArgs();
        args["taskId"] = task;
        const auto lost = invoke(store,runtime,script,"lost","git_open_pr",args);
        check(lost.at("status") == "ambiguous" && persistence(path,operationId).at("status") == "ambiguous",
              "lost response was not recorded unresolved");
        initial = target(store.inbox(localOwner,"owner").at(0));
    }
    host->inspect = {};
    const auto repo = GitRepository::parse(hostName,repoName);
    host->prs.push_back({{{repo,repo,"main","unrelated"},4,"https://github.com/org/flock/pull/4",operationId,sha,true},
                        "Change",FakeHost::State::open});
    host->prs.push_back({{{repo,repo,"old","change"},5,"https://github.com/org/flock/pull/5",operationId,sha,false},
                        "Change",FakeHost::State::closed});
    for (const auto collisionState : {FakeHost::State::open,FakeHost::State::closed,FakeHost::State::merged}) {
        const auto number = static_cast<Id>(host->prs.size() + 10);
        host->prs.push_back({{{repo,repo,"main","change"},number,
                             "https://github.com/org/flock/pull/" + std::to_string(number),"",sha,false},
                             "Change",collisionState});
    }
    if (wrongCorrelation) host->prs.front().pr.operationId = "someone-elses-operation";
    if (multipleMatches) {
        auto extra = host->prs.front();
        extra.pr.number = 6;
        extra.pr.url = "https://github.com/org/flock/pull/6";
        host->prs.push_back(std::move(extra));
    }
    {
        Store store(path);
        Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
        auto args = openArgs();
        args["taskId"] = task;
        const auto recovered = invoke(store,runtime,script,"retry","git_open_pr",args);
        check(host->createCalls == 1 && host->lookupCalls == 1,"retry attempted a second create");
        if (losesRequest || multipleMatches || wrongCorrelation) {
            check(recovered.at("status") == "ambiguous" && !recovered.contains("prNumber"),"unverified PR was adopted");
            const auto items = store.inbox(localOwner,"owner");
            check(items.size() == 1 && items.at(0).at("itemKind") == "pr_reconciliation","reconciliation item missing or duplicated");
            const auto decision = target(items.at(0));
            const auto candidates = items.at(0).at("candidates");
            if (multipleMatches) {
                check(candidates.size() == 2,"Only exact correlations should be candidates");
                try {
                    store.decideInbox(localOwner,initial,"reject_pr");
                    throw std::runtime_error("An outdated reconciliation decision was accepted");
                } catch (const DecisionUnavailable&) {}
                try {
                    store.decideInbox(localOwner,decision,"link_pr",{{"prNumber",4}});
                    throw std::runtime_error("An unverified candidate was accepted");
                } catch (const Conflict&) {}
                store.decideInbox(localOwner,decision,"link_pr",{{"prNumber",1}});
                check(persistence(path,operationId).at("prNumber") == 1,"Selecting a verified PR did not link it");
                check(watchedPrUrl(path) == "https://github.com/org/flock/pull/1","Selected PR was not watched");
            } else {
                check(candidates.empty(),"Title collision was offered for manual linking");
                store.decideInbox(localOwner,decision,"reject_pr");
                check(store.inbox(localOwner,"owner").empty() && persistence(path,operationId).at("status") == "rejected",
                      "Rejecting reconciliation did not close the decision");
                check(invoke(store,runtime,script,"retry-rejected","git_open_pr",args).at("status") == "rejected" &&
                      host->createCalls == 1,"Rejected unknown operation was resent");
            }
        } else {
            check(recovered.at("status") == "created" && recovered.at("prNumber") == 1 &&
                  recovered.at("url") == "https://github.com/org/flock/pull/1" &&
                  recovered.at("operationId") == operationId && persistence(path,operationId).at("prNumber") == 1,
                  "the original PR was not recovered");
            check(store.inbox(localOwner,"owner").dump().find("pr_reconciliation") == std::string::npos,
                  "resolved reconciliation still pending");
            check(watchedPrUrl(path) == recovered.at("url").get<std::string>(),"recovered pull request is not watched");
        }
    }
}

void accessAndActions() {
    Directory directory;
    Store store(directory.path/"state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    const auto task = store.submit(intake());
    const auto other = store.submit(intake("other"));
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    const std::vector<std::pair<std::string,Json>> commands{
        {"git_repo_info",Json::object()}, {"git_open_pr",openArgs()}, {"git_update_pr",{{"prNumber",1},{"title","Changed"}}},
        {"git_pr_status",{{"prNumber",1}}}, {"git_commit_status",{{"sha",sha}}},
        {"git_read_ci_logs",{{"jobId",81},{"maxBytes",32}}}, {"git_request_review",{{"prNumber",1},{"reviewers",{"alice"}}}},
        {"git_submit_review",{{"prNumber",1},{"verdict","approve"}}},
        {"git_comment",{{"target","pr"},{"number",1},{"body","Initial"}}},
        {"git_comment",{{"target","pr"},{"number",1},{"body","Updated"},{"commentId",101}}},
        {"git_manage_issue",{{"action","list"}}}, {"git_manage_issue",{{"action","read"},{"issueNumber",2}}},
        {"git_manage_issue",{{"action","list_comments"},{"issueNumber",2}}},
        {"git_manage_issue",{{"action","create"},{"title","Follow-up"},{"labels",{"bug"}},{"assignees",{"alice"}}}},
        {"git_manage_issue",{{"action","update"},{"issueNumber",2},{"state","closed"},{"labels",Json::array()}}},
    };
    grant(store,"owner","repository",repoGrant,"read");
    grant(store,"owner","repository",repoGrant,"write");
    auto readOnly = openArgs();
    readOnly["taskId"] = task;
    check(invoke(store,runtime,script,"read-is-not-pr","git_open_pr",readOnly).at("ok") == false &&
          host->createCalls == 0,"repository read or write allowed PR creation");
    const auto deniedIssueComment = invoke(store,runtime,script,"read-is-not-issues","git_comment",
        {{"host",hostName},{"repo",repoName},{"taskId",task},{"target","issue"},{"number",2},{"body","Comment"}});
    check(deniedIssueComment.at("ok") == false && host->hostCalls == 0,"repository read or write allowed an issue comment");
    for (const auto* operation : {"pr","review","issues"}) grant(store,"owner","repository",repoGrant,operation);
    auto argsFor = [task](Json args, std::string repo) {
        args.update({{"host",hostName},{"repo",std::move(repo)},{"taskId",task}});
        return args;
    };
    int counter = 0;
    for (const auto& [tool,args] : commands) {
        const auto denied = invoke(store,runtime,script,"denied-" + std::to_string(++counter),tool,argsFor(args,"org/private"));
        check(denied.at("ok") == false && denied.at("error") == "Repository access denied","ungranted repo reached Git host");
    }
    check(host->hostCalls == 0 && host->createCalls == 0 && host->logCalls == 0,"denied call reached adapter");
    host->upstreamPermitted = false;
    check(invoke(store,runtime,script,"upstream-denied","git_repo_info",argsFor(Json::object(),repoName)).at("ok") == false &&
          host->hostCalls == 0,"upstream permission was ignored");
    host->upstreamPermitted = true;
    const auto find = invoke(store,runtime,script,"find","git_find_repos",{{"host",hostName},{"query","FLOCK"}});
    check(find.at("repos") == Json::array({repoName}),"repository discovery exposed ungranted repositories");
    auto foreign = argsFor(openArgs("foreign"),repoName);
    foreign["taskId"] = other;
    check(invoke(store,runtime,script,"foreign-task","git_open_pr",foreign).at("ok") == false &&
          host->createCalls == 0,"another Bot's task was mutated");
    auto cross = argsFor(openArgs("cross"),repoName);
    cross["headRepo"] = "fork/private";
    check(invoke(store,runtime,script,"cross-repo","git_open_pr",cross).at("ok") == false &&
          host->createCalls == 0,"ungranted fork was used");
    for (const auto& [tool,args] : commands) {
        const auto name = "granted-" + std::to_string(++counter);
        const auto result = invoke(store,runtime,script,name,tool,argsFor(args,repoName));
        check(result.at("ok") == true,"granted action failed: " + tool + " " + result.dump());
    }
    const auto duplicate = invoke(store,runtime,script,"retry-comment-create","git_comment",
                                  argsFor({{"target","pr"},{"number",1},{"body","Initial"}},repoName));
    check(duplicate.at("data").at("id") == 101 && host->comments == 1,"repeated comment creation posted a duplicate");
    auto edit = argsFor({{"target","pr"},{"number",1},{"body","Updated"},{"commentId",101}},repoName);
    const auto retry = invoke(store,runtime,script,"retry-edit","git_comment",edit);
    check(retry.at("data").at("id") == 101 && host->comments == 1 && host->edits == 2,"comment edit retry duplicated a comment");
    check(historyCount(store,task,"git_mutation") == 18 && historyCount(store,other,"git_mutation") == 0,
          "Git mutations were not recorded on the owning task");

    const auto review = invoke(store,runtime,script,"review-existing-pr","git_request_review",
                               argsFor({{"prNumber",20},{"reviewers",{"alice"}}},repoName));
    check(review.at("ok") == true && review.at("watchAttached") == true,"Review request did not watch the existing PR");
    check(store.receiveGitWebhook({hostName,"existing-pr-review",repoName,"review","submitted",{20},sha,"Approved",0}),
          "Could not enqueue a review for the existing PR");
    runtime.runConversations();
    check(script.notifications == 1 && historyCount(store,task,"git_webhook") == 1 &&
          historyCount(store,other,"git_webhook") == 0,"Review result did not go to the requesting task");
}

void newAttempts() {
    Directory directory;
    Store store(directory.path/"state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    grant(store,"owner","repository",repoGrant,"pr");
    const auto originalTask = store.submit(intake());
    auto args = openArgs();
    args["taskId"] = originalTask;
    host->create = FakeHost::Create::rejected;
    const auto failed = invoke(store,runtime,script,"definite-rejection","git_open_pr",args);
    check(failed.at("status") == "failed","Definite rejection was treated as unknown");
    host->create = FakeHost::Create::success;
    const auto created = invoke(store,runtime,script,"fresh-attempt","git_open_pr",args);
    check(created.at("prNumber") == 1 && created.at("operationId") != failed.at("operationId") && host->createCalls == 2,
          "A fresh request could not recover from a definitive rejection");

    auto comment = Json{{"taskId",originalTask},{"host",hostName},{"repo",repoName},
                        {"target","pr"},{"number",1},{"body","Update requested"}};
    host->rejectComment = true;
    check(invoke(store,runtime,script,"comment-rejected","git_comment",comment).at("status") == "failed",
          "Definite comment rejection was not recorded");
    host->rejectComment = false;
    check(invoke(store,runtime,script,"comment-retried","git_comment",comment).at("ok") == true && host->comments == 1,
          "A fresh comment request could not retry a definitive rejection");

    host->prs.front().state = FakeHost::State::merged;
    args["taskId"] = store.submit(intake());
    const auto next = invoke(store,runtime,script,"replacement-pr","git_open_pr",args);
    check(next.at("prNumber") == 2 && host->createCalls == 3,"Branch pair was locked after the previous task's PR merged");
}

void redaction() {
    Directory directory;
    const auto path = directory.path/"state.db";
    Store store(path);
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    const std::string credential = "fixture-pr-credential-value";
    const std::string secret = "fixture-protected-other-bot-value";
    store.protectGitCredential(credential);
    auto access = store.access();
    const auto input = access.requestSecret("other",{"example"});
    access.submitSecret(localOwner,"other",input,secret);
    grant(store,"owner","repository",repoGrant,"read");
    grant(store,"owner","repository",repoGrant,"pr");
    const auto task = store.submit(intake());
    host->result["description"] = credential + " " + secret;
    host->result[credential] = secret;
    const auto repo = Json{{"host",hostName},{"repo",repoName}};
    const auto details = invoke(store,runtime,script,"details","git_repo_info",repo).dump();
    check(details.find(secret) == details.npos && details.find(credential) == details.npos &&
          details.find("[redacted]") != details.npos,"Repository output exposed secrets");
    host->log = {"build error " + credential.substr(0,12),true};
    auto args = repo;
    args.update({{"jobId",81},{"maxBytes",32}});
    const auto log = invoke(store,runtime,script,"redacted-log","git_read_ci_logs",args).at("data");
    check(log.at("text") == "build error [redacted]" && log.at("truncated") == true,
          "Log byte limit exposed a partial credential");

    auto open = openArgs();
    open["taskId"] = task;
    check(invoke(store,runtime,script,"open-pr","git_open_pr",open).at("ok") == true,"Could not create watch");
    check(store.receiveGitWebhook({hostName,"secret-event",repoName,"review","submitted",{1},sha,credential + " " + secret,0}),
          "Webhook with redactable content was rejected");
    runtime.runConversations();
    for (const auto& content : {store.history(task).dump(),store.inbox(localOwner,"owner").dump()})
        check(content.find(secret) == content.npos && content.find(credential) == content.npos,
              "Webhook output exposed secrets");
    for (const auto& file : {path.string(),path.string()+"-wal"}) {
        std::ifstream database(file,std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(database)),{});
        check(bytes.find(secret) == bytes.npos && bytes.find(credential) == bytes.npos,"Raw webhook content was persisted");
    }
}

void notificationBudgetAndRevocation() {
    Directory directory;
    Store store(directory.path/"state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store,script,sessions,[](std::string_view) { return std::string{}; },options(host));
    grant(store,"owner","repository",repoGrant,"read");
    grant(store,"owner","repository",repoGrant,"pr");
    auto limited = intake();
    limited.config["modelCallLimit"] = 1;
    const auto task = store.submit(limited);
    auto args = openArgs();
    args["taskId"] = task;
    check(invoke(store,runtime,script,"create-watch","git_open_pr",args).at("ok") == true,"Could not create watch");
    check(store.receiveGitWebhook({hostName,"budget-event",repoName,"ci","completed",{1},sha,"A job failed",81}),
          "Could not enqueue job");
    runtime.runConversations();
    const auto inbox = store.inbox(localOwner,"owner");
    const auto approval = std::find_if(inbox.begin(),inbox.end(),[](const Json& row) {
        return row.at("itemKind") == "conversation_model_budget";
    });
    check(approval != inbox.end(),"The notification did not pause for model budget");
    store.decideInbox(localOwner,target(*approval),"approve");
    runtime.runConversations();
    const auto history = store.history(task);
    const auto event = std::find_if(history.begin(),history.end(),[](const Json& row) {
        return row.at("kind") == "git_webhook";
    });
    check(event != history.end() && event->at("data").at("jobId") == 81 &&
          event->at("data").at("headSha") == sha && event->at("data").at("prNumber") == 1,
          "Budget approval erased the Git delivery from task history");
    check(history.dump().find("CI failed. linker: missing symbol") != std::string::npos,"Approved model call did not resume");

    script.onNotification = [&] {
        store.access().resourceGrant(localOwner,"owner",{"repository",repoGrant,"read"},GrantChange::revoke);
    };
    const auto replies = historyCount(store,task,"conversation_reply");
    check(store.receiveGitWebhook({hostName,"revoked-turn",repoName,"comment","created",{1},sha,"An update",0}),
          "Could not enqueue new event");
    runtime.runConversations();
    check(script.notifications == 2 && historyCount(store,task,"conversation_reply") == replies,
          "A queued notification posted a reply after grant revocation");
    check(store.receiveGitWebhook({hostName,"revoked-event",repoName,"comment","created",{1},sha,"Another update",0}),
          "Could not enqueue event after revocation");
    runtime.runConversations();
    check(historyCount(store,task,"git_webhook") == 2,"Revoked grant received another task event");
}

void schemas() {
    ModelInput input{.task=Json::object(),.event={{"kind","conversation_turn"}},.history=Json::array(),
        .notes={},.tools=gitTools(),.feedback={},.evidence=Json::array(),.config=intake().config};
    const auto request = app::responseRequest(input);
    const auto functions = request.at("tools").at(0).at("tools");
    check(functions.size() == 11,"missing Git model schemas");
    for (const auto& function : functions) {
        const auto& schema = function.at("parameters");
        check(schema.at("additionalProperties") == false && schema.at("properties").contains("host"),
              "unscoped Git schema");
        if (function.at("name") == "git_read_ci_logs") {
            check(schema.at("properties").at("maxBytes").at("maximum") == 16384 &&
                  std::ranges::find(schema.at("required"),"jobId") != schema.at("required").end(),"incorrect log schema");
        }
        if (function.at("name") == "git_comment") {
            check(schema.at("properties").at("commentId").at("type") == "integer" &&
                  schema.at("properties").at("target").at("enum") == Json::array({"pr","issue"}),"incorrect comment schema");
        }
    }
}
}

int main() {
    try {
        freshPrReviewSchema();
        schemas();
        runner();
        webhookHeadPeriods();
        reviewAfterTaskProgress();
        reconciliationAfterTaskRevision();
        recovery(FakeHost::State::open,false);
        recovery(FakeHost::State::closed,false);
        recovery(FakeHost::State::merged,false);
        recovery(FakeHost::State::open,true);
        recovery(FakeHost::State::open,false,true);
        recovery(FakeHost::State::open,false,false,true);
        accessAndActions();
        newAttempts();
        redaction();
        notificationBudgetAndRevocation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
