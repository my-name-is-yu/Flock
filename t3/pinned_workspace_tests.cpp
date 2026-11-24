#include "internal.hpp"
#include "flock/policy.hpp"
#include "flock/t3_connection.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
void check(bool passed, std::string_view reason) {
    if (!passed) throw std::logic_error(std::string(reason));
}

struct ModelFixture : Model {
    Decision action;
    Json result;
    ModelTurn decide(const ModelInput& input) override {
        const auto& turn = input.event.at("turn");
        if (turn.empty()) return {{action}};
        result = turn.back().at("result");
        return {{{"reply",{{"text","Acknowledged"}},"Acknowledge the tool result"}}};
    }
};

struct WorkflowFixture {
    ModelFixture model;
    T3& adapter;
    Store store;
    Seconds now = unixTime();
    int sequence = 0;
    std::unique_ptr<Runtime> runtime;

    WorkflowFixture(T3& t3, const std::filesystem::path& db) : adapter(t3), store(db) {
        T3Connections(store).connect("env-1","owner",4321);
        restart();
    }

    void restart() {
        runtime.reset();
        RuntimeOptions options;
        options.clock = [&] { return now; };
        options.requireT3Connection = true;
        runtime = std::make_unique<Runtime>(store,model,adapter,[](auto) { return std::string{}; },options);
    }

    Json call(std::string tool, Json args, std::string environment = "env-1") {
        model.action = {std::move(tool),std::move(args),"Apply the operation"};
        model.result = nullptr;
        store.receive({"owner",true},{"bot","",std::move(environment),"project",Json::object()},
                      std::to_string(++sequence),"Workflow fixture operation");
        runtime->runConversations();
        return model.result;
    }

    Json status() { return store.workflows("bot").at(0); }
    void ticks(int count) {
        for (int i = 0; i < count; ++i) runtime->tick();
    }
};

void exerciseWorkflow(T3& adapter, const std::filesystem::path& directory,
                      const std::function<void(const Json&)>& fixture, const std::function<Json()>& stock) {
    for (const auto mode : {"recover", "stop-provisioned", "stop-before-receipt", "missing-proof", "stop-settled"}) {
        const std::string scenario = mode;
        const bool stopBeforeReceipt = scenario=="stop-before-receipt";
        const bool recover = scenario=="recover";
        fixture({{"resetStock",true},{"stockWorkflow",true},{"stockProvisioning",true},
                 {"stockLoseLaunchReceipt",recover || stopBeforeReceipt},{"stockMissingWorktree",false},
                 {"stockUnsettled",false},{"stockLosePromptReceipt",false},{"stockHoldPrompt",false}});
        WorkflowFixture f(adapter,directory/(scenario+".db"));
        const auto started = f.call("start_workflow",{{"request","Implement the fixture change"}});
        check(started.at("ok")==true,started.dump());
        f.ticks(5); // Implementation completes, then the verifier provisions a Thread.
        const auto verify = f.status().at("stages").at(1);
        const Id task = verify.at("taskId");
        const auto launches = stock().at("stockLaunches");
        check(task!=0 && launches.size()==2 && stock().at("stockPromptSends").empty(),
              "the verifier sent a prompt before the worktree was ready");
        const auto provisional = std::ranges::find_if(launches,[](const Json& launch) {
            return launch.at("response").at("runId").is_null();
        });
        check(provisional!=launches.end(),"a provision-only launch was not recorded");
        const auto thread = provisional->at("response").at("threadId");
        const auto key = std::string("project:")+provisional->at("payload").at("clientRequestId").get<std::string>();
        f.restart();
        f.now += 9;
        if (scenario=="stop-provisioned" || stopBeforeReceipt) {
            f.call("stop_workflow",{{"workflowId",started.at("workflowId")}});
            f.ticks(3);
            check(f.status().at("allStopped")==true && stock().at("stockPromptSends").empty() &&
                  stock().at("stockLaunches").at(key).at("replays")==0,
                  "stop before the first prompt dispatched a Run or replayed the empty launch");
            continue;
        }
        if (recover) {
            f.ticks(2);
            check(stock().at("stockLaunches").at(key).at("replays")==1 &&
                  stock().at("stockPromptSends").empty(),"lost provision receipt did not recover the exact idle Thread");
            f.restart();
            f.now += 9;
        }
        fixture({{"stockProvisioning",false},{"stockLosePromptReceipt",recover},{"stockHoldPrompt",recover}});
        f.ticks(1);
        if (recover) {
            f.restart();
            f.now += 5;
            f.ticks(1);
            check(f.store.task(task).at("activeRun").at("threadId")==thread &&
                  f.store.task(task).at("activeRun").at("runId")=="next-run",
                  "the first-prompt receipt was lost and the exact queued Run was not recovered");
            fixture({{"stockHoldPrompt",false}});
            f.ticks(1);
            const auto prompts = stock().at("stockPromptSends");
            check(f.status().at("status")=="completed" && f.status().at("verdicts").size()==1 &&
                  prompts.size()==1 && prompts.begin()->at("replays")==0,
                  "recovery replayed the first prompt or lost its exact verdict");
            continue;
        }
        check(f.store.task(task).at("activeRun").at("threadId")==thread,"verifier lost its provisioned Thread");
        const bool cancel = scenario=="stop-settled";
        fixture({{"stockMissingWorktree",true},{"stockUnsettled",true}});
        const Id next = f.store.submit({"bot","Next task","env-1","project",Json::object()});
        if (cancel) f.call("stop_workflow",{{"workflowId",started.at("workflowId")}});
        f.ticks(1);
        check(!f.store.task(task).at("activeRun").is_null() &&
              !f.store.task(next).at("blockedBy").is_null(),
              "missing worktree proof released outstanding delegated work");
        fixture({{"stockUnsettled",false}});
        f.ticks(1);
        check(f.store.task(task).at("activeRun").is_null(),"settled verifier stayed active without worktree proof");
        if (!cancel) {
            f.restart();
            f.now += 5;
            f.ticks(1);
            f.now += 10;
            f.ticks(1);
        }
        const auto status = f.status();
        const auto history = f.store.history(task);
        check(status.at("status")== (cancel ? "stopped" : "failed") && status.at("verdicts").empty() &&
              f.store.task(next).at("blockedBy").is_null() &&
              std::ranges::any_of(history,[&](const Json& entry) {
                  if (entry.at("kind")!="report") return false;
                  const auto& data = entry.at("data");
                  return data.at("evidenceStatus")=="report_unavailable" && data.at("resultRunId")=="next-run" &&
                         data.at("run").at("threadId")==thread;
              }),"missing worktree proof produced a verdict, lost attribution, or kept the project blocked");
    }
}

void rejectRemoteWorkflow(T3& adapter, const std::filesystem::path& directory,
                          const std::function<void(const Json&)>& fixture, const std::function<Json()>& stock) {
    fixture({{"resetStock",true}});
    const auto file = std::filesystem::directory_iterator(directory)->path();
    std::ifstream source(file);
    auto credential = Json::parse(source);
    credential["mcpUrl"] = "https://remote.invalid/mcp";
    std::ofstream(file) << credential.dump();
    for (const std::string environment : {"env-1",""}) {
        WorkflowFixture f(adapter,directory.parent_path()/(environment.empty() ? "sole.db" : "default.db"));
        const auto rejected = f.call("start_workflow",{{"request","Implement the fixture change"}},environment);
        check(rejected.at("ok")==false && rejected.at("error").get<std::string>().find("local T3")!=std::string::npos &&
              f.store.workflows("bot").empty() && f.store.tasks("bot").empty() && stock().at("stockLaunches").empty(),
              "remote workflow was admitted before verifying its destination");
        const auto task = f.call("start_task",{{"request","Start ordinary T3 work"}},environment);
        check(task.at("ok")==true && f.store.task(task.at("taskId")).at("environmentId")=="env-1",
              "the workflow locality rule rejected an ordinary remote task");
    }
}

}

int main(int argc, char** argv) {
    try {
        check(argc == 3,"expected fixture URL and credential directory");
        const std::string origin = argv[1];
        const std::filesystem::path directory = argv[2];
        auto fixture = [&](const Json& settings) {
            check(http(origin+"/fixture","POST",{{"Content-Type","application/json"}},settings.dump()).status == 200,
                  "fixture rejected configuration");
        };
        const auto commits = Json::parse(http(origin+"/fixture/stock","GET").body);
        const auto pin = commits.at("stockPin").get<std::string>();
        loginT3(directory,"env-1",origin+"/mcp",[&](const auto& auth) { fixture({{"authorization",auth}}); });
        const std::string environment = "env-1";
        auto adapter = makeT3Mcp(directory);
        const auto generation = adapter->authGeneration(environment,Json::object());
        Command command;
        command.actionId = "pinned-worker";
        command.tool = "delegate";
        command.environmentId = environment;
        command.projectId = "project";
        command.authGeneration = generation;
        command.instruction = "Check fixture";
        command.beginPinnedPrompt = [] { return true; };
        command.config["session"] = {{"runtimeMode","approval-required"},
            {"workspaceStrategy",{{"type","worktree"},{"baseRef",pin},{"startFromOrigin",false},{"expectedCommit",pin}}}};
        check(!adapter->preflight(command), "valid pinned specialist preflight");
        const auto provision = [&] {
            command.launchReceiptThreadId.clear();
            const auto receipt = adapter->send(command);
            check(receipt.kind == Delivery::Kind::Provisioned && receipt.run.runId.empty(),
                  "pinned launch did not return an idle Thread receipt");
            command.launchReceiptThreadId = receipt.run.threadId;
        };
        for (const auto mode : {"unstaged","staged"}) {
            fixture({{"stockTrackedEdit",mode}});
            command.actionId = std::string("dirty-") + mode;
            provision();
            const auto rejected = adapter->send(command);
            check(rejected.kind == Delivery::Kind::NotAccepted && rejected.definitelyUnsent,
                  "tracked " + std::string(mode) + " edit blocks the first specialist prompt");
            fixture({{"stockTrackedEdit","clean"}});
        }
        command.actionId = "pinned-worker";
        t3detail::Client provisionFixture(t3detail::loadCredential(directory,environment));
        const auto provisioned = provisionFixture.call("t3_thread_launch",{{"projectId",command.projectId},
            {"title","Flock task"},{"clientRequestId",command.actionId},{"runtimeMode","approval-required"},
            {"workspaceStrategy",{{"type","worktree"},{"baseRef",pin},{"startFromOrigin",false}}}});
        check(provisioned.at("threadId")=="session-"+command.actionId && provisioned.at("runId").is_null(),
              "fixture did not leave a provision-only launch for same-key recovery");
        provision();
        check(command.launchReceiptThreadId==provisioned.at("threadId").get<std::string>(),
              "same-key retry returned a different Thread");
        command.beginPinnedPrompt = [] { return false; };
        const auto stopped = adapter->send(command);
        check(stopped.kind==Delivery::Kind::Unknown &&
              Json::parse(http(origin+"/fixture/stock","GET").body).at("stockPromptSends").empty(),
              "T3 received a first prompt after its send claim was refused");
        command.beginPinnedPrompt = [] { return true; };
        const auto started = adapter->send(command);
        check(started.kind == Delivery::Kind::Accepted && started.run.threadId == command.launchReceiptThreadId &&
              started.run.runId == "next-run", "idle stock workspace starts only after its input pin matches");
        for (const auto mode : {"unstaged","staged"}) {
            fixture({{"stockTrackedEdit",mode}});
            const auto dirty = adapter->observe(environment,command.config,started.run);
            check(dirty.kind == Observation::Kind::Finished && dirty.status == "failed" && !dirty.report.empty(),
                  "tracked " + std::string(mode) + " edit did not produce terminal failed evidence");
            fixture({{"stockTrackedEdit","clean"}});
        }
        const auto report = adapter->observe(environment,command.config,started.run);
        check(report.kind == Observation::Kind::Finished && report.status == "completed" && report.report == "Pinned report",
              "provider report from the pinned workspace is accepted");
        fixture({{"stockProjectUnavailable",true}});
        const auto missing = adapter->observe(environment,command.config,started.run);
        check(missing.kind == Observation::Kind::Finished && missing.resultRunId == started.run.runId &&
              missing.evidence == RunEvidence::Availability::ReportUnavailable && missing.report.empty(),
              "an unavailable project lookup erased independently observed settlement");
        fixture({{"stockProjectUnavailable",false}});
        fixture({{"advanceStockWorker",true}});
        const auto changed = adapter->observe(environment,command.config,started.run);
        check(changed.kind == Observation::Kind::Finished && changed.status == "failed" && changed.report != "Pinned report",
              "settled specialist result is invalidated when the actual workspace drifted");
        auto recovery = command;
        recovery.launchReceiptThreadId = started.run.threadId;
        const auto recovered = adapter->reconcile(recovery);
        check(recovered.kind == Delivery::Kind::Accepted && recovered.run.runId == started.run.runId,
              "worktree drift prevented recovery of the exact Run for stopping");
        auto followUp = command;
        followUp.tool = "follow_up";
        followUp.actionId = "verify-rerun";
        followUp.target = started.run;
        const auto refused = adapter->send(followUp);
        check(refused.kind == Delivery::Kind::Settings && refused.definitelyUnsent &&
              refused.detail.find("changed from the saved commit") != std::string::npos,
              "follow-up did not reject worktree drift: " + refused.detail);
        command.actionId = "wrong-worker";
        provision();
        const auto wrong = adapter->send(command);
        check(wrong.kind == Delivery::Kind::NotAccepted && wrong.definitelyUnsent,
              "different actual worktree input blocks the first prompt");
        const auto effects = Json::parse(http(origin+"/fixture/effects","GET").body);
        const auto stock = Json::parse(http(origin+"/fixture/stock","GET").body);
        check(effects.at("t3_thread_send").size() == 1 && effects.at("t3_thread_launch").size() == 5 &&
              !effects.contains("t3_thread_interrupt") && stock.at("stockLaunches").size()==4 &&
              stock.at("stockLaunches").at("project:pinned-worker").at("replays")==1 &&
              stock.at("stockPromptSends").at("session-pinned-worker:pinned-worker:start").at("replays")==0,
              "same-key provision recovery created another thread, resent the first prompt, or sent to invalid work");

        const auto readStock = [&] { return Json::parse(http(origin+"/fixture/stock","GET").body); };
        exerciseWorkflow(*adapter,directory.parent_path(),fixture,readStock);
        rejectRemoteWorkflow(*adapter,directory,fixture,readStock);

        std::cout << "Pinned T3 workspace verification passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
