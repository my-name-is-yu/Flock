#include "flock/core.hpp"
#include "flock/access.hpp"

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
    bool loseReceipt = false, receiptVisible = true;
    std::function<void()> onObserve;

    std::string authGeneration(const std::string&,const Json&) override { return "test"; }
    Delivery send(const Command& command) override {
        if (command.tool=="answer_session_question") return {Delivery::Kind::NotAccepted,{},"Answer definitely unsent",true};
        commands.push_back(command);
        auto suffix = std::to_string(commands.size());
        RunRef run{command.tool=="delegate" ? "thread-"+suffix : command.target.threadId,"run-"+suffix,"link-"+suffix};
        require(runs.emplace(command.actionId,run).second,"duplicate T3 dispatch");
        if (std::exchange(loseReceipt,false)) return {Delivery::Kind::Unknown,{},{}};
        return {Delivery::Kind::Accepted,run,{}};
    }
    Delivery reconcile(const Command& command) override {
        if (!receiptVisible) return {};
        const auto sent = std::ranges::find(commands,command.actionId,&Command::actionId);
        require(sent!=commands.end() && sent->instruction==command.instruction,"reconciliation changed the dispatched instruction");
        return {Delivery::Kind::Accepted,runs.at(command.actionId),{}};
    }
    Observation observe(const std::string&,const Json&,const RunRef& run) override {
        if (auto callback = std::exchange(onObserve,{})) callback();
        return observations[run.runId];
    }
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
    std::function<Decision(const ModelInput&)> conversation, work;
    Decision decide(const ModelInput& input) override {
        if (input.event.at("kind")=="question")
            return {"answer_session_question",{{"requestId","q"},{"answers",{{"choice","yes"}}}},"Answer the Session"};
        if (input.event.at("kind")!="conversation_turn" && work) return work(input);
        if (input.event.at("kind")!="conversation_turn")
            throw std::runtime_error("Unexpected task decision: "+input.event.dump());
        history = input.history;
        if (conversation) return conversation(input);
        if (!action.tool.empty())
            require(std::ranges::find(input.tools,action.tool)!=input.tools.end(),"tool missing from conversation runtime");
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
    explicit Fixture(bool grantLaunch = true) {
        char name[] = "/tmp/flock-workflow-context-XXXXXX";
        if (!mkdtemp(name)) throw std::runtime_error("Cannot create test directory");
        dir = name;
        restart();
        if (grantLaunch)
            store->access().resourceGrant({"owner",true},"bot",{"environment","env","launch"},GrantChange::grant);
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
    void ticks(int count) { for (int i = 0; i < count; ++i) runtime->tick(); }
};

const std::string shaA(40,'a'), shaB(40,'b');

void workflowLaunchUsesGrantsAndRedactsQueuedPrompt() {
    Fixture f(false);
    auto access = f.store->access();
    const Person owner{"owner",true};
    const auto grant = access.request("bot",{"environment","env","launch"});
    const std::string secret = "workflow-credential-registered-after-queueing";
    f.call("start_workflow",{{"request","Collect the release notes " + secret},{"stages",Json::array({
        {{"stageId","collect"},{"role","collect"}}})}});
    f.ticks(2);
    const Id task = f.status()["stages"][0]["taskId"];
    const auto wait = f.store->task(task).at("wait");
    require(wait.at("reason")=="settings" && f.t3.commands.empty(),"unapproved workflow launch reached T3");
    const auto input = access.requestSecret("bot",{"workflow-credential"});
    access.submitSecret(owner,"bot",input,secret);
    access.decide(owner,"bot",grant,true);
    f.store->operate(task,wait.at("id"),"reconcile");
    f.t3.loseReceipt = true;
    f.ticks(1);
    require(f.t3.commands.size()==1 && f.t3.commands[0].instruction.find(secret)==std::string::npos &&
            f.t3.commands[0].instruction.find("Collect the release notes [redacted]")!=std::string::npos,
            "the approved workflow prompt included newly protected text or lost the stage request");
    f.now += 5;
    f.ticks(2);
    require(f.t3.commands.size()==1 && f.store->task(task).at("state")=="running",
            "the redacted workflow launch was not reconciled");
}

void expansionRedactsBeforeSearchAndPaging() {
    Fixture f;
    const std::string secret = "secret-value-for-compaction";
    const std::string prefix(1995,'x');
    const auto original = f.turn(prefix+secret+" tail");
    const auto compacted = f.call("compact_conversation",{{"endMessageId",original},{"summary","Notes: "+secret}});
    const auto compaction = compacted.at("compaction").at("compactionId");
    auto access = f.store->access();
    access.submitSecret({"owner",true},"bot",access.requestSecret("bot",{"context-credential"}),secret);
    const auto first = f.call("expand_context",{{"compactionId",compaction},{"startMessageId",original}});
    const auto second = f.call("expand_context",{{"compactionId",compaction},{"startMessageId",first.at("nextMessageId")},
                                                {"offset",first.at("nextOffset")}});
    require(first.at("messages").at(0).at("text").get<std::string>()+second.at("messages").at(0).at("text").get<std::string>()
            ==prefix+"[redacted] tail","paging split the protected value before redaction");
    require(f.bot.history.front().at("summary")=="Notes: [redacted]","protected value reached the compacted model history");
    require(f.call("expand_context",{{"compactionId",compaction},{"query","value-for-compaction"}}).at("messages").empty(),
            "expansion search matched a fragment of the protected value");
}

void freshStandaloneSessionIncludesEarlierEvidence() {
    Fixture f;
    f.call("start_workflow",{{"request","Compare the options"},{"stages",Json::array({{{"stageId","compare"},{"role","collect"}}})}});
    f.ticks(2);
    const Id sourceTask = f.status()["stages"][0]["taskId"];
    f.t3.finish("run-1","Option B must preserve the retry window");
    f.ticks(1);
    const auto history = f.store->history(sourceTask);
    const auto report = std::ranges::find_if(history,[](const Json& item) { return item.at("kind")=="report"; });
    require(report!=history.end(),"fixture did not save the source report");
    f.bot.work = [](const ModelInput& input) -> Decision {
        return {input.event.at("kind")=="intake" ? "delegate" : "follow_up",
                {{"instruction","Continue implementing"}},"Work remains"};
    };
    const Id task = f.call("start_task",{{"request","Implement the chosen option"},{"doneWhen","Tests pass"},
                                        {"sourceResultMessageId",report->at("id")}}).at("taskId");
    f.ticks(3);
    f.t3.finish("run-2","Earlier Run identified a timeout");
    f.ticks(3);
    f.t3.finish("run-3","Second Run found a duplicate entry");
    f.ticks(3);
    f.call("continue_fresh_session",{{"taskId",task}});
    f.t3.finish("run-4","Latest report: "+std::string(9000,'z'));
    f.ticks(5);
    const auto& next = f.t3.commands.back();
    require(f.t3.commands.size()==5 && next.tool=="delegate" &&
            next.instruction.find("Option B must preserve the retry window")!=std::string::npos &&
            next.instruction.find("Latest report:")!=std::string::npos &&
            next.instruction.find("Second Run found a duplicate entry")!=std::string::npos &&
            next.instruction.find("Earlier Run identified a timeout")!=std::string::npos,
            "fresh Session lost the linked result or reports from earlier Runs");
}

void stageRequestKeepsEmbeddedNul() {
    Fixture f;
    std::string request(1,'\0');
    request += "Compare the bytes after the marker";
    f.call("start_workflow",{{"request","Inspect the binary marker"},{"stages",Json::array({
        {{"stageId","inspect"},{"role","collect"},{"request",request}}})}});
    f.ticks(2);
    const auto task = f.store->task(f.status()["stages"][0]["taskId"]);
    require(task["request"]==request && task["intent"]==request && task["doneWhen"]==request,
            "stage request changed before the first revision was saved");
    require(f.t3.commands.size()==1 &&
            f.t3.commands.front().instruction.find("Task intent:\n"+request+"\n\nCompletion criteria:\n"+request)!=std::string::npos,
            "stage dispatch lost the request bytes after the marker");
}

void verifyAfterRestart(bool advance) {
    Fixture f;
    const Id id = f.call("start_workflow",{{"request","Implement feature"},{"verificationCriteria","Run tests"}}).at("workflowId");
    f.ticks(2);
    require(f.t3.commands.size()==1,"expected single implementation Session");
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(1);
    require(f.status().at("stages")[1]["status"]=="pending","verification dispatched prematurely");
    f.restart();
    f.ticks(3);
    auto status = f.status();
    require(f.t3.commands.size()==2 && f.t3.commands.back().tool=="delegate" &&
        f.t3.commands.back().instruction.find(shaA)!=std::string::npos &&
        f.t3.commands.back().instruction.find("Implement feature")!=std::string::npos &&
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

void reviseQueuedVerifier() {
    Fixture f;
    f.call("start_workflow",{{"request","Preserve duplicate rows"},{"verificationCriteria","Run smoke tests"}});
    f.ticks(2);
    const Id workflow = f.status().at("workflowId");
    const auto blocker = f.call("start_workflow",{{"request","Collect logs"},
        {"stages",Json::array({{{"stageId","logs"},{"role","collect"}}})}}).at("workflowId");
    f.ticks(1);
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(3);
    const auto blocked = f.call("workflow_status",{{"workflowId",workflow}});
    const auto task = f.store->task(blocked["stages"][1]["taskId"]);
    require(task["state"]=="queued" && task["blockedBy"]["id"]==f.status()["stages"][0]["taskId"],
            "verifier was not queued behind the other workflow");
    f.call("change_task",{{"taskId",task["id"]},{"generation",task["generation"]},{"revisionId",task["currentRevision"]},
        {"doneWhen","Run the duplicate-row regression"},{"instruction","Use the revised criteria."}});
    f.t3.finish("run-2","Logs collected");
    f.ticks(4);
    require(f.status()["workflowId"]==blocker && f.status()["status"]=="completed" && f.t3.commands.size()==3,
            "verifier did not start when the project slot became available");
    const auto& assignment = f.t3.commands.back().instruction;
    require(assignment.find("Run the duplicate-row regression")!=std::string::npos &&
            assignment.find("Pinned input artifact version: "+shaA)!=std::string::npos &&
            assignment.find("Return only JSON with artifactVersion and verdict (pass or fail)")!=std::string::npos &&
            assignment.find("Requested change: Preserve duplicate rows")!=std::string::npos,
            "queued criteria edit lost the verification assignment");
    f.t3.finish("run-3",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(1);
    const auto settled = f.call("workflow_status",{{"workflowId",workflow}});
    require(settled["status"]=="completed" && settled["stages"][1]["verified"]==true &&
            settled["verdicts"].size()==1 && settled["verdicts"][0]["artifactVersion"]==shaA,
            "revised verifier did not settle against its pinned artifact");
}

void rerunRevisedDependentStage() {
    Fixture f;
    const Id workflow = f.call("start_workflow",{{"request","Implement feature"},{"verificationCriteria","Run smoke tests"}})
        .at("workflowId");
    f.ticks(2);
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(3);
    const auto task = f.store->task(f.status()["stages"][1]["taskId"]);
    const std::string intent = "Verify backward compatibility", criteria = "Run the migration suite";
    f.call("change_task",{{"taskId",task["id"]},{"generation",task["generation"]},{"revisionId",task["currentRevision"]},
        {"intent",intent},{"doneWhen",criteria},{"instruction","Use the revised assignment."}});
    f.ticks(2);
    require(f.t3.interrupts.contains("run-2"),"active verifier did not stop for the revision");
    f.t3.finish("run-2",Json::object(),"interrupted");
    f.ticks(5);
    require(f.t3.commands.size()==3,"revised verifier did not start");
    f.t3.finish("run-3",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(1);
    require(f.status()["stages"][1]["verified"]==true,"revised verifier did not settle");

    f.call("instruct_workflow_stage",{{"workflowId",workflow},{"stageId","implement"},{"instruction","Build the next revision"}});
    f.ticks(3);
    f.t3.finish("run-4",{{"artifactVersion",shaB}});
    f.ticks(1);
    f.call("instruct_workflow_stage",{{"workflowId",workflow},{"stageId","verify"},{"instruction","Repeat with the new build"}});
    f.restart();
    f.ticks(3);
    const Id rerun = f.status()["stages"][1]["taskId"];
    const auto revised = f.store->task(rerun);
    require(revised["id"]!=task["id"] && revised["intent"]==intent && revised["doneWhen"]==criteria,
            "dependent rerun lost the task revision");
    const auto& assignment = f.t3.commands.back().instruction;
    require(f.t3.commands.size()==5 && assignment.find(intent)!=std::string::npos &&
            assignment.find(criteria)!=std::string::npos && assignment.find(shaB)!=std::string::npos &&
            assignment.find(shaA)==std::string::npos && assignment.find("Repeat with the new build")!=std::string::npos,
            "dependent rerun lost its revised assignment or rebound to the wrong artifact");
    f.t3.finish("run-5",{{"artifactVersion",shaB},{"verdict","pass"}});
    f.ticks(1);
    require(f.status()["status"]=="completed" && f.status()["verdicts"].size()==2,
            "dependent rerun did not settle or lost earlier evidence");
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

void stopDuringUnconfirmedStart() {
    Fixture f;
    f.t3.loseReceipt = true;
    f.t3.receiptVisible = false;
    const auto id = f.call("start_workflow",{{"request","Confirm before stopping"}}).at("workflowId");
    f.ticks(2);
    auto status = f.call("stop_workflow",{{"workflowId",id}});
    require(status["stages"][0]["status"]=="unreachable" && status["stages"][0]["stopRequested"]==true &&
            status["stages"][0]["lastConfirmedAt"]==0 && status["allStopped"]==false,"stop obscured the lost launch receipt");
    f.restart();
    f.ticks(3);
    status.erase("effect");
    require(f.status()==status && f.t3.commands.size()==1,"restart retried or confirmed the uncertain launch");
    f.now += 5;
    f.t3.receiptVisible = true;
    f.ticks(2);
    require(f.t3.interrupts.contains("run-1") && !f.status()["allStopped"].get<bool>(),"recovered Run was not stopped");
    f.t3.finish("run-1",Json::object(),"interrupted");
    f.t3.observations["run-1"].settled = false;
    f.ticks(1);
    require(f.status()["stages"][0]["lastConfirmedState"]=="settling" && f.status()["allStopped"]==false,
            "unsettled delegated work was reported stopped");
    f.t3.observations["run-1"].settled = true;
    f.ticks(2);
    require(f.status()["allStopped"]==true && f.t3.commands.size()==1,"reconciled stop admitted a downstream stage");
}

void reopenedStageWaitsForProject() {
    Fixture f;
    const Json one = {{"request","Independent work"},{"stages",Json::array({{{"stageId","only"},{"role","collect"}}})}};
    const auto first = f.call("start_workflow",one).at("workflowId");
    f.ticks(2);
    f.t3.finish("run-1","Initial findings");
    f.ticks(1);
    f.call("start_workflow",one);
    f.ticks(2);
    const auto revised = f.call("instruct_workflow_stage",{{"workflowId",first},{"stageId","only"},{"instruction","Check again"}});
    const Id task = revised["stages"][0]["taskId"];
    require(f.store->task(task)["blockedBy"]["id"]==f.status()["stages"][0]["taskId"],"reopened task bypassed the queue");
    f.ticks(4);
    require(f.t3.commands.size()==2,"older stage took another workflow's active slot");
    f.t3.finish("run-2","Second workflow done");
    f.ticks(4);
    require(f.t3.commands.size()==3 && f.t3.commands.back().tool=="follow_up" &&
            f.t3.commands.back().target.threadId=="thread-1","reopened task lost its Session");
}

void invalidateDependencyChain() {
    Fixture f;
    const auto id = f.call("start_workflow",{{"request","Release tested changes"},{"stages",Json::array({
        {{"stageId","build"},{"role","implement"}},{{"stageId","notes"},{"role","collect"}},
        {{"stageId","test"},{"role","verify"},{"dependsOn",{"build"}}},
        {{"stageId","release"},{"role","publish"},{"dependsOn",{"test","notes"}},{"inputFrom","test"}}})}}).at("workflowId");
    f.ticks(2);
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.t3.finish("run-2","Include the operator notes");
    f.ticks(3);
    f.t3.finish("run-3",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(3);
    const auto instruction = f.t3.commands.back().instruction;
    require(instruction.find("Include the operator notes")!=std::string::npos && instruction.find("run-2")!=std::string::npos &&
            instruction.find(shaA)!=std::string::npos && instruction.find("run-3")!=std::string::npos,
            "join did not receive both attributed reports");
    f.t3.finish("run-4","Released A");
    f.ticks(1);
    require(f.status()["status"]=="completed","first release did not finish");
    const auto revise = [&](std::string stage) {
        f.call("instruct_workflow_stage",{{"workflowId",id},{"stageId",stage},{"instruction","Repeat for B"}});
        f.ticks(3);
    };
    revise("build");
    f.t3.finish("run-5",{{"artifactVersion",shaB}});
    f.ticks(1);
    require(f.status()["stages"][3]["outdated"]==true,"transitively stale release counted as current");
    revise("test");
    f.t3.finish("run-6",{{"artifactVersion",shaB},{"verdict","pass"}});
    f.ticks(1);
    f.restart();
    require(f.status()["stages"][2]["verified"]==true && f.status()["stages"][3]["outdated"]==true &&
            f.status()["status"]=="needs_verification","B reused the release for A");
    revise("release");
    require(f.t3.commands.size()==7 && f.t3.commands.back().instruction.find(shaB)!=std::string::npos,
            "reopened release did not bind the new dependency");
    f.t3.finish("run-7","Released B");
    f.ticks(1);
    require(f.status()["status"]=="completed","updated release remained stale");
}

void settledWhileReadingInstruction() {
    Fixture f;
    f.call("start_workflow",{{"request","Preserve duplicate input rows"}});
    f.ticks(2);
    const Id task = f.status()["stages"][0]["taskId"];
    f.t3.finish("run-1",{{"artifactVersion",shaA},{"finding","Keep the original row count"}});
    f.t3.onObserve = [&] {
        f.store->receive({"bot","","env","project",Json::object(),{},0},"fresh","Continue in a fresh Session");
        f.bot.action = {"continue_fresh_session",{{"taskId",task}},"Continue the same task"};
    };
    f.ticks(1);
    require(f.store->task(task)["state"]!="completed" && f.status()["stages"][1]["taskId"]==0,
            "stage closed before the pending message was read");
    f.restart();
    f.ticks(6);
    const auto& next = f.t3.commands.back();
    require(f.t3.commands.size()==2 && next.tool=="delegate" && f.store->task(task)["runs"].size()==2 &&
            next.instruction.find("Keep the original row count")!=std::string::npos &&
            next.instruction.find("run-1")!=std::string::npos && f.status()["artifacts"][0]["artifactVersion"]==shaA &&
            f.status()["stages"][1]["taskId"]==0,"fresh Session lost its settled report or started a verifier");
    f.t3.finish("run-2",{{"artifactVersion",shaB}});
    f.ticks(3);
    require(f.status()["stages"][1]["inputArtifactVersion"]==shaB &&
            f.t3.commands.back().instruction.find("Preserve duplicate input rows")!=std::string::npos,
            "default verifier lost the requested behavior or pin");
    f.t3.finish("run-3",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(1);
    require(f.status()["verdicts"].empty() && f.status()["stages"][1]["verified"]==false,
            "verifier's mismatched report was recorded against its assigned version");
}

void observeAfterUnsentAnswer() {
    Fixture f;
    f.call("start_workflow",{{"request","Ask for the output format"}});
    f.ticks(2);
    auto& run = f.t3.observations["run-1"];
    run.kind = Observation::Kind::Question;
    run.question = {"q",Json::array({{{"id","choice"},{"freeform",true}}})};
    f.ticks(3);
    const Id task = f.status()["stages"][0]["taskId"];
    require(f.store->task(task)["wait"]["reason"]=="unknown_delivery" && f.store->task(task)["pendingAction"]["phase"]=="ready",
            "unsent answer fixture did not reach its safe retry point");
    f.restart();
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(3);
    require(f.t3.commands.size()==2 && f.status()["stages"][1]["inputArtifactVersion"]==shaA,
            "unsent answer prevented observation of a completed Run");
}

void stopBeforeFirstSend() {
    for (int prepared : {0,1}) {
        Fixture f;
        const auto id = f.call("start_workflow",{{"request","Pending launch"}}).at("workflowId");
        f.ticks(prepared);
        f.call("stop_workflow",{{"workflowId",id}});
        f.restart();
        f.ticks(3);
        require(f.t3.commands.empty() && f.status()["allStopped"]==true,"stop admitted pending or prepared work");
    }
}

void scopedCompactionPages() {
    Fixture f;
    std::string original;
    for (int i=0; i<1000; ++i) original += "根拠\n\t\x01";
    const Id first = f.turn(original);
    const Id end = f.turn("Boundary");
    const auto summary = f.call("compact_conversation",{{"endMessageId",end},{"summary","Keep the original evidence"}});
    require(summary["compaction"]["originalCount"]==3,"summary counted messages outside its boundary");
    f.bot.action = {"expand_context",{{"compactionId",summary["compaction"]["compactionId"]}},"Look up original evidence"};
    f.store->receive({"other","","env","project",Json::object(),{},0},"read","Read first Bot's evidence");
    f.runtime->runConversations();
    require(f.bot.result.at("ok")==false && f.bot.history.empty(),"compaction crossed Bot histories");
    f.turn("Bad offset","expand_context",{{"startMessageId",first},{"offset",1}});
    require(f.bot.result.at("ok")==false,"cursor split a UTF-8 character");
    std::string replay;
    std::size_t offset = 0;
    do {
        const auto page = f.call("expand_context",{{"startMessageId",first},{"endMessageId",first},{"offset",offset},{"limit",1}});
        require(page.dump().size()<=8192 && page.at("messages").size()==1 && page["messages"][0]["messageId"]==first,
                "escaped evidence exceeded its byte or item cap");
        replay += page["messages"][0]["text"].get<std::string>();
        if (!page.contains("nextOffset")) break;
        offset = page.at("nextOffset");
        require(offset==replay.size() && page.at("nextMessageId")==first,"non-advancing evidence cursor");
    } while (replay.size()<original.size());
    require(replay==original,"UTF-8 cursor skipped or duplicated original bytes");

    int calls = 0;
    bool reviewed = false;
    f.bot.conversation = [&](const ModelInput& input) -> Decision {
        const auto& steps = input.event.at("turn");
        std::size_t expanded = 0;
        for (const auto& step : steps) {
            const auto& result = step.at("result");
            if (result.contains("messages")) expanded += result.dump().size();
        }
        require(expanded<=8192 && expanded+input.history.dump().size()<=24000,"expansions accumulated beyond model context budget");
        if (++calls <= 5) return {"expand_context",{{"startMessageId",first},{"endMessageId",first}},"Read original evidence"};
        require(steps.front()["result"].value("evidenceOmitted",false) && steps.back()["result"].contains("messages"),
                "bounded context lost the latest requested evidence");
        reviewed = true;
        return {"reply",{{"text","Reviewed"}},"Finish"};
    };
    f.turn("Read several pages");
    require(calls==6 && reviewed,"multi-step expansion did not respect the evidence budget");
}

void historyKeepsLargestFittingPrefix() {
    Fixture f;
    std::string original;
    for (int i=0; i<500; ++i) original += "根拠\n\t\"\\\x01";
    const Id target = f.turn(original);
    for (int i=0; i<4; ++i) f.turn(std::string(4000,'x'));
    f.turn(std::string(3987,'x'));
    f.turn("Inspect history near its byte limit");
    auto history = f.bot.history;
    const auto partial = std::ranges::find_if(history,[&](const Json& entry) { return entry["messageId"]==target; });
    require(partial!=history.end(),"history dropped a message whose prefix fit");
    const auto excerpt = partial->at("text").get<std::string>();
    require(!excerpt.empty() && excerpt.size()<4000 && original.starts_with(excerpt) &&
            history.dump().size()<24000,"history prefix did not fit the serialized byte budget");
    auto next = excerpt.size()+1;
    while ((static_cast<unsigned char>(original.at(next))&0xc0)==0x80) ++next;
    (*partial)["text"] = original.substr(0,next);
    require(next>excerpt.size()+1 && history.dump().size()>24000,
            "the next multi-byte character should exceed the remaining serialized bytes");
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
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
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
        workflowLaunchUsesGrantsAndRedactsQueuedPrompt();
        expansionRedactsBeforeSearchAndPaging();
        freshStandaloneSessionIncludesEarlierEvidence();
        verifyAfterRestart(false);
        verifyAfterRestart(true);
        stageRequestKeepsEmbeddedNul();
        reviseQueuedVerifier();
        rerunRevisedDependentStage();
        rejectInvalidGraphAndArtifact();
        stopAndDeadline();
        uncertainLaunchAndStageStop();
        stopDuringUnconfirmedStart();
        reopenedStageWaitsForProject();
        invalidateDependencyChain();
        settledWhileReadingInstruction();
        observeAfterUnsentAnswer();
        stopBeforeFirstSend();
        scopedCompactionPages();
        historyKeepsLargestFittingPrefix();
        compactAndContinue();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
