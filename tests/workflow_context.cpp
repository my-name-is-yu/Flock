#include "flock/core.hpp"
#include "flock/policy.hpp"

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
    std::function<RunEvidence(const RunRef&)> recoverEvidence;

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
    RunEvidence readRunEvidence(const std::string& env,const Json& config,const RunRef& run,std::string_view status) override {
        return recoverEvidence ? recoverEvidence(run) : T3::readRunEvidence(env,config,run,status);
    }
    InterruptOutcome interrupt(const std::string&,const Json&,const RunRef& run) override { interrupts.insert(run.runId); return InterruptOutcome::Requested; }
    void finish(std::string run, Json report, std::string status = "completed") {
        auto& observation = observations[run];
        observation.kind = Observation::Kind::Finished;
        observation.status = std::move(status);
        observation.report = report.dump();
    }
};

struct Bot : Model {
    Decision action;
    Json history, result;
    std::function<Decision(const ModelInput&)> conversation, work;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
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
    explicit Fixture() {
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
        runtime = std::make_unique<Runtime>(*store,bot,t3,[](auto){return std::string{};},options);
    }
    Id turn(std::string text, std::string tool = {}, Json args = Json::object()) {
        bot.action = {tool,args,"Execute the requested operation"};
        bot.result = nullptr;
        auto receipt = store->receive({"owner",true},{"bot","","env","project",Json::object()},std::to_string(++sequence),text);
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

void workflowLaunchUsesPolicyAndRedactsQueuedPrompt() {
    Fixture f;
    auto policy = f.store->policy();
    const Person owner{"owner",true};
    policy.changeNeverRule(owner,Action::environment_launch,"env",true);
    const std::string secret = "workflow-credential-registered-after-queueing";
    f.call("start_workflow",{{"request","Collect the release notes " + secret}});
    f.ticks(2);
    const Id task = f.status()["stages"][0]["taskId"];
    const auto wait = f.store->task(task).at("wait");
    require(wait.at("reason")=="settings" && f.t3.commands.empty(),"unapproved workflow launch reached T3");
    const auto input = policy.requestSecret("bot",{"workflow-credential"});
    policy.submitSecret(owner,"bot",input,secret);
    policy.changeNeverRule(owner,Action::environment_launch,"env",false);
    f.store->operate(owner,TaskTarget::from(f.store->task(task),wait.at("id")),"reconcile");
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
    auto policy = f.store->policy();
    policy.submitSecret({"owner",true},"bot",policy.requestSecret("bot",{"context-credential"}),secret);
    const auto first = f.call("expand_context",{{"compactionId",compaction},{"startMessageId",original}});
    const auto second = f.call("expand_context",{{"cursor",first.at("nextCursor")}});
    require(first.at("messages").at(0).at("text").get<std::string>()+second.at("messages").at(0).at("text").get<std::string>()
            ==prefix+"[redacted] tail","paging split the protected value before redaction");
    require(f.bot.history.front().at("summary")=="Notes: [redacted]","protected value reached the compacted model history");
    require(f.call("expand_context",{{"compactionId",compaction},{"query","value-for-compaction"}}).at("messages").empty(),
            "expansion search matched a fragment of the protected value");
}

void expansionRejectsStaleCursors() {
    Fixture f;
    const std::string secret = "fixture-" + std::string(64,'k');
    const std::string instruction = " DO-NOT-PUBLISH ";
    const auto originalText = secret + std::string(1928,'x') + "根" + instruction + std::string(2500,'y');
    const auto original = f.turn(originalText);
    const auto saved = f.call("compact_conversation",{{"endMessageId",original},{"summary","Keep the instruction"}});
    const auto compaction = saved.at("compaction").at("compactionId");
    const Json range{{"compactionId",compaction},{"startMessageId",original},{"endMessageId",original}};
    const auto first = f.call("expand_context",range);
    auto continuation = range;
    continuation["cursor"] = first.at("nextCursor");
    f.restart();
    const auto text = [](const Json& page) { return page.at("messages").at(0).at("text").get<std::string>(); };
    const auto continued = text(f.call("expand_context",continuation));
    require(!continued.empty() && std::string_view(originalText).substr(text(first).size()).starts_with(continued),
            "An unchanged projection did not resume after restart");

    bool checked = false;
    f.bot.conversation = [&](const ModelInput& input) -> Decision {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return {"expand_context",range,"Read the first page"};
        if (steps.size()==1) {
            auto policy = f.store->policy();
            policy.submitSecret({"owner",true},"bot",policy.requestSecret("bot",{"paging-fixture"}),secret);
            return {"expand_context",continuation,"Continue the first page"};
        }
        require(steps.at(0).at("result").at("cursorStale")==true &&
                !steps.at(0).at("result").contains("messages") && steps.at(1).at("result").at("ok")==false,
                "A saved page was replayed after the active redaction view changed");
        checked = true;
        return {"reply",{{"text","I will start a fresh read"}},"Acknowledge the stale cursor"};
    };
    f.turn("Detect redaction changes within the turn");
    f.bot.conversation = {};
    require(checked,"The test did not reach the model with the stale page");
    f.turn("Continue the saved expansion","expand_context",continuation);
    require(f.bot.result.at("ok")==false && !f.bot.result.contains("messages"),
            "A cursor from a different redaction view returned a page");
    const auto redacted = f.call("expand_context",range);
    continuation["cursor"] = redacted.at("nextCursor");
    const auto second = f.call("expand_context",continuation);
    const auto third = f.call("expand_context",{{"compactionId",compaction},{"endMessageId",original},
                                                {"cursor",second.at("nextCursor")}});
    require(text(redacted)+text(second)+text(third) == "[redacted]" + std::string(1928,'x') + "根" +
                instruction + std::string(2500,'y') && redacted.at("nextCursor").is_number_integer(),
            "A fresh expansion skipped an instruction, included secret text, or embedded text in its reference");
}

void expansionCursorKeepsItsSearchRange() {
    Fixture f;
    f.turn("ALPHA evidence one");
    const auto lastMatch = f.turn("ALPHA evidence two");
    const auto boundary = f.turn("Unrelated entry at the end");
    f.call("compact_conversation",{{"endMessageId",boundary},{"summary","Collate the evidence"}});
    const auto first = f.call("expand_context",{{"query","ALPHA"},{"limit",1}});
    const auto cursor = first.at("nextCursor");
    const auto wider = f.turn("ALPHA evidence added later");
    f.call("compact_conversation",{{"endMessageId",wider},{"summary","Collate the new evidence"}});
    f.turn("Continue with a smaller range","expand_context",{{"cursor",cursor},{"endMessageId",lastMatch}});
    require(f.bot.result.at("ok") == false,"A cursor accepted a smaller search range");
    const auto continued = f.call("expand_context",{{"cursor",cursor}});
    require(continued.at("messages").size()==1 && continued.at("messages").at(0).at("messageId")==lastMatch &&
            !continued.contains("nextCursor"),"A cursor resumed a wider range or lost its search filter");
}

void overlapKeepsEachSearchFilter() {
    Fixture f;
    const auto message = f.turn("ALPHA BETA " + std::string(3000,'x'));
    const auto end = f.turn("ALPHA detail");
    f.call("compact_conversation",{{"endMessageId",end},{"summary","Two searches"}});
    const auto alpha = f.call("expand_context",{{"query","ALPHA"}});
    const auto beta = f.call("expand_context",{{"query","BETA"}});
    require(alpha.at("messages") == beta.at("messages") && alpha.at("nextCursor") != beta.at("nextCursor"),
            "the fixture did not issue independent cursors for the overlapping search result");
    f.turn("Continue overlapping search","expand_context",
           {{"cursor",alpha.at("nextCursor")},{"query","BETA"}});
    require(f.bot.result.at("ok") == false,"a cursor accepted a different search filter");
    const auto alphaPage = f.call("expand_context",{{"cursor",alpha.at("nextCursor")}});
    const auto betaPage = f.call("expand_context",{{"cursor",beta.at("nextCursor")}});
    require(alphaPage.at("messages").back().at("messageId") == end &&
            betaPage.at("messages").size() == 1 && betaPage.at("messages").at(0).at("messageId") == message,
            "the overlapping searches lost their own filters");
}

void freshStandaloneSessionIncludesEarlierEvidence() {
    Fixture f;
    f.bot.work = [](const ModelInput& input) -> Decision {
        if (input.event.at("kind")=="intake")
            return {"delegate",{{"instruction","Compare the available options"}},"Compare them in T3"};
        return {"complete",{{"report",input.event.at("report")}},"Report the comparison"};
    };
    const Id sourceTask = f.call("start_task",{{"request","Compare the options"},{"doneWhen","Report the differences"}}).at("taskId");
    f.ticks(3);
    f.t3.finish("run-1","Option B must preserve the retry window");
    f.ticks(2);
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
    f.call("start_workflow",{{"request",request}});
    f.ticks(2);
    const auto task = f.store->task(f.status()["stages"][0]["taskId"]);
    require(task["request"]==request && task["intent"]==request && task["doneWhen"]==request,
            "stage request changed before the first revision was saved");
    require(f.t3.commands.size()==1 &&
            f.t3.commands.front().instruction.find("Task intent:\n"+request+"\n\nCompletion criteria:\n"+request)!=std::string::npos,
            "stage dispatch lost the request bytes after the marker");
}

void verifyAfterRestart() {
    Fixture f;
    const Id id = f.call("start_workflow",{{"request","Implement feature"},{"verificationCriteria","Run tests"}}).at("workflowId");
    f.ticks(2);
    require(f.t3.commands.size()==1,"expected single implementation Session");
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(1);
    const auto beforeRestart = f.status();
    require(beforeRestart.at("stages")[1]["status"]=="pending" && f.t3.commands.size()==1,
            "verification dispatched before the implementation settled");
    f.restart();
    require(f.status()==beforeRestart,"restart lost workflow IDs or artifact evidence");
    f.ticks(3);
    const auto status = f.status();
    const auto& verifier = f.t3.commands.back();
    const auto& session = verifier.config.at("session");
    const Json worktree = {{"type","worktree"},{"baseRef",shaA},{"startFromOrigin",false},{"expectedCommit",shaA}};
    require(f.t3.commands.size()==2 && verifier.tool=="delegate" &&
            verifier.environmentId==f.t3.commands.front().environmentId && verifier.projectId==f.t3.commands.front().projectId &&
            verifier.instruction.find("Run tests")!=std::string::npos &&
            verifier.instruction.find("Implement feature")!=std::string::npos &&
            verifier.instruction.find("run-1")!=std::string::npos &&
            status["stages"][1]["inputArtifactVersion"]==shaA &&
            session.at("runtimeMode")=="approval-required" && session.at("workspaceStrategy")==worktree,
            "verification did not use the implementation's exact commit, evidence and project");
    f.t3.finish("run-2",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(2);
    const auto finished = f.call("workflow_status",{{"workflowId",id}});
    require(finished["status"]=="completed" && finished["stages"][1]["verified"]==true &&
            finished["stages"][1]["currentVerdict"]=="pass" && finished["verdicts"].size()==1 &&
            finished["verdicts"][0]["artifactVersion"]==shaA &&
            finished["verdicts"][0]["runRef"]["threadId"]=="thread-2" && f.t3.commands.size()==2,
            "exact-version verification was lost, misattributed, or duplicated");
}

void reviseActiveVerifier() {
    Fixture f;
    f.call("start_workflow",{{"request","Implement feature"},{"verificationCriteria","Run smoke tests"}});
    f.ticks(2);
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(3);
    const auto task = f.store->task(f.status()["stages"][1]["taskId"]);
    f.call("change_task",{{"taskId",task["id"]},{"generation",task["generation"]},{"revisionId",task["currentRevision"]},
        {"doneWhen","Run the migration suite"},{"instruction","Use the revised criteria."}});
    f.ticks(2);
    require(f.t3.interrupts.contains("run-2"),"active verifier did not stop for the revision");
    f.t3.finish("run-2",Json::object(),"interrupted");
    f.ticks(5);
    require(f.t3.commands.size()==3 &&
            f.t3.commands.back().instruction.find("Run the migration suite")!=std::string::npos &&
            f.t3.commands.back().config["session"]["workspaceStrategy"]["expectedCommit"]==shaA,
            "revised verification did not retain its exact pin and revised criteria");
    f.t3.finish("run-3",{{"artifactVersion",shaA},{"verdict","pass"}});
    f.ticks(1);
    require(f.status()["stages"][1]["verified"]==true,"revised verification did not settle");
}

void rejectCustomStagesAndMutableArtifact() {
    Fixture f;
    f.turn("Collect notes","start_workflow",{{"request","Collect release notes"},{"stages",Json::array({
        {{"stageId","notes"},{"role","collect"}}})}});
    require(f.bot.result.at("ok")==false && f.store->workflows("bot").empty() && f.store->tasks("bot").empty(),
            "custom stages were accepted or partially materialized");
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
    const auto id = f.call("start_workflow",{{"request","Implement with a soft deadline"},{"softDeadlineSeconds",1}}).at("workflowId");
    f.ticks(2);
    ++f.now;
    auto status = f.call("workflow_status",{{"workflowId",id}});
    require(status["stages"][0]["deadlineExceeded"]==true && status["stages"][0]["needsAttention"]==true &&
        status["stages"][0]["status"]=="running" && f.t3.interrupts.empty(),"deadline interrupted the Session");
    require(f.call("stop_workflow",{{"workflowId",id}})["allStopped"]==false,"request claimed stop confirmation");
    f.ticks(1);
    require(f.t3.interrupts==std::set<std::string>{"run-1"},"missing interrupt");
    f.t3.observations["run-1"].kind = Observation::Kind::Unavailable;
    f.ticks(2);
    status = f.call("workflow_status",{{"workflowId",id}});
    require(status["allStopped"]==false && status["stages"][0]["status"]=="unreachable" &&
        status["stages"][0]["lastConfirmedState"]=="running","unreachable Session marked stopped");
    f.restart();
    require(f.call("workflow_status",{{"workflowId",id}})==status,"stop state lost on restart");
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(3);
    require(f.status()["allStopped"]==true && f.status()["status"]=="stopped" && f.t3.commands.size()==1,
        "completed implementation started the verifier after a stop request");
}

void uncertainLaunchAndStop() {
    Fixture f;
    f.t3.loseReceipt = true;
    const auto id = f.call("start_workflow",{{"request","New implementation"}}).at("workflowId");
    f.ticks(2);
    require(f.status()["stages"][0]["status"]=="unreachable","unknown delivery reported as confirmed");
    f.restart();
    f.ticks(1);
    require(f.status()["stages"][0]["runRef"]["runId"]=="run-1" && f.t3.commands.size()==1,
            "ambiguous launch was duplicated or lost");
    f.call("stop_workflow",{{"workflowId",id}});
    f.ticks(1);
    require(f.t3.interrupts.contains("run-1") && f.status()["stages"][0]["status"]=="stop_requested",
            "stage interrupt acknowledgement treated as terminal");
    f.t3.finish("run-1",Json::object(),"cancelled");
    f.ticks(2);
    require(f.status()["stages"][0]["status"]=="stopped" && f.t3.commands.size()==1,"dependent of stopped stage ran");
}

void missingStageReportCannotAdvanceWorkflow() {
    for (const bool recover : {false,true}) {
        Fixture f;
        f.call("start_workflow",{{"request","Implement a release change"}});
        f.ticks(2);
        f.t3.finish("run-1",{{"artifactVersion",shaA}});
        auto& observation = f.t3.observations.at("run-1");
        observation.evidence = RunEvidence::Availability::ReportUnavailable;
        observation.resultRunId = "collect-result";
        observation.report.clear();
        f.ticks(1);
        const auto pending = f.status();
        const Id task = pending["stages"][0]["taskId"];
        require(f.store->task(task)["activeRun"].is_null() && pending["stages"][0]["lastConfirmedState"]=="completed" &&
                pending["stages"][0]["needsAttention"]==true && pending["stages"][1]["taskId"]==0 &&
                pending["artifacts"].empty(),"workflow either lost settlement or advanced without the stage report");
        if (recover) f.t3.recoverEvidence = [&](const RunRef& source) {
            require(source.runId=="collect-result" && source.threadId=="thread-1","workflow retried an unrelated result");
            return RunEvidence{RunEvidence::Availability::Available,Json{{"artifactVersion",shaA}}.dump(),"collected","",false};
        };
        f.restart();
        f.now += 5;
        f.ticks(1);
        f.now += 10;
        f.ticks(3);
        const auto status = f.status();
        if (recover) {
            require(status["stages"][0]["status"]=="completed" && status["stages"][1]["taskId"]!=0 &&
                    status["artifacts"].size()==1 && status["artifacts"][0]["artifactVersion"]==shaA &&
                    status["artifacts"][0]["resultRunId"]=="collect-result",
                    "restored stage report did not unlock the dependent stage with its artifact provenance");
        } else {
            require(status["stages"][0]["status"]=="failed" && status["stages"][1]["taskId"]==0 &&
                    status["artifacts"].empty() && f.t3.commands.size()==1,
                    "a stage without required report evidence produced an artifact or admitted a dependent");
        }
    }
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
    f.t3.observations["run-1"].kind = Observation::Kind::Settling;
    f.ticks(1);
    require(f.status()["stages"][0]["lastConfirmedState"]=="settling" && f.status()["allStopped"]==false,
            "unsettled delegated work was reported stopped");
    f.t3.observations["run-1"].kind = Observation::Kind::Finished;
    f.ticks(2);
    require(f.status()["allStopped"]==true && f.t3.commands.size()==1,"reconciled stop admitted a downstream stage");
}

void settledWhileReadingInstruction() {
    Fixture f;
    f.call("start_workflow",{{"request","Preserve duplicate input rows"}});
    f.ticks(2);
    const Id task = f.status()["stages"][0]["taskId"];
    f.t3.finish("run-1",{{"artifactVersion",shaA},{"finding","Keep the original row count"}});
    f.t3.onObserve = [&] {
        f.store->receive({"owner",true},{"bot","","env","project",Json::object()},"fresh","Continue in a fresh Session");
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

void stopBeforeVerificationDispatch() {
    Fixture f;
    const Id id = f.call("start_workflow",{{"request","Prepare changes for verification"}}).at("workflowId");
    f.ticks(2);
    f.t3.finish("run-1",{{"artifactVersion",shaA}});
    f.ticks(1);
    const auto verify = f.status()["stages"][1];
    require(verify["taskId"]!=0 && verify["status"]=="pending" && f.t3.commands.size()==1,
            "verification fixture did not reach the queued handoff");
    f.call("stop_workflow",{{"workflowId",id}});
    f.restart();
    f.ticks(4);
    const auto stopped = f.status();
    require(stopped["allStopped"]==true && stopped["stages"][1]["status"]=="stopped" &&
            stopped["artifacts"].size()==1 && stopped["verdicts"].empty() && f.t3.commands.size()==1,
            "a queued verification survived the stop or lost implementation evidence");
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
    f.store->receive({"owner",true},{"other","","env","project",Json::object()},"read","Read first Bot's evidence");
    f.runtime->runConversations();
    require(f.bot.result.at("ok")==false && f.bot.history.empty(),"compaction crossed Bot histories");
    std::string replay;
    Json args = {{"startMessageId",first},{"endMessageId",first},{"limit",1}};
    while (true) {
        const auto page = f.call("expand_context",args);
        require(page.dump().size()<=8192 && page.at("messages").size()==1 && page["messages"][0]["messageId"]==first,
                "escaped evidence exceeded its byte or item cap");
        const auto chunk = page["messages"][0]["text"].get<std::string>();
        require(!chunk.empty() && replay.size()+chunk.size()<=original.size(),"non-advancing evidence cursor");
        replay += chunk;
        if (!page.contains("nextCursor")) break;
        args["cursor"] = page.at("nextCursor");
    }
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
    require(expanded.dump().size()<=8192 && expanded.contains("nextCursor") && expanded.at("messages").at(0).at("messageId")==end,
        "unbounded expansion or missing cursor");
    f.turn("Invalid boundary","compact_conversation",{{"endMessageId",first},{"summary","Discard coverage"}});
    require(f.bot.result.at("ok")==false,"accepted shrinking boundary");
    f.turn("Same boundary","compact_conversation",{{"endMessageId",end},{"summary",std::string(3200,'z')}});
    require(f.bot.result.at("ok")==false && f.store->compactions("bot").size()==1 &&
            f.store->compactions("bot").at(0)==compact.at("compaction"),
            "A same-boundary summary replaced the durable compaction");
    const auto nextBoundary = f.turn("New evidence to include");
    const auto advanced = f.call("compact_conversation",{{"endMessageId",nextBoundary},{"summary","Include newer evidence"}});
    require(advanced.at("compaction").at("endMessageId")==nextBoundary &&
            f.store->compactions("bot").size()==2,"a valid compaction did not advance coverage");

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
        workflowLaunchUsesPolicyAndRedactsQueuedPrompt();
        expansionRedactsBeforeSearchAndPaging();
        expansionRejectsStaleCursors();
        expansionCursorKeepsItsSearchRange();
        overlapKeepsEachSearchFilter();
        freshStandaloneSessionIncludesEarlierEvidence();
        verifyAfterRestart();
        stageRequestKeepsEmbeddedNul();
        reviseActiveVerifier();
        rejectCustomStagesAndMutableArtifact();
        stopAndDeadline();
        uncertainLaunchAndStop();
        missingStageReportCannotAdvanceWorkflow();
        stopDuringUnconfirmedStart();
        settledWhileReadingInstruction();
        observeAfterUnsentAnswer();
        stopBeforeFirstSend();
        stopBeforeVerificationDispatch();
        scopedCompactionPages();
        historyKeepsLargestFittingPrefix();
        compactAndContinue();
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
