#include "flock/core.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
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

using Step = std::function<Decision(const ModelInput&)>;

Json listed(const ModelInput& input, Id id) {
    for (const auto& task : input.task.at("tasks"))
        if (task.at("taskId") == id) return task;
    throw std::runtime_error("the turn input omitted task " + std::to_string(id));
}

Step start(Json args = Json::object()) {
    return [args](const ModelInput& input) {
        auto request = args;
        if (!request.contains("request")) request["request"] = input.event.at("text");
        return Decision{"start_task",request,"Scripted request"};
    };
}
Step send(Id id, Json extra = Json::object()) {
    return [id,extra](const ModelInput& input) {
        Json args = {{"taskId",id},{"generation",listed(input,id).at("generation")},{"instruction",input.event.at("text")}};
        args.update(extra);
        return Decision{"send_to_task",args,"Scripted instruction"};
    };
}
Step change(Id id, Json fields) {
    return [id,fields](const ModelInput& input) {
        const auto task = listed(input,id);
        Json args = {{"taskId",id},{"generation",task.at("generation")},{"revisionId",task.at("revisionId")},
                     {"instruction",input.event.at("text")}};
        args.update(fields);
        return Decision{"change_task",args,"Scripted change"};
    };
}
Step stop(Id id) {
    return [id](const ModelInput& input) {
        return Decision{"cancel_task",{{"taskId",id},{"generation",listed(input,id).at("generation")}},"Scripted stop"};
    };
}
Step answer(Id id) {
    return [id](const ModelInput& input) {
        return Decision{"answer_task_question",{{"taskId",id},{"waitId",listed(input,id).at("wait").at("waitId")},
                                                {"text",input.event.at("text")}},"Scripted answer"};
    };
}
Step note(Id id) {
    return [id](const ModelInput& input) {
        return Decision{"message_session",{{"taskId",id},{"generation",listed(input,id).at("generation")},
                                           {"text",input.event.at("text")}},"Scripted note"};
    };
}
Step interrupt(Id id) {
    return [id](const ModelInput& input) {
        return Decision{"interrupt_session",{{"taskId",id},{"generation",listed(input,id).at("generation")}},"Scripted interrupt"};
    };
}
Step say(std::string text) {
    return [text](const ModelInput&) { return Decision{"reply",{{"text",text}},"Scripted reply"}; };
}

struct Script : Model {
    std::map<std::string,std::vector<Step>> turns;
    std::function<void(const ModelInput&)> beforeTurn;
    std::function<void(const ModelInput&)> beforeIntake;
    std::function<void(const ModelInput&)> beforeComplete;
    std::map<Id,Json> questions;
    std::function<Decision(const ModelInput&)> onResult;
    std::vector<ModelInput> turnInputs;
    Decision decide(const ModelInput& input) override {
        if (input.event.value("kind","") == "conversation_turn") {
            turnInputs.push_back(input);
            if (beforeTurn) beforeTurn(input);
            const auto text = input.event.at("text").get<std::string>();
            const auto script = turns.find(text);
            if (script == turns.end()) throw std::runtime_error("Missing turn fixture for: " + text);
            const auto step = input.event.at("turn").size();
            if (step < script->second.size()) return script->second[step](input);
            return {"reply",{{"text","Done."}},"Scripted reply"};
        }
        if (input.event["kind"] == "intake") {
            if (beforeIntake) beforeIntake(input);
            const auto question = questions.find(input.task["id"].get<Id>());
            if (question != questions.end() && !input.event.contains("userAnswer"))
                return {"ask_user",question->second,"The fixture needs a user answer"};
            Json args = {{"instruction","Measure and report the fixture"}};
            if (input.task["doneWhen"] == "") args["doneWhen"] = "Report baseline latency";
            return {"delegate",args,"Collect the fixture evidence"};
        }
        check(input.event["kind"] == "result","unexpected model event");
        if (beforeComplete) beforeComplete(input);
        if (onResult) return onResult(input);
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

    std::vector<Command> steered;
    std::string authGeneration(const std::string&,const Json&) override { return login; }
    Delivery send(const Command& command) override {
        if (command.tool == "steer") {
            steered.push_back(command);
            check(command.authGeneration == login,"a note lacks its authentication generation");
            const auto active = observations.find(command.target.runId);
            if (active == observations.end() || active->second.kind != Observation::Kind::Running)
                return {Delivery::Kind::NotAccepted,{},"The assigned Run is no longer running",true};
            return {Delivery::Kind::Accepted,command.target,{}};
        }
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

Id askAgain(const std::filesystem::path& path, Id task, const std::string& question) {
    sqlite3* raw = nullptr;
    check(sqlite3_open(path.c_str(),&raw) == SQLITE_OK,"could not open the question fixture");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    const auto run = [&](const std::string& sql) {
        check(sqlite3_exec(db.get(),sql.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,"the question fixture failed");
    };
    const auto id = std::to_string(task);
    const auto text = Json(question).dump();
    run("INSERT INTO waits(task_id,data) VALUES(" + id + ",json_object('reason','user_question','text'," + text + "))");
    const auto wait = std::to_string(sqlite3_last_insert_rowid(db.get()));
    run("INSERT INTO journal(task_id,time,kind,data) VALUES(" + id + ",0,'waiting',json_object('id'," + wait +
        ",'reason','user_question','text'," + text + "))");
    const auto anchor = std::to_string(sqlite3_last_insert_rowid(db.get()));
    run("UPDATE tasks SET state='waiting',generation=generation+1,data=json_set(data,'$.wait',json_object('id'," + wait +
        ",'reason','user_question','text'," + text + ",'messageId'," + anchor +
        ",'eventId',json_extract(data,'$.eventId'),'run',json_extract(data,'$.activeRun'))) WHERE id=" + id);
    return std::stoll(wait);
}

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    Script model;
    SimulatedT3 t3;
    std::function<void()> beforeNotes;
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
        runtime = std::make_unique<Runtime>(*store,model,t3,[this](auto) {
            if (beforeNotes) beforeNotes();
            return BotNotes{};
        },RuntimeOptions{[this] { return now; }});
    }
    Intake routing(std::string request = "計測", std::string bot = "alpha", std::string environment = "desktop",
                   std::string project = "project") {
        return {std::move(bot),std::move(request),std::move(environment),std::move(project),
            {{"conversationEnvironments",{{"クラウドMac","cloud"},{"ローカルMac","desktop"}}}}, {},0};
    }
    Json enqueue(std::string_view key,std::string_view text,Id replyTo = 0) {
        Store httpWriter(dir/"state.db");
        return httpWriter.receive(routing(),key,text,replyTo);
    }
    void script(std::string_view text, std::vector<Step> steps) {
        model.turns.insert_or_assign(std::string(text),std::move(steps));
    }
    Json receive(std::string_view key,std::string_view text,Id replyTo = 0) {
        const auto receipt = enqueue(key,text,replyTo);
        runtime->runConversations();
        return receipt;
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
    Json response(const Json& receipt) {
        for (const auto& row : store->conversation("alpha"))
            if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == receipt["messageId"]) return row["data"];
        return nullptr;
    }
    Json event(Id id,std::string_view kind) {
        const auto history = store->history(id,0,200);
        const auto found = std::find_if(history.rbegin(),history.rend(),[&](const Json& row) { return row["kind"] == Json(kind); });
        return found == history.rend() ? Json(nullptr) : (*found)["data"];
    }
    Json lastTurn() { return model.turnInputs.back().event.at("turn"); }
};

void durableInboxAndTargetedRevision() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    const auto other = f.store->submit(f.routing("Unrelated work","alpha","cloud"));
    f.reach(id,"running");
    f.reach(other,"running");
    f.t3.finish(f.task(id)["activeRun"]["runId"].get<std::string>(),"Old report");
    f.t3.finish(f.task(other)["activeRun"]["runId"].get<std::string>(),"Unrelated report");
    f.reach(id,"deciding");
    constexpr auto criteria = "完了条件は CPU 時間を計測";
    f.script(criteria,{change(id,{{"doneWhen","CPU 時間を計測"}})});
    f.model.beforeComplete = [&](const ModelInput& input) {
        if (input.task["id"] != id) return;
        f.enqueue("raw",criteria);
        f.runtime->runConversations();
    };
    f.tick();
    check(f.event(id,"completed").is_null() && f.t3.sent.size() == 2 &&
          f.task(other)["state"] == "completed","a saved revision accepted a stale completion or held another task");
    const auto receipt = f.store->receipt("alpha","raw",criteria);
    check(receipt && f.conversationMessage((*receipt)["messageId"])["status"] == "stopping",
          "the accepted revision was not recorded");
    f.model.beforeComplete = {};
    f.restart();
    f.reach(id,"running");
    check(f.task(id)["doneWhen"] == "CPU 時間を計測" && f.t3.sent.size() == 3 &&
          f.t3.sent.back().instruction.find("CPU 時間を計測") != std::string::npos &&
          f.conversationMessage((*receipt)["messageId"])["deliveredToT3"] == true,
          "restart did not deliver the accepted revision");
}

void linkedResultStartsANewSession() {
    Fixture f;
    const auto original = f.store->submit(f.routing("計測方法の提案"));
    f.reach(original,"running");
    const std::string report = "1. Measure by hand\n2. Capture a fixture trace\n3. Measure after release";
    f.t3.finish("run-1",report);
    f.reach(original,"completed");
    const auto earlier = f.event(original,"completed");
    const auto resultId = f.store->latestTaskResult(original).at("id");
    f.script("2案目を実装して",{start({{"request","Implement option 2: capture a fixture trace"},
                                       {"sourceResultMessageId",resultId}})});
    f.receive("implement","2案目を実装して");
    const auto task = f.store->tasks("alpha").front();
    const auto id = task["id"].get<Id>();
    check(id != original && task["sourceTaskId"] == original && task["sourceResultRefs"].size() == 1 &&
          task["sourceResultRefs"][0]["messageId"] == resultId && task["sourceResultRefs"][0]["text"] == report,
          "implementation was not linked to the earlier result");
    f.reach(id,"running");
    check(f.t3.sent.size() == 2 && f.t3.sent.back().tool == "delegate" &&
          f.task(original)["runs"][0]["threadId"] != f.task(id)["runs"][0]["threadId"] &&
          f.event(original,"completed") == earlier,"linked implementation changed prior work or reused its Thread");
    const auto& instruction = f.t3.sent.back().instruction;
    check(instruction.find("Implement option 2") != std::string::npos &&
          instruction.find("(reference only):\n" + report) != std::string::npos,
          "the new Session did not receive the referenced result");
}

void independentRequestQueuesSeparately() {
    Fixture f;
    const auto running = f.store->submit(f.routing());
    f.reach(running,"running");
    constexpr auto request = "依存パッケージのライセンス一覧も必要です";
    f.script(request,{start()});
    f.receive("new-request",request);
    const auto tasks = f.store->tasks("alpha");
    check(tasks.size() == 2 && tasks.front()["request"] == request &&
          tasks.front()["state"] == "queued" && f.task(running)["pendingInstructions"].is_null() &&
          f.t3.sent.size() == 1 && f.t3.interrupted.empty(),"a new request altered the running task");
}

void followUpCarriesTheReferencedReport() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    const std::string text = "1. Measure by hand\n2. Capture a fixture trace\n3. Measure after release";
    f.t3.finish("run-1",text);
    f.reach(id,"deciding");
    const Id report = f.store->latestTaskResult(id)["id"];
    f.model.onResult = [](const ModelInput&) {
        return Decision{"delegate",{{"instruction","Check the baseline separately"}},"Open a separate Session"};
    };
    f.reach(id,"running");
    check(f.task(id)["activeRun"]["threadId"] == "session-2","the replacement Session did not start");
    f.script("案2を実装",{send(id,{{"sourceResultMessageId",report}})});
    const auto message = f.receive("selected-option","案2を実装",report);
    const auto pending = f.task(id)["pendingInstructions"][0];
    check(pending["source"]["messageId"] == report && pending["source"]["text"] == text &&
          f.task(id)["sourceResultRefs"] == Json::array({pending["source"]}),
          "the active task did not save the referenced result with the pending instruction");
    f.restart();
    f.t3.finish("run-2","Separate baseline checked");
    f.reach(id,"deciding");
    f.reach(id,"running");
    check(f.t3.sent.size() == 3 && f.t3.sent.back().tool == "follow_up" &&
          f.t3.sent.back().target.threadId == "session-2" &&
          f.t3.sent.back().instruction.find("案2を実装") != std::string::npos &&
          f.t3.sent.back().instruction.find("Capture a fixture trace") != std::string::npos &&
          f.conversationMessage(message["messageId"])["deliveredToT3"] == true,
          "the other Session did not receive the referenced result");
}

void questionAnswerReachesTheBotBeforeContinuingWork() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    bool answered = false;
    f.model.onResult = [&](const ModelInput& input) {
        if (!input.event.contains("userAnswer"))
            return Decision{"ask_user",{{"question","Which account should be used?"}},"The account is missing"};
        check(input.event["userQuestion"] == "Which account should be used?" &&
              input.event["userAnswer"] == "Use account Quartz-717","the Bot received the wrong question or answer");
        answered = true;
        return Decision{"follow_up",{{"instruction",input.event["userAnswer"]}},"Use the confirmed account"};
    };
    f.t3.finish("run-1","Baseline evidence");
    f.reach(id,"waiting");
    f.script("Include the table as CSV",{send(id)});
    const auto added = f.receive("format","Include the table as CSV");
    f.script("Use account Quartz-717",{answer(id)});
    f.receive("account","Use account Quartz-717",f.task(id)["wait"]["messageId"].get<Id>());
    check(f.event(id,"user_answer")["text"] == "Use account Quartz-717","the answer did not reach the question");
    f.restart();
    f.reach(id,"running");
    check(answered && f.t3.sent.size() == 2 && f.t3.sent.back().target.threadId == "session-1" &&
          f.t3.sent.back().instruction.find("Quartz-717") != std::string::npos &&
          f.t3.sent.back().instruction.find("as CSV") != std::string::npos &&
          f.conversationMessage(added["messageId"])["deliveredToT3"] == true,
          "continuation lost the Bot's requested answer or the additional instruction");
}

void answersStayOnTheirQuestion() {
    Fixture f;
    const auto naming = f.store->submit(f.routing("名前を決める"));
    const auto color = f.store->submit(f.routing("色を決める","alpha","cloud"));
    f.model.questions.emplace(naming,Json{{"question","何という名前にしますか？"}});
    f.model.questions.emplace(color,Json{{"question","どの色にしますか？"}});
    f.reach(naming,"waiting");
    f.reach(color,"waiting");
    const auto namingWait = f.task(naming)["wait"];
    const auto colorWait = f.task(color)["wait"];

    f.script("青",{answer(color)});
    f.receive("wrong-anchor","青",namingWait.at("messageId").get<Id>());
    check(f.lastTurn()[0]["result"]["ok"] == false && f.task(color)["wait"] == colorWait &&
          f.event(color,"user_answer").is_null(),"a reply to one question answered another");

    Id reopened = 0;
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (input.event["text"] != "Flock" || !input.event["turn"].empty()) return;
        {
            Store writer(f.dir/"state.db");
            writer.operate(naming,namingWait.at("id"),"answer",{{"text","別の名前"}});
        }
        reopened = askAgain(f.dir/"state.db",naming,"もう一度、名前を確認させてください");
    };
    f.script("Flock",{answer(naming),answer(naming)});
    f.receive("late-answer","Flock");
    f.model.beforeTurn = {};
    const auto turn = f.lastTurn();
    check(turn[0]["result"]["ok"] == false && turn[1]["result"]["ok"] == false &&
          turn[1]["result"]["error"].get<std::string>().find("opened after") != std::string::npos &&
          f.event(naming,"user_answer")["text"] == "別の名前" && f.task(naming)["wait"]["id"] == reopened,
          "an in-flight answer overwrote an answered question or answered one that opened later");
}

void answersNeedTheirOwnReplyTargetAndAStableTask() {
    Fixture f;
    const auto color = f.store->submit(f.routing("色を決める"));
    f.model.questions.emplace(color,Json{{"question","どの色にしますか？"},{"choices",{"青","赤"}}});
    f.reach(color,"waiting");
    const auto wait = f.task(color)["wait"];
    f.script("どちらだっけ",{say("青か赤です。")});
    f.receive("ask-back","どちらだっけ",wait.at("messageId").get<Id>());
    check(f.model.turnInputs.back().event["replyTo"]["choices"] == Json::array({"青","赤"}),
          "a reply to a question did not show the question's choices");
    f.script("hello",{say("Hi.")});
    const auto greeting = f.receive("hello","hello");
    f.script("yes",{answer(color)});
    f.receive("unrelated-yes","yes",f.response(greeting)["messageId"].get<Id>());
    check(f.lastTurn()[0]["result"]["ok"] == false && f.task(color)["wait"] == wait,
          "a reply to an unrelated message answered a task question");
    f.script("青。完了条件も色見本を付けることに",{change(color,{{"doneWhen","Attach a color sample"}}),answer(color)});
    f.receive("change-then-answer","青。完了条件も色見本を付けることに");
    check(f.lastTurn()[0]["result"]["ok"] == true && f.lastTurn()[1]["result"]["ok"] == false &&
          f.event(color,"user_answer").is_null(),"an answer that replanning would discard was reported as answered");
}

void pausedTurnHoldsCompletion() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.t3.finish("run-1","Timing evidence");
    f.reach(id,"deciding");
    auto limited = f.routing();
    limited.config["modelCallLimit"] = 1;
    f.script("Check what is running, then change the criteria",{
        [](const ModelInput&) { return Decision{"find_tasks",Json::object(),"Look first"}; },
        change(id,{{"doneWhen","Report p95 timing"}})});
    const auto receipt = f.store->receive(limited,"paused","Check what is running, then change the criteria");
    f.tick();
    check(f.store->pausedConversations("alpha").size() == 1 && f.event(id,"completed").is_null(),
          "a task completed while a turn that may change it was paused");
    f.store->approveMessageModelCall("alpha",f.store->pausedConversations("alpha").at(0).at("messageId"));
    f.tick();
    check(f.task(id)["doneWhen"] == "Report p95 timing" && f.event(id,"completed").is_null(),
          "the approved turn did not apply its change before completion");
    (void)receipt;
}

void cancelledReportCanBeReferencedWithoutRestarting() {
    Fixture f;
    const auto old = f.store->submit(f.routing("Benchmark the reference dataset"));
    f.script("Complete when you export raw latency samples",{change(old,{{"doneWhen","export raw latency samples"}})});
    f.receive("benchmark-criteria","Complete when you export raw latency samples");
    f.script("Use a pinned regression fixture",{send(old)});
    f.receive("benchmark-direction","Use a pinned regression fixture");
    f.reach(old,"running");
    constexpr auto report = "Partial benchmark found a spike in the first sample";
    f.t3.finish("run-1",report);
    f.reach(old,"deciding");
    const auto source = f.store->latestTaskResult(old).at("id");
    f.store->operate(old,0,"cancel");
    f.reach(old,"cancelled");

    constexpr auto request = "Write release notes explaining these findings";
    f.script(request,{start({{"request",request},{"sourceResultMessageId",source}})});
    f.receive("release-notes",request,source.get<Id>());
    const auto next = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(next,"running");
    const auto task = f.task(next);
    const auto& instruction = f.t3.sent.back().instruction;
    check(next != old && task["sourceTaskId"] == old && task["doneWhen"] == "Report baseline latency" &&
          task["sourceResultRefs"][0]["messageId"] == source && task["sourceResultRefs"][0]["text"] == report &&
          !task.contains("restartContext") && instruction.find(report) != instruction.npos &&
          instruction.find("export raw latency samples") == instruction.npos &&
          instruction.find("Use a pinned regression fixture") == instruction.npos,
          "new work did not keep the cancelled report separate from the cancelled task's criteria and directions");
}

void resumeRestoresCancelledTaskDirections() {
    Fixture f;
    const auto old = f.store->submit(f.routing("Measure the reference dataset"));
    f.script("Use a pinned regression fixture",{send(old)});
    f.receive("past-direction","Use a pinned regression fixture");
    f.reach(old,"running");
    f.script("Keep the raw timing file",{send(old)});
    f.receive("pending-direction","Keep the raw timing file");
    f.store->operate(old,0,"cancel");
    f.t3.finish("run-1","Cancelled work settled");
    f.reach(old,"cancelled");
    Id cancellation = 0;
    for (const auto& row : f.store->history(old))
        if (row["kind"] == "cancelled") cancellation = row["id"];
    check(cancellation > 0,"the cancelled task had no resume anchor");

    const auto running = f.store->submit(f.routing("Another task","alpha","desktop","other"));
    f.reach(running,"running");
    constexpr auto request = "この作業をクラウドMacで再開して";
    f.script(request,{start({{"request","Resume the measurement"},{"sourceResultMessageId",cancellation},
                             {"resume",true},{"environment","クラウドMac"}})});
    f.script("Resume the other one",{start({{"request","Resume"},{"resume",true}})});
    f.receive("resume-wrong-source","Resume the other one");
    check(f.lastTurn()[0]["result"]["ok"] == false && f.store->tasks("alpha").size() == 2,
          "resume accepted a source that is not a cancelled task's result");
    f.receive("restart-work",request,cancellation);
    const auto next = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(next,"running");
    const auto& instruction = f.t3.sent.back().instruction;
    check(f.task(next)["doneWhen"] == f.task(old)["doneWhen"] && f.task(next)["sourceTaskId"] == old && next != old &&
          f.task(next)["environmentId"] == "cloud" &&
          instruction.find("Measure the reference dataset") != std::string::npos &&
          instruction.find("Report baseline latency") != std::string::npos &&
          instruction.find("Use a pinned regression fixture") != std::string::npos &&
          instruction.find("Keep the raw timing file") != std::string::npos && f.t3.sent.back().tool == "delegate",
          "resume omitted the original intent, criteria, or accepted directions");
}

void appendWaitsForSafeBoundary() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    const auto next = f.store->submit(f.routing("別件","beta"));
    f.script("補足として表をCSV形式で出して",{send(id)});
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

void twoInstructionsFromOneMessageAreTrackedSeparately() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    constexpr auto text = "表をCSVにして、グラフも付けて";
    f.script(text,{send(id,{{"instruction","Export the table as CSV"}}),
                   send(id,{{"instruction","Add a chart"}})});
    const auto message = f.receive("two-instructions",text);
    const auto pending = f.task(id)["pendingInstructions"];
    check(pending.size() == 2 && pending[0]["step"] == 1 && pending[1]["step"] == 2 &&
          pending[0]["messageId"] == message["messageId"] && pending[1]["messageId"] == message["messageId"] &&
          f.response(message)["actions"].size() == 2,"one message's two instructions were merged or lost");
    f.t3.finish("run-1","Baseline evidence");
    f.reach(id,"deciding");
    f.reach(id,"running");
    check(f.t3.sent.back().instruction.find("Export the table as CSV") != std::string::npos &&
          f.t3.sent.back().instruction.find("Add a chart") != std::string::npos &&
          f.conversationMessage(message["messageId"])["deliveredToT3"] == true,
          "the instructions were not both delivered");
}

void unknownSendThenScopeChange() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"sending");
    f.t3.loseNextSend = true;
    f.reach(id,"waiting");
    const auto before = f.task(id);
    f.script("完了条件は p95 レイテンシを報告",{change(id,{{"doneWhen","p95 レイテンシを報告"}})});
    const auto change = f.receive("revision","完了条件は p95 レイテンシを報告");
    check(f.task(id)["revisions"].size() == 2 && f.task(id)["doneWhen"] == "p95 レイテンシを報告",
          "explicit criteria did not create a revision");
    f.restart();
    f.t3.login = "different-login";
    f.t3.deliveryVisible = false;
    f.tick(3);
    check(f.t3.sent.size() == 1 && !f.t3.reconciled.empty() && f.t3.interrupted.empty(),
          "an unconfirmed launch was resent, skipped reconciliation, or stopped before identifying its Run");
    const auto firstCommand = f.t3.sent.front();
    check(std::all_of(f.t3.reconciled.begin(),f.t3.reconciled.end(),[&](const Command& command) {
        return command.actionId == firstCommand.actionId && command.authGeneration == firstCommand.authGeneration &&
            command.instruction == firstCommand.instruction && command.tool == firstCommand.tool &&
            command.environmentId == firstCommand.environmentId && command.projectId == firstCommand.projectId;
    }),"recovery changed the original action ID, payload or authentication generation");
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
    f.script("完了条件はメモリを計測",{change(id,{{"doneWhen","メモリを計測"}})});
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

void receivedRevisionPrecedesCompletion() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.t3.finish("run-1","Old timing evidence");
    f.reach(id,"deciding");

    constexpr auto request = "Change the completion criteria to report CPU timing";
    f.script(request,{change(id,{{"doneWhen","report CPU timing"}})});
    Json receipt;
    f.model.beforeComplete = [&](const ModelInput& input) {
        if (input.event["resultRunId"] == "run-1" && receipt.is_null())
            receipt = f.enqueue("received-revision",request);
    };
    f.tick();
    check(receipt["status"] == "received" && f.response(receipt).is_null() && f.event(id,"completed").is_null(),
          "the old task completed after the revision was received but before its turn ran");

    f.reach(id,"running");
    check(f.response(receipt)["actions"][0]["effect"] == "replanning" && f.task(id)["doneWhen"] == "report CPU timing" &&
          f.t3.sent.size() == 2 && f.t3.sent.back().instruction.find("report CPU timing") != std::string::npos,
          "the earlier receipt did not drive the next Run on the original task");
    f.t3.finish("run-2","New CPU timing evidence");
    f.reach(id,"completed");
    check(f.event(id,"completed")["text"] == "New CPU timing evidence","completion reused evidence from the old criteria");
}

void unfinishedTurnHoldsCompletion() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.t3.finish("run-1","Timing evidence");
    f.reach(id,"deciding");
    const auto report = f.store->latestTaskResult(id).at("id");
    int completionCalls = 0;
    f.model.beforeComplete = [&](const ModelInput&) { ++completionCalls; };
    bool failing = true;
    constexpr auto request = "What did the timing show?";
    f.script(request,{
        [&](const ModelInput&) { return Decision{"read_result",{{"messageId",report}},"Read the report"}; },
        [&](const ModelInput&) -> Decision {
            if (failing) throw ModelError("The model connection dropped",true);
            return {"reply",{{"text","It shows the baseline."}},"Answer"};
        }});
    const auto receipt = f.enqueue("unfinished",request);
    f.tick();
    check(f.event(id,"completed").is_null() && f.response(receipt).is_null() && completionCalls == 1,
          "a task completed while a conversation turn was unfinished");
    failing = false;
    f.tick();
    check(f.response(receipt)["text"] == "It shows the baseline." && f.lastTurn().size() == 1 &&
          f.lastTurn()[0]["result"]["ok"] == true,"the retried turn repeated or lost its finished step");
    f.reach(id,"completed");
    check(completionCalls == 1,"the held completion asked the model again after a read-only turn");
}

void pendingReadOnlyMessageDoesNotSpendAnotherCompletionCall() {
    Fixture f;
    auto routing = f.routing();
    routing.config["modelCallLimit"] = 2;
    const auto id = f.store->submit(routing);
    const auto other = f.store->submit(f.routing("Separate task","beta","cloud"));
    f.reach(id,"running");
    f.reach(other,"running");
    for (const auto task : {id,other})
        f.t3.finish(f.task(task)["activeRun"]["runId"].get<std::string>(),"Settled evidence");
    f.tick();

    constexpr auto request = "How do you define elapsed time?";
    f.script(request,{say("Time from start to finish.")});
    bool received = false;
    int completionCalls = 0;
    f.model.beforeComplete = [&](const ModelInput& input) {
        if (!std::exchange(received,true)) f.enqueue("read-only",request);
        if (input.task["id"] == id) ++completionCalls;
    };
    f.tick();
    check(f.task(other)["state"] == "completed" && f.task(id)["state"] == "deciding" && completionCalls == 1,
          "a received message failed to hold same-Bot completion or held another Bot's completion");

    f.restart();
    f.reach(id,"completed");
    check(f.event(id,"completed")["text"] == "Settled evidence" && completionCalls == 1,
          "the buffered completion was lost or requested another model call after the read-only turn");
}

void unrelatedUpdatesDoNotDisturbATurn() {
    Fixture f;
    const auto targetId = f.store->submit(f.routing("Benchmark"));
    const auto other = f.store->submit(f.routing("Unrelated task","alpha","cloud"));
    constexpr auto request = "Add a table to the Benchmark";
    f.script(request,{send(targetId)});
    f.script("status",{say("Working on it.")});
    int laterMessages = 0;
    f.beforeNotes = [&] { if (!laterMessages++) f.enqueue("arrived-later","status"); };
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (input.event["text"] != request || !input.event["turn"].empty()) return;
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open(f.store->path().c_str(),&raw);
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
        check(opened == SQLITE_OK,"could not open the generation fixture");
        const auto sql = "UPDATE tasks SET generation=generation+1 WHERE id=" + std::to_string(other);
        check(sqlite3_exec(db.get(),sql.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,
              "could not change the unrelated task generation");
    };
    const auto receipt = f.receive("append",request);
    check(f.event(targetId,"instruction_accepted")["messageId"] == receipt["messageId"] &&
          f.event(other,"instruction_accepted").is_null() && f.response(receipt)["actions"].size() == 1,
          "an unrelated generation change or later receipt disturbed the turn");
    const auto later = f.store->receipt("alpha","arrived-later","status");
    check(later && f.response(*later)["text"] == "Working on it.",
          "the later message did not get its own turn after the earlier one");
}

void staleTargetIsReportedToTheModel() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    constexpr auto instruction = "計測結果を表にして";
    f.script(instruction,{send(id)});
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (!input.event["turn"].empty()) return;
        Store writer(f.dir/"state.db");
        writer.operate(id,0,"cancel");
    };
    const auto receipt = f.receive("racing-instruction",instruction);
    const auto result = f.lastTurn()[0]["result"];
    check(result["ok"] == false && result["error"] == "stale" && result["applied"] == 0 &&
          result["current"]["state"] == "cancelled" && f.task(id)["state"] == "cancelled" &&
          f.event(id,"instruction_accepted").is_null() && f.response(receipt)["actions"].empty() && f.t3.sent.empty(),
          "a stale instruction was applied or the model did not see the current state");
}

void pendingTurnDoesNotBlockIndependentProject() {
    using namespace std::chrono_literals;
    Fixture f;
    const auto project = f.store->submit(f.routing("Separate project", "alpha", "desktop", "other-project"));
    f.reach(project,"deciding");
    std::promise<void> turnEntered, allowTurn, projectDeciding;
    const auto turning = turnEntered.get_future().share();
    const auto resume = allowTurn.get_future();
    const auto projectProgress = projectDeciding.get_future();
    bool signalled = false;
    f.model.beforeTurn = [&](const ModelInput&) {
        if (std::exchange(signalled,true)) return;
        turnEntered.set_value();
        resume.wait();
    };
    f.model.beforeIntake = [&](const ModelInput&) {
        if (turning.wait_for(5s) == std::future_status::ready) projectDeciding.set_value();
    };
    constexpr auto request = "Create a release note";
    f.script(request,{start()});
    f.enqueue("slow-turn",request);
    auto dispatch = std::async(std::launch::async,[&] { f.runtime->tickConcurrent(); });
    const bool started = projectProgress.wait_for(5s) == std::future_status::ready;
    allowTurn.set_value();
    dispatch.get();
    f.runtime.reset();
    check(started && f.task(project)["state"] == "sending" && f.store->tasks("alpha").size() == 2,
          "a pending conversation turn blocked an independent project's model decision");
}

void concurrentTasksShareModelBudget() {
    using namespace std::chrono_literals;
    Fixture f;
    const auto text = std::string("ローカルMacで利用件数を集計して。\nクラウドMacでメモリを計測して");
    f.script(text,{start({{"request","ローカルMacで利用件数を集計して"},{"environment","desktop"}}),
                   start({{"request","クラウドMacでメモリを計測して"},{"environment","クラウドMac"}})});
    auto routing = f.routing();
    routing.config["modelCallLimit"] = 4;
    f.store->receive(routing,"compound-budget",text);
    f.runtime->runConversations();
    const auto tasks = f.store->tasks("alpha");
    check(tasks.size() == 2 && f.task(tasks[1]["id"])["environmentId"] == "desktop" &&
          f.task(tasks[0]["id"])["environmentId"] == "cloud","one message did not create separate tasks");
    f.tick();

    std::atomic_int modelCalls = 0;
    std::promise<void> releaseModel;
    const auto release = releaseModel.get_future().share();
    f.model.beforeIntake = [&](const ModelInput&) {
        if (++modelCalls == 1) release.wait();
    };
    f.runtime->tickConcurrent();
    Json paused;
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (paused.is_null() && std::chrono::steady_clock::now() < deadline) {
        for (const auto& task : tasks) {
            const auto current = f.task(task.at("id"));
            if (current["state"] == "waiting" && current["wait"]["reason"] == "model_budget") paused = current;
        }
        if (paused.is_null()) std::this_thread::sleep_for(5ms);
    }
    releaseModel.set_value();
    f.restart();
    check(modelCalls == 1 && !paused.is_null(),
          "concurrent tasks used the same last slot or held SQLite locked during inference");
    const Id pausedId = paused.at("id"), waitId = paused.at("wait").at("id");
    f.tick(3);
    check(modelCalls == 1 && f.task(pausedId)["wait"]["id"] == waitId,"restart reset the shared allowance");

    const auto approve = [&] {
        Store writer(f.dir/"state.db");
        try { writer.operate(pausedId,waitId,"approve"); return true; }
        catch (const Conflict&) { return false; }
    };
    auto first = std::async(std::launch::async,approve);
    auto second = std::async(std::launch::async,approve);
    const bool firstApplied = first.get(), secondApplied = second.get();
    check(firstApplied != secondApplied,"two SQLite connections applied the same approval");
    f.reach(pausedId,"running");
    check(modelCalls == 2 && f.t3.sent.size() == 2,"approval did not resume the blocked task exactly once");
}

void cancellationReleasesOnlyAfterStopConfirmation() {
    Fixture f;
    const auto stopped = f.store->submit(f.routing("計測作業"));
    const auto follower = f.store->submit(f.routing("同じプロジェクトの次の作業"));
    f.reach(stopped,"running");
    const auto independent = f.store->submit(f.routing("別プロジェクトの作業","alpha","desktop","other-project"));
    f.script("計測作業を停止して",{stop(stopped)});
    const auto receipt = f.receive("cancel-active","計測作業を停止して");
    check(f.response(receipt)["status"] == "stopping","the stop was not reported as in progress");
    f.t3.loseNextStop = true;
    f.reach(independent,"running");
    check(f.task(stopped)["state"] == "cancelling" && f.task(follower)["state"] == "queued" &&
          f.t3.interrupted == std::vector{std::pair<std::string,std::string>{"desktop","run-1"}} &&
          f.t3.sent.size() == 2 && f.t3.sent.back().projectId == "other-project",
          "a lost stop acknowledgment released the same-project gate or blocked an independent project");
    f.restart();
    f.t3.finish("run-1","Stopped, with delegated work still settling",false);
    f.now += 20;
    f.tick(3);
    check(f.task(stopped)["state"] == "cancelling" && f.task(follower)["state"] == "queued" &&
          f.t3.interrupted.size() == 1 && f.t3.sent.size() == 2,
          "cancellation was published or the follower ran before the prior Run settled");
    f.t3.finish("run-1","Delegated work settled");
    f.reach(stopped,"cancelled");
    f.reach(follower,"running");
    check(f.t3.sent.size() == 3 && f.t3.sent.back().projectId == "project" &&
          f.task(independent)["state"] == "running", "settled cancellation did not release its own queue");
}

void cloudDestinationWaitsForItsProject() {
    Fixture f;
    const auto occupied = f.store->submit(f.routing("Cloud work","beta","cloud"));
    const auto moving = f.store->submit(f.routing());
    f.script("Include the table as CSV",{send(moving)});
    f.receive("format-before-launch","Include the table as CSV");
    f.reach(occupied,"running");
    f.reach(moving,"running");
    f.script("Run the measurement on the cloud Mac",{change(moving,{{"environment","クラウドMac"}})});
    f.receive("where","Run the measurement on the cloud Mac");
    const auto following = f.store->submit(f.routing("Cloud follower","beta","cloud"));
    const auto desktopFollowing = f.store->submit(f.routing("Desktop follower","beta"));
    f.tick(3);
    check(f.t3.interrupted.size() == 1 && f.t3.interrupted[0] == std::pair<std::string,std::string>{"desktop","run-2"} &&
          f.task(moving)["revisions"].size() == 1,
          "a confirmed environment change interrupted the wrong Run or revised the criteria");
    f.t3.finish("run-2","Old location is still settling",false);
    f.tick(2);
    check(f.task(desktopFollowing)["state"] == "queued" && f.task(desktopFollowing)["blockedBy"]["id"] == moving,
          "the desktop slot was released before the old Run settled");
    f.t3.finish("run-2","Old location stopped");
    f.restart();
    f.reach(desktopFollowing,"running");
    check(f.task(following)["state"] == "queued" && f.t3.sent.size() == 3 &&
          f.t3.sent.back().environmentId == "desktop" && f.task(moving)["environmentId"] == "desktop",
          "work entered a Cloud project where another Task was still active");
    f.t3.finish("run-1","Cloud project complete");
    f.reach(occupied,"completed");
    f.reach(moving,"running");
    const auto task = f.task(moving);
    check(f.t3.sent.size() == 4 && f.t3.sent.back().tool == "delegate" && f.t3.sent.back().environmentId == "cloud" &&
          f.t3.sent.back().instruction.find("as CSV") != std::string::npos &&
          task["remaining"] == 2 && task["runs"][0]["environmentId"] == "desktop" &&
          task["runs"][1]["environmentId"] == "cloud" && task["runs"][0]["threadId"] != task["runs"][1]["threadId"] &&
          f.task(following)["state"] == "queued" && f.task(following)["blockedBy"]["id"] == moving,
          "the Task switched Session or budget incorrectly");
    const auto nextDesktop = f.store->submit(f.routing("Next desktop task","beta"));
    f.t3.finish("run-3","Desktop follower complete");
    f.reach(nextDesktop,"running");
    check(f.t3.sent.size() == 5 && f.t3.sent.back().environmentId == "desktop" && f.task(moving)["state"] == "running",
          "the transferred Task still held the desktop queue after moving");
}

void queuedMoveWaitsForTheNewerDestinationOwner() {
    Fixture f;
    const auto local = f.store->submit(f.routing("Existing local work","beta"));
    const auto moving = f.store->submit(f.routing("Transfer this task"));
    const auto remote = f.store->submit(f.routing("Newer destination owner","beta","cloud"));
    f.reach(local,"running");
    f.reach(remote,"running");
    f.script("Transfer this task to cloud",{change(moving,{{"environment","cloud"}})});
    f.receive("move-queued","Transfer this task to cloud");
    f.restart();
    f.advanceUntil([&] { return f.task(moving).value("environmentReleased",false); },
                   "the queued transfer stayed behind unrelated work in its old queue");
    check(f.task(moving)["blockedBy"]["id"] == remote && f.t3.sent.size() == 2 && f.t3.interrupted.empty(),
          "the transferred Task bypassed the newer active destination owner");
    f.t3.finish(f.task(remote)["activeRun"]["runId"].get<std::string>(),"Destination settled");
    f.reach(moving,"running");
    check(f.t3.sent.size() == 3 && f.t3.sent.back().environmentId == "cloud" && f.task(local)["state"] == "running",
          "the queued Task did not retain its route through restart");
}

void latestTargetPrecedesABatchedBacklog() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.script("計測はクラウドMacで",{change(id,{{"environment","クラウドMac"}})});
    f.receive("old-location","計測はクラウドMacで");
    std::vector<std::string> notes;
    for (int i = 0; i < 5; ++i) {
        notes.push_back("補足として表をCSV形式で出して " + std::string(14500,'a') + " fixture note " + std::to_string(i));
        f.script(notes.back(),{send(id)});
        f.receive("notes-" + std::to_string(i),notes.back());
    }
    f.script("計測はローカルMacで",{change(id,{{"environment","ローカルMac"}})});
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
        const auto instruction = "補足として表をCSV形式で出して for fixture " + std::to_string(run);
        f.script(instruction,{send(id)});
        f.receive("instruction-" + std::to_string(run),instruction);
        f.t3.finish("run-" + std::to_string(run),"Prior evidence");
        f.reach(id,"deciding");
        f.reach(id,run < 4 ? "running" : "waiting");
    }
    check(f.task(id)["wait"]["reason"] == "budget" && f.task(id)["remaining"] == 0 &&
          f.t3.sent.size() == 4 && f.store->tasks("alpha").size() == 1,
          "the exhausted budget still permitted a new Run or Task");
    const auto wait = f.task(id)["wait"];
    f.script("はい",{answer(id)});
    f.receive("unrelated-agreement","はい",wait["messageId"].get<Id>());
    check(f.lastTurn()[0]["result"]["ok"] == false && f.task(id)["wait"] == wait && f.task(id)["remaining"] == 0 &&
          f.event(id,"budget_granted").is_null(),"a conversation turn approved a budget wait");
    f.store->operate(id,wait["id"],"approve");
    f.reach(id,"running");
    check(f.t3.sent.size() == 5 && f.task(id)["remaining"] == 0 && f.task(id)["additionalUsed"] == 4 &&
          f.store->tasks("alpha").size() == 1,"explicit budget approval did not permit precisely one extra Run");
    check(f.t3.sent.back().instruction.find("for fixture 4") != std::string::npos,
          "the approved Run lost the instruction that was held for approval");
}

void revisionOfProvablyUnsentAction() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"sending");
    f.script("完了条件はレポートを出す",{change(id,{{"doneWhen","レポートを出す"}})});
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
    f.script("完了条件は p99 レイテンシを報告",{change(id,{{"doneWhen","p99 レイテンシを報告"}})});
    const auto receipt = f.receive("criteria-before-launch","完了条件は p99 レイテンシを報告");
    f.reach(id,"running");
    const auto task = f.task(id);
    check(task["revisions"].size() == 1 && task["revisions"][0]["sourceMessageId"] == receipt["messageId"] &&
          task["revisions"][0]["doneWhen"] == "p99 レイテンシを報告" &&
          task["runs"][0]["revisionId"] == task["currentRevision"] && f.t3.sent.size() == 1 &&
          f.t3.sent.front().instruction.find("p99 レイテンシを報告") != std::string::npos,
          "the initial Run did not receive the user's completion criteria");
}

void startCriteriaAreUsedForFirstDelegation() {
    Fixture f;
    constexpr auto request = "Measure startup and report p95";
    f.script(request,{start({{"request","Measure startup"},{"doneWhen","Report p95 startup time"}})});
    const auto receipt = f.receive("start-with-criteria",request);
    const auto id = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(id,"running");
    const auto task = f.task(id);
    check(task["doneWhen"] == "Report p95 startup time" && task["revisions"].size() == 1 &&
          task["revisions"][0]["sourceMessageId"] == receipt["messageId"] &&
          task["runs"][0]["revisionId"] == task["currentRevision"],
          "the criteria given when the task started were not used");
}

void notesReachTheRunningSession() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    const auto queued = f.store->submit(f.routing("Queued work"));
    f.reach(id,"running");
    constexpr auto text = "Use the staging database for the rest of this run";
    f.script(text,{note(id),note(queued)});
    const auto message = f.receive("steer",text);
    const auto turn = f.lastTurn();
    check(f.t3.steered.size() == 1 && f.t3.steered[0].instruction == text &&
          Json(f.t3.steered[0].target.runId) == f.task(id)["activeRun"]["runId"] && f.t3.sent.size() == 1 &&
          turn[0]["result"]["effect"] == "delivered" && turn[1]["result"]["ok"] == false &&
          f.event(id,"instruction_steered")["text"] == text && f.task(id)["pendingInstructions"].is_null() &&
          f.conversationMessage(message["messageId"])["deliveredToT3"] == true &&
          f.response(message)["status"] == "working",
          "a note did not reach the running Run directly, or one reached a task without a Run");
    f.t3.finish("run-1","Finished");
    f.reach(id,"completed");
    f.script("One more note",{[&](const ModelInput&) {
        return Decision{"message_session",{{"taskId",id},{"generation",f.task(id)["generation"]},{"text","late"}},"Late"};
    }});
    f.receive("late-note","One more note");
    check(f.lastTurn()[0]["result"]["ok"] == false && f.t3.steered.size() == 1,
          "a note for a finished task reached T3");
}

void interruptLeavesTheTaskOpen() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    const auto run = f.task(id)["activeRun"]["runId"].get<std::string>();
    f.script("Pause that run",{interrupt(id)});
    const auto message = f.receive("interrupt","Pause that run");
    check(f.t3.interrupted == std::vector{std::pair<std::string,std::string>{"desktop",run}} &&
          f.lastTurn()[0]["result"]["effect"] == "interrupt_requested" && f.event(id,"interrupt_requested").is_object() &&
          f.event(id,"cancel_requested").is_null() && f.task(id)["state"] != "cancelling" &&
          f.response(message)["status"] == "stopping","an interrupt request cancelled the task or missed the Run");
}

void environmentsResolveByNameOrId() {
    Fixture f;
    auto routing = f.routing();
    auto& aliases = routing.config["conversationEnvironments"];
    aliases["cloud"] = "staging";
    for (int i = 0; i < 15; ++i) aliases["alias" + std::to_string(i)] = "executor-" + std::to_string(i);
    aliases["z-final"] = "executor-final";
    constexpr auto text = "Run the benchmark on cloud, the archive on z-final, and the audit on mars";
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (!input.event["turn"].empty()) return;
        const auto& environments = input.task.at("environments");
        check(std::ranges::any_of(environments,[](const Json& item) { return item["name"] == "z-final" &&
                  item["environmentId"] == "executor-final"; }),"the environment list truncated the last alias");
    };
    f.script(text,{start({{"request","Benchmark"},{"environment","cloud"}}),
                   start({{"request","Archive"},{"environment","z-final"}}),
                   start({{"request","Audit"},{"environment","mars"}})});
    const auto message = f.store->receive(routing,"environments",text);
    f.runtime->runConversations();
    const auto tasks = f.store->tasks("alpha");
    check(tasks.size() == 2 && f.task(tasks[1]["id"])["environmentId"] == "cloud" &&
          f.task(tasks[0]["id"])["environmentId"] == "executor-final" &&
          f.lastTurn()[2]["result"]["ok"] == false && f.response(message)["actions"].size() == 2,
          "a canonical ID was shadowed by an alias, an alias was missed, or an unknown environment was accepted");
}
}

int main() {
    try {
        durableInboxAndTargetedRevision();
        linkedResultStartsANewSession();
        independentRequestQueuesSeparately();
        followUpCarriesTheReferencedReport();
        questionAnswerReachesTheBotBeforeContinuingWork();
        answersStayOnTheirQuestion();
        answersNeedTheirOwnReplyTargetAndAStableTask();
        pausedTurnHoldsCompletion();
        cancelledReportCanBeReferencedWithoutRestarting();
        resumeRestoresCancelledTaskDirections();
        appendWaitsForSafeBoundary();
        twoInstructionsFromOneMessageAreTrackedSeparately();
        unknownSendThenScopeChange();
        completeRacingWithNewCriteria();
        receivedRevisionPrecedesCompletion();
        unfinishedTurnHoldsCompletion();
        pendingReadOnlyMessageDoesNotSpendAnotherCompletionCall();
        unrelatedUpdatesDoNotDisturbATurn();
        staleTargetIsReportedToTheModel();
        pendingTurnDoesNotBlockIndependentProject();
        concurrentTasksShareModelBudget();
        cancellationReleasesOnlyAfterStopConfirmation();
        cloudDestinationWaitsForItsProject();
        queuedMoveWaitsForTheNewerDestinationOwner();
        latestTargetPrecedesABatchedBacklog();
        continuationUsesTheCurrentBudget();
        revisionOfProvablyUnsentAction();
        queuedCriteriaAreUsedForFirstDelegation();
        startCriteriaAreUsedForFirstDelegation();
        environmentsResolveByNameOrId();
        notesReachTheRunningSession();
        interruptLeavesTheTaskOpen();
        std::cout << "Conversation Runtime acceptance passed with simulated T3\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
