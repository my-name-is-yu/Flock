#include "flock/core.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <sqlite3.h>
#include <unistd.h>

using namespace flockv0;

namespace {
Decision converse(const ModelInput& input, const std::string& tool, Id taskId, Json args) {
    if (!input.event.at("turn").empty()) return {"reply",{{"text","Done."}},"Report the change"};
    if (taskId) {
        for (const auto& task : input.task.at("tasks")) {
            if (task.at("taskId") != taskId) continue;
            args["taskId"] = taskId;
            args["generation"] = task.at("generation");
            if (tool == "change_task") args["revisionId"] = task.at("revisionId");
        }
    }
    const auto text = tool == "start_task" ? "request" : "instruction";
    if (!args.contains(text)) args[text] = input.event.at("text");
    return {tool,std::move(args),"Scripted turn"};
}
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void executeSql(const std::filesystem::path& databaseFile, const char* sql) {
    sqlite3* database = nullptr;
    const auto opened = sqlite3_open(databaseFile.c_str(),&database);
    if (opened != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "Could not open the test database";
        if (database) sqlite3_close(database);
        throw std::runtime_error(message);
    }
    char* error = nullptr;
    const auto result = sqlite3_exec(database,sql,nullptr,nullptr,&error);
    const std::string message = error ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK) throw std::runtime_error(message);
}
void setSessionReadJournalFailure(const std::filesystem::path& databaseFile, bool active) {
    executeSql(databaseFile,active ?
        "CREATE TRIGGER fail_session_read_journal BEFORE INSERT ON journal "
        "WHEN NEW.kind='session_read' BEGIN SELECT RAISE(ABORT,'injected journal failure'); END" :
        "DROP TRIGGER fail_session_read_journal");
}
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
Decision botReply(const char* kind, std::string_view message) {
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
    std::function<void()> beforeRead;
    std::vector<RunRef> interruptedRuns;
    std::function<void()> onPreflight;
    std::function<void()> beforeWorkAcknowledgement;
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
        if (auto callback = std::exchange(beforeWorkAcknowledgement,{})) callback();
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
        interruptedRuns.push_back(ref);
        ++interrupts;
    }
    Excerpt read(const ReadRequest& req) override {
        const bool reported = std::any_of(runs.begin(),runs.end(),[&](const auto& entry) {
            return entry.second.resultRunId == req.runId;
        });
        check(!req.runId.empty() && (runs.contains(req.runId) || reported),"read unrelated Run");
        readRequests.push_back(req);
        ++reads;
        if (beforeRead) beforeRead();
        return {reads == 1 ? "commit wanted, updates enabled" : "RSS 25 MiB",
                req.threadId,req.runId,"item-"+std::to_string(reads),"next",100,reads==1};
    }
    void question(std::string run) {
        runs.at(run).kind = Observation::Kind::Question;
        runs.at(run).question = {"branch-question",Json::array({{{"id","branch"},{"options",Json::array({
            {{"label","Main branch"},{"value","main"}},{{"label","Development branch"},{"value","dev"}}
        })}}})};
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
    check(f.model.calls == 5 && f.logs(id,"model_failure") == 3 && f.logs(id,"tool_error") == 2 &&
          f.t3.launches == 0 && f.logs(id,"decision") == 0,"invalid initial action executed");
    f.store->operate(id,wait,"retry");
    f.rejects(id,wait,"retry");
    f.model.script = [](const ModelInput& input) {
        check(input.feedback.empty() && input.event.at("decisionFeedback").empty(),"retry kept prior errors");
        return work();
    };
    f.reach(id,"running");
    f.t3.question("run-1");
    f.t3.runs.at("run-1").question.fields.push_back({{"id","checks"},{"multiple",true},{"freeform",true},
        {"options",Json::array({{{"label","Lint"},{"value","lint"}}})}});
    f.model.script = [](const ModelInput&) { return work(); };
    const auto questionWait = f.wait(id,"model_failure");
    check(f.t3.launches == 1 && f.t3.replies == 0 && f.task(id)["doneWhen"] == Evidence,"invalid in-question action executed");
    f.store->operate(id,questionWait,"retry");
    int answerAttempts = 0;
    f.model.script = [&](const ModelInput& input) {
        auto answer = reply();
        if (++answerAttempts == 1) answer.args["answers"]["checks"] = Json::array({"lint","another check"});
        else {
            check(!input.event.at("decisionFeedback").empty(),"model did not receive feedback on its invalid answer");
            answer.args["answers"]["checks"] = "another check";
        }
        return answer;
    };
    f.reach(id,"running");
    check(answerAttempts == 2 && f.t3.replies == 1 && f.t3.seen.back().answers.at("checks") == "another check",
          "T3 received the invalid answer instead of the model's correction");
    f.t3.finish("run-1");
    f.model.script = [](const ModelInput&) { return work(); };
    f.wait(id,"model_failure");
    check(f.t3.launches == 1 && f.task(id)["doneWhen"] == Evidence,"doneWhen overwritten");
}

void correctInvalidTaskArguments() {
    Fixture f;
    const auto id = f.submit();
    const Json badArgs = {{"question","Which branch?"},{"choices",12}};
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (f.model.calls == 1) return {"ask_user",badArgs,"Need a branch"};
        const auto& feedback = input.event.at("decisionFeedback");
        check(feedback.size() == 1 && feedback[0].at("tool") == "ask_user" && feedback[0].at("args") == badArgs &&
              feedback[0].at("result").at("ok") == false &&
              feedback[0].at("result").at("error") == "Expected up to twelve short textual choices",
              "model did not receive the rejected arguments and validation result");
        return ask();
    };
    f.reach(id,"deciding");
    f.runtime->tick();
    f.restart();
    const auto wait = f.wait(id,"user_question");
    check(f.model.calls == 2 && !f.logs(id,"model_failure") && f.logs(id,"decision") == 1,
          "correcting an invalid tool call consumed a model-failure strike");
    f.store->operate(id,wait,"answer",{{"text","main"}});
    f.model.script = [](const ModelInput& input) {
        check(input.event.at("decisionFeedback").empty(),"validation errors leaked into the user's answer event");
        return work();
    };
    f.reach(id,"running");
}

void replayQuestionDecision() {
    for (const bool stillOpen : {true,false}) {
        Fixture f;
        const auto id = f.store->submit({"alpha","Use main","e","p",{{"modelCallLimit",2}}});
        f.model.script = [&](const ModelInput& input) {
            if (input.event.at("kind") == "intake") return work();
            // Lose T3 visibility at the post-inference liveness check, before any reply can commit.
            f.t3.runs.at("run-1").kind = Observation::Kind::Unavailable;
            return reply();
        };
        f.reach(id,"running");
        f.t3.question("run-1");
        f.reach(id,"deciding");
        f.runtime->tick();
        check(f.model.calls == 2 && f.task(id)["state"] == "deciding" && !f.t3.replies,
              "reply did not stop between inference and effect");
        f.restart();
        if (stillOpen) f.t3.question("run-1");
        else f.t3.runs.at("run-1") = {};
        f.reach(id,"running");
        check(f.model.calls == 2 && f.t3.replies == int(stillOpen) && !f.task(id).contains("pendingDecision"),
              "restart repeated inference or applied a stale question reply");
    }
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
        return work(used == 1 ? "delegate" : "follow_up",false);
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
    check(f.task(id)["wait"]["attempted"] == "follow_up" && f.task(id)["remaining"] == 0 &&
          f.t3.seen.size() == 4,"normal answer permitted an extra follow-up");
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
            const auto task = f.task(id);
            check(task["question"]["target"] == Json{{"waitId",wait},{"eventId",task["eventId"]},
                {"run",task["activeRun"]},{"requestId","branch-question"}},"user wait lost its Session target");
            executeSql(f.store->path(),"UPDATE tasks SET data=json_set(data,'$.question.target.eventId',0) WHERE state='waiting'");
            f.rejects(id,wait,"answer",{{"text","main"}});
            check(f.logs(id,"user_answer") == 0,"answer for a stale question target was recorded");
            executeSql(f.store->path(),"UPDATE tasks SET data=json_set(data,'$.question.target.eventId',"
                                       "json_extract(data,'$.eventId')) WHERE state='waiting'");
            f.restart();
            f.store->operate(id,wait,"answer",{{"text","main"}});
            const auto answered = f.task(id);
            check(answered["question"]["target"]["waitId"].is_null() &&
                  answered["question"]["target"]["eventId"] == answered["eventId"] &&
                  answered["eventId"] != task["eventId"],"answer did not advance the question target");
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

void failedPreflightIsUnsent() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.t3.onPreflight = [] { throw ModelError("Missing T3 settings",false); };
    const auto wait = f.wait(id,"settings");
    check(f.t3.seen.empty() && f.logs(id,"send_started") == 0,"failed preflight started a delivery");
    f.restart();
    f.t3.onPreflight = {};
    f.store->operate(id,wait,"reconcile");
    f.reach(id,"running");
    check(f.t3.launches == 1 && f.t3.reconciles == 0 && f.logs(id,"send_started") == 1,
          "preflight failure did not leave one unsent action");
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

void cancelDuringDelivery() {
    Fixture f;
    Store cli(f.directory/"state.db");
    const auto independent = f.submit("Other project work","beta","other");
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(independent,"running");
    const auto id = f.submit("Work being cancelled");
    const auto queued = f.submit("Next project task");
    f.model.script = [=](const ModelInput& i) {
        return i.task["id"] == queued ? ask() : work();
    };
    f.t3.beforeWorkAcknowledgement = [&] { cli.operate(id,0,"cancel"); };
    for (int i = 0; i < 12 && f.t3.launches < 2; ++i) f.runtime->tick();
    f.t3.beforeWorkAcknowledgement = {};
    f.restart();

    const auto cancelling = f.task(id);
    check(cancelling["state"] == "cancelling" && cancelling["activeRun"]["runId"] == "run-2",
          "cancellation lost the Run acknowledged by T3");
    for (int i = 0; i < 12 && f.t3.interruptedRuns.empty(); ++i) f.runtime->tick();
    check(f.t3.interruptedRuns.size() == 1 &&
          f.t3.interruptedRuns[0].threadId == "thread-2" &&
          f.t3.interruptedRuns[0].runId == "run-2",
          "cancelling delivery did not interrupt the Run that T3 accepted");
    check(f.task(queued)["state"] == "queued" && f.task(independent)["state"] == "running",
          "cancelling delivery changed the project queue or unrelated work");

    f.t3.finish("run-2","interrupted");
    f.reach(id,"cancelled");
    f.wait(queued,"user_question");
    check(f.t3.launches == 2 && f.logs(id,"report") == 1,
          "settling cancelled delivery duplicated a Run or its result");
}

void settleWhileAnswerDeliveryIsUnknown() {
    for (const bool cancel : {false,true}) {
        Fixture f;
        const auto id = f.submit();
        f.model.script = [](const ModelInput& i) {
            if (i.event["kind"] == "intake") return work();
            if (i.event["kind"] == "question") return reply();
            return ask();
        };
        f.reach(id,"running");
        f.t3.question("run-1");
        f.t3.loseAcknowledgement = true;
        const auto wait = f.wait(id,"unknown_delivery");
        f.t3.question("run-1");
        f.restart();

        if (cancel) {
            f.store->operate(id,0,"cancel");
            for (int i = 0; i < 12 && f.t3.interruptedRuns.empty(); ++i) f.runtime->tick();
            check(f.task(id)["state"] == "cancelling" &&
                  f.t3.interruptedRuns.size() == 1 &&
                  f.t3.interruptedRuns[0].threadId == "thread-1" &&
                  f.t3.interruptedRuns[0].runId == "run-1",
                  "unknown answer delivery blocked interruption of the known Run");
        }
        f.t3.finish("run-1",cancel ? "interrupted" : "completed");
        if (!cancel) {
            for (int i = 0; i < 12 && f.task(id)["wait"]["id"] == wait; ++i) f.runtime->tick();
            const auto review = f.wait(id,"user_question");
            f.rejects(id,wait,"reconcile");
            f.store->operate(id,review,"cancel");
        }
        f.reach(id,"cancelled");
        check(f.logs(id,"report") == 1 && f.t3.replies == 1 && f.t3.launches == 1,
              "settled Run duplicated its answer or report");
    }
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

void taskPagesShowProjectOwners() {
    Fixture f;
    f.model.script = [](const ModelInput&) { return work(); };
    const auto desktop = f.submit("Verify main", "beta");
    f.reach(desktop,"running");
    const auto cloud = f.store->submit({"beta","Check cloud","cloud","p"});
    const auto desktopNext = f.submit("Desktop follow-up");
    const auto independent = f.submit("Separate release","alpha","another-project");
    const auto cancelled = f.submit("Withdrawn release");
    f.store->operate(cancelled,0,"cancel");
    const auto cloudNext = f.store->submit({"alpha","Cloud follow-up","cloud","p"});
    f.restart();

    const auto newest = f.store->tasks("alpha",0,2);
    const Json cloudOwner = {{"id",cloud},{"botId","beta"},{"request","Check cloud"},{"state","queued"}};
    check(newest.size() == 2 && newest[0].at("id") == cloudNext && newest[0].at("blockedBy") == cloudOwner &&
          newest[1].at("id") == cancelled && newest[1].at("state") == "cancelled" && newest[1].at("blockedBy").is_null(),
          "the newest page lost the off-page cloud owner or blocked cancelled work");
    const auto older = f.store->tasks("alpha",cancelled,2);
    const Json desktopOwner = {{"id",desktop},{"botId","beta"},{"request","Verify main"},{"state","running"}};
    check(older.size() == 2 && older[0].at("id") == independent && older[0].at("blockedBy").is_null() &&
          older[1].at("id") == desktopNext && older[1].at("blockedBy") == desktopOwner &&
          f.store->tasks("alpha",desktopNext,2).empty(),
          "the older page skipped a task, repeated the cursor or mixed up project owners");
    const auto active = f.store->activeTasks("alpha");
    check(active.size() == 3 && active.front().at("id") == cloudNext && active.front().at("blockedBy") == cloudOwner &&
          active.back().at("id") == desktopNext && active.back().at("blockedBy") == desktopOwner,
          "the active task list lost the owners of work in separate environments");

    const auto history = f.store->tasks({},cloud,1);
    check(history.size() == 1 && history[0].at("id") == desktop && history[0].at("blockedBy").is_null() &&
          history[0].at("runs").size() == 1 && history[0].at("runs")[0].at("link") == "https://t3.test/thread" &&
          history[0].at("revisions").size() == 1 && history[0].at("revisions")[0].at("doneWhen") == Evidence &&
          history[0].at("runs")[0].at("revisionId") == history[0].at("revisions")[0].at("id"),
          "the unfiltered task page lost the running owner's Session link or revision after restart");
}

void peerWorkAndRestart() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Decide whether the PR can ship");
    const auto unrelated = f.submit("Ordinary request queued before the handoff");
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
            return complete();
        }
        check(input.task["botId"] == "beta" && input.notes.soul == "Second SOUL" && input.notes.memory == "BETA_ONLY",
              "work did not run as the recipient Bot");
        if (event["kind"] == "bot_request") {
            check(event["fromBotId"] == "alpha" && event["text"] == "Verify the PR before release" &&
                  input.task["request"] == "Verify the PR before release" &&
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
    check(f.t3.reads == 1 && f.logs(root,"completed") == 0,"reading evidence did not leave a result to evaluate");
    const auto& read = f.t3.readRequests.back();
    check(read.environmentId == "e" && read.threadId == "thread-1" && read.runId == "run-1-result" &&
          read.config.at("model") == "beta-model","peer evidence did not read the recipient's result with its own settings");
    f.restart();
    f.reach(root,"completed");
    f.wait(unrelated,"user_question");
    check(f.store->tasks("beta").size() == 1 && f.t3.launches == 1 && f.t3.reads == 1 &&
          f.task(root)["additionalUsed"] == 0 && f.logs(root,"user_answer") == 1 && f.logs(root,"completed") == 1,
          "restart duplicated the recipient, Session, evidence read, user answer or completion");
    const struct Message {
        const char* type;
        const char* from;
        const char* to;
        const char* text;
    } expected[] = {
        {"request","alpha","beta","Verify the PR before release"},
        {"question","beta","alpha","Which branch?"},
        {"answer","alpha","beta","Use main"},
        {"result","beta","alpha","Verified the PR on main"},
    };
    for (const auto bot : {"alpha","beta"}) {
        const auto messages = f.botMessages(bot);
        check(messages.size() == std::size(expected),"conversation lost or duplicated a message after restart");
        const auto exchange = messages.front().at("exchangeId").get<std::string>();
        check(!exchange.empty(),"conversation lost its exchange identity");
        std::set<std::string> ids;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            const auto& message = messages[i];
            const auto& want = expected[i];
            const auto id = message.at("messageId").get<std::string>();
            check(message.at("messageType") == want.type && message.at("fromBotId") == want.from &&
                  message.at("toBotId") == want.to && message.at("text") == want.text,
                  "conversation showed the wrong message or participants");
            check(!id.empty() && ids.insert(id).second && message.at("exchangeId") == exchange,
                  "conversation lost the identity of a reply or exchange");
            if (i > 0)
                check(message.at("replyTo") == messages[i - 1].at("messageId"),"conversation broke the reply chain");
        }
        check(messages.back().at("status") == "completed","conversation lost the successful outcome");
    }
}

void peerWorkSharesBudgetWithDirectT3Work() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Verify and report the PR");
    int peerReplies = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] != root) {
            ++peerReplies;
            return botReply("result","Verified the requested pass");
        }
        if (input.event["kind"] == "bot_result" && peerReplies == 4) return work("delegate",false);
        Json args = {{"botId","beta"},{"kind","work"},{"text","Verify the PR"}};
        if (input.event["kind"] == "intake") args["doneWhen"] = Evidence;
        return {"send_bot_message",std::move(args),"Use the configured verifier",{{"total_tokens",37}}};
    };

    for (int step = 0; step < 64 && f.task(root)["state"] != "waiting"; ++step) f.runtime->tick();
    const auto budget = f.wait(root,"budget");
    const auto beforeApproval = f.task(root);
    check(beforeApproval["wait"]["attempted"] == "delegate" && beforeApproval["remaining"] == 0 &&
          beforeApproval["additionalUsed"] == 3 && f.t3.launches == 0 && f.store->tasks("beta").size() == 4,
          "direct T3 work bypassed the allowance exhausted by peer work");
    f.store->operate(root,budget,"approve");
    f.reach(root,"running");
    check(f.t3.launches == 1 && f.task(root)["remaining"] == 0 && f.task(root)["additionalUsed"] == 4,
          "one approval did not admit exactly one direct T3 action");
    f.t3.finish("run-1");
    const auto secondBudget = f.wait(root,"budget");
    const auto paused = f.task(root);
    check(secondBudget != budget && paused["remaining"] == 0 && paused["additionalUsed"] == 4 &&
          f.store->tasks("beta").size() == 4,"the approved direct T3 action also granted unapproved peer work");
    const auto& wait = paused.at("wait");
    check(wait.at("attempted") == "send_bot_message" && wait.at("decisionReason") == "Use the configured verifier" &&
          wait.at("usage") == Json{{"total_tokens",37}},"the peer budget wait lost the proposed action or its usage");
}

void checkPeerClarificationAfterRun(std::string_view readMode) {
    Fixture f;
    f.read = readMode;
    f.restart();
    f.allowBeta();
    const auto root = f.submit("Verify the release");
    bool requestedClarificationRead = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        if (input.task["id"] == root) {
            if (event["kind"] == "intake") return botRequest("beta","work","Verify the release");
            if (event["kind"] == "bot_question") {
                check(event["text"] == "Which branch should I retest?","wrong question reached Release");
                if (!event.contains("userAnswer")) return ask();
                check(event["userAnswer"] == "main","wrong human answer reached Release");
                return botReply("answer","Retest main");
            }
            check(event["kind"] == "bot_result" && event["text"] == "Retested main" &&
                  event["references"][0]["runId"] == "run-2-result","Release lost the final evidence");
            if (input.evidence.empty())
                return {"read_bot_evidence",{{"messageId",event["messageId"]}},"Inspect the retest report"};
            check(input.evidence.back()["runId"] == "run-2-result","Release read a different Run's evidence");
            return complete();
        }
        if (event["kind"] == "bot_request") return work();
        if (event["kind"] == "bot_answer") {
            const auto& report = event.at("sessionResult");
            check(event["text"] == "Retest main" && report["run"]["runId"] == "run-1" &&
                  report["resultRunId"] == "run-1-result","Verifier lost the clarification or its session context");
            check(hasTool(input,"session_read") == (readMode == "on_demand"),"history read ignored the selected mode");
            if (readMode == "on_demand" && input.evidence.empty()) {
                requestedClarificationRead = true;
                return {"session_read",{{"threadId",report["run"]["threadId"]},{"runId",report["resultRunId"]}},
                        "Inspect the earlier verification"};
            }
            if (readMode == "on_demand")
                check(requestedClarificationRead,"on-demand reading exposed evidence without a request");
            if (readMode != "off")
                check(!input.evidence.empty() && input.evidence.back()["runId"] ==
                      (readMode == "prefill" ? "run-1" : "run-1-result"),"Verifier lost the read excerpt");
            check(hasTool(input,"follow_up"),"Verifier lost the settled Session");
            return {"follow_up",{{"instruction","Retest main"}},"Continue the existing verification Session"};
        }
        check(event["kind"] == "result","unexpected event in Verifier");
        if (event["run"]["runId"] == "run-1") return botReply("question","Which branch should I retest?");
        return botReply("result","Retested main");
    };

    f.reach(root,"peer_waiting");
    const auto recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"running");
    const auto initialRun = f.task(recipient)["activeRun"];
    f.t3.finish("run-1");
    const auto waitId = f.wait(root,"user_question");
    f.store->operate(root,waitId,"answer",{{"text","main"}});
    const auto answerWasDelivered = [&] {
        const auto messages = f.botMessages("beta");
        return !messages.empty() && messages.back().at("messageType") == "answer" &&
               messages.back().at("text") == "Retest main";
    };
    for (int step = 0; step < 12 && !answerWasDelivered(); ++step) f.runtime->tick();
    check(answerWasDelivered(),"recipient conversation did not show the clarification before restart");
    f.restart();
    const auto readsBeforeAnswer = f.t3.reads;
    f.reach(recipient,"running");
    if (readMode == "off")
        check(f.t3.reads == readsBeforeAnswer,"the Bot answer read T3 despite session history being off");
    else
        check(f.t3.reads == readsBeforeAnswer + 1 && f.t3.readRequests.back().runId ==
              (readMode == "prefill" ? "run-1" : "run-1-result"),
              "the Bot answer did not reopen the earlier verification record");

    const auto continuation = f.t3.seen.back();
    check(f.t3.launches == 1 && f.t3.follows == 1 && continuation.target.threadId == initialRun["threadId"].get<std::string>() &&
          continuation.target.runId == initialRun["runId"].get<std::string>() &&
          continuation.instruction.find("Retest main") != std::string::npos && f.task(recipient)["additionalUsed"] == 1,
          "the retest did not continue the same settled T3 Session");
    f.t3.finish("run-2");
    f.reach(root,"completed");
    check(f.logs(recipient,"report") == 2 && f.logs(root,"completed") == 1 && f.botMessages("alpha").size() == 4,
          "the settled verification result was not delivered exactly once");
}

void peerClarificationAfterRun() {
    for (const auto mode : {"on_demand","prefill","off"}) checkPeerClarificationAfterRun(mode);
}

void revisionWaitsForPeerClarification(bool direct = false) {
    Fixture f;
    f.allowBeta();
    const Intake routing{"alpha","Release check","e","p",
                         {{"conversationEnvironments",{{"cloud","cloud"}}}}};
    const auto root = f.store->submit(routing);
    bool answeredPeer = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") {
            if (input.event.at("text") == "Use main")
                return converse(input,"answer_task_question",root,{{"waitId",input.event.at("replyTo").at("waitId")},
                                {"text",input.event.at("text")}});
            return converse(input,"change_task",root,{{"environment","cloud"}});
        }
        if (input.task.at("botId") == "beta") {
            if (kind == "bot_request") return botReply("question","Which branch?");
            check(kind == "bot_answer" && input.event.at("text") == "Use main",
                  "The recipient did not receive its clarification");
            return botReply("result","Verified main");
        }
        if (kind == "bot_question") {
            if (!input.event.contains("userAnswer")) return ask();
            check(f.task(root).at("environmentId") == "e" && f.t3.launches == 0,
                  "Replanning ran before the Bot exchange finished");
            answeredPeer = true;
            return botReply("answer",input.event.at("userAnswer").get_ref<const std::string&>());
        }
        check(kind == "intake","Unexpected event for the sender");
        return answeredPeer ? work() : botRequest("beta","consult","Check the release");
    };

    f.wait(root,"user_question");
    const auto question = f.task(root).at("wait").at("messageId").get<Id>();
    f.store->receive(routing,"move-release-check","Run Release check on cloud");
    f.runtime->runConversations();
    check(f.task(root).value("replanning",false) && f.task(root).at("pendingEnvironmentId") == "cloud",
          "The requested destination change did not reach the task");
    if (direct) f.store->operate(root,f.task(root).at("wait").at("id"),"answer",{{"text","Use main"}});
    else {
        f.store->receive(routing,"branch-answer","Use main",question);
        f.runtime->runConversations();
    }
    check(f.logs(root,"user_answer") == 1,"The conversation rejected the answer the peer is waiting for");
    f.reach(root,"running");
    const auto messages = f.botMessages("alpha");
    check(answeredPeer && messages.size() == 4 && messages.back().at("messageType") == "result" &&
          f.t3.launches == 1 && f.t3.seen.front().environmentId == "cloud",
          "The task did not finish the Bot exchange before launching once in the new environment");
}

void peerCompletionAfterEnvironmentMove() {
    for (const auto [kind,environment] : {std::pair{"work","cloud"},{"consult","cloud"},{"work","e"}}) {
        Fixture f;
        f.allowBeta();
        const Intake routing{"alpha","Release check","e","p",{{"conversationEnvironments",{{"cloud","cloud"}}}}};
        const auto sender = f.store->submit(routing);
        f.model.script = [&](const ModelInput& input) -> Decision {
            const auto& event = input.event;
            if (event.at("kind") == "conversation_turn")
                return converse(input,"change_task",sender,{{"environment","cloud"}});
            if (input.task.at("botId") == "beta")
                return {"reply_to_bot",{{"kind","result"},{"text",event.at("text")}},"Return the requested result"};
            if (event.at("kind") == "intake") {
                if (!input.task.contains("userInstructions")) return botRequest("beta","work","Original result");
                check(f.task(sender).at("environmentId") == "cloud" && !hasTool(input,"complete"),
                      "the pre-move result unlocked completion after the move");
                return {"send_bot_message",{{"botId","beta"},{"kind",kind},{"text","Cloud result"}},"Verify the moved task"};
            }
            if (std::string_view(environment) != "cloud") {
                check(!hasTool(input,"complete"),"a peer on the old route unlocked completion");
                return ask();
            }
            check(event.at("kind") == "bot_result" && event.at("text") == "Cloud result" && hasTool(input,"complete"),
                  "the current peer result did not unlock completion");
            const auto& move = input.task.at("userInstructions").at(0);
            return {"complete",{{"report","Cloud result"},{"acknowledgedInstructions",Json::array({
                {{"messageId",move.at("messageId")},{"step",move.at("step")}}})}},"Verified after the move"};
        };
        f.reach(sender,"peer_waiting");
        f.peers.at("alpha").front().intake.environmentId = environment;
        f.store->receive(routing,"move-release-check","Move Release check to cloud");
        f.runtime->runConversations();
        if (std::string_view(environment) != "cloud") {
            f.wait(sender,"user_question");
            continue;
        }
        f.reach(sender,"completed");
        const auto task = f.task(sender);
        check(task.at("revisions").size() == 1 && !task.contains("newSessionRequired") &&
              !task.contains("pendingInstructions") && f.t3.launches == 0 && f.logs(sender,"completed") == 1,
              "peer completion left the moved task unfinished");
    }
}

void recipientRevisionContinuesTheOpenWorkExchange() {
    Fixture f;
    f.allowBeta();
    const auto sender = f.submit("Release verification");
    Id recipient = 0;
    bool attemptedFailure = false;
    constexpr auto revisedCriteria = "Confirm the rollback plan with the revised evidence";
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto kind = event.at("kind").get<std::string>();
        if (kind == "conversation_turn" && event["text"] == "How is verification going?")
            return {"reply",{{"text","Checking."}},"Report progress"};
        if (kind == "conversation_turn") return converse(input,"change_task",recipient,{{"doneWhen",revisedCriteria}});
        if (input.task["id"] == sender) {
            if (kind == "intake") return botRequest("beta","work","Verify release");
            check(kind == "bot_result" && event["text"] == "Verified under revised criteria" &&
                  event["references"].size() == 1 && event["references"][0]["runId"] == "run-2-result",
                  "the sender resumed from old or unrelated peer evidence");
            return complete();
        }
        if (kind == "bot_request") return work();
        if (input.task.contains("userInstructions") && kind == "result") {
            if (!std::exchange(attemptedFailure,true)) {
                f.store->receive({"beta","","e","p",Json::object()},"status","How is verification going?");
                return botReply("failed","Unable to verify the original criteria");
            }
            return work("follow_up",false);
        }
        check(kind == "result" && input.task["doneWhen"] == revisedCriteria &&
              event["run"]["runId"] == "run-2",
              "the old Run result was exposed as the receiving Bot's revised evidence");
        return botReply("result","Verified under revised criteria");
    };

    f.reach(sender,"peer_waiting");
    recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"running");
    const std::string revisionText = "Revise Verify release completion criteria to " + std::string(revisedCriteria);
    const Intake betaRouting{"beta",revisionText,"e","p",{{"runTimeoutSeconds",10},{"model","beta-model"}}};
    f.store->receive(betaRouting,"recipient-revision",revisionText);
    f.runtime->runConversations();
    check(f.task(recipient).value("replanning",false) && f.task(recipient)["doneWhen"] == revisedCriteria &&
          f.task(sender)["state"] == "peer_waiting" && f.botMessages("alpha").size() == 1,
          "the recipient revision did not remain in the existing work exchange");

    f.restart();
    for (int step = 0; step < 4; ++step) f.runtime->tick();
    check(f.t3.seen.size() == 1 && f.task(recipient)["activeRun"]["runId"] == "run-1" &&
          f.task(sender)["state"] == "peer_waiting" && f.botMessages("alpha").size() == 1,
          "restart dispatched revised work or returned old evidence before the prior Run settled");
    f.t3.finish("run-1","completed",false);
    for (int step = 0; step < 4; ++step) f.runtime->tick();
    check(f.t3.seen.size() == 1 && f.task(recipient)["activeRun"]["runId"] == "run-1" &&
          f.task(sender)["state"] == "peer_waiting" && f.botMessages("alpha").size() == 1,
          "revised work started before the old Run was settled");

    f.t3.finish("run-1");
    for (int step = 0; step < 12 && !attemptedFailure; ++step) f.runtime->tick();
    check(attemptedFailure && !f.task(recipient).contains("pendingDecision") && f.logs(recipient,"tool_error") == 1 &&
          !f.logs(recipient,"peer_completed"),"failed reply bypassed the outstanding revision before deferral");
    for (int step = 0; step < 24 && f.t3.follows == 0; ++step) f.runtime->tick();
    check(f.t3.follows == 1 && f.t3.seen.size() == 2 && f.t3.seen.back().tool == "follow_up" &&
          f.t3.seen.back().target.runId == "run-1" &&
          f.t3.seen.back().instruction.find(revisedCriteria) != std::string::npos &&
          f.task(recipient)["activeRun"]["runId"] == "run-2" && f.botMessages("alpha").size() == 1,
          "settling the old Run did not continue the same exchange under revised criteria");

    f.t3.finish("run-2");
    f.reach(recipient,"completed");
    f.reach(sender,"completed");
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 2 && messages[1]["messageType"] == "result" &&
          messages[1]["text"] == "Verified under revised criteria" &&
          messages[1]["evidence"][0]["runId"] == "run-2-result" &&
          messages[0]["exchangeId"] == messages[1]["exchangeId"] &&
          f.botMessages("beta").size() == 2 && f.logs(recipient,"report") == 2 &&
          f.logs(recipient,"peer_completed") == 1 && f.logs(sender,"completed") == 1 &&
          f.t3.launches == 1 && f.t3.follows == 1,
          "the revised result did not close the exchange once and resume its sender");
}

void checkRecipientRevisionDuringPeerQuestion(bool moves) {
    Fixture f;
    f.peers["alpha"] = {{"Verifier",{"beta","","e","beta-project",
        {{"runTimeoutSeconds",10},{"model","beta-model"}}}}};
    const auto sender = f.submit("Verify the release");
    Id recipient = 0;
    constexpr auto revisedCriteria = "Confirm the rollback plan with the revised evidence";
    bool attemptedInvalidDecision = false;
    bool sawAnswer = false;
    bool offeredFollowUp = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto kind = event.at("kind").get<std::string>();
        if (kind == "conversation_turn") {
            Json fields = {{"doneWhen",revisedCriteria}};
            if (moves) fields["environment"] = "cloud";
            return converse(input,"change_task",recipient,fields);
        }
        if (input.task["id"] == sender) {
            if (kind == "intake") return botRequest("beta","work","Verify the release");
            if (kind == "bot_question") {
                if (!event.contains("userAnswer")) return ask();
                check(event["userAnswer"] == "Use main","the sender lost the human's answer to its question");
                return botReply("answer","Use main");
            }
            check(kind == "bot_result" && event["text"] == "Retested main under revised criteria" &&
                  event["references"].size() == 1 && event["references"][0]["runId"] == "run-2-result",
                  "the sender resumed from the wrong peer evidence");
            return complete();
        }
        if (kind == "bot_request") return work();
        if (kind == "result") {
            if (event["run"]["runId"] == "run-1") return botReply("question","Which branch?");
            check(event["run"]["runId"] == "run-2" && input.task["doneWhen"] == revisedCriteria,
                  "the revised result did not use the new completion criteria");
            return botReply("result","Retested main under revised criteria");
        }
        check(kind == "bot_answer" && event["text"] == "Use main" &&
              event["sessionResult"]["run"]["runId"] == "run-1" &&
              input.task["doneWhen"] == revisedCriteria &&
              input.task.at("userInstructions").front().at("text").get<std::string>().find(revisedCriteria) != std::string::npos,
              "the sender answer or revised instructions did not reach the recipient model");
        offeredFollowUp = hasTool(input,"follow_up");
        if (!attemptedInvalidDecision) {
            attemptedInvalidDecision = true;
            return moves ? work("follow_up",false) : botReply("result","Result from the old criteria");
        }
        sawAnswer = true;
        if (moves) return {"delegate",{{"instruction","Repeat verification"}},"Start a fresh Session in cloud"};
        return {"follow_up",{{"instruction","Retest main after the sender answered: Use main"}},
                "Continue under the revised completion criteria"};
    };

    f.reach(sender,"peer_waiting");
    recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"running");
    f.t3.finish("run-1");
    f.t3.runs.at("run-1").report = "Previous verification found one failing check";
    const auto waitId = f.wait(sender,"user_question");
    check(f.task(recipient)["state"] == "peer_waiting" && f.botMessages("beta").size() == 2 &&
          f.botMessages("beta").back()["messageType"] == "question",
          "the recipient did not leave its settled Run in a peer question");

    const std::string revisionText = "Revise Verify the release completion criteria to " + std::string(revisedCriteria) +
        (moves ? " and move it to cloud" : "");
    const Intake betaRouting{"beta",revisionText,"e","beta-project",{{"runTimeoutSeconds",10},{"model","beta-model"},
        {"conversationEnvironments",{{"cloud","cloud"}}}}};
    f.store->receive(betaRouting,"recipient-question-revision",revisionText);
    f.runtime->runConversations();
    for (int step = 0; step < 3; ++step) f.runtime->tick();
    check(f.task(recipient).value("replanning",false) && f.task(recipient)["state"] == "peer_waiting" &&
          f.task(recipient)["doneWhen"] == revisedCriteria && f.task(sender)["wait"]["reason"] == "user_question",
          "the recipient replanned during the pending peer question");

    f.store->operate(sender,waitId,"answer",{{"text","Use main"}});
    f.reach(recipient,"running");
    const auto command = f.t3.seen.back();
    if (moves) {
        check(f.t3.launches == 2 && f.t3.follows == 0 && command.tool == "delegate" && command.environmentId == "cloud" &&
              command.target.threadId.empty() && command.target.runId.empty() &&
              command.instruction.find("Verify the release") != std::string::npos &&
              command.instruction.find("Previous verification found one failing check") != std::string::npos,
              "the fresh Session received the old target or lost the task context");
    } else {
        check(f.t3.launches == 1 && f.t3.follows == 1 && command.tool == "follow_up" && command.target.runId == "run-1",
              "the same-environment revision lost its existing Session");
    }
    check(attemptedInvalidDecision && sawAnswer && offeredFollowUp == !moves && f.t3.seen.size() == 2 &&
          command.instruction.find("Use main") != std::string::npos &&
          command.instruction.find(revisedCriteria) != std::string::npos &&
          f.botMessages("alpha").size() == 3 && f.botMessages("beta").back()["messageType"] == "answer" &&
          f.task(sender)["wait"].is_null() && f.task(recipient)["wait"].is_null(),
          "the answer did not reach revised work or an old-evidence result closed the exchange");

    f.t3.finish("run-2");
    f.reach(recipient,"completed");
    f.reach(sender,"completed");
    const auto alphaMessages = f.botMessages("alpha");
    const auto betaMessages = f.botMessages("beta");
    check(alphaMessages.size() == 4 && betaMessages.size() == 4 &&
          alphaMessages.back()["messageType"] == "result" &&
          alphaMessages.back()["text"] == "Retested main under revised criteria" &&
          alphaMessages.back()["evidence"][0]["environmentId"] == (moves ? "cloud" : "e") &&
          alphaMessages.back()["evidence"][0]["runId"] == "run-2-result" &&
          f.logs(recipient,"peer_completed") == 1 && f.logs(sender,"completed") == 1 &&
          f.logs(recipient,"tool_error") == 1 && f.logs(sender,"model_failure") == 0 &&
          f.task(sender)["wait"].is_null() && f.task(recipient)["wait"].is_null(),
          "the revised result did not close the same exchange once without model-failure waits");
}

void recipientRevisionDuringPeerQuestionPreservesTheAnswer() { checkRecipientRevisionDuringPeerQuestion(false); }
void recipientMoveDuringPeerQuestionStartsAFreshSession() { checkRecipientRevisionDuringPeerQuestion(true); }

void checkWorkRecipientMove(bool sharesSenderQueue) {
    Fixture f;
    const std::string project = sharesSenderQueue ? "p" : "beta-project";
    const Intake betaRouting{"beta","Verify the release","cloud",project,
        {{"conversationEnvironments",{{"desktop","e"}}}}};
    f.peers["alpha"] = {{"Verifier",betaRouting}};
    const auto sender = f.submit("Release check");
    Id recipient = 0, unrelated = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") return converse(input,"change_task",recipient,{{"environment","desktop"}});
        if (input.task["id"] == sender) {
            if (kind == "intake") return botRequest("beta","work","Verify the release");
            check(kind == "bot_result" && input.event["references"][0]["environmentId"] == "e",
                  "the sender did not receive evidence from the destination");
            return complete();
        }
        if (input.task["id"] == unrelated) return kind == "intake" ? work() : complete();
        if (kind == "bot_request") return work();
        if (kind == "result" && input.task.contains("userInstructions")) return work("delegate",false);
        check(kind == "result" && input.event["environmentId"] == "e","the recipient judged the earlier Run");
        return botReply("result","Verification complete");
    };

    f.reach(sender,"peer_waiting");
    recipient = f.store->tasks("beta").front()["id"].get<Id>();
    f.reach(recipient,"running");
    unrelated = f.submit("Unrelated verification","alpha",project);
    if (!sharesSenderQueue) f.reach(unrelated,"running");
    const auto launchesBeforeMove = f.t3.launches;
    f.store->receive(betaRouting,"recipient-move","Move Verify the release to desktop");
    f.runtime->runConversations();
    check(f.task(recipient).value("replanning",false) && f.task(recipient)["pendingEnvironmentId"] == "e",
          "the destination change did not reach the task");
    f.t3.finish("run-1");

    if (!sharesSenderQueue) {
        for (int step = 0; step < 5; ++step) f.runtime->tick();
        check(f.task(recipient).value("environmentReleased",false) && f.t3.launches == launchesBeforeMove &&
              f.task(unrelated)["state"] == "running","the move bypassed an unrelated destination owner");
        f.t3.finish("run-2");
        f.reach(unrelated,"completed");
    }
    for (int step = 0; step < 12 && f.t3.launches == launchesBeforeMove; ++step) f.runtime->tick();
    f.reach(recipient,"running");
    check(f.t3.launches == launchesBeforeMove + 1 && f.t3.follows == 0 && f.t3.seen.back().environmentId == "e" &&
          f.task(sender)["state"] == "peer_waiting" &&
          (!sharesSenderQueue || f.task(unrelated)["blockedBy"]["id"] == recipient),
          "the moved recipient did not inherit the exchange's queue position");
    f.t3.finish(f.task(recipient)["activeRun"]["runId"].get<std::string>());
    f.reach(recipient,"completed");
    f.reach(sender,"completed");
    check(f.logs(recipient,"peer_completed") == 1 && f.logs(sender,"completed") == 1,
          "the environment move did not finish the existing exchange once");
    if (sharesSenderQueue) f.reach(unrelated,"running");
}

void workRecipientMoveSharesSenderQueue() { checkWorkRecipientMove(true); }
void workRecipientMoveWaitsForUnrelatedOwner() { checkWorkRecipientMove(false); }

void consultationRepliesAfterUserInstructions() {
    for (const bool revise : {false,true}) {
        Fixture f;
        f.allowBeta();
        const auto sender = f.submit("Release consultation");
        Id recipient = 0;
        const std::string instruction = revise ? "Revise Check the release to inspect rollback policy" :
            "Also inspect rollback policy for Check the release\n" + std::string(7000,'x') +
            std::string(7000,'\n') + "exclude deprecated branches";
        f.model.script = [&](const ModelInput& input) -> Decision {
            const auto& event = input.event;
            const auto& kind = event.at("kind");
            if (kind == "conversation_turn") {
                if (revise) return converse(input,"change_task",recipient,{{"intent","inspect rollback policy"}});
                return converse(input,"send_to_task",recipient,Json::object());
            }
            if (input.task["id"] == sender) {
                if (kind == "intake") return botRequest("beta","consult","Check the release");
                if (kind == "bot_question") {
                    if (!event.contains("userAnswer")) return ask();
                    return botReply("answer","Use main");
                }
                check(kind == "bot_result" && event["text"] == "Rollback policy confirmed",
                      "the sender did not receive the consultation result");
                return complete();
            }
            if (kind == "bot_request") return botReply("question","Which branch?");
            check(kind == "bot_answer" && event["text"] == "Use main" && !hasTool(input,"delegate") &&
                  input.task.at("userInstructions").front().at("text").get<std::string>()
                          .find(revise ? instruction : "exclude deprecated branches") != std::string::npos,
                  "the consultation lost the peer answer or the user instruction");
            return botReply("result","Rollback policy confirmed");
        };

        f.reach(sender,"peer_waiting");
        recipient = f.store->tasks("beta").front()["id"].get<Id>();
        const auto wait = f.wait(sender,"user_question");
        f.store->receive({"beta","","e","p",Json::object()},"consultation-instruction",instruction);
        f.runtime->runConversations();
        check(f.logs(recipient,"instruction_accepted") == 1 && f.task(recipient).value("replanning",false) == revise,
              "the instruction did not reach the open consultation");
        f.store->operate(sender,wait,"answer",{{"text","Use main"}});
        f.reach(recipient,"completed");
        f.reach(sender,"completed");
        check(f.t3.seen.empty() && f.logs(recipient,"model_failure") == 0 && f.botMessages("alpha").size() == 4,
              "the consultation did not close normally after the user instruction");
    }
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
    check(deniedAttempts == 6 && f.store->tasks().size() == 1 && f.botMessages("alpha").empty(),
          "an unlisted recipient received a message or the available send tool was not exercised");
    f.store->operate(denied,0,"cancel");
    f.reach(denied,"cancelled");

    f.peers.at("alpha").front().intake.projectId = "second";
    const auto busy = f.submit("Run a verification in the Verifier's project","beta","second");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == busy) return work();
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
    f.reach(busy,"running");
    const auto consultation = f.submit("Ask the Verifier about our release policy");
    f.reach(consultation,"peer_waiting");
    const auto consulting = f.store->tasks("beta",0,1);
    check(consulting.size() == 1 && consulting[0].at("request") == "What is the release policy?" &&
          consulting[0].at("state") == "queued" && consulting[0].at("blockedBy").is_null(),
          "the consultation was shown as blocked by the Verifier's running task");
    f.reach(consultation,"completed");
    const auto messages = f.botMessages("alpha");
    check(f.t3.seen.size() == 1 && f.t3.interrupts == 0 && f.task(busy)["state"] == "running" &&
          f.task(consultation)["additionalUsed"] == 0 && f.store->tasks("beta").size() == 2,
          "consultation used T3, consumed work allowance or affected the Verifier's active Run");
    check(messages.size() == 2 && messages.front().at("text") == "What is the release policy?" &&
          messages.back().at("fromBotId") == "beta" && messages.back().at("text") == "Require a verified PR",
          "consultation did not record the corresponding request and response");
}

void requestedCriteriaApplyToFirstPeerWork() {
    Fixture f;
    f.allowBeta();
    const auto receipt = f.store->receive({"alpha","","e","p"},"work-with-criteria","Check the release");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "conversation_turn")
            return converse(input,"start_task",0,{{"doneWhen","Confirm the signature"}});
        check(!input.needsDoneWhen && input.task.at("requestedDoneWhen") == "Confirm the signature",
              "peer work required the model to repeat the user's criterion");
        return {"send_bot_message",{{"kind","work"},{"text","Verify the release"},{"botId","beta"}},"Ask the verifier"};
    };
    f.runtime->runConversations();
    const Id sender = f.store->tasks("alpha").at(0).at("id");
    f.reach(sender,"peer_waiting");
    const auto task = f.task(sender);
    check(task.at("doneWhen") == "Confirm the signature" && task.at("remaining") == 3 &&
          task.at("revisions").at(0).at("sourceMessageId") == receipt.at("messageId"),
          "first peer work did not adopt the saved criterion and its source");
}

void modelBudgetFollowsBotConsultation() {
    Fixture f;
    f.allowBeta();
    constexpr auto request = "Check the release policy";
    const auto receipt = f.store->receive({"alpha",request,"e","p",{{"modelCallLimit",3}}},"policy",request);
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event["kind"] == "conversation_turn") return converse(input,"start_task",0,Json::object());
        if (input.event["kind"] == "intake") return botRequest("beta","consult","What is the release policy?");
        if (input.event["kind"] == "bot_request") {
            check(input.config["model"] == "beta-model","recipient did not get its own model configuration");
            return botReply("result","Require a verified PR");
        }
        check(input.event["kind"] == "bot_result","sender did not resume with the consultation reply");
        return complete();
    };
    f.runtime->runConversations();
    const Id sender = f.store->tasks("alpha").front().at("id");
    check(f.task(sender)["sourceMessageId"] == receipt["messageId"],"routed task lost its originating message");
    f.reach(sender,"peer_waiting");
    const Id recipient = f.store->tasks("beta").front().at("id");
    const auto recipientWait = f.wait(recipient,"model_budget");
    f.restart();
    for (int i = 0; i < 4; ++i) f.runtime->tick();
    check(f.model.calls == 3 && f.task(recipient)["wait"]["id"] == recipientWait,
          "the recipient reached the provider without approval, or restart changed its wait");
    Store writer(f.directory/"state.db");
    writer.operate(recipient,recipientWait,"approve");
    f.rejects(recipient,recipientWait,"approve");
    const auto senderWait = f.wait(sender,"model_budget");
    check(f.model.calls == 4 && f.task(recipient)["state"] == "completed",
          "one approval did not resume only one call in the exchange");
    writer.operate(sender,senderWait,"approve");
    f.reach(sender,"completed");
    check(f.model.calls == 5 && f.t3.launches == 0 && f.task(sender)["remaining"] == 3,
          "the sender did not finish under its shared model budget and separate Run allowance");
}

void peerResultWaitsForRecipientTurn() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Check the release");
    int recipientDecisions = 0;
    bool noted = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") return {"reply",{{"text","Noted."}},"Acknowledge the note"};
        if (input.task.at("botId") == "beta") {
            ++recipientDecisions;
            if (!std::exchange(noted,true))
                f.store->receive({"beta","","e","p",Json::object()},"beta-note","How is the release check going?");
            return botReply("result","Verified");
        }
        if (kind == "bot_result") return complete();
        return botRequest("beta","consult","Check the release");
    };
    f.reach(root,"peer_waiting");
    for (int i = 0; i < 4 && !noted; ++i) f.runtime->tick();
    const Id recipient = f.store->tasks("beta").front().at("id");
    check(noted && f.task(recipient)["state"] != "completed" && f.task(root)["state"] == "peer_waiting",
          "the recipient closed the exchange while its own conversation turn was unfinished");
    f.reach(root,"completed");
    check(recipientDecisions == 1 && f.task(recipient)["state"] == "completed",
          "the held peer result asked the model again or was lost");
}

void modelBudgetCountsFailedAttempts() {
    Fixture f;
    const auto id = f.store->submit({"alpha","Retry a decision","e","p",{{"modelCallLimit",2}}});
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] != id || f.model.calls >= 3) return ask();
        if (f.model.calls == 1) throw ModelError("Transient failure with usage",true,{{"input_tokens",42}});
        throw ModelError("Transient failure without usage",true);
    };
    f.reach(id,"deciding");
    f.runtime->tick();
    f.now += 5;
    f.runtime->tick();
    f.now += 5;
    const auto wait = f.wait(id,"model_budget");
    f.restart();
    f.runtime->tick();
    check(f.model.calls == 2 && f.task(id)["wait"]["id"] == wait,"the third failed retry reached the model");

    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(f.store->path().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    check(opened == SQLITE_OK,"could not read the attempt ledger");
    std::vector<Json> usage;
    const auto result = sqlite3_exec(db.get(),"SELECT coalesce(usage,'null') FROM model_attempts ORDER BY id",
        [](void* context,int,char** values,char**) {
            static_cast<std::vector<Json>*>(context)->push_back(Json::parse(values[0],nullptr,false));
            return 0;
        },&usage,nullptr);
    check(result == SQLITE_OK && usage == std::vector<Json>{{{"input_tokens",42}},nullptr},
          "usage was lost or missing usage was counted as zero");

    const auto independent = f.store->submit({"alpha","Independent request","e","other",{{"modelCallLimit",2}}});
    f.wait(independent,"user_question");
    check(f.model.calls == 3 && f.task(id)["wait"]["id"] == wait,"an independent request needed the paused budget");
    f.store->operate(id,wait,"approve");
    f.wait(id,"user_question");
    check(f.model.calls == 4 && f.task(id)["remaining"] == 3,"approval did not resume the saved retry event");
}

void modelBudgetAllowsT3ObservationAndRecovery() {
    Fixture f;
    f.model.script = [](const ModelInput& input) { return input.event["kind"] == "intake" ? work() : reply(); };
    const auto id = f.store->submit({"alpha","Use one model call","e","p",{{"modelCallLimit",1}}});
    f.reach(id,"sending");
    f.t3.loseAcknowledgement = true;
    const auto deliveryWait = f.wait(id,"unknown_delivery");
    f.store->operate(id,deliveryWait,"reconcile");
    f.reach(id,"running");
    check(f.model.calls == 1 && f.t3.reconciles == 1 && f.t3.launches == 1,
          "delivery recovery used a model call or resent the Run");
    f.t3.question("run-1");
    const auto modelWait = f.wait(id,"model_budget");
    const auto polls = f.t3.polls;
    f.restart();
    f.runtime->tick();
    check(f.t3.polls > polls && f.model.calls == 1 && f.task(id)["wait"]["id"] == modelWait,
          "model approval blocked T3 observation or reset the pending decision");
    f.store->operate(id,modelWait,"approve");
    f.reach(id,"running");
    check(f.model.calls == 2 && f.t3.replies == 1 && f.task(id)["remaining"] == 3,
          "the approved call did not answer the original question");
}

void unsuccessfulConsultationsDoNotComplete() {
    for (const bool cancel : {false,true}) {
        Fixture f;
        f.allowBeta();
        const auto id = f.submit("Consult before deciding whether to release");
        f.model.script = [&](const ModelInput& input) -> Decision {
            if (input.event["kind"] == "intake") return botRequest("beta","consult","Can we release?");
            if (input.event["kind"] == "bot_request") return botReply("failed","Required evidence unavailable");
            check(input.event["kind"] == "bot_result" && input.event["status"] == (cancel ? "cancelled" : "failed") &&
                  !hasTool(input,"complete") && input.task["doneWhen"] == "",
                  "unsuccessful initial consultation unlocked completion");
            return ask();
        };
        f.reach(id,"peer_waiting");
        if (cancel) f.store->operate(f.store->tasks("beta").front().at("id"),0,"cancel");
        f.wait(id,"user_question");
        check(f.logs(id,"completed") == 0 && f.t3.launches == 0,"unsuccessful consultation completed or launched work");
    }
}

void concurrentPeerHandoff() {
    Fixture f;
    f.allowBeta();
    const auto source = f.submit("Confirm the release");
    const auto next = f.submit("Next in the project queue");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == next) return ask();
        if (input.task["id"] == source) {
            if (input.event["kind"] == "intake") return botRequest("beta","work","Verify the release");
            check(input.event["kind"] == "bot_result" && input.event["status"] == "completed",
                  "concurrent handoff lost the recipient result");
            return complete();
        }
        if (input.event["kind"] == "bot_request") return work();
        check(input.event["kind"] == "result","recipient missed its Session result");
        return botReply("result","Verified the release");
    };
    const auto until = [&](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            f.runtime->tickConcurrent();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(ready(),"concurrent handoff did not advance before its deadline");
    };
    until([&] {
        const auto recipients = f.store->tasks("beta");
        return recipients.size() == 1 && recipients.front()["state"] == "running";
    });
    const auto recipient = f.store->tasks("beta").front().at("id").get<Id>();
    check(f.task(source)["state"] == "peer_waiting" && f.task(next)["blockedBy"]["id"] == recipient,
          "the queued Task did not report the active peer as the owner");
    const auto page = f.store->tasks("alpha",0,1);
    const Json owner = {{"id",recipient},{"botId","beta"},{"request","Verify the release"},{"state","running"}};
    check(page.size() == 1 && page[0].at("id") == next && page[0].at("blockedBy") == owner,
          "the queued task page lost the work recipient that inherited the sender's slot");
    f.runtime.reset();
    f.t3.finish("run-1");
    f.restart();
    until([&] { return f.task(next)["state"] == "waiting"; });
    f.runtime.reset();
    check(f.task(source)["state"] == "completed" && f.task(recipient)["state"] == "completed" &&
          f.task(next)["wait"]["reason"] == "user_question" && f.t3.launches == 1 &&
          f.t3.follows == 0 && f.botMessages("alpha").size() == 2,
          "the concurrent handoff skipped or duplicated the source, recipient, or next Task");
}

void consultationRestoresOwnEvidence() {
    for (const auto mode : {"on_demand","prefill","off","prefill-unavailable"}) {
        Fixture f;
        const bool unavailable = std::string_view(mode) == "prefill-unavailable";
        f.read = unavailable ? "prefill" : mode;
        if (unavailable) f.t3.beforeRead = [&] {
            if (f.t3.reads == 2) throw std::runtime_error("Fixture T3 history unavailable after the consultation");
        };
        f.allowBeta();
        f.restart();
        const auto source = f.submit("Verify with a Session and consult");
        f.model.script = [&](const ModelInput& input) -> Decision {
            if (input.event["kind"] == "intake") return work();
            if (input.event["kind"] == "result") return botRequest("beta","consult","Confirm policy");
            if (input.event["kind"] == "bot_request") return botReply("result","Policy confirmed");
            check(input.event["kind"] == "bot_result" && input.event["sessionResult"]["resultRunId"] == "run-1-result",
                  "the consultation result omitted the source Bot's settled report");
            check(hasTool(input,"session_read") == (f.read == "on_demand") && !hasTool(input,"read_bot_evidence"),
                  "the source Bot's history ignored its selected read mode");
            if (f.read == "on_demand" && input.evidence.empty())
                return {"session_read",{{"threadId","thread-1"},{"runId","run-1-result"}},"Read original evidence"};
            check(input.evidence.size() == (f.read == "off" || unavailable ? 0 : 1),"own Session evidence did not match its selected mode");
            return complete();
        };
        f.reach(source,"running");
        f.t3.finish("run-1");
        f.reach(source,"completed");
        const auto expected = f.read == "off" ? 0 : f.read == "prefill" ? 2 : 1;
        check(f.t3.reads == expected && f.t3.launches == 1,"the consultation changed read or Run counts");
    }
}

void freshResultSurvivesUnavailableHistory() {
    Fixture f;
    f.read = "prefill";
    f.restart();
    f.t3.beforeRead = [] { throw std::runtime_error("Fixture T3 history unavailable"); };
    const auto task = f.submit("Verify fresh evidence");
    f.model.script = [&](const ModelInput& input) {
        if (input.event["kind"] == "intake") return work();
        check(input.event.at("historyUnavailable") == true && input.evidence.empty(),"missing history was not visible");
        return complete();
    };
    f.reach(task,"running");
    f.t3.finish("run-1");
    f.reach(task,"completed");
    check(f.model.calls == 2 && f.logs(task,"completed") == 1,"settled report was lost after prefill failed");
}

void prefillPersistenceFailureDoesNotProceed() {
    Fixture f;
    f.read = "prefill";
    f.allowBeta();
    f.restart();
    const auto source = f.submit("Verify with a Session and consult");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == source) {
            if (input.event["kind"] == "intake") return work();
            if (input.event["kind"] == "result") return botRequest("beta","consult","Confirm policy");
            check(input.event["kind"] == "bot_result","unexpected source event before persistence failure");
            return complete();
        }
        check(input.event["kind"] == "bot_request","unexpected recipient event before persistence failure");
        return botReply("result","Policy confirmed");
    };
    f.reach(source,"running");
    f.t3.finish("run-1");
    for (int step = 0; step < 12; ++step) {
        f.runtime->tick();
        if (f.t3.reads == 1 && f.task(source)["state"] == "peer_waiting") break;
    }
    check(f.t3.reads == 1 && f.task(source)["state"] == "peer_waiting",
          "test did not finish the initial prefill before consulting");
    for (int step = 0; step < 12; ++step) {
        f.runtime->tick();
        const auto messages = f.botMessages("alpha");
        if (messages.size() == 2 && messages.back()["messageType"] == "result" &&
            f.task(source)["state"] == "deciding") break;
    }
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 2 && messages.back()["messageType"] == "result" &&
          f.t3.reads == 1 && f.logs(source,"report") == 1,
          "test did not reach a settled report with an incoming peer result");
    setSessionReadJournalFailure(f.directory/"state.db",true);
    f.runtime->tick();
    check(f.t3.reads == 2 && f.model.calls == 3 && f.logs(source,"session_read") == 1 &&
          f.logs(source,"model_failure") == 1 && f.logs(source,"completed") == 0 &&
          f.task(source)["state"] == "deciding",
          "the model proceeded with a prefill excerpt that SQLite did not durably save");
    setSessionReadJournalFailure(f.directory/"state.db",false);
    f.runtime->tick();
    check(f.t3.reads == 3 && f.model.calls == 3 && f.logs(source,"session_read") == 2,
          "prefill did not reread and save the excerpt before retrying the model");
    f.reach(source,"completed");
    check(f.t3.reads == 3 && f.model.calls == 4,"the saved prefill excerpt did not resume the peer result");
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
    check(f.t3.interrupts == 1 && f.t3.interruptedRuns.back().threadId == "thread-1" &&
          f.t3.interruptedRuns.back().runId == "run-1" && f.task(unrelated)["state"] == "queued",
          "cancel interrupted the wrong Run or released its slot before settlement");
    f.restart();
    f.t3.finish("run-1");
    f.reach(recipient,"cancelled");
    f.reach(root,"cancelled");
    f.wait(unrelated,"user_question");
    const auto messages = f.botMessages("alpha");
    check(lateDecisions == 0 && f.t3.launches == 1 && f.logs(root,"completed") == 0 &&
          messages.size() == 2 && messages.back().at("status") == "cancelled",
          "a late worker result reopened or replaced the cancelled exchange");
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
    const auto messagesBeforeCancel = f.botMessages("beta");
    check(f.task(beta)["doneWhen"] == "" && f.store->tasks("alpha").size() == 1 &&
          messagesBeforeCancel.size() == 1 && messagesBeforeCancel.front().at("fromBotId") == "alpha",
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
    bool cancelledDuringPreflight = false;
    f.t3.onPreflight = [&] {
        Store cli(f.directory/"state.db");
        cli.operate(source,0,"cancel");
        cancelledDuringPreflight = true;
    };
    for (int step = 0; step < 8 && !cancelledDuringPreflight; ++step) f.runtime->tick();
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
            {"invalid decisions",invalidDecisions},{"correct invalid task arguments",correctInvalidTaskArguments},
            {"failed preflight",failedPreflightIsUnsent},
            {"replay saved question decision",replayQuestionDecision},{"clarification and failure",ambiguityAndFailure},
            {"deadline, approval and cancel",deadlineApprovalAndCancel},{"additional work budget",budgetAndStaleWaits},
            {"branch questions",sessionQuestions},{"stale waits",waitsExpireWithReality},
            {"delivery recovery",recoveredDelivery},{"cancel during delivery",cancelDuringDelivery},
            {"Run settlement with unknown answer delivery",settleWhileAnswerDeliveryIsUnknown},
            {"history reading restart",evidenceResume},{"project owners on task pages",taskPagesShowProjectOwners},
            {"Bot handoff and restart",peerWorkAndRestart},{"peer work budget shared with direct T3",peerWorkSharesBudgetWithDirectT3Work},
            {"Bot clarification after a settled Run",peerClarificationAfterRun},
            {"Task revision during Bot clarification",[] { revisionWaitsForPeerClarification(); }},
            {"Direct answer during Bot clarification revision",[] { revisionWaitsForPeerClarification(true); }},
            {"peer completion after environment move",peerCompletionAfterEnvironmentMove},
            {"recipient revision continues its work exchange",recipientRevisionContinuesTheOpenWorkExchange},
            {"recipient revision during peer question preserves answer",recipientRevisionDuringPeerQuestionPreservesTheAnswer},
            {"recipient move during peer question starts a fresh Session",recipientMoveDuringPeerQuestionStartsAFreshSession},
            {"work recipient move shares its sender's queue",workRecipientMoveSharesSenderQueue},
            {"work recipient move waits for an unrelated owner",workRecipientMoveWaitsForUnrelatedOwner},
            {"consultation replies after user instructions",consultationRepliesAfterUserInstructions},
            {"Bot consultation and permission",peerConsultationAndPermission},
            {"Requested criteria for first peer work",requestedCriteriaApplyToFirstPeerWork},
            {"Model budget across a conversation turn and Bot consultation",modelBudgetFollowsBotConsultation},
            {"Peer result waits for the recipient's conversation turn",peerResultWaitsForRecipientTurn},
            {"Model budget counts failed attempts",modelBudgetCountsFailedAttempts},
            {"Model budget permits T3 observation and reconciliation",modelBudgetAllowsT3ObservationAndRecovery},
            {"Unsuccessful consultations",unsuccessfulConsultationsDoNotComplete},
            {"Concurrent Bot handoff",concurrentPeerHandoff},{"Consultation own-Session evidence",consultationRestoresOwnEvidence},
            {"Fresh result prefill failure",freshResultSurvivesUnavailableHistory},
            {"Prefill persistence failure",prefillPersistenceFailureDoesNotProceed},
            {"Bot handoff cancellation",peerCancelWithLateSessionResult},{"Bot project cycle",peerProjectCycle},
            {"Queued Bot handoff cancellation",peerCancelWhileQueued},
            {"Bot handoff cancellation during T3 preflight",peerCancelDuringPreflight},
            {"stale in-flight model decision",inferenceInvalidatedByCancel}}) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
    }
}
