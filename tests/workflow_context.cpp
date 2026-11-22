#include "flock/core.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <unistd.h>

using namespace flockv0;

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

struct SessionService : T3 {
    std::vector<Command> commands;
    std::set<std::string> interrupts;
    std::map<std::string,Observation> observations;
    std::map<std::string,RunRef> runs;
    bool loseReceipt = false;

    std::string authGeneration(const std::string&,const Json&) override { return "test"; }
    Delivery send(const Command& command) override {
        commands.push_back(command);
        auto suffix = std::to_string(commands.size());
        RunRef run{command.tool=="delegate" ? "thread-"+suffix : command.target.threadId,"run-"+suffix,"link-"+suffix};
        require(runs.emplace(command.actionId,run).second,"duplicate T3 dispatch");
        if (std::exchange(loseReceipt,false)) return {Delivery::Kind::Unknown,{},{}};
        return {Delivery::Kind::Accepted,run,{}};
    }
    Delivery reconcile(const Command& command) override { return {Delivery::Kind::Accepted,runs.at(command.actionId),{}}; }
    Observation observe(const std::string&,const Json&,const RunRef& run) override { return observations[run.runId]; }
    void interrupt(const std::string&,const Json&,const RunRef& run) override { interrupts.insert(run.runId); }
    void finish(std::string run, Json report, std::string status = "completed") {
        auto& observation = observations[run];
        observation.kind = Observation::Kind::Finished;
        observation.status = std::move(status);
        observation.settled = true;
        observation.report = report.dump();
    }
};

struct Bot : Model {
    Decision action;
    Json history, result;
    Decision decide(const ModelInput& input) override {
        if (input.event.at("kind")!="conversation_turn")
            throw std::runtime_error("Unexpected task decision: "+input.event.dump());
        history = input.history;
        if (input.event.at("turn").empty() && !action.tool.empty()) return action;
        if (!input.event.at("turn").empty()) result = input.event.at("turn").back().at("result");
        return {"reply",{{"text","Received"}},"Acknowledge the turn"};
    }
};

struct Fixture {
    std::filesystem::path dir;
    Bot bot;
    SessionService t3;
    int sequence = 0;
    Seconds now = unixTime();
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    Fixture() {
        char name[] = "/tmp/flock-workflow-context-XXXXXX";
        if (!mkdtemp(name)) throw std::runtime_error("Cannot create test directory");
        dir = name;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(dir);
    }
    void restart() {
        runtime.reset();
        store.reset();
        store = std::make_unique<Store>(dir/"state.db");
        RuntimeOptions options;
        options.clock = [this]{return now;};
        runtime = std::make_unique<Runtime>(*store,bot,t3,[](auto){return BotNotes{};},options);
    }
    Id turn(std::string text, std::string tool = {}, Json args = Json::object()) {
        bot.action = {tool,args,"Execute the requested operation"};
        bot.result = nullptr;
        auto receipt = store->receive({"bot","","env","project",Json::object(),{},0},std::to_string(++sequence),text);
        runtime->runConversations();
        return receipt.at("messageId");
    }
    Json call(std::string tool, Json args) {
        turn(tool,tool,std::move(args));
        require(bot.result.is_object() && bot.result.value("ok",false),tool+" failed: "+bot.result.dump());
        return bot.result;
    }
    Json status() { return store->workflows("bot").at(0); }
    void ticks(int count) { while (count-->0) runtime->tick(); }
};

const std::string shaA(40,'a'), shaB(40,'b');

void verifyAfterRestart(bool advance) {
    Fixture f;
    const Id id = f.call("start_workflow",{{"request","Implement feature"},{"verificationCriteria","Run tests"}}).at("workflowId");
    f.ticks(2);
    require(f.t3.commands.size()==1,"expected single implementation Session");
    f.t3.finish("run-1",{{"commitSha",shaA}});
    f.ticks(1);
    require(f.status().at("stages")[1]["status"]=="pending","verification dispatched prematurely");
    f.restart();
    f.ticks(3);
    auto status = f.status();
    require(f.t3.commands.size()==2 && f.t3.commands.back().tool=="delegate" &&
        f.t3.commands.back().instruction.find(shaA)!=std::string::npos &&
        status["stages"][1]["inputArtifactVersion"]==shaA,"lost pin or reran implementation");
    if (advance) {
        f.call("instruct_workflow_stage",{{"workflowId",id},{"stageId","implement"},{"instruction","Revise implementation"}});
        f.ticks(3);
        require(f.t3.commands.size()==3 && f.t3.commands.back().target.threadId=="thread-1","wrong revision Session");
        f.t3.finish("run-3",{{"artifactVersion",shaB}});
        f.ticks(1);
        require(f.status()["stages"][0]["outputArtifactVersion"]==shaB,"lost new version");
    }
    f.t3.finish("run-2",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(2);
    status = f.call("workflow_status",{{"workflowId",id}});
    require(status["stages"][1]["verified"]==!advance && status["verdicts"].size()==1 &&
        status["verdicts"][0]["artifactVersion"]==shaA && status["verdicts"][0]["verdict"]=="pass" &&
        status["status"]==(advance ? "needs_verification" : "completed"),"incorrect versioned verification");
    if (advance) {
        f.call("instruct_workflow_stage",{{"workflowId",id},{"stageId","verify"},{"instruction","Check the revised artifact"}});
        f.ticks(2);
        require(f.t3.commands.back().instruction.find(shaB)!=std::string::npos,"new verification was given an obsolete pin");
        f.t3.finish("run-4",{{"artifactVersion",shaB},{"verdict","pass"}});
        f.ticks(1);
        status = f.status();
        require(status["status"]=="completed" && status["stages"][1]["verified"]==true &&
            status["verdicts"].size()==2 && status["verdicts"][0]["artifactVersion"]==shaA &&
            status["verdicts"][1]["artifactVersion"]==shaB,"new verification overwrote old evidence");
    }
}

void rejectInvalidGraphAndArtifact() {
    Fixture f;
    f.turn("Cycle","start_workflow",{{"request","Invalid cycle"},{"stages",Json::array({
        {{"stageId","a"},{"role","implement"},{"dependsOn",{"b"}}},
        {{"stageId","b"},{"role","verify"},{"dependsOn",{"a"}}}})}});
    require(f.bot.result.at("ok")==false && f.store->workflows("bot").empty(),"partial cyclic workflow was saved");
    f.call("start_workflow",{{"request","Implement feature"}});
    f.ticks(2);
    f.t3.finish("run-1",{{"artifactVersion","main"}});
    f.ticks(3);
    const auto status = f.status();
    require(status.at("artifacts").empty() && status["stages"][0]["status"]=="failed" &&
            status["stages"][1]["taskId"]==0 && f.t3.commands.size()==1,"mutable output reached verification");
}

void stopAndDeadline() {
    Fixture f;
    auto started = f.call("start_workflow",{{"request","Check independent subsystems"},{"softDeadlineSeconds",1},
        {"stages",Json::array({{{"stageId","one"},{"role","collect"}},{{"stageId","two"},{"role","collect"}},
            {{"stageId","three"},{"role","combine"},{"dependsOn",{"one","two"}}}})}});
    const auto id = started.at("workflowId");
    f.ticks(2);
    require(f.t3.commands.size()==2,"expected two concurrent Sessions");
    ++f.now;
    f.ticks(1);
    require(f.status()["stages"][0]["deadlineExceeded"]==true && f.status()["stages"][0]["needsAttention"]==true &&
        f.status()["stages"][0]["status"]=="running" && f.t3.interrupts.empty(),"deadline interrupted a Session");
    require(f.call("stop_workflow",{{"workflowId",id}})["allStopped"]==false,"request claimed stop confirmation");
    f.ticks(1);
    require(f.t3.interrupts==std::set<std::string>{"run-1","run-2"},"missing individual stop request");
    f.t3.finish("run-1",Json::object(),"cancelled");
    f.t3.observations["run-2"].kind = Observation::Kind::Unavailable;
    f.ticks(2);
    auto status = f.status();
    require(status["allStopped"]==false && status["stages"][1]["status"]=="unreachable" &&
        status["stages"][1]["lastConfirmedState"]=="running","unreachable Session marked stopped");
    f.restart();
    require(f.status()==status,"stop state lost on restart");
    f.t3.finish("run-2",Json::object(),"cancelled");
    f.ticks(3);
    require(f.status()["allStopped"]==true && f.status()["status"]=="stopped" && f.t3.commands.size()==2,
        "stop did not settle or downstream stage started");
}

void uncertainLaunchAndStageStop() {
    Fixture f;
    f.t3.loseReceipt = true;
    const auto id = f.call("start_workflow",{{"request","New implementation"}}).at("workflowId");
    f.ticks(2);
    require(f.status()["stages"][0]["status"]=="unreachable","unknown delivery reported as confirmed");
    f.restart();
    f.ticks(1);
    require(f.status()["stages"][0]["runRef"]["runId"]=="run-1" && f.t3.commands.size()==1,
            "ambiguous launch was duplicated or lost");
    f.call("stop_workflow_stage",{{"workflowId",id},{"stageId","implement"}});
    f.ticks(1);
    require(f.t3.interrupts.contains("run-1") && f.status()["stages"][0]["status"]=="stop_requested",
            "stage interrupt acknowledgement treated as terminal");
    f.t3.finish("run-1",Json::object(),"cancelled");
    f.ticks(2);
    require(f.status()["stages"][0]["status"]=="stopped" && f.t3.commands.size()==1,"dependent of stopped stage ran");
}

void compactAndContinue() {
    Fixture f;
    const std::string original = "Keep original instruction and artifact "+shaA;
    const auto first = f.turn(original);
    const auto end = f.turn("Evidence "+std::string(3400,'x'));
    f.turn("Newer direction");
    const auto compact = f.call("compact_conversation",{{"endMessageId",end},{"summary","Keep the tested revision."}});
    f.restart();
    f.turn("Inspect history");
    require(f.bot.history.front().at("compactionId")==compact["compaction"]["compactionId"] &&
        f.bot.history.front().at("startMessageId")==first && f.bot.history.front().at("endMessageId")==end &&
        f.bot.history.dump().find("Newer direction")!=std::string::npos &&
        f.bot.history.dump().find(original)==std::string::npos,"model history does not respect compaction");
    require(f.store->conversation("bot").dump().find(original)!=std::string::npos,"audit history was lost");
    auto expanded = f.call("expand_context",{{"query",shaA},{"limit",100}});
    require(expanded.at("messages").size()==1 && expanded["messages"][0]["text"]==original,"missing original evidence");
    expanded = f.call("expand_context",{{"compactionId",compact["compaction"]["compactionId"]},{"startMessageId",end}});
    require(expanded.dump().size()<=8192 && expanded.at("nextOffset")>0 && expanded.at("nextMessageId")==end,
        "unbounded expansion or missing cursor");
    f.turn("Invalid boundary","compact_conversation",{{"endMessageId",first},{"summary","Discard coverage"}});
    require(f.bot.result.at("ok")==false,"accepted shrinking boundary");

    f.call("start_workflow",{{"request","Implement feature"}});
    f.ticks(2);
    f.t3.finish("run-1",{{"commitSha",shaA}});
    f.ticks(3);
    const Id task = f.status()["stages"][1]["taskId"];
    auto before = f.store->task(task);
    const auto history = f.store->history(task);
    f.call("continue_fresh_session",{{"taskId",task},{"instruction","Repeat checks with fresh context"}});
    f.ticks(3);
    require(f.t3.commands.size()==2 && f.t3.interrupts.contains("run-2"),"fresh handoff bypassed settlement");
    f.t3.finish("run-2",Json::object(),"cancelled");
    f.ticks(6);
    auto after = f.store->task(task);
    require(after["doneWhen"]==before["doneWhen"] && after["currentRevision"]==before["currentRevision"] &&
        after["inputArtifactVersion"]==shaA && after["activeRun"]["threadId"]=="thread-3" &&
        f.t3.commands.back().tool=="delegate" && f.t3.commands.back().instruction.find(shaA)!=std::string::npos &&
        f.store->history(task,0,static_cast<unsigned>(history.size()))==history,"fresh continuation lost task provenance");
}
}

int main() {
    try {
        verifyAfterRestart(false);
        verifyAfterRestart(true);
        rejectInvalidGraphAndArtifact();
        stopAndDeadline();
        uncertainLaunchAndStageStop();
        compactAndContinue();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
