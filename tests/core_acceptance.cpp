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
Decision botRequest(std::string bot, const char* kind, const char* message) {
    auto args = Json{{"botId",std::move(bot)},{"kind",kind},{"text",message}};
    if (std::string_view(kind) == "work") args["doneWhen"] = Evidence;
    return {"send_bot_message",std::move(args),"Ask the specialist"};
}
Decision botReply(const char* kind, const char* message) {
    return {"reply_to_bot",{{"kind",kind},{"text",message}},"Respond to the current exchange"};
}
bool hasTool(const ModelInput& input, std::string_view tool) {
    return std::find(input.tools.begin(),input.tools.end(),tool) != input.tools.end();
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
    std::vector<ReadRequest> readRequests;
    std::function<void()> onPreflight;
    std::string authGeneration(const std::string&,const Json&) override { return generation; }
    std::optional<Delivery> preflight(const Command&) override {
        if (onPreflight) onPreflight();
        return std::nullopt;
    }
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
        const bool reported = std::any_of(runs.begin(),runs.end(),[&](const auto& entry) {
            return entry.second.resultRunId == req.runId;
        });
        check(!req.runId.empty() && (runs.contains(req.runId) || reported),"read unrelated Run");
        readRequests.push_back(req);
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
    std::map<std::string,std::vector<PeerBot>,std::less<>> peers;

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
        RuntimeOptions options{[&] { return now; },read};
        options.peers = [&](std::string_view source) {
            const auto found = peers.find(source);
            return found == peers.end() ? std::vector<PeerBot>{} : found->second;
        };
        runtime = std::make_unique<Runtime>(*store,model,t3,[&](std::string_view id) {
            return notes.at(std::string(id));
        },std::move(options));
    }
    void allowBeta() {
        peers["alpha"] = {{"Verifier",{"beta","","e","p",{{"runTimeoutSeconds",10},{"model","beta-model"}}}}};
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
    std::vector<Json> botMessages(std::string_view bot) {
        std::vector<Json> messages;
        for (const auto& item : store->conversation(bot))
            if (item["kind"] == "bot_message") messages.push_back(item["data"]);
        return messages;
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

void peerWorkAndRestart() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Decide whether the PR can ship");
    const auto unrelated = f.submit("Ordinary request queued before the handoff");
    int replyDecisions = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto taskId = input.task["id"].get<Id>();
        if (taskId == unrelated) return ask();
        if (taskId == root) {
            if (event["kind"] == "intake") {
                check(hasTool(input,"send_bot_message"),"configured peer was unavailable");
                return botRequest("beta","work","Verify the PR before release");
            }
            if (event["kind"] == "bot_question") {
                check(event["fromBotId"] == "beta" && event["text"] == "Which branch?",
                      "peer question arrived without its sender or text");
                if (!event.contains("userAnswer")) return ask();
                check(event["userAnswer"] == "main","the human answer did not return to the original Bot question");
                return botReply("answer","Use main");
            }
            check(event["kind"] == "bot_result" && event["text"] == "Verified the PR on main" &&
                  input.task["doneWhen"] == Evidence,"peer result did not resume the original work contract");
            if (input.evidence.empty()) {
                check(hasTool(input,"read_bot_evidence"),"peer result evidence could not be inspected");
                return {"read_bot_evidence",{{"messageId",event["messageId"]}},"Verify the specialist's evidence"};
            }
            check(input.evidence.back()["text"] == "commit wanted, updates enabled",
                  "owned peer Session evidence was not returned");
            ++replyDecisions;
            return complete();
        }
        check(input.task["botId"] == "beta" && input.notes.memory == "BETA_ONLY",
              "work did not run as the recipient Bot");
        if (event["kind"] == "bot_request") {
            check(event["fromBotId"] == "alpha" && event["text"] == "Verify the PR before release" &&
                  input.history.dump().find("ALPHA_ONLY") == std::string::npos,
                  "peer intake lost its sender or inherited the sender's memory");
            return botReply("question","Which branch?");
        }
        if (event["kind"] == "bot_answer") {
            check(event["text"] == "Use main","clarification did not resume the recipient's task");
            return work();
        }
        check(event["kind"] == "result","recipient did not receive its T3 result");
        return botReply("result","Verified the PR on main");
    };

    f.reach(root,"peer_waiting");
    check(f.task(root)["wait"].is_null() && f.task(root)["doneWhen"] == Evidence,
          "waiting for a peer became a human wait or lost the completion contract");
    f.restart();
    const auto recipientTasks = f.store->tasks("beta");
    check(recipientTasks.size() == 1,"handoff was not persisted exactly once");
    const auto recipient = recipientTasks.front()["id"].get<Id>();
    check(recipient > unrelated,"test did not put unrelated work first in the normal queue");
    check(f.task(recipient)["blockedBy"].is_null(),"recipient was shown as blocked despite having the execution slot");
    const auto humanWait = f.wait(root,"user_question");
    check(f.task(recipient)["state"] == "peer_waiting" && f.task(unrelated)["state"] == "queued" && f.t3.launches == 0,
          "clarification started unrelated work or launched a Session prematurely");
    f.restart();
    f.store->operate(root,humanWait,"answer",{{"text","main"}});
    f.rejects(root,humanWait,"answer",{{"text","dev"}});
    f.reach(recipient,"running");
    check(f.t3.launches == 1 && f.t3.seen.back().projectId == "p" && f.t3.seen.back().config.at("model") == "beta-model" &&
          f.task(unrelated)["state"] == "queued" && f.task(root)["state"] == "peer_waiting",
          "recipient did not inherit the project execution slot using its own configuration");

    f.restart();
    f.t3.finish("run-1");
    f.reach(recipient,"completed");
    f.restart();
    for (int step = 0; step < 8 && f.t3.reads == 0; ++step) f.runtime->tick();
    check(f.t3.reads == 1 && f.task(root)["state"] == "deciding","reading evidence consumed the pending peer result");
    check(f.t3.readRequests.back().config.at("model") == "beta-model",
          "peer evidence used the sender's execution settings");
    f.restart();
    f.reach(root,"completed");
    f.wait(unrelated,"user_question");
    check(f.store->tasks("beta").size() == 1 && f.t3.launches == 1 && f.t3.reads == 1 && replyDecisions == 1 &&
          f.task(root)["additionalUsed"] == 0 && f.logs(root,"user_answer") == 1,
          "restart duplicated the recipient, Session, read, reply, or user answer");
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 4 && messages == f.botMessages("beta"),
          "the two Bots did not show the same four messages after restart");
    for (std::size_t i = 1; i < messages.size(); ++i)
        check(messages[i]["exchangeId"] == messages.front()["exchangeId"] &&
              messages[i]["replyTo"] == messages[i - 1]["messageId"],"the recorded reply chain broke");
}

void peerConsultationAndPermission() {
    Fixture f;
    f.allowBeta();
    const auto denied = f.submit("Consult a Bot outside the configured grants");
    int deniedAttempts = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        check(hasTool(input,"send_bot_message"),"permit validation was not tested with an available send tool");
        ++deniedAttempts;
        return botRequest("gamma","consult","What is the release policy?");
    };
    f.wait(denied,"model_failure");
    check(deniedAttempts == 3 && f.store->tasks().size() == 1 && f.botMessages("alpha").empty(),
          "an unlisted recipient received a message or the available send tool was not exercised");
    f.store->operate(denied,0,"cancel");
    f.reach(denied,"cancelled");

    const auto consultation = f.submit("Ask the Verifier about our release policy");
    f.model.script = [](const ModelInput& input) -> Decision {
        if (input.event["kind"] == "intake") return botRequest("beta","consult","What is the release policy?");
        if (input.event["kind"] == "bot_request") {
            check(!hasTool(input,"delegate") && !hasTool(input,"send_bot_message"),
                  "consultation inherited T3 work permissions or a reverse grant");
            return botReply("result","Require a verified PR");
        }
        check(input.event["kind"] == "bot_result" && input.event["text"] == "Require a verified PR",
              "consultation did not deliver the response in context");
        return complete();
    };
    f.reach(consultation,"completed");
    check(f.t3.seen.empty() && f.botMessages("alpha").size() == 2 && f.store->tasks("beta").size() == 1,
          "consultation started a Session or created extra messages");
}

void peerCancelWithLateSessionResult() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Ask the specialist to verify the PR");
    const auto unrelated = f.submit("Next ordinary request");
    int lateDecisions = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == unrelated) return ask();
        if (input.event["kind"] == "bot_request") return work();
        if (input.event["kind"] == "intake") return botRequest("beta","work","Verify the PR");
        ++lateDecisions;
        return botReply("result","Late verification");
    };
    f.reach(root,"peer_waiting");
    const auto recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"running");
    f.store->operate(root,0,"cancel");
    for (int step = 0; step < 8 && f.t3.interrupts == 0; ++step) f.runtime->tick();
    check(f.t3.interrupts == 1 && f.task(unrelated)["state"] == "queued",
          "cancel did not target the delegated Run or released the slot before it settled");
    f.restart();
    f.t3.finish("run-1");
    f.reach(recipient,"cancelled");
    f.reach(root,"cancelled");
    f.wait(unrelated,"user_question");
    check(lateDecisions == 0 && f.t3.launches == 1 && f.logs(root,"completed") == 0,
          "a late worker result reopened the cancelled exchange");
}

void peerProjectCycle() {
    Fixture f;
    f.allowBeta();
    f.peers.at("alpha").front().intake.projectId = "second";
    f.peers["beta"] = {{"Release",{"alpha","","e","p"}}};
    const auto alpha = f.submit("Verify in the second project");
    const auto beta = f.submit("Verify in the first project","beta","second");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event["kind"] == "bot_request") return work();
        if (input.event["kind"] == "result") {
            check(input.event["status"] == "failed","recipient did not receive the failed Session outcome");
            return botReply("failed","Could not verify the second project");
        }
        if (input.event["kind"] == "bot_result") {
            check(input.event["status"] == "failed" && input.event["text"] == "Could not verify the second project",
                  "sender lost the recipient's explicit failure");
            return ask();
        }
        if (input.task["id"] == alpha) return botRequest("beta","work","Verify in the second project");
        return botRequest("alpha","work","Verify in the first project");
    };
    f.wait(beta,"model_failure");
    check(f.task(beta)["wait"]["text"].get<std::string>().find("project queues") != std::string::npos &&
          f.task(beta)["doneWhen"] == "" && f.store->tasks("alpha").size() == 1,
          "cross-project cycle was not rejected before creating work or saving a completion contract");
    f.store->operate(beta,0,"cancel");
    const auto recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"running");
    check(f.t3.launches == 1 && f.t3.seen.back().projectId == "second",
          "the original work request did not progress on its recipient project after the cycle was prevented");
    f.t3.finish("run-1","failed");
    f.wait(alpha,"user_question");
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 2 && messages.back()["status"] == "failed" && f.logs(alpha,"completed") == 0,
          "the original exchange hid the failed outcome or completed the sender's work");
}

void peerCancelWhileQueued() {
    Fixture f;
    f.allowBeta();
    f.peers.at("alpha").front().intake.projectId = "second";
    const auto running = f.submit("Existing verification","beta","second");
    const auto source = f.submit("Request separate verification in the occupied project");
    bool receivedCancellation = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == running) return work();
        check(input.task["id"] == source,"queued work started before its project was available");
        if (input.event["kind"] == "intake") return botRequest("beta","work","Verify the second project");
        receivedCancellation = input.event["kind"] == "bot_result" && input.event["status"] == "cancelled";
        return ask();
    };
    f.reach(running,"running");
    f.reach(source,"peer_waiting");
    const auto queued = f.store->tasks("beta").front()["id"].get<Id>();
    check(f.task(queued)["state"] == "queued" && f.task(queued)["blockedBy"]["id"] == running,
          "test did not queue the delegated task behind an active Run");
    f.store->operate(queued,0,"cancel");
    f.reach(queued,"cancelled");
    f.wait(source,"user_question");
    check(receivedCancellation && f.task(running)["state"] == "running" && f.t3.launches == 1 && f.t3.interrupts == 0,
          "queued cancellation stranded the sender or affected the unrelated active Run");
}

void peerCancelDuringPreflight() {
    Fixture f;
    f.allowBeta();
    f.peers.at("alpha").front().intake.projectId = "second";
    const auto source = f.submit("Delegate verification to the second project");
    f.model.script = [](const ModelInput& input) {
        if (input.event["kind"] == "bot_request") return work();
        return botRequest("beta","work","Verify the second project");
    };
    f.reach(source,"peer_waiting");
    const auto recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"sending");

    bool cancelledDuringPreflight = false;
    f.t3.onPreflight = [&] {
        Store cli(f.directory/"state.db");
        cli.operate(source,0,"cancel");
        cancelledDuringPreflight = true;
    };
    f.runtime->tick();
    f.t3.onPreflight = {};
    check(cancelledDuringPreflight && f.t3.seen.empty(),"a peer launch started after its sender was cancelled");
    f.reach(source,"cancelled");
    f.reach(recipient,"cancelled");
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
            {"Bot handoff and restart",peerWorkAndRestart},{"Bot consultation and permission",peerConsultationAndPermission},
            {"Bot handoff cancellation",peerCancelWithLateSessionResult},{"Bot project cycle",peerProjectCycle},
            {"Queued Bot handoff cancellation",peerCancelWhileQueued},
            {"Bot handoff cancellation during T3 preflight",peerCancelDuringPreflight},
            {"stale in-flight model decision",inferenceInvalidatedByCancel}}) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
    }
}
