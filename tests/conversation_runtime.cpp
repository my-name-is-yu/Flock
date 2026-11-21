#include "flock/core.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <unistd.h>
#include <utility>

using namespace flockv0;

namespace {
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

struct Script : Model {
    std::function<void(const ModelInput&)> beforeComplete;
    Decision decide(const ModelInput& input) override {
        if (input.event["kind"] == "intake") {
            Json args = {{"instruction","Measure and report the fixture"}};
            if (input.task["doneWhen"] == "") args["doneWhen"] = "Report baseline latency";
            return {"delegate",args,"Collect the fixture evidence"};
        }
        check(input.event["kind"] == "result","unexpected model event");
        if (beforeComplete) beforeComplete(input);
        return {"complete",{{"report",input.event["report"]}},"The fixture report includes the requested evidence"};
    }
};

struct SimulatedT3 : T3 {
    std::vector<Command> sent, reconciled;
    std::vector<std::pair<std::string,std::string>> interrupted;
    std::map<std::string,RunRef> deliveries;
    std::map<std::string,Observation> observations;
    bool loseNextSend = false, loseNextStop = false, deliveryVisible = true;
    std::string login = "original-login";

    std::string authGeneration(const std::string&,const Json&) override { return login; }
    Delivery send(const Command& command) override {
        sent.push_back(command);
        check(!command.actionId.empty(),"T3 received work without a request ID");
        check(command.authGeneration == login,"a dispatched instruction lacks its authentication generation");
        const auto suffix = std::to_string(sent.size());
        const RunRef run{command.tool == "delegate" ? "session-" + suffix : command.target.threadId,
                         "run-" + suffix,"https://example.test/session-" + suffix};
        check(deliveries.emplace(command.actionId,run).second,"the same work was sent twice");
        observations.emplace(run.runId,Observation{});
        if (std::exchange(loseNextSend,false)) return {};
        return {Delivery::Kind::Accepted,run,{}};
    }
    Delivery reconcile(const Command& command) override {
        reconciled.push_back(command);
        if (!deliveryVisible || !deliveries.contains(command.actionId)) return {};
        return {Delivery::Kind::Accepted,deliveries.at(command.actionId),{}};
    }
    Observation observe(const std::string&,const Json&,const RunRef& run) override {
        return observations.at(run.runId);
    }
    void interrupt(const std::string& environment,const Json&,const RunRef& run) override {
        interrupted.emplace_back(environment,run.runId);
        if (std::exchange(loseNextStop,false)) throw std::runtime_error("simulated interrupted acknowledgement");
    }
    void finish(std::string_view run, std::string text, bool settled = true) {
        auto& report = observations.at(std::string(run));
        report.kind = Observation::Kind::Finished;
        report.status = "completed";
        report.report = std::move(text);
        report.resultRunId = run;
        report.itemId = "report-" + std::string(run);
        report.settled = settled;
    }
};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    Script model;
    SimulatedT3 t3;
    Seconds now = unixTime();

    Fixture() {
        char path[] = "/tmp/flock-conversation-runtime-XXXXXX";
        const auto* created = mkdtemp(path);
        if (!created) throw std::runtime_error("cannot create a test directory");
        dir = created;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(dir);
    }
    void restart() {
        runtime.reset();
        store = std::make_unique<Store>(dir/"state.db");
        runtime = std::make_unique<Runtime>(*store,model,t3,[](auto) { return BotNotes{}; },
                                             RuntimeOptions{[&] { return now; }});
    }
    Intake routing(std::string request = "計測", std::string bot = "alpha", std::string environment = "desktop") {
        return {std::move(bot),std::move(request),std::move(environment),"project",
            {{"conversationEnvironments",{{"クラウドMac","cloud"},{"ローカルMac","desktop"}}}}};
    }
    Json receive(std::string_view key,std::string_view text,Id replyTo = 0) {
        Store httpWriter(dir/"state.db");
        return httpWriter.receive(routing(),key,text,replyTo);
    }
    Json task(Id id) { return store->task(id); }
    void tick(int times = 1) {
        for (int i = 0; i < times; ++i) runtime->tick();
    }
    void advanceUntil(const std::function<bool()>& reached, const std::string& why) {
        for (int i = 0; i < 16; ++i) {
            if (reached()) return;
            tick();
        }
        if (!reached()) throw std::runtime_error(why);
    }
    void reach(Id id,std::string_view state) {
        advanceUntil([&] { return task(id)["state"] == Json(state); },
                     "state " + std::string(state) + " was not reached for task " + std::to_string(id));
    }
    Json conversationMessage(Id id) {
        const auto timeline = store->conversation("alpha");
        const auto found = std::find_if(timeline.begin(),timeline.end(),[&](const Json& row) { return row["id"] == id; });
        return found == timeline.end() ? Json(nullptr) : (*found)["data"];
    }
    Json event(Id id,std::string_view kind) {
        const auto history = store->history(id,0,200);
        const auto found = std::find_if(history.rbegin(),history.rend(),[&](const Json& row) { return row["kind"] == Json(kind); });
        return found == history.rend() ? Json(nullptr) : (*found)["data"];
    }
};

void rawInboxSurvivesRestartAndBlocksCompletion() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.t3.finish("run-1","Old report");
    f.reach(id,"deciding");
    f.model.beforeComplete = [&](const ModelInput&) {
        // Store::receive also classifies messages. Seed the persisted state left by a crash between the two commits.
        sqlite3* writer = nullptr;
        check(sqlite3_open((f.dir/"state.db").c_str(),&writer) == SQLITE_OK,"cannot open the raw inbox fixture");
        const auto saved = sqlite3_exec(writer,R"SQL(
BEGIN IMMEDIATE;
INSERT INTO journal(time,kind,data,bot_id) VALUES(0,'conversation_message','{}','alpha');
INSERT INTO messages(id,bot_id,client_message_id,sequence,text,routing)
VALUES(last_insert_rowid(),'alpha','raw',1,'完了条件は CPU 時間を計測',
       '{"environmentId":"desktop","projectId":"project","config":{}}');
COMMIT;
)SQL",nullptr,nullptr,nullptr);
        sqlite3_close(writer);
        check(saved == SQLITE_OK,"cannot save the unclassified message");
    };
    f.tick();
    check(f.event(id,"completed").is_null() && f.t3.sent.size() == 1,
          "the unclassified message allowed stale completion or new work");
    const auto receipt = f.store->receipt("alpha","raw","完了条件は CPU 時間を計測");
    check(receipt && f.conversationMessage((*receipt)["messageId"])["status"] == "received",
          "the saved message was not visible before routing");
    f.model.beforeComplete = {};
    f.restart();
    f.reach(id,"running");
    check(f.task(id)["doneWhen"] == "CPU 時間を計測" && f.t3.sent.size() == 2 &&
          f.t3.sent.back().instruction.find("CPU 時間を計測") != std::string::npos &&
          f.conversationMessage((*receipt)["messageId"])["deliveredToT3"] == true,
          "restart did not route and deliver the received message");
}

void linkedResultStartsANewSession() {
    Fixture f;
    const auto original = f.store->submit(f.routing("計測方法の提案"));
    f.reach(original,"running");
    f.t3.finish("run-1","1. Measure by hand\n2. Capture a fixture trace\n3. Measure after release");
    f.reach(original,"completed");
    const auto earlier = f.event(original,"completed");
    f.receive("implement","2案目で実装して");
    const auto task = f.store->tasks("alpha").front();
    const auto id = task["id"].get<Id>();
    check(id != original && task["sourceTaskId"] == original &&
          task["sourceResultRefs"].size() == 1 &&
          task["sourceResultRefs"][0]["text"] == "2. Capture a fixture trace",
          "implementation was not linked to the selected earlier result");
    f.reach(id,"running");
    check(f.t3.sent.size() == 2 && f.t3.sent.back().tool == "delegate" &&
          f.task(original)["runs"][0]["threadId"] != f.task(id)["runs"][0]["threadId"] &&
          f.event(original,"completed") == earlier,"linked implementation changed prior work or reused its Thread");
}

void appendWaitsForSafeBoundary() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    const auto next = f.store->submit(f.routing("別件","beta"));
    const auto message = f.receive("format","補足として表をCSV形式で出して");
    check(f.receive("format","補足として表をCSV形式で出して") == message,"retry changed a durable receipt");
    f.tick(3);
    check(f.task(id)["remaining"] == 3 && f.task(next)["state"] == "queued" &&
          f.t3.sent.size() == 1 && f.t3.interrupted.empty() &&
          f.conversationMessage(message["messageId"])["deliveredToT3"] == false,"non-conflicting instruction ran before the safe boundary");
    f.restart();
    f.t3.finish("run-1","Baseline evidence");
    f.reach(id,"deciding");
    f.reach(id,"running");
    const auto work = f.task(id);
    check(work["remaining"] == 2 && work["additionalUsed"] == 1 &&
          work["runs"].size() == 2 && work["runs"][0]["threadId"] == work["runs"][1]["threadId"],
          "same-Session continuation changed Task budget or provenance");
    check(f.t3.sent.back().instruction.find("CSV形式で") != std::string::npos &&
          f.conversationMessage(message["messageId"])["deliveredToT3"] == true &&
          f.task(next)["state"] == "queued","continuation lost the user instruction or the project gate");
    f.t3.finish("run-2","Latest CSV evidence");
    f.reach(id,"completed");
    check(f.event(id,"completed")["text"] == "Latest CSV evidence","an old report was published as completion");
    f.reach(next,"running");
    check(f.t3.sent.size() == 3,"the queued task did not inherit the released project slot");
}

void unknownSendThenScopeChange() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"sending");
    f.t3.loseNextSend = true;
    f.reach(id,"waiting");
    const auto before = f.task(id);
    const auto change = f.receive("revision","完了条件は p95 レイテンシを報告");
    check(f.task(id)["revisions"].size() == 2 && f.task(id)["doneWhen"] == "p95 レイテンシを報告",
          "explicit criteria did not create a revision");
    f.restart();
    f.t3.login = "different-login";
    f.t3.deliveryVisible = false;
    f.tick(3);
    check(f.t3.sent.size() == 1 && !f.t3.reconciled.empty() && f.t3.interrupted.empty(),
          "an unconfirmed launch was resent, skipped reconciliation, or stopped before identifying its Run");
    const auto firstAction = f.t3.sent.front().actionId;
    check(std::all_of(f.t3.reconciled.begin(),f.t3.reconciled.end(),[&](const Command& command) {
        return command.actionId == firstAction && command.authGeneration == "original-login";
    }),"recovery used a different action or login generation");
    f.restart();
    f.now += 3;
    f.t3.deliveryVisible = true;
    f.t3.loseNextStop = true;
    f.advanceUntil([&] { return !f.t3.interrupted.empty(); },"the recovered Run was not interrupted");
    check(f.t3.interrupted == std::vector{std::pair<std::string,std::string>{"desktop","run-1"}} &&
          f.t3.sent.size() == 1,"the replacement started early or the wrong Run was interrupted");
    f.restart();
    f.t3.finish("run-1","Old response",false);
    f.now += 20;
    f.tick(3);
    check(f.t3.interrupted.size() == 1 && f.t3.sent.size() == 1 && f.event(id,"completed").is_null(),
          "the stop was retried or the revision ran before delegated work settled");
    f.t3.finish("run-1","Old response");
    f.reach(id,"sending");
    f.t3.loseNextSend = true;
    f.reach(id,"waiting");
    check(f.t3.sent.size() == 2,"the uncertain replacement did not reach T3 exactly once");
    const auto replacementAction = f.t3.sent.back().actionId;
    f.restart();
    f.store->operate(id,f.task(id)["wait"]["id"],"reconcile");
    f.reach(id,"running");
    check(f.t3.sent.size() == 2 && f.t3.reconciled.back().actionId == replacementAction &&
          f.t3.sent.back().instruction.find("p95 レイテンシ") != std::string::npos &&
          f.conversationMessage(change["messageId"])["deliveredToT3"] == true,
          "the replacement did not reconcile as one delivery for the new revision");
    const auto runs = f.task(id)["runs"];
    check(runs[0]["revisionId"] == before["currentRevision"] &&
          runs[1]["revisionId"] == f.task(id)["currentRevision"] &&
          f.event(id,"report")["revisionId"] == before["currentRevision"],
          "the prior report or Run was silently attributed to the new revision");
    f.t3.finish("run-2","New p95 evidence");
    f.reach(id,"completed");
    const auto result = f.event(id,"completed");
    check(result["revisionId"] == runs[1]["revisionId"] && result["source"]["runId"] == "run-2" &&
          result["text"] == "New p95 evidence","completion lacks the latest revision's evidence reference");
}

void completeRacingWithNewCriteria() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.t3.finish("run-1","Old report offered for completion");
    f.reach(id,"deciding");
    f.model.beforeComplete = [&](const ModelInput& input) {
        if (input.event["resultRunId"] == "run-1") f.receive("racing-revision","完了条件はメモリを計測");
    };
    f.tick();
    check(f.event(id,"completed").is_null() && f.task(id)["doneWhen"] == "メモリを計測",
          "stale model completion won the revision race");
    f.reach(id,"running");
    check(f.t3.sent.size() == 2 && f.t3.sent.back().instruction.find("メモリを計測") != std::string::npos,
          "the replacement Run did not receive the criteria accepted on this task");
    f.t3.finish("run-2","New memory evidence");
    f.reach(id,"completed");
    const auto result = f.event(id,"completed");
    check(result["source"]["runId"] == "run-2" && result["text"] == "New memory evidence" &&
          result["doneWhen"] == "メモリを計測","the final result relied on stale work or criteria");
}

void unresolvedStopHoldsOnlyItsCandidates() {
    Fixture f;
    const auto a = f.store->submit(f.routing("計測作業"));
    const auto b = f.store->submit(f.routing("リリース作業","alpha","cloud"));
    const auto other = f.store->submit(f.routing("Separate task","beta","third"));
    for (auto id : {a,b,other}) f.reach(id,"running");
    f.receive("ambiguous-stop","それ止めて");
    const auto clarifications = f.store->clarifications("alpha");
    check(clarifications.size() == 1 && clarifications[0]["candidates"].size() == 2 && f.t3.interrupted.empty(),
          "an ambiguous stop did not remain one confirmation for the two candidates");
    const auto& candidates = clarifications[0]["candidates"];
    for (auto id : {a,b}) {
        check(std::any_of(candidates.begin(),candidates.end(),[&](const Json& candidate) {
            return candidate["taskId"] == id;
        }),"the clarification omitted an eligible task");
    }
    for (auto id : {a,b,other}) f.t3.finish(f.task(id)["activeRun"]["runId"].get<std::string>(),"Evidence");
    f.reach(other,"completed");
    check(f.event(a,"completed").is_null() && f.event(b,"completed").is_null(),
          "a candidate published a completion while its stop request remained ambiguous");
    f.receive("resolved-stop","計測作業",clarifications[0]["messageId"]);
    f.reach(a,"cancelled");
    f.reach(b,"completed");
    check(f.store->clarifications("alpha").empty() && f.t3.interrupted.empty() && f.t3.sent.size() == 3,
          "resolving the target did not release exactly the intended holds");
}

void cloudDestinationWaitsForItsProject() {
    Fixture f;
    const auto occupied = f.store->submit(f.routing("Cloud work","beta","cloud"));
    const auto moving = f.store->submit(f.routing());
    f.receive("format-before-launch","補足として表をCSV形式で出して");
    f.reach(occupied,"running");
    f.reach(moving,"running");
    f.receive("where","計測はクラウドMacで");
    const auto following = f.store->submit(f.routing("Cloud follower","beta","cloud"));
    f.tick(3);
    check(f.t3.interrupted.size() == 1 && f.t3.interrupted[0] == std::pair<std::string,std::string>{"desktop","run-2"} &&
          f.task(moving)["revisions"].size() == 1,
          "a confirmed environment change interrupted the wrong Run or revised the criteria");
    f.t3.finish("run-2","Old location stopped");
    f.tick(4);
    check(f.task(following)["state"] == "queued" && f.t3.sent.size() == 2,
          "work entered a Cloud project where another Task was still active");
    f.t3.finish("run-1","Cloud project complete");
    f.reach(occupied,"completed");
    f.reach(moving,"running");
    const auto task = f.task(moving);
    check(f.t3.sent.size() == 3 && f.t3.sent.back().tool == "delegate" && f.t3.sent.back().environmentId == "cloud" &&
          f.t3.sent.back().instruction.find("CSV形式で") != std::string::npos &&
          task["remaining"] == 2 && task["runs"][0]["environmentId"] == "desktop" &&
          task["runs"][1]["environmentId"] == "cloud" && task["runs"][0]["threadId"] != task["runs"][1]["threadId"] &&
          f.task(following)["state"] == "queued","the Task switched Session or budget incorrectly");
}

void latestTargetPrecedesABatchedBacklog() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.receive("old-location","計測はクラウドMacで");
    std::vector<std::string> notes;
    for (int i = 0; i < 5; ++i) {
        notes.push_back("補足として表をCSV形式で出して " + std::string(14500,'a') + " fixture note " + std::to_string(i));
        f.receive("notes-" + std::to_string(i),notes.back());
    }
    f.receive("new-location","計測はローカルMacで");
    f.t3.finish("run-1","Settled before the next destination");
    f.advanceUntil([&] { return f.t3.sent.size() > 1; },"the backlog did not reach T3");
    check(f.t3.sent[1].instruction.find("ローカルMacで") != std::string::npos,
          "the first continuation did not take the latest destination into account");
    for (std::size_t run = 2; run <= f.t3.sent.size(); ++run) {
        check(run <= 4,"the backlog exceeded the additional work allowance");
        f.t3.finish("run-" + std::to_string(run),"Continuation settled");
        f.advanceUntil([&] { return f.task(id)["state"] == "completed" || f.t3.sent.size() > run; },
                       "the accepted backlog did not finish");
    }
    std::string sentInstructions;
    for (auto command = std::next(f.t3.sent.begin()); command != f.t3.sent.end(); ++command) {
        check(command->environmentId == "desktop" && command->instruction.find("クラウドMacで") == std::string::npos,
              "a continuation was sent with a superseded destination");
        sentInstructions += command->instruction;
        sentInstructions += '\n';
    }
    for (const auto& note : notes) {
        const auto first = sentInstructions.find(note);
        check(first != std::string::npos && sentInstructions.find(note,first + note.size()) == std::string::npos,
              "an accepted backlog instruction was lost, truncated, or sent twice");
    }
}

void continuationUsesTheCurrentBudget() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    for (int run = 1; run <= 4; ++run) {
        f.receive("instruction-" + std::to_string(run),"補足として表をCSV形式で出して for fixture " + std::to_string(run));
        f.t3.finish("run-" + std::to_string(run),"Prior evidence");
        f.reach(id,"deciding");
        f.reach(id,run < 4 ? "running" : "waiting");
    }
    check(f.task(id)["wait"]["reason"] == "budget" && f.task(id)["remaining"] == 0 &&
          f.t3.sent.size() == 4 && f.store->tasks("alpha").size() == 1,
          "the exhausted budget still permitted a new Run or Task");
    f.receive("budget","追加1回");
    f.reach(id,"running");
    check(f.t3.sent.size() == 5 && f.task(id)["remaining"] == 0 && f.task(id)["additionalUsed"] == 4 &&
          f.store->tasks("alpha").size() == 1,"explicit budget reply did not permit precisely one extra Run");
    check(f.t3.sent.back().instruction.find("for fixture 4") != std::string::npos,
          "the approved Run lost the instruction that was held for approval");
}

void revisionOfProvablyUnsentAction() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"sending");
    f.receive("ready-revision","完了条件はレポートを出す");
    f.reach(id,"running");
    const auto task = f.task(id);
    check(f.t3.sent.size() == 1 && task["remaining"] == 3 &&
          f.t3.sent.front().instruction.find("レポートを出す") != std::string::npos,
          "replacing unsent work delivered the wrong criteria or used an additional Run");
    check(task["runs"][0]["revisionId"] == task["currentRevision"] &&
          task["revisions"][0]["doneWhen"] == "Report baseline latency" &&
          task["revisions"][1]["doneWhen"] == "レポートを出す",
          "a revision overwrote the prior immutable completion criteria");
}

void queuedCriteriaAreUsedForFirstDelegation() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    const auto receipt = f.receive("criteria-before-launch","完了条件は p99 レイテンシを報告");
    f.reach(id,"running");
    const auto task = f.task(id);
    check(task["revisions"].size() == 1 && task["revisions"][0]["sourceMessageId"] == receipt["messageId"] &&
          task["revisions"][0]["doneWhen"] == "p99 レイテンシを報告" &&
          task["runs"][0]["revisionId"] == task["currentRevision"] && f.t3.sent.size() == 1 &&
          f.t3.sent.front().instruction.find("p99 レイテンシを報告") != std::string::npos,
          "the initial Run did not receive the user's completion criteria");
}
}

int main() {
    try {
        appendWaitsForSafeBoundary();
        unknownSendThenScopeChange();
        completeRacingWithNewCriteria();
        unresolvedStopHoldsOnlyItsCandidates();
        rawInboxSurvivesRestartAndBlocksCompletion();
        linkedResultStartsANewSession();
        cloudDestinationWaitsForItsProject();
        latestTargetPrecedesABatchedBacklog();
        continuationUsesTheCurrentBudget();
        revisionOfProvablyUnsentAction();
        queuedCriteriaAreUsedForFirstDelegation();
        std::cout << "Conversation Runtime acceptance passed with simulated T3\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
