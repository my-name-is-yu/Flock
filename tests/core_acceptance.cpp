#include "flock/core.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <unistd.h>

using namespace flockv0;

namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
constexpr auto Evidence = "report and independent verification";

Decision work(const char* tool = "delegate", bool initial = true) {
    auto args = Json{{"instruction","Check the fixture and report"}};
    if (initial) args["doneWhen"] = Evidence;
    return {tool,args,"Need the fixture report"};
}
Decision complete() { return {"complete",{{"report","Verified the fixture"}},"The report includes the required evidence"}; }
Decision ask() { return {"ask_user",{{"question","Which branch?"}},"The branch needs user confirmation"}; }
Decision reply() {
    Json answers = {{"branch","main"}};
    return {"answer_session_question",{{"requestId","branch-question"},{"answers",answers}},"Use the branch specified by the user"};
}

struct Script : Model {
    std::function<Decision(const ModelInput&)> script;
    int calls = 0;
    Decision decide(const ModelInput& input) override { ++calls; return script(input); }
};

struct SimulatedT3 : T3 {
    int launches = 0, follows = 0, replies = 0, interrupts = 0, reconciles = 0, reads = 0, polls = 0;
    std::string generation = "same-oauth-session";
    bool loseAcknowledgement = false;
    bool hasReconciledRun = true;
    std::map<std::string,Observation> runs;
    std::map<std::string,RunRef> assignments;
    std::vector<Command> seen, recovery;
    std::string authGeneration(const std::string&,const Json&) override { return generation; }
    Delivery send(const Command& cmd) override {
        seen.push_back(cmd);
        check(cmd.authGeneration == generation,"OAuth generation missing from committed send");
        if (cmd.tool == "answer_session_question") {
            ++replies;
            check(cmd.questionId == "branch-question" && cmd.answers["branch"] == "main","wrong T3 answer");
            runs.at(cmd.target.runId) = {};
            if (std::exchange(loseAcknowledgement,false)) return {};
            return {Delivery::Kind::Accepted,{},{}};
        }
        check(cmd.instruction.find("[Flock action ID: " + cmd.actionId + "]") != std::string::npos,
              "missing full action marker");
        const auto serial = launches + follows + 1;
        RunRef run{"thread-" + std::to_string(launches + 1),"run-" + std::to_string(serial),"https://t3.test/thread"};
        if (cmd.tool == "delegate") ++launches;
        else {
            check(cmd.tool == "follow_up" && !cmd.target.runId.empty(),"invalid follow-up target");
            run.threadId = cmd.target.threadId;
            ++follows;
        }
        runs.emplace(run.runId,Observation{});
        check(assignments.emplace(cmd.actionId,run).second,"duplicated action");
        if (std::exchange(loseAcknowledgement,false)) return {};
        return {Delivery::Kind::Accepted,run,{}};
    }
    Delivery reconcile(const Command& cmd) override {
        ++reconciles;
        recovery.push_back(cmd);
        if (cmd.tool == "answer_session_question" && runs.at(cmd.target.runId).kind == Observation::Kind::Running)
            return {Delivery::Kind::Accepted,{},{}};
        if (!hasReconciledRun || !assignments.contains(cmd.actionId)) return {};
        return {Delivery::Kind::Accepted,assignments.at(cmd.actionId),{}};
    }
    Observation observe(const std::string&,const Json&,const RunRef& ref) override {
        ++polls;
        return runs.at(ref.runId);
    }
    void interrupt(const std::string&,const Json&,const RunRef& ref) override {
        check(runs.contains(ref.runId),"interrupted unrelated Run");
        ++interrupts;
    }
    Excerpt read(const ReadRequest& req) override {
        check(runs.contains(req.runId),"read unrelated Run");
        ++reads;
        return {reads == 1 ? "commit wanted, updates enabled" : "RSS 25 MiB",
                req.threadId,req.runId,"item-"+std::to_string(reads),"next",100,reads==1};
    }
    void question(std::string run) {
        runs.at(run).kind = Observation::Kind::Question;
        runs.at(run).question = {"branch-question",Json::array({{{"id","branch"},{"options",Json::array({"main","dev"})}}})};
    }
    void external(std::string run) {
        runs.at(run).kind = Observation::Kind::ExternalInput;
        runs.at(run).status = "Approval is waiting in T3";
    }
    void finish(std::string run, std::string status = "completed", bool settled = true) {
        auto& observation = runs.at(run);
        observation.kind = Observation::Kind::Finished;
        observation.status = std::move(status);
        observation.report = "checked";
        observation.settled = settled;
        observation.resultRunId = observation.status == "failed" ? run : run + "-result";
        observation.itemId = "final-answer";
    }
};

struct Fixture {
    std::filesystem::path directory;
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    SimulatedT3 t3;
    Script model;
    Seconds now = unixTime();
    std::string read = "off";
    std::map<std::string,BotNotes> notes{{"alpha",{"Initial SOUL","ALPHA_ONLY"}},{"beta",{"Second SOUL","BETA_ONLY"}}};

    Fixture() {
        char temp[] = "/tmp/flock-acceptance-XXXXXX";
        const auto* path = mkdtemp(temp);
        if (!path) throw std::runtime_error("Cannot create test directory");
        directory = path;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(directory);
    }
    void restart() {
        runtime.reset();
        store = std::make_unique<Store>(directory/"state.db");
        runtime = std::make_unique<Runtime>(*store,model,t3,[&](std::string_view id) {
            return notes.at(std::string(id));
        },RuntimeOptions{[&] { return now; },read});
    }
    Id submit(std::string request = "inspect", std::string bot = "alpha", std::string project = "p") {
        return store->submit({bot,request,"e",project,{{"runTimeoutSeconds",10}}});
    }
    Json task(Id id) { return store->task(id); }
    void reach(Id id, std::string state) {
        for (int i = 0; i < 12; ++i) {
            if (task(id)["state"] == Json(state)) return;
            runtime->tick();
        }
        throw std::runtime_error("Could not reach " + state + " from " + task(id)["state"].get<std::string>());
    }
    Id wait(Id id, const char* reason) {
        reach(id,"waiting");
        const auto value = task(id)["wait"];
        check(value["reason"] == reason,"wrong waiting reason");
        return value["id"].get<Id>();
    }
    void rejects(Id id, Id waitId, const char* operation, Json data=Json::object()) {
        try { store->operate(id,waitId,operation,data); }
        catch (const Conflict&) { return; }
        throw std::runtime_error("Expected stale or unsupported operation rejection");
    }
    int logs(Id id, std::string kind) {
        auto log = store->history(id,0,200);
        return static_cast<int>(std::count_if(log.begin(),log.end(),[&](const Json& j) {
            return j["kind"] == Json(kind);
        }));
    }
};

void invalidDecisions() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [&](const ModelInput& input) -> Decision {
        check(input.event["kind"] == "intake","wrong decision event");
        if (f.model.calls == 1) return complete();
        if (f.model.calls == 2) return {"invented",Json::object(),"unknown tool"};
        throw ModelError("The model returned two tool calls",false);
    };
    f.runtime->tick();
    f.runtime->tick();
    f.restart();
    const auto wait = f.wait(id,"model_failure");
    check(f.model.calls == 3 && f.t3.launches == 0 && f.logs(id,"decision") == 0,"invalid initial action executed");
    f.store->operate(id,wait,"retry");
    f.rejects(id,wait,"retry");
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(id,"running");
    f.t3.question("run-1");
    f.model.script = [](const ModelInput&) { return work(); };
    const auto questionWait = f.wait(id,"model_failure");
    check(f.t3.launches == 1 && f.t3.replies == 0 && f.task(id)["doneWhen"] == Evidence,"invalid in-question action executed");
    f.store->operate(id,questionWait,"retry");
    f.model.script = [](const ModelInput&) { return reply(); };
    f.reach(id,"running");
    f.t3.finish("run-1");
    f.model.script = [](const ModelInput&) { return work(); };
    f.wait(id,"model_failure");
    check(f.t3.launches == 1 && f.task(id)["doneWhen"] == Evidence,"doneWhen overwritten");
}

void ambiguityAndFailure() {
    Fixture f;
    const auto id = f.submit("Unclear branch");
    const auto next = f.submit("Queued", "beta");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == next) {
            check(input.notes.memory == "BETA_ONLY" && input.history.empty(),"Other Bot or task data leaked");
            return ask();
        }
        check(input.notes.memory == "ALPHA_ONLY","wrong initial memory");
        if (input.event["kind"] == "intake" && !input.event.contains("userAnswer")) return ask();
        if (input.event["kind"] == "intake") {
            check(input.event["userAnswer"] == "main","lost clarification");
            return work();
        }
        check(input.task["doneWhen"] == Evidence && input.notes.soul == "Edited SOUL","did not reload notes or preserve conditions");
        if (input.event["status"] == "failed") return work("follow_up",false);
        return complete();
    };
    const auto wait = f.wait(id,"user_question");
    check(f.t3.launches == 0 && f.task(next)["state"] == "queued" && f.task(next)["blockedBy"]["id"] == id,
          "ambiguous task delegated or did not hold project order");
    f.restart();
    f.store->operate(id,wait,"answer",{{"text","main"}});
    f.rejects(id,wait,"answer",{{"text","dev"}});
    f.reach(id,"running");
    f.notes.at("alpha").soul = "Edited SOUL";
    f.t3.finish("run-1","failed");
    f.restart();
    f.reach(id,"deciding");
    f.reach(id,"running");
    check(f.t3.follows == 1 && f.t3.seen.back().target.runId == "run-1","did not follow up after failure");
    f.t3.finish("run-2");
    f.reach(id,"completed");
    f.wait(next,"user_question");
    check(f.logs(id,"report") == 2 && f.logs(id,"user_answer") == 1,"bad task history");
}

void deadlineApprovalAndCancel() {
    Fixture f;
    const auto id = f.submit();
    const auto queued = f.submit("Next");
    const auto independent = f.submit("Independent","beta","other");
    f.model.script = [=](const ModelInput& input) {
        return input.task["id"] == id ? work() : ask();
    };
    f.reach(id,"running");
    f.wait(independent,"user_question");
    f.t3.external("run-1");
    f.wait(id,"t3_input");
    const auto calls = f.model.calls;
    f.runtime->tick();
    check(f.model.calls == calls && f.t3.interrupts == 0 && f.t3.launches == 1,"automatic work during T3 approval");
    f.t3.runs.at("run-1") = {};
    f.reach(id,"running");
    f.now += 20;
    f.wait(id,"deadline");
    f.runtime->tick();
    check(f.model.calls == calls && f.t3.interrupts == 0 && f.t3.launches == 1,"automatic work at deadline");
    f.store->operate(id,0,"cancel");
    f.runtime->tick();
    check(f.t3.interrupts == 1 && f.task(id)["state"] == "cancelling" && f.task(queued)["state"] == "queued",
          "cancel released project before confirmation");
    f.restart();
    f.t3.finish("run-1","interrupted",false);
    f.runtime->tick();
    check(f.task(id)["state"] == "cancelling" && f.t3.interrupts == 1,"cancel ignored delegated work");
    f.t3.finish("run-1","interrupted");
    f.reach(id,"cancelled");
    f.wait(queued,"user_question");
    check(f.t3.launches == 1 && f.logs(id,"report") == 1,"cancel re-delivered work");
}

void budgetAndStaleWaits() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [&](const ModelInput& input) {
        if (input.event["kind"] == "intake") return work();
        if (input.event["kind"] == "question") return reply();
        const auto used = input.task["additionalUsed"].get<int>();
        if (used == 3 && !input.event.contains("userAnswer") && f.logs(id,"user_answer")==0) return ask();
        return work(used == 0 || used == 2 ? "follow_up" : "delegate",false);
    };
    f.reach(id,"running");
    for (int i = 1; i <= 3; ++i) {
        f.t3.finish("run-" + std::to_string(i));
        f.reach(id,"deciding");
        f.reach(id,"running");
    }
    check(f.t3.launches == 2 && f.t3.follows == 2 && f.task(id)["remaining"] == 0,"wrong shared work budget");
    f.t3.finish("run-4");
    const auto question = f.wait(id,"user_question");
    f.rejects(id,question,"approve");
    f.store->operate(id,question,"answer",{{"text","keep going"}});
    const auto budget = f.wait(id,"budget");
    f.rejects(id,budget,"answer",{{"text","yes"}});
    check(f.task(id)["remaining"] == 0 && f.t3.seen.size()==4,"normal answer increased budget or fourth work sent");
    f.store->operate(id,budget,"approve");
    f.rejects(id,budget,"approve");
    f.store->operate(id,0,"cancel");
    f.reach(id,"cancelled");
    f.rejects(id,budget,"approve");
    f.runtime->tick();
    check(f.task(id)["remaining"] == 1 && f.logs(id,"budget_granted") == 1 && f.t3.seen.size() == 4,
          "replayed permit or late work after cancel");
}

void sessionQuestions() {
    for (const bool needsUser : {false,true}) {
        Fixture f;
        const auto id = f.submit(needsUser ? "check code" : "check main");
        f.model.script = [&](const ModelInput& input) -> Decision {
            if (input.event["kind"] == "intake") return work();
            if (input.event["kind"] == "question") {
                if (needsUser && !input.event.contains("userAnswer")) return ask();
                return reply();
            }
            if (input.task["additionalUsed"] == 0) return work("follow_up",false);
            return complete();
        };
        f.reach(id,"running");
        f.t3.question("run-1");
        f.reach(id,"deciding");
        if (needsUser) {
            const auto wait = f.wait(id,"user_question");
            f.restart();
            f.store->operate(id,wait,"answer",{{"text","main"}});
        }
        f.reach(id,"running");
        check(f.t3.replies==1 && f.t3.launches==1 && f.t3.follows==0 &&
              f.task(id)["additionalUsed"]==0 && f.logs(id,"user_answer")==needsUser,"wrong branch reply behavior");
        f.t3.finish("run-1");
        f.reach(id,"deciding");
        f.reach(id,"running");
        check(f.t3.follows==1 && f.task(id)["additionalUsed"]==1,"follow-up did not add one Run and one unit");
        f.t3.finish("run-2");
        f.reach(id,"completed");
    }
}

void waitsExpireWithReality() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) {
        if (i.event["kind"] == "intake") return work();
        if (i.event["kind"] == "question") return ask();
        return complete();
    };
    f.reach(id,"running");
    f.t3.question("run-1");
    const auto question = f.wait(id,"user_question");
    f.t3.runs.at("run-1") = {};
    f.reach(id,"running");
    f.rejects(id,question,"answer",{{"text","main"}});
    f.now += 20;
    const auto deadline = f.wait(id,"deadline");
    f.t3.finish("run-1");
    f.reach(id,"deciding");
    f.rejects(id,deadline,"extend",{{"deadline",f.now+100}});
    f.reach(id,"completed");
    check(f.t3.replies==0 && f.logs(id,"user_answer")==0 && f.t3.seen.size()==1,
          "stale waiting operation executed");
}

void recoveredDelivery() {
    for (const bool launch : {true,false}) {
        Fixture f;
        const auto id = f.submit();
        f.model.script = [](const ModelInput& i) {
            if (i.event["kind"] == "intake") return work();
            if (i.task["additionalUsed"] == 0) return work("follow_up",false);
            return complete();
        };
        if (!launch) {
            f.reach(id,"running");
            f.t3.finish("run-1");
        }
        f.reach(id,"sending");
        f.restart();
        f.t3.loseAcknowledgement = true;
        auto waiting = f.wait(id,"unknown_delivery");
        f.restart();
        f.t3.generation = "fresh-oauth-session";
        f.t3.hasReconciledRun = false;
        f.store->operate(id,waiting,"reconcile");
        waiting = f.wait(id,"unknown_delivery");
        check(f.t3.seen.size() == (launch ? 1ul : 2ul),"unmatched delivery was resent");
        f.t3.hasReconciledRun = true;
        f.store->operate(id,waiting,"reconcile");
        f.reach(id,"running");
        check(f.t3.reconciles == 2 && f.t3.recovery.back().authGeneration == "same-oauth-session",
              "recovery lost original authentication generation");
        f.restart();
        if (launch) {
            f.t3.finish("run-1");
            f.reach(id,"deciding");
            f.reach(id,"running");
        }
        f.t3.finish("run-2");
        f.reach(id,"completed");
        f.runtime->tick();
        check(f.t3.launches==1 && f.t3.follows==1 && f.logs(id,"report")==2,"duplicate Run or report");
    }
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) { return i.event["kind"] == "intake" ? work() : reply(); };
    f.reach(id,"running");
    f.t3.question("run-1");
    f.reach(id,"sending");
    f.t3.loseAcknowledgement = true;
    const auto wait = f.wait(id,"unknown_delivery");
    f.restart();
    f.store->operate(id,wait,"reconcile");
    f.reach(id,"running");
    check(f.t3.replies==1 && f.t3.launches==1 && f.t3.reconciles==1 && f.task(id)["additionalUsed"]==0,
          "recovered answer created duplicate work");
}

void evidenceResume() {
    Fixture f;
    f.read = "on_demand";
    f.restart();
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) {
        if (i.event["kind"] == "intake") return work();
        if (i.evidence.size()==2) return complete();
        check(std::find(i.tools.begin(),i.tools.end(),"session_read") != i.tools.end(),"read tool not presented");
        auto req = Json{{"threadId",i.event["run"]["threadId"]},{"runId",i.event["run"]["runId"]},{"view","activity"}};
        if (!i.evidence.empty()) req["cursor"] = i.evidence.back()["nextCursor"];
        return Decision{"session_read",req,"Check the commit and setting"};
    };
    f.reach(id,"running");
    f.t3.finish("run-1");
    f.reach(id,"deciding");
    f.runtime->tick();
    check(f.t3.reads==1 && f.task(id)["additionalUsed"]==0 && f.logs(id,"decision")==1 &&
          f.task(id)["state"]=="deciding","read committed a terminal action");
    f.restart();
    f.reach(id,"completed");
    check(f.t3.reads==2 && f.t3.launches==1 && f.t3.follows==0 && f.logs(id,"session_read")==2,
          "history read did not resume its budget");
}

void inferenceInvalidatedByCancel() {
    Fixture f;
    const auto id = f.submit();
    std::mutex mutex;
    std::condition_variable ready;
    bool inModel = false, cancelled = false;
    f.model.script = [&](const ModelInput&) {
        std::unique_lock lock(mutex);
        inModel = true;
        ready.notify_one();
        ready.wait(lock,[&] { return cancelled; });
        return work();
    };
    f.runtime->tick();
    std::thread deciding([&] { f.runtime->tick(); });
    {
        std::unique_lock lock(mutex);
        ready.wait(lock,[&] { return inModel; });
        Store cli(f.directory/"state.db");
        cli.operate(id,0,"cancel");
        cancelled = true;
        ready.notify_one();
    }
    deciding.join();
    f.reach(id,"cancelled");
    check(f.t3.launches == 0 && f.logs(id,"decision") == 0,"cancelled inference committed");
}
}

int main() {
    for (const auto& [name,run] : std::vector<std::pair<const char*, void(*)()>>{
            {"invalid decisions",invalidDecisions},{"clarification and failure",ambiguityAndFailure},
            {"deadline, approval and cancel",deadlineApprovalAndCancel},{"additional work budget",budgetAndStaleWaits},
            {"branch questions",sessionQuestions},{"stale waits",waitsExpireWithReality},
            {"delivery recovery",recoveredDelivery},{"history reading restart",evidenceResume},
            {"stale in-flight model decision",inferenceInvalidatedByCancel}}) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
    }
}
