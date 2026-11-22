#include "fake_git.hpp"
#include "model.hpp"

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

    Decision decide(const ModelInput& input) override {
        auto request = app::responseRequest(input);
        auto next = choose(input);
        next.args["reason"] = "Use the verified fixture evidence";
        const Json call{{"type","function_call"},{"name",next.tool},{"arguments",next.args.dump()}};
        return app::responseDecision(Json::array({call}),request);
    }

private:
    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "intake")
            return {"delegate",{{"instruction","Push change"},{"doneWhen","Open a PR for the pushed branch"}}, {},{}};
        if (event.at("kind") == "result") {
            const auto steps = event.value("gitResults",Json::array());
            if (openAfterSession && steps.empty()) return {"git_open_pr",openArgs(),{},{}};
            if (!steps.empty()) {
                results["session-pr"] = steps.at(0).at("result");
                check(results["session-pr"].value("ok",false),"task PR open failed: " + results["session-pr"].dump());
            }
            return {"complete",{{"report","Change prepared and checked"}},{},{}};
        }
        const auto& steps = event.at("turn");
        if (event.contains("gitEvent")) {
            const auto& gitEvent = event.at("gitEvent");
            if (steps.empty()) {
                ++notifications;
                check(gitEvent.at("run").at("threadId") == "thread-project","Webhook lost its originating Session");
                check(std::ranges::find(input.tools,"git_open_pr") == input.tools.end(),"Webhook supplied mutation tools");
                if (gitEvent.at("jobId") == 81)
                    return {"git_read_ci_logs",{{"host",gitEvent.at("host")},{"repo",gitEvent.at("repo")},
                        {"jobId",gitEvent.at("jobId")},{"maxBytes",32}},{},{}};
                return {"reply",{{"text","Git notification received."}},{},{}};
            }
            results["log"] = steps.at(0).at("result");
            const auto& log = results.at("log").at("data");
            check(log.at("truncated") == true && log.at("text").get<std::string>().size() == 32,
                  "CI excerpt is not explicitly bounded");
            return {"reply",{{"text","CI failed. " + log.at("text").get<std::string>()}},{},{}};
        }
        const auto name = event.at("text").get<std::string>();
        if (steps.empty()) return calls.at(name);
        results[name] = steps.at(0).at("result");
        return {"reply",{{"text","Git tool result recorded"}},{},{}};
    }
};

Json invoke(Store& store, Runtime& runtime, Script& model, std::string id, std::string tool, Json args) {
    model.calls.insert_or_assign(id,Decision{std::move(tool),std::move(args),{},{}});
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

void runner() {
    Directory directory;
    const auto path = directory.path/"state.db";
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Script script;
    script.openAfterSession = true;
    Id task, other;
    {
        Store store(path);
        grant(store,"owner","environment","sandbox","launch");
        grant(store,"owner","repository",repoGrant,"pr");
        grant(store,"owner","repository",repoGrant,"read");
        task = store.submit(intake());
        other = store.submit(intake("other","other-project"));
        Runtime runtime(store,script,sessions,[](std::string_view) { return BotNotes{}; },options(host));
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
        Runtime runtime(store,script,sessions,[](std::string_view) { return BotNotes{}; },options(host));
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
    }
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
    {
        Store store(path);
        task = store.submit(intake());
        grant(store,"owner","repository",repoGrant,"pr");
        Runtime runtime(store,script,sessions,[](std::string_view) { return BotNotes{}; },options(host));
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
    }
    host->inspect = {};
    const auto repo = GitRepository::parse(hostName,repoName);
    host->prs.push_back({{{repo,repo,"main","unrelated"},4,"https://github.com/org/flock/pull/4",operationId,sha,true},
                        "Change",FakeHost::State::open});
    host->prs.push_back({{{repo,repo,"old","change"},5,"https://github.com/org/flock/pull/5",operationId,sha,false},
                        "Change",FakeHost::State::closed});
    if (wrongCorrelation) host->prs.front().pr.operationId = "someone-elses-operation";
    if (multipleMatches) {
        auto extra = host->prs.front();
        extra.pr.number = 6;
        extra.pr.url = "https://github.com/org/flock/pull/6";
        host->prs.push_back(std::move(extra));
    }
    {
        Store store(path);
        Runtime runtime(store,script,sessions,[](std::string_view) { return BotNotes{}; },options(host));
        auto args = openArgs();
        args["taskId"] = task;
        const auto recovered = invoke(store,runtime,script,"retry","git_open_pr",args);
        check(host->createCalls == 1 && host->lookupCalls == 1,"retry attempted a second create");
        if (losesRequest || multipleMatches || wrongCorrelation) {
            check(recovered.at("status") == "ambiguous" && !recovered.contains("prNumber"),"unverified PR was adopted");
            const auto items = store.inbox(localOwner,"owner");
            check(items.size() == 1 && items.at(0).at("itemKind") == "pr_reconciliation","reconciliation item missing or duplicated");
            store.decideInbox(localOwner,target(items.at(0)),"acknowledge");
            check(store.inbox(localOwner,"owner").empty() && persistence(path,operationId).at("status") == "ambiguous",
                  "acknowledging reconciliation asserted an unverified identity");
        } else {
            check(recovered.at("status") == "created" && recovered.at("prNumber") == 1 &&
                  recovered.at("url") == "https://github.com/org/flock/pull/1" &&
                  recovered.at("operationId") == operationId && persistence(path,operationId).at("prNumber") == 1,
                  "the original PR was not recovered");
            check(store.inbox(localOwner,"owner").dump().find("pr_reconciliation") == std::string::npos,
                  "resolved reconciliation still pending");
            check(store.board(localOwner,"owner").at(0).at("prRefs").at(0).at("url") == recovered.at("url"),
                  "recovered pull request is missing from the task board");
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
    Runtime runtime(store,script,sessions,[](std::string_view) { return BotNotes{}; },options(host));
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
    auto readOnly = openArgs();
    readOnly["taskId"] = task;
    check(invoke(store,runtime,script,"read-is-not-pr","git_open_pr",readOnly).at("ok") == false &&
          host->createCalls == 0,"repository read allowed PR creation");
    const auto deniedIssueComment = invoke(store,runtime,script,"read-is-not-issues","git_comment",
        {{"host",hostName},{"repo",repoName},{"taskId",task},{"target","issue"},{"number",2},{"body","Comment"}});
    check(deniedIssueComment.at("ok") == false && host->hostCalls == 0,"read access allowed an issue comment");
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
        schemas();
        runner();
        recovery(FakeHost::State::open,false);
        recovery(FakeHost::State::closed,false);
        recovery(FakeHost::State::merged,false);
        recovery(FakeHost::State::open,true);
        recovery(FakeHost::State::open,false,true);
        recovery(FakeHost::State::open,false,false,true);
        accessAndActions();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
