#include "flock/board_inbox.hpp"
#include "internal.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <sqlite3.h>
#include <thread>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
constexpr auto bot = "release";
constexpr auto request = "Verify the release fixture";
constexpr auto holdLocal = "Hold the local project";
constexpr auto threadId = "shared-thread";
constexpr auto runId = "shared-run";

void check(bool pass, std::string_view detail) {
    if (!pass) throw std::runtime_error(std::string(detail));
}

struct Endpoint {
    std::string name, origin;

    std::string id() const { return "env-" + name; }
    std::string project() const { return "project-" + name; }
    std::string link() const { return origin + "/" + id() + "/" + threadId; }

    void configure(const Json& settings) const {
        check(http(origin+"/fixture","POST",{{"Content-Type","application/json"}},settings.dump()).status == 200,
              "could not configure the " + name + " fixture");
    }

    Json effects(const char* tool) const {
        const auto response = http(origin+"/fixture/effects","GET");
        check(response.status == 200,"could not read the " + name + " effects");
        return Json::parse(response.body).value(tool,Json::array());
    }

    void login(const std::filesystem::path& directory) const {
        const auto signedIn = loginT3(directory,id(),origin+"/mcp",[&](const auto& authorization) {
            configure({{"authorization",authorization}});
        });
        check(signedIn.environmentId == id() && signedIn.operate,"did not connect " + name);
    }
};

struct Script final : Model {
    explicit Script(std::string sha) : commit(std::move(sha)) {}

    std::string commit;
    Json startResult;
    bool parallel = true;

    ModelTurn decide(const ModelInput& input) override {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") {
            const auto& turn = input.event.at("turn");
            if (turn.empty() && input.event.value("text","") == request) {
                Json args{{"request",request}};
                if (parallel) {
                    args["environments"] = Json::array({"local","mini"});
                    if (!commit.empty()) args["commit"] = commit;
                }
                return {{{"start_task",std::move(args),"Run the release verification"}}};
            }
            if (!turn.empty()) startResult = turn.back().at("result");
            return {{{"reply",{{"text","I have the fixture request."}},"End this turn"}}};
        }
        if (kind == "intake" && input.task.at("request") == holdLocal)
            return {{{"ask_user",{{"question","Can the project be released?"}},"Hold the local slot"}}};
        if (kind == "intake")
            return {{{"delegate",{{"instruction",request},{"doneWhen","The checks have a result"}},"Run the checks"}}};
        if (kind == "result")
            return {{{"complete",{{"report",input.event.at("report")}},"Return the checks"}}};
        throw std::logic_error("unexpected model event: " + input.event.dump());
    }
};

struct Scenario {
    const Endpoint& local;
    const Endpoint& mini;
    Script model;
    Store store;
    std::unique_ptr<T3> adapter;
    std::atomic<Seconds> now{unixTime()};
    Runtime runtime;
    Id taskId = 0;

    Scenario(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory,
             const std::string& commit, std::string_view name)
        : local(local), mini(mini), model(commit), store(directory.parent_path()/(std::string(name)+".db")),
          adapter(makeT3Mcp(directory)), runtime(store,model,*adapter,{},RuntimeOptions{[&] { return now.load(); }}) {}

    void start(bool parallel = true, bool loseMiniReply = false) {
        model.parallel = parallel;
        local.configure({{"multiReset",true},{"multiSingle",!parallel},{"multiCommit",model.commit}});
        mini.configure({{"multiReset",true},{"multiHoldLaunchReply",loseMiniReply},{"multiCommit",model.commit}});
        const Intake routing{bot,{},local.id(),local.project(),{{"conversationEnvironments",{
            {"local",{{"environmentId",local.id()},{"projectId",local.project()}}},
            {"mini",{{"environmentId",mini.id()},{"projectId",mini.project()}}}
        }}}};
        store.receive(localOwner,routing,"start-fixture",request);
        runtime.runConversations();
        check(model.startResult.is_object() && model.startResult.value("ok",false),
              "conversation could not start the verification: " + model.startResult.dump());
        taskId = model.startResult.at("taskId");
    }

    Json task() { return store.task(taskId); }

    static Json result(const Json& task, const Endpoint& endpoint) {
        for (const auto& run : task.value("runResults",Json::array()))
            if (run.value("environmentId","") == endpoint.id()) return run;
        return Json::object();
    }

    template<class Predicate>
    void advanceUntil(Predicate done, std::string_view detail) {
        using namespace std::chrono_literals;
        const auto end = std::chrono::steady_clock::now() + 8s;
        for (int tick = 0; std::chrono::steady_clock::now() < end; ++tick) {
            const auto snapshot = task();
            if (done(snapshot)) return;
            if (tick % 10 == 0) ++now;
            runtime.tickConcurrent();
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error(std::string(detail) + ": " + task().dump());
    }

    void awaitBothRunning() {
        advanceUntil([&](const Json& snapshot) {
            return result(snapshot,local).value("status","") == "running" &&
                   result(snapshot,mini).value("status","") == "running";
        },"both environments must start before either Run settles");
        checkLaunches();
    }

    void checkLaunches() {
        const auto localLaunches = local.effects("t3_thread_launch");
        const auto miniLaunches = mini.effects("t3_thread_launch");
        check(localLaunches.size() == 1 && miniLaunches.size() == 1 &&
              localLaunches.at(0).at("clientRequestId") != miniLaunches.at(0).at("clientRequestId") &&
              local.effects("t3_thread_send").empty() && mini.effects("t3_thread_send").empty(),
              "each environment must accept exactly one distinct action");
    }

    void checkResult(const Endpoint& endpoint, const Json& outcome, std::string_view status,
                     std::string_view commit) {
        check(outcome.at("name") == endpoint.name && outcome.at("projectId") == endpoint.project() &&
              outcome.at("status") == status && outcome.at("commit") == commit &&
              outcome.at("threadId") == threadId && outcome.at("runId") == runId && outcome.at("link") == endpoint.link(),
              "incorrect per-environment Run outcome: " + outcome.dump());
        if (status == "completed")
            check(outcome.at("report") == endpoint.id()+" verification completed","a success report was lost");
    }

    void cancel() { store.operate(localOwner,TaskTarget::from(task(),0),"cancel"); }
};

void normal(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.mini.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },"both Runs should complete");
    const auto task = fixture.task();
    check(task.at("runResults").size() == 2 && task.at("runOutcome") == "all_completed" &&
          fixture.store.tasks(bot).size() == 1,
          "the original request must contain both outcomes");
    fixture.checkResult(fixture.local,Scenario::result(task,fixture.local),"completed",fixture.model.commit);
    fixture.checkResult(fixture.mini,Scenario::result(task,fixture.mini),"completed",fixture.model.commit);
}

void ordinaryMulti(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local,mini,directory,{},"ordinary-multi");
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.mini.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },
                         "ordinary multi-environment work should complete without a requested commit");
    const auto task = fixture.task();
    check(!task.contains("requestedCommit") && task.at("runOutcome") == "all_completed" &&
          task.at("runResults").size() == 2,"ordinary work must aggregate both destinations without SHA policy");
    fixture.checkResult(fixture.local,Scenario::result(task,fixture.local),"completed","");
    fixture.checkResult(fixture.mini,Scenario::result(task,fixture.mini),"completed","");
}

void independentSlots(Scenario& fixture) {
    const auto blocker = fixture.store.submit({"owner",holdLocal,fixture.local.id(),fixture.local.project()});
    fixture.start();
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,fixture.mini).value("status","") == "running";
    },"the free route must start while the other project is blocked");
    check(fixture.store.task(blocker).at("state") == "waiting" &&
          fixture.local.effects("t3_thread_launch").empty() &&
          Scenario::result(fixture.task(),fixture.local).at("status") == "queued",
          "the blocked Run was dispatched before its project became free");
    fixture.mini.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,fixture.mini).value("status","") == "completed";
    },"the free route must settle independently of the blocked sibling");
    fixture.store.operate(localOwner,TaskTarget::from(fixture.store.task(blocker),0),"cancel");
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,fixture.local).value("status","") == "running";
    },"the waiting route must launch once its project is free");
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },
                         "the batch must complete once both Runs settle");
    fixture.checkLaunches();
}

void deadlines(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    const auto started = fixture.task();
    const auto local = Scenario::result(started,fixture.local);
    const auto mini = Scenario::result(started,fixture.mini);
    fixture.now = std::max(local.at("deadline").get<Seconds>(),mini.at("deadline").get<Seconds>()) + 1;
    fixture.advanceUntil([](const Json& task) {
        const auto& wait = task.at("wait");
        return wait.is_object() && wait.value("reason","") == "deadline";
    },"an expired Run must notify the user");
    const auto waiting = fixture.task();
    const auto wait = waiting.at("wait");
    const bool localExpiredFirst = wait.at("runActionId") == local.at("actionId");
    const auto& overdue = localExpiredFirst ? fixture.local : fixture.mini;
    const auto& sibling = localExpiredFirst ? fixture.mini : fixture.local;
    check(wait.at("run").at("environmentId") == overdue.id() && wait.at("run").at("link") == overdue.link(),
          "the deadline must point to its own Run and environment");
    sibling.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,sibling).value("status","") == "completed";
    },"a deadline wait must not stop another Run from settling");
    const auto stillWaiting = fixture.task();
    const auto waitId = wait.at("id").get<Id>();
    check(stillWaiting.at("wait").at("id") == waitId && stillWaiting.at("generation") != waiting.at("generation"),
          "the sibling did not advance the task while preserving the original deadline");
    const auto extension = fixture.now.load() + 120;
    fixture.store.operate(localOwner,TaskTarget::from(waiting,waitId),"extend",{{"deadline",extension}});
    check(fixture.task().at("wait").is_null() &&
          Scenario::result(fixture.task(),overdue).at("deadline") == extension, "extending changed the wrong deadline");
    overdue.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },
                         "the extended Run must finish normally");
    fixture.checkLaunches();
}

void partial(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    const auto follower = fixture.store.submit({bot,"Queued behind the local verification",fixture.local.id(),fixture.local.project()});
    check(fixture.store.task(follower).at("blockedBy").at("id") == fixture.taskId,
          "the active Run should hold its project slot");
    fixture.mini.configure({{"multiDisconnected",true}});
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,fixture.local).value("status","") == "completed" &&
               Scenario::result(task,fixture.mini).value("status","") == "confirmation_pending";
    },"disconnected Mini must not suppress the local report");
    auto task = fixture.task();
    const auto success = Scenario::result(task,fixture.local);
    check(task.at("state") != "completed","a disconnected Run is not terminal");
    fixture.checkResult(fixture.local,success,"completed",fixture.model.commit);
    const auto unblocked = fixture.store.task(follower);
    check(unblocked.at("blockedBy").is_null(),"an unrelated task should proceed in the settled local project's queue");
    fixture.store.operate(localOwner,TaskTarget::from(unblocked,0),"cancel");
    fixture.mini.configure({{"multiDisconnected",false},{"multiStatus","failed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },"both outcomes should settle");
    task = fixture.task();
    check(task.at("runOutcome") == "partial_failure" && Scenario::result(task,fixture.local) == success,
          "a later Mini failure changed the local success or the aggregate outcome");
    fixture.checkResult(fixture.mini,Scenario::result(task,fixture.mini),"failed",fixture.model.commit);
    fixture.checkLaunches();
}

void mismatch(Scenario& fixture) {
    constexpr auto otherCommit = "fedcba9876543210fedcba9876543210fedcba98";
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.mini.configure({{"multiStatus","completed"},{"multiActualCommit",otherCommit}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },"both outcomes should settle");
    const auto task = fixture.task();
    check(task.at("runOutcome") == "partial_failure","a different verified commit must fail the Mini check");
    fixture.checkResult(fixture.local,Scenario::result(task,fixture.local),"completed",fixture.model.commit);
    fixture.checkResult(fixture.mini,Scenario::result(task,fixture.mini),"failed",otherCommit);
}

void commitEvidence(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    const Json config{{"commitVerification",{{"projectId",fixture.local.project()},
                                            {"expectedCommit",fixture.model.commit}}}};
    const RunRef run{threadId,runId,fixture.local.link()};
    for (const auto* mode : {"missing-post","duplicate-pre","unmarked","reversed","foreign-post",
                             "failed-post","truncated-post","post-then-work","missing-report",
                             "foreign-report","empty-final-report"}) {
        fixture.local.configure({{"multiStatus","completed"},{"multiEvidence",mode}});
        const auto observation = fixture.adapter->observe(fixture.local.id(),config,run);
        check(observation.kind == Observation::Kind::Finished && observation.status == "completed" &&
              observation.checkedCommit.empty() && observation.commitEvidenceUnavailable,
              std::string("incomplete or unattributed commit evidence was accepted: ") + mode);
    }
    const std::string wrongCommit(40,'e');
    fixture.local.configure({{"multiEvidence","rechecked-mismatch"},{"multiActualCommit",wrongCommit}});
    check(fixture.adapter->observe(fixture.local.id(),config,run).checkedCommit == wrongCommit,
          "a later matching check must not overwrite an attributed mismatch");
    auto legacy = config;
    legacy["commitVerification"]["markedChecks"] = false;
    fixture.local.configure({{"multiEvidence","unmarked"},{"multiActualCommit",fixture.model.commit}});
    check(fixture.adapter->observe(fixture.local.id(),legacy,run).checkedCommit == fixture.model.commit,
          "a saved unmarked prompt still needs two distinct Git commands");
    fixture.local.configure({{"multiEvidence","unmarked-missing-post"}});
    check(fixture.adapter->observe(fixture.local.id(),legacy,run).checkedCommit.empty(),
          "legacy recovery must require the final command");
    fixture.local.configure({{"multiEvidence","truncated-report"}});
    const auto longReport = fixture.adapter->observe(fixture.local.id(),config,run);
    check(longReport.truncated && longReport.checkedCommit == fixture.model.commit && !longReport.commitEvidenceUnavailable,
          "truncating the written report must preserve separately verified commit evidence");
    fixture.local.configure({{"multiEvidence","restarted"}});
    const auto observation = fixture.adapter->observe(fixture.local.id(),config,run);
    check(observation.kind == Observation::Kind::Finished && observation.status == "completed" &&
          observation.checkedCommit == fixture.model.commit && !observation.commitEvidenceUnavailable &&
          observation.resultRunId == "continued-run",
          "a pre-check and a post-check in the attributable restart must establish the commit");
}

void supervised(Scenario& fixture) {
    fixture.store.policy().setMode(localOwner,"supervised");
    fixture.start();
    auto card = [&](const Endpoint& endpoint) {
        for (const auto& item : fixture.store.inbox(localOwner,bot))
            if (item.at("target") == endpoint.id() && item.at("status") == "pending") return item;
        return Json(nullptr);
    };
    auto approve = [&](const Json& item) {
        fixture.store.decideInbox(localOwner,{item.at("botId"),InboxKind::policy_approval,
            item.at("itemId"),item.at("waitId"),item.at("taskId"),item.at("targetRevision"),
            item.at("targetGeneration")},"allow_once");
    };
    fixture.advanceUntil([&](const Json&) { return !card(fixture.local).is_null() && !card(fixture.mini).is_null(); },
                         "both dispatches should request separate approvals");
    check(fixture.local.effects("t3_thread_launch").empty() && fixture.mini.effects("t3_thread_launch").empty(),
          "a Run started before its own approval");
    const auto miniCard = card(fixture.mini);
    approve(card(fixture.local));
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,fixture.local).value("status","") == "running";
    },"the approved local Run should start while the Mini still awaits approval");
    check(fixture.mini.effects("t3_thread_launch").empty() &&
          fixture.task().at("generation") > miniCard.at("targetGeneration"),
          "one approval launched both Runs or failed to advance the request");
    approve(miniCard);
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.mini.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },"both approved Runs should settle");
    check(fixture.task().at("runOutcome") == "all_completed","approving both Runs must preserve the verification results");
}

void stop(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    fixture.mini.configure({{"multiDisconnected",true},{"multiHoldSettlement",true}});
    fixture.local.configure({{"multiHoldSettlement",true}});
    fixture.cancel();
    fixture.advanceUntil([&](const Json& task) {
        return fixture.local.effects("t3_thread_interrupt").size() == 1 &&
               Scenario::result(task,fixture.mini).value("status","") == "confirmation_pending";
    },"the connected Run should be interrupted while the disconnected Run awaits confirmation");
    check(fixture.task().at("state") == "cancelling" && fixture.mini.effects("t3_thread_interrupt").empty(),
          "stop released a disconnected Run or interrupted the wrong environment");
    fixture.local.configure({{"multiHoldSettlement",false}});
    fixture.mini.configure({{"multiDisconnected",false}});
    fixture.advanceUntil([&](const Json&) {
        return fixture.mini.effects("t3_thread_interrupt").size() == 1;
    },"the Mini should be interrupted after reconnecting");
    check(fixture.task().at("state") == "cancelling","stop released work before the delegated settlement");
    fixture.mini.configure({{"multiHoldSettlement",false}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "cancelled"; },
                         "the original request should cancel after both Runs settle");
    const auto task = fixture.task();
    check(Scenario::result(task,fixture.local).at("status") == "interrupted" &&
          Scenario::result(task,fixture.mini).at("status") == "interrupted",
          "the original request should retain each stop outcome");
    const auto interrupts = Json::array({{{"threadId",threadId},{"runId",runId}}});
    check(fixture.local.effects("t3_thread_interrupt") == interrupts &&
          fixture.mini.effects("t3_thread_interrupt") == interrupts,
          "stop must target exactly the assigned Run in each saved environment");
    fixture.checkLaunches();
}

void single(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory,
            const std::string& commit, bool stopEarly) {
    const std::string name = stopEarly ? "single-stop" : "single";
    Id taskId;
    {
        Scenario fixture(local,mini,directory,commit,name);
        fixture.start(false);
        fixture.advanceUntil([](const Json& task) { return task.at("state") == "running"; },"single Run did not start");
        taskId = fixture.taskId;
    }
    {
        const auto path = directory.parent_path()/(name+".db");
        sqlite3* connection = nullptr;
        const auto opened = sqlite3_open_v2(path.c_str(),&connection,SQLITE_OPEN_READWRITE,nullptr);
        const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(connection,sqlite3_close);
        check(opened == SQLITE_OK,"could not open the fixture database");
        const auto saved = sqlite3_exec(database.get(),R"SQL(
UPDATE outbox SET data=json_remove(data,'$.projectId');
UPDATE tasks SET data=json_remove(data,'$.activeRun.projectId');
)SQL",nullptr,nullptr,nullptr);
        check(saved == SQLITE_OK,"could not prepare a legacy single Run");
    }
    Scenario fixture(local,mini,directory,commit,name);
    fixture.taskId = taskId;
    if (stopEarly) fixture.cancel();
    else fixture.local.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([&](const Json& task) {
        return task.at("state") == (stopEarly ? "cancelled" : "completed");
    },"single Run did not settle");
    check(fixture.local.effects("t3_thread_launch").size() == 1 &&
          fixture.mini.effects("t3_thread_launch").empty() &&
          fixture.local.effects("t3_thread_interrupt").size() == (stopEarly ? 1u : 0u),
          "existing start_task must use only the default environment");
}

void recover(Scenario& fixture) {
    const auto originals = fixture.store.tasks(bot);
    check(originals.size() == 1,"a restart must keep the original request");
    fixture.taskId = originals.at(0).at("id");
    fixture.mini.configure({{"multiHideLaunch",true}});
    fixture.advanceUntil([&](const Json& task) {
        const auto local = Scenario::result(task,fixture.local);
        const auto mini = Scenario::result(task,fixture.mini);
        return local.value("status","") == "running" && local.contains("deadline") &&
               mini.value("status","") == "confirmation_pending" && mini.contains("deadline");
    },"recovered assigned and unconfirmed Runs both need deadlines");
    fixture.local.configure({{"multiStatus","completed"}});
    fixture.advanceUntil([&](const Json& task) {
        return Scenario::result(task,fixture.local).value("status","") == "completed" &&
               Scenario::result(task,fixture.mini).value("status","") == "confirmation_pending";
    },"only the Mini should await confirmation after restart");
    check(Scenario::result(fixture.task(),fixture.mini).value("runId","").empty(),
          "Flock attributed an unconfirmed launch");
    fixture.checkLaunches();
    const auto pending = Scenario::result(fixture.task(),fixture.mini);
    fixture.now = pending.at("deadline").get<Seconds>() + 1;
    fixture.advanceUntil([&](const Json& task) {
        const auto& wait = task.at("wait");
        return wait.is_object() && wait.at("reason") == "deadline" && wait.at("runActionId") == pending.at("actionId");
    },"a lost launch receipt must surface its deadline during reconciliation");
    fixture.mini.configure({{"multiHideLaunch",false},{"multiStatus","completed"}});
    fixture.advanceUntil([](const Json& task) { return task.at("state") == "completed"; },
                         "restart must adopt the saved Mini Run");
    const auto task = fixture.task();
    fixture.checkResult(fixture.local,Scenario::result(task,fixture.local),"completed",fixture.model.commit);
    fixture.checkResult(fixture.mini,Scenario::result(task,fixture.mini),"completed",fixture.model.commit);
    fixture.checkLaunches();
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 6,"expected two fixture URLs, credential directory, commit and phase");
        const Endpoint local{"local",argv[1]}, mini{"mini",argv[2]};
        const std::filesystem::path directory = argv[3];
        const std::string commit = argv[4];
        const std::string_view phase = argv[5];
        if (phase == "normal") {
            local.login(directory);
            mini.login(directory);
            { Scenario fixture(local,mini,directory,commit,"normal"); normal(fixture); }
            { Scenario fixture(local,mini,directory,commit,"independent"); independentSlots(fixture); }
            { Scenario fixture(local,mini,directory,commit,"deadlines"); deadlines(fixture); }
            { Scenario fixture(local,mini,directory,commit,"partial"); partial(fixture); }
            { Scenario fixture(local,mini,directory,commit,"mismatch"); mismatch(fixture); }
            { Scenario fixture(local,mini,directory,commit,"commit-evidence"); commitEvidence(fixture); }
            { Scenario fixture(local,mini,directory,commit,"supervised"); supervised(fixture); }
            { Scenario fixture(local,mini,directory,commit,"stop"); stop(fixture); }
            ordinaryMulti(local,mini,directory);
            single(local,mini,directory,commit,false);
            single(local,mini,directory,commit,true);
        } else {
            Scenario fixture(local,mini,directory,commit,"recovery");
            if (phase == "crash") {
                fixture.start(true,true);
                fixture.advanceUntil([](const Json&) { return false; },"expected Flock to be killed before launch reply");
            } else {
                check(phase == "recover","unknown fixture phase");
                recover(fixture);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
