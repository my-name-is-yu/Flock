#include "flock/policy.hpp"
#include "flock/board_inbox.hpp"
#include "flock/participation.hpp"
#include "flock/t3_connection.hpp"
#include "task_history.hpp"
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
#include <string_view>
#include <unistd.h>
#include <utility>

using namespace flockv0;
using flockv0::test::lastTaskEvent;

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
        const auto task = listed(input,id);
        return Decision{"answer_task_question",{{"taskId",id},{"waitId",task.at("wait").at("waitId")},
                                                {"revisionId",task.at("revisionId")},{"generation",task.at("generation")},
                                                {"text",input.event.at("text")}},"Scripted answer"};
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
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
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
            if (input.needsDoneWhen) args["doneWhen"] = "Report baseline latency";
            return {"delegate",args,"Collect the fixture evidence"};
        }
        check(input.event["kind"] == "result","unexpected model event");
        if (beforeComplete) beforeComplete(input);
        if (onResult) return onResult(input);
        if (input.task.contains("userInstructions")) {
            const auto canFollow = std::ranges::find(input.tools,"follow_up") != input.tools.end();
            return {canFollow ? "follow_up" : "delegate",{{"instruction","Apply the pending instructions"}},
                    "The report predates the user instructions"};
        }
        return {"complete",{{"report",input.event["report"]}},"The fixture report includes the requested evidence"};
    }
};

struct SimulatedT3 : T3 {
    std::vector<Command> sent, reconciled;
    std::vector<std::pair<std::string,std::string>> interrupted;
    std::map<std::string,RunRef> deliveries;
    std::map<std::string,Observation> observations;
    std::map<std::pair<std::string,std::string>,SessionMetadata> sessions;
    std::map<std::string,std::vector<T3Project>> projectsByEnvironment;
    bool loseNextSend = false, loseNextStop = false, deliveryVisible = true;
    bool deferNextRunId = false;
    std::string login = "original-login";
    InterruptOutcome interruptOutcome = InterruptOutcome::Requested;

    std::vector<T3Project> projects(std::string_view environment) override {
        const auto found = projectsByEnvironment.find(std::string(environment));
        return found == projectsByEnvironment.end() ? std::vector<T3Project>{{"project","Fixture Project",{}}} : found->second;
    }

    std::pair<std::vector<std::string>,std::optional<Id>> projectSessions(
        std::string_view environment, std::string_view project, Id, SessionReadDeadline) override {
        std::vector<std::string> threads;
        for (const auto& [target,metadata] : sessions)
            if (target.first == environment && (project.empty() || project == metadata.projectId))
                threads.push_back(target.second);
        return {std::move(threads),std::nullopt};
    }
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override {
        const auto item = sessions.find({target.environmentId,target.threadId});
        return item == sessions.end() ? std::nullopt : std::optional(item->second);
    }
    std::string authGeneration(const std::string&,const Json&) override {
        return login;
    }
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
        if (std::exchange(deferNextRunId,false))
            return {Delivery::Kind::Unknown,{run.threadId,{},run.link},{}};
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
    InterruptOutcome interrupt(const std::string& environment,const Json&,const RunRef& run) override {
        interrupted.emplace_back(environment,run.runId);
        if (std::exchange(loseNextStop,false)) throw std::runtime_error("simulated interrupted acknowledgement");
        return interruptOutcome;
    }
    void finish(std::string_view run, std::string text, bool settled = true) {
        auto& report = observations.at(std::string(run));
        report.kind = settled ? Observation::Kind::Finished : Observation::Kind::Settling;
        report.status = "completed";
        report.report = std::move(text);
        report.resultRunId = run;
        report.itemId = "report-" + std::string(run);
    }
};

struct Fixture {
    std::filesystem::path dir;
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    Script model;
    SimulatedT3 t3;
    std::function<void()> beforeNotes;
    Store::MemorySeed memorySeed;
    bool shareMemoryWithSessions = false;
    Seconds now = unixTime();

    explicit Fixture(Store::MemorySeed seed = {}) : memorySeed(std::move(seed)) {
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
        store = std::make_unique<Store>(dir/"state.db",memorySeed);
        runtime = std::make_unique<Runtime>(*store,model,t3,[this](auto) {
            if (beforeNotes) beforeNotes();
            return std::string{};
        },RuntimeOptions{[this] { return now; }});
    }
    Intake routing(std::string request = "計測", std::string bot = "alpha", std::string environment = "desktop",
                   std::string project = "project") {
        return {std::move(bot),std::move(request),std::move(environment),std::move(project),
            {{"conversationEnvironments",{{"クラウドMac","cloud"},{"ローカルMac","desktop"}}},
             {"shareMemoryWithSessions",shareMemoryWithSessions}}};
    }
    Json enqueue(std::string_view key,std::string_view text,Id replyTo = 0) {
        Store httpWriter(dir/"state.db");
        return httpWriter.receive(localOwner,routing(),key,text,replyTo);
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
    Json response(const Json& receipt, std::string_view bot = "alpha") {
        for (const auto& row : store->conversation(bot))
            if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == receipt["messageId"]) return row["data"];
        return nullptr;
    }
    Json event(Id id,std::string_view kind) {
        const auto entry = lastTaskEvent(*store,id,kind);
        return entry.is_null() ? Json(nullptr) : entry.at("data");
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
    const auto receipt = f.store->receipt(localOwner,"alpha","raw",criteria);
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
    f.t3.sessions[{"desktop","origin"}] = {.title="Plan Session",.projectId="project"};
    T3Connections connector(*f.store);
    const auto token = connector.connect("desktop",localOwner.id,48123);
    const auto connection = connector.authenticate(token);
    SessionParticipation participation(*f.store,f.t3);
    f.script("計測方法の提案",{start()});
    participation.ask(*connection,{"alpha","計測方法の提案","origin","original-plan"},
                      [&](auto,auto) { return f.routing(); });
    f.runtime->runConversations();
    const auto original = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(original,"running");
    const std::string report = "1. Measure by hand\n2. Capture a fixture trace\n3. Measure after release";
    f.t3.finish("run-1",report);
    f.reach(original,"completed");
    const auto earlier = f.event(original,"completed");
    const Id resultId = lastTaskEvent(*f.store,original,"completed").at("id");
    f.store->setWebIdentityResolver([](std::string_view person) -> std::optional<bool> {
        return person == localOwner.id ? std::optional{true} : std::nullopt;
    });
    f.script("Which Session produced the plan?",{[](const ModelInput& input) {
        check(input.event.at("sessionOrigin").at("threadId") == "origin","The result reply lost the Session origin");
        return Decision{"reply",{{"text","The plan came from the linked child Session."}},"Report the Session"};
    }});
    const auto reply = f.store->receiveWeb(localOwner,f.routing(),"reply-to-plan","Which Session produced the plan?",resultId);
    f.runtime->runConversations();
    check(f.response(reply).at("text") == "The plan came from the linked child Session.","The explicit result reply failed");
    participation.stop(*connection,"alpha","origin");
    f.script("2案目を実装して",{start({{"request","Implement option 2: capture a fixture trace"},
                                       {"sourceResultMessageId",resultId}})});
    f.store->receiveWeb(localOwner,f.routing(),"implement","2案目を実装して");
    f.runtime->runConversations();
    const auto task = f.store->tasks("alpha").front();
    const auto id = task["id"].get<Id>();
    check(id != original && task["sourceTaskId"] == original && task["sourceResultRefs"].size() == 1 &&
          task["sourceResultRefs"][0]["messageId"] == resultId && task["sourceResultRefs"][0]["text"] == report,
          "implementation was not linked to the earlier result");
    f.reach(id,"running");
    check(f.t3.sent.size() == 2 && f.t3.sent.front().sessionOrigin && !f.t3.sent.back().sessionOrigin &&
          f.t3.sent.back().tool == "delegate" &&
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
    const Id report = lastTaskEvent(*f.store,id,"report").at("id");
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
    f.model.onResult = {};
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
    f.script("Which accounts can I use?",{[](const ModelInput&) {
        return Decision{"reply",{{"text","Choose an account for the measurement."},{"continuesQuestion",true}},
                        "Clarify the task question"};
    }});
    const auto clarification = f.response(f.receive("clarify","Which accounts can I use?",f.task(id)["wait"].at("messageId")));
    f.script("Which one is for testing?",{[](const ModelInput&) {
        return Decision{"reply",{{"text","Quartz-717 is the test account."},{"suggestions",{"Use account Quartz-717"}},
                                 {"continuesQuestion",true}},
                        "Offer the test account"};
    }});
    const auto explanation = f.response(f.receive("explain","Which one is for testing?",clarification.at("messageId")));
    f.restart();
    for (int i = 0; i < 16; ++i) f.store->submit(f.routing("Later queued task " + std::to_string(i)));
    f.script("Use account Quartz-717",{answer(id)});
    f.receive("account",explanation.at("suggestions").at(0).get<std::string>(),explanation.at("messageId"));
    check(f.event(id,"user_answer")["text"] == "Use account Quartz-717","the answer did not reach the question");
    const auto& continuation = f.model.turnInputs.back();
    check(std::ranges::none_of(continuation.history,[&](const Json& entry) {
        return entry.value("taskId",Id{0}) == id && entry.value("kind",std::string{}) == "user_answer" &&
               entry.value("text",std::string{}) == "Use account Quartz-717";
    }),"the written answer was duplicated as a task answer in history");
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

    f.model.beforeTurn = [&](const ModelInput& input) {
        if (!input.event.at("turn").empty()) return;
        Store writer(f.dir/"state.db");
        writer.operate(localOwner,TaskTarget::from(writer.task(naming),namingWait.at("id")),"answer",{{"text","別の名前"}});
    };
    f.script("Flock",{answer(naming)});
    f.receive("late-answer","Flock");
    f.model.beforeTurn = {};
    check(f.lastTurn()[0]["result"]["ok"] == false && f.event(naming,"user_answer")["text"] == "別の名前",
          "an in-flight reply overwrote the answer that already reached the task");

    f.reach(naming,"running");
    f.model.onResult = [&](const ModelInput&) {
        f.enqueue("early-answer","Confirmed");
        return Decision{"ask_user",{{"question","Confirm the name?"}},"The result needs confirmation"};
    };
    f.script("Confirmed",{answer(naming)});
    f.t3.finish(f.task(naming).at("activeRun").at("runId").get<std::string>(),"Name selected");
    f.reach(naming,"waiting");
    const auto reopened = f.task(naming).at("wait");
    f.runtime->runConversations();
    check(f.lastTurn()[0]["result"]["ok"] == false && f.task(naming).at("wait") == reopened &&
          f.event(naming,"user_answer")["text"] == "別の名前","a message answered a question that opened later");
}

void answersNeedTheirOwnReplyTargetAndAStableTask() {
    Fixture f;
    const auto color = f.store->submit(f.routing("色を決める"));
    f.model.questions.emplace(color,Json{{"question","どの色にしますか？"},{"choices",{"青","赤"}}});
    f.reach(color,"waiting");
    const auto wait = f.task(color)["wait"];
    f.script("どちらだっけ",{say("青か赤です。")});
    const auto explanation = f.response(f.receive("ask-back","どちらだっけ",wait.at("messageId").get<Id>()));
    check(f.model.turnInputs.back().event["replyTo"]["choices"] == Json::array({"青","赤"}),
          "a reply to a question did not show the question's choices");
    f.script("hello",{say("Hi.")});
    const auto greeting = f.receive("hello","hello");
    f.script("yes",{answer(color)});
    f.receive("unrelated-yes","yes",f.response(greeting)["messageId"].get<Id>());
    check(!listed(f.model.turnInputs.back(),color).at("wait").at("answerable").get<bool>() &&
          f.lastTurn()[0]["result"]["ok"] == false && f.task(color)["wait"] == wait,
          "a reply to an unrelated message answered a task question");
    const auto foreign = f.store->receive(localOwner,f.routing("","beta"),"foreign-greeting","hello",wait.at("messageId"));
    f.runtime->runConversations();
    f.receive("foreign-yes","yes",f.response(foreign,"beta").at("messageId"));
    check(f.lastTurn()[0]["result"]["ok"] == false && f.task(color)["wait"] == wait,
          "a reply in another Bot's conversation gave access to this question");
    f.script("青。完了条件も色見本を付けることに",{change(color,{{"doneWhen","Attach a color sample"}}),answer(color)});
    f.receive("change-then-answer","青。完了条件も色見本を付けることに");
    check(f.lastTurn()[0]["result"]["ok"] == true && f.lastTurn()[1]["result"]["ok"] == false &&
          f.event(color,"user_answer").is_null(),"an answer that replanning would discard was reported as answered");
    bool rejected = false;
    try { f.store->operate(localOwner,TaskTarget::from(f.task(color),wait.at("id")),"answer",{{"text","青"}}); }
    catch (const Conflict&) { rejected = true; }
    check(rejected && f.task(color).at("wait") == wait && f.event(color,"user_answer").is_null(),
          "a direct answer was accepted for a question that replanning will discard");
    f.advanceUntil([&] {
        const auto current = f.task(color).at("wait");
        return current.is_object() && current.at("id") != wait.at("id");
    },"the revised task did not ask its new question");
    f.receive("old-explanation","yes",explanation.at("messageId"));
    f.receive("old-question","yes",wait.at("messageId"));
    check(f.task(color).at("state") == "waiting" && f.event(color,"user_answer").is_null(),
          "an old question or its explanation answered the replacement question");
    const auto replacement = f.task(color).at("wait");
    f.script("青",{answer(color)});
    const auto stale = f.receive("old-choice","青",explanation.at("messageId"));
    check(f.lastTurn()[0]["result"]["ok"] == false && f.response(stale).at("actions").empty() && f.task(color).at("wait") == replacement &&
          f.event(color,"user_answer").is_null() && f.event(color,"cancel_requested").is_null(),
          "an exact choice for a stale question changed the replacement task");
    f.model.beforeTurn = [&](const ModelInput& input) {
        if (input.event.at("turn").empty()) f.store->operate(localOwner,TaskTarget::from(f.task(color),replacement.at("id")),"cancel");
    };
    f.receive("answer-after-cancel","yes",replacement.at("messageId"));
    check(f.lastTurn()[0]["result"]["ok"] == false && f.event(color,"user_answer").is_null(),
          "an in-flight reply recorded an answer after cancellation");
}

void exactOfferedAnswersUseTheModel() {
    for (const bool rewriteAnswer : {true,false}) {
        Fixture f;
        const auto id = f.store->submit(f.routing("Decide whether the task should continue"));
        f.model.questions.emplace(id,Json{{"question","Should I keep going?"},
                                          {"choices",{"Cancel this task","Keep going"}}});
        f.reach(id,"waiting");
        const auto wait = f.task(id).at("wait");
        constexpr auto choice = "Cancel this task";
        f.script("What are the choices?",{say("Cancel this task or Keep going.")});
        f.receive("ask-choices","What are the choices?",wait.at("messageId"));
        const auto tools = f.model.turnInputs.front().tools;
        const std::string expected = rewriteAnswer ? "Keep going" : choice;
        const auto firstChoiceCall = f.model.turnInputs.size();
        f.script(choice,{[id,expected](const ModelInput& input) {
            auto call = answer(id)(input);
            call.args["text"] = expected;
            return call;
        },say("Thanks for clarifying.")});
        const auto receipt = f.receive("cancel-choice",choice,wait.at("messageId"));
        const auto response = f.response(receipt);
        const auto recorded = f.event(id,"user_answer");
        const auto& input = f.model.turnInputs.at(firstChoiceCall);
        check(input.tools == tools && input.event.at("replyTo").at("choices") == Json::array({choice,"Keep going"}) &&
              response.at("text") == "Thanks for clarifying.",
              "the exact choice did not get the normal tools, question context, and model reply");
        const auto decisions = f.store->decisionHistory(localOwner,"alpha").size();
        check(response.at("actions") == Json::array({{{"tool","answer_task_question"},{"effect","answered"},{"taskId",id}}}) &&
              recorded.at("text") == expected && recorded.at("messageId") == receipt.at("messageId") &&
              recorded.at("waitId") == wait.at("id") && f.task(id).at("wait").is_null() &&
              f.event(id,"cancel_requested").is_null() && f.store->tasks("alpha").size() == 1,
              "the model answer was not recorded on the open question");

        f.restart();
        check(f.receive("cancel-choice",choice,wait.at("messageId")) == receipt && f.response(receipt) == response &&
              f.store->decisionHistory(localOwner,"alpha").size() == decisions,
              "retrying an exact choice recorded a second decision");
        f.reach(id,"running");
        check(f.t3.sent.size() == 1 && f.t3.sent.front().instruction.find(expected) != std::string::npos,
              "the next Session did not receive the recorded choice");
    }
}

void latestSuggestionsTakePrecedenceOverAnInheritedQuestion() {
    Fixture f;
    const auto id = f.store->submit(f.routing("Publish the draft only with approval"));
    f.model.questions.emplace(id,Json{{"question","Publish the draft?"},{"choices",{"Yes","No"}}});
    f.reach(id,"waiting");
    const auto wait = f.task(id).at("wait");
    f.script("Can I see a preview first?",{[](const ModelInput&) {
        return Decision{"reply",{{"text","Show a preview?"},{"suggestions",{"Yes","No"}}},"Offer a preview"};
    }});
    const auto preview = f.response(f.receive("request-preview","Can I see a preview first?",wait.at("messageId")));
    f.script("Yes",{answer(id),say("Here is a preview.")});
    const auto reply = f.response(f.receive("accept-preview","Yes",preview.at("messageId")));
    check(reply.at("text") == "Here is a preview." && f.task(id).at("wait") == wait &&
          f.event(id,"user_answer").is_null(),"a newer suggestion answered an older question");
}

void attachedChoiceCanBeInspectedBeforeAnswering() {
    Fixture f;
    const auto id = f.store->submit(f.routing("Check the error"));
    constexpr auto choice = "Cancel this task";
    f.model.questions.emplace(id,Json{{"question","What should I do if the error is present?"},
                                      {"choices",{choice,"Keep going"}}});
    f.reach(id,"waiting");
    const auto wait = f.task(id).at("wait");
    f.script(choice,{
        [](const ModelInput& input) {
            return Decision{"inspect_attachment",{{"messageId",input.event.at("messageId")},
                {"attachmentId",input.event.at("attachments").at(0).at("attachmentId")}},"Inspect the attached screenshot"};
        },
        [id](const ModelInput& input) {
            auto call = answer(id)(input);
            call.args["text"] = "Keep going";
            return call;
        },
        say("I recorded your answer.")
    });
    const auto receipt = f.store->receive(localOwner,f.routing(),"attached-choice",choice,wait.at("messageId"),
        Json::array({{{"name","screenshot.png"},{"mimeType","image/png"},{"dataBase64",
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"}}}));
    f.runtime->runConversations();
    const auto steps = f.lastTurn();
    check(steps.at(0).at("result").at("data").at("width") == 1,
          "the conversation could not inspect the attachment");
    const auto recorded = f.event(id,"user_answer");
    check(recorded.at("text") == "Keep going" && recorded.at("messageId") == receipt.at("messageId") &&
          recorded.at("waitId") == wait.at("id") && f.response(receipt).at("text") == "I recorded your answer.",
          "the conversation lost the model answer or reply");
}

void resumedChoicesRecoverSavedSessionCalls() {
    for (const bool started : {false,true}) {
        Fixture f;
        const auto questionTask = f.store->submit(f.routing("Decide what happens next"));
        f.model.questions.emplace(questionTask,Json{{"question","What next?"},{"choices",{"Interrupt","Continue"}}});
        f.reach(questionTask,"waiting");
        const auto other = f.store->submit(f.routing("Independent work","alpha","desktop","other-project"));
        f.reach(other,"running");
        const auto receipt = f.enqueue("resume-saved-choice","Interrupt",f.task(questionTask).at("wait").at("messageId"));
        const Id id = receipt.at("messageId");

        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open(f.store->path().c_str(),&raw);
        const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
        check(opened == SQLITE_OK,"could not open the saved turn database");
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(db.get(),"INSERT INTO conversation_steps(message_id,step,decision,effect) VALUES(?,1,?,?)",
                                 -1,&statement,nullptr) == SQLITE_OK,"could not prepare the saved tool call");
        const std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(statement,sqlite3_finalize);
        const auto decision = Json::array({{{"tool","interrupt_session"},{"reason","Previously selected operation"},
                                           {"args",{{"taskId",other},{"generation",f.task(other).at("generation")}}}}}).dump();
        const auto effect = Json{{"taskId",other}}.dump();
        check(sqlite3_bind_int64(row.get(),1,id) == SQLITE_OK &&
              sqlite3_bind_text(row.get(),2,decision.c_str(),-1,SQLITE_TRANSIENT) == SQLITE_OK &&
              (started ? sqlite3_bind_text(row.get(),3,effect.c_str(),-1,SQLITE_TRANSIENT)
                       : sqlite3_bind_null(row.get(),3)) == SQLITE_OK &&
              sqlite3_step(row.get()) == SQLITE_DONE,"could not save the decision and effect");

        f.script("Interrupt",{interrupt(other),answer(questionTask),say("Answer recorded.")});
        f.restart();
        f.runtime->runConversations();
        const auto previous = f.lastTurn().at(0).at("result");
        if (started)
            check(previous.at("ok") == false && previous.at("delivery") == "unknown" && f.t3.interrupted.empty(),
                  "the started Session operation was dispatched again or lost its recovery result");
        else
            check(previous.at("ok") == true && f.t3.interrupted.size() == 1,
                  "the saved Session operation did not dispatch through the normal tool loop");
        check(f.event(questionTask,"user_answer").at("text") == "Interrupt" && f.task(other).at("state") == "running",
              "resuming a saved call lost the anchored answer or changed another Task");
    }
}

void directChoiceRechecksWebAuthority() {
    Fixture f;
    const auto task = f.store->submit(f.routing("Choose a name"));
    f.model.questions.emplace(task,Json{{"question","Which name?"},{"choices",{"Maple","Juniper"}}});
    f.reach(task,"waiting");
    const auto wait = f.task(task).at("wait");
    const auto receipt = f.store->receiveWeb({"web-root",true},f.routing(),"queued-web-choice","Maple",wait.at("messageId"));
    int lookups = 0;
    f.store->setWebIdentityResolver([&](std::string_view person) -> std::optional<bool> {
        if (person == "web-root" && ++lookups == 1) return true;
        return std::nullopt;
    });
    f.runtime->runConversations();
    check(lookups == 2 && f.response(receipt).at("route") == "access" && f.model.turnInputs.empty() &&
          f.event(task,"user_answer").is_null() && f.task(task).at("wait") == wait,
          "a queued choice used its admitted Web root after that authority was revoked");
}

void directAnswersRemainInConversationHistory() {
    Fixture f;
    const auto id = f.store->submit(f.routing("Choose a name"));
    f.model.questions.emplace(id,Json{{"question","Which name?"},{"choices",{"Maple","Juniper"}}});
    f.reach(id,"waiting");
    const auto wait = f.task(id).at("wait");
    f.store->operate(localOwner,TaskTarget::from(f.task(id),wait.at("id")),"answer",{{"text","Maple"}});
    f.script("Which name did I choose?",{say("Maple")});
    f.receive("remember-name","Which name did I choose?");
    const auto& history = f.model.turnInputs.back().history;
    check(std::ranges::any_of(history,[&](const Json& entry) {
        return entry.value("kind",std::string{}) == "user_answer" && entry.at("role") == "user" &&
               entry.at("taskId") == id && entry.at("waitId") == wait.at("id") && entry.at("text") == "Maple";
    }),"the conversation lost the selected answer or its question reference");
}

void queuedTurnsSeeEarlierReplies() {
    Fixture f;
    f.script("Recommend a plan",{say("Use plan Maple")});
    f.script("Explain your recommendation",{say("Maple needs fewer resources")});
    const auto first = f.enqueue("recommend","Recommend a plan");
    const auto second = f.enqueue("explain-recommendation","Explain your recommendation");
    f.runtime->runConversations();
    check(f.response(first).at("messageId") > second.at("messageId"),"the replies were not queued");
    f.runtime->runConversations();
    const auto& history = f.model.turnInputs.back().history;
    check(std::ranges::any_of(history,[](const Json& entry) {
        return entry.at("role") == "bot" && entry.at("text") == "Use plan Maple";
    }),"the queued follow-up lost the reply to the earlier request");
}

void destinationClarificationContinuesFromConversationHistory() {
    Fixture f;
    constexpr auto request = "Run the release audit";
    f.script(request,{say("Which environment should I use?")});
    const auto first = f.receive("destination-request",request);
    const auto clarification = f.response(first);
    f.script("Use the cloud Mac",{start({{"request",request},{"environment","クラウドMac"}})});
    f.receive("destination-choice","Use the cloud Mac",clarification["messageId"].get<Id>());
    const auto& input = f.model.turnInputs.back();
    const auto hasHistoryEntry = [&](std::string_view role,std::string_view text) {
        return std::ranges::any_of(input.history,[&](const Json& entry) {
            return entry["role"].get<std::string>() == role && entry["text"].get<std::string>() == text;
        });
    };
    const auto tasks = f.store->tasks("alpha");
    check(hasHistoryEntry("user",request) && hasHistoryEntry("bot","Which environment should I use?") &&
          tasks.size() == 1 && f.task(tasks.front()["id"])["environmentId"] == "cloud",
          "a destination clarification lost its original request or selected environment");
}

void connectedEnvironmentSelectsProjectBeforeTakingItsQueueSlot() {
    Fixture f;
    T3Connections(*f.store).connect("desktop",localOwner.id,48123);
    f.t3.projectsByEnvironment["desktop"] = {{"docs","Docs","docs"},{"release","Release","release"}};
    const auto held = f.store->submit(f.routing("Keep the release project occupied","beta","desktop","release"));
    f.reach(held,"running");

    const Intake automatic{"alpha","","",""};
    const auto request = "Check the build";
    f.script(request,{start()});
    f.store->receive(localOwner,automatic,"new-project",request);
    f.runtime->runConversations();
    const Id task = f.store->tasks("alpha").front().at("id");
    f.reach(task,"waiting");
    auto waiting = f.task(task);
    check(waiting.at("environmentId") == "desktop" && waiting.at("projectId") == "" &&
          waiting.at("wait").at("choices") == Json::array({"Docs","Release"}) && f.t3.sent.size() == 1,
          "first work on the connected environment did not wait for a named project");
    const auto target = TaskTarget::from(waiting,waiting.at("wait").at("id"));
    bool invalid = false;
    try { f.store->operate(localOwner,target,"answer",{{"text","Arbitrary project"}}); }
    catch (const Conflict&) { invalid = true; }
    check(invalid && f.task(task).at("state") == "waiting","an answer selected an unlisted project");

    f.restart();
    f.store->operate(localOwner,target,"answer",{{"text","Release"}});
    f.tick(3);
    check(f.task(task).at("state") == "queued" && f.task(task).at("projectId") == "release" &&
          f.task(task).at("blockedBy").at("id") == held && f.t3.sent.size() == 1,
          "selecting a project bypassed its occupied execution slot");
    f.t3.finish("run-1","Release check complete");
    f.reach(task,"running");
    check(f.t3.sent.back().projectId == "release","first launch did not use the selected project");
    f.t3.finish("run-2","Build checked");
    f.reach(task,"completed");

    const auto next = f.store->submit({"alpha","Verify the fix","desktop",""});
    f.reach(next,"running");
    check(f.t3.sent.back().projectId == "release","a later task did not use the remembered project");

    T3Connections(*f.store).connect("cloud",localOwner.id,48123);
    f.t3.projectsByEnvironment["cloud"] = {{"cloud-project","Cloud only","cloud"}};
    const auto remote = f.store->submit({"alpha","Verify remotely","cloud",""});
    f.reach(remote,"running");
    check(f.t3.sent.back().environmentId == "cloud" && f.t3.sent.back().projectId == "cloud-project",
          "the remembered project crossed environments or the sole project needed an answer");
}

void unansweredProjectChoiceDoesNotBlockAnotherBot() {
    Fixture f;
    f.t3.projectsByEnvironment["desktop"] = {{"docs","Docs",{}},{"release","Release",{}}};
    const auto first = f.store->submit({"alpha","Choose a project first","desktop",""});
    f.reach(first,"waiting");
    const auto second = f.store->submit({"beta","Check the documentation","desktop",""});
    check(f.task(second)["blockedBy"].is_null(),"a projectless task appeared to hold an execution slot");
    f.reach(second,"waiting");
    const auto choice = f.task(second);
    check(choice["wait"]["choices"] == Json::array({"Docs","Release"}),"the second Bot did not reach project selection");
    f.store->operate(localOwner,TaskTarget::from(choice,choice["wait"]["id"]),"answer",{{"text","Docs"}});
    f.reach(second,"running");
    check(f.task(first)["state"] == "waiting" && f.t3.sent.size() == 1 &&
          f.t3.sent.front().projectId == "docs","an independent project could not start before the first Bot's answer");
}

void revisingWorkKeepsTheProjectChoice() {
    Fixture f;
    f.t3.projectsByEnvironment["desktop"] = {{"docs","Docs",{}},{"release","Release",{}}};
    const auto id = f.store->submit(f.routing("Check the release","alpha","desktop",""));
    f.reach(id,"waiting");
    const auto original = f.task(id);
    constexpr auto revision = "Include the test results";
    f.script(revision,{change(id,{{"doneWhen",revision}})});
    f.receive("revise-before-project",revision);
    f.advanceUntil([&] {
        const auto task = f.task(id);
        return task["state"] == "waiting" && task["wait"]["id"] != original["wait"]["id"] &&
            task["wait"].contains("choices");
    },"revising the task did not open a new project choice");
    const auto revised = f.task(id);
    check(revised["wait"]["choices"] == Json::array({"Docs","Release"}) && f.t3.sent.empty(),
          "revising the task bypassed its project choice");
    f.store->operate(localOwner,TaskTarget::from(revised,revised["wait"]["id"]),"answer",{{"text","Release"}});
    f.reach(id,"running");
    check(f.t3.sent.size() == 1 && f.t3.sent.front().projectId == "release" &&
          f.t3.sent.front().instruction.find(revision) != std::string::npos,
          "launch after project selection lost the revised criteria");
}

void suggestionsStayWithTheBotMessage() {
    Fixture f;
    const auto suggest = [](Json choices) {
        return Step([choices](const ModelInput&) {
            return Decision{"reply",{{"text","Which one?"},{"suggestions",choices}},"Offer choices"};
        });
    };
    f.script("Pick a name for one",{suggest({"Juniper","Maple"})});
    const auto first = f.receive("names-a","Pick a name for one");
    f.script("Pick a name for another",{suggest({"Maple","Juniper"})});
    const auto second = f.receive("names-b","Pick a name for another");
    f.script("2番目",{say("OK.")});
    f.receive("second-a","2番目",f.response(first)["messageId"].get<Id>());
    const auto afterFirst = f.model.turnInputs.back();
    f.receive("second-b","2番目",f.response(second)["messageId"].get<Id>());
    const auto afterSecond = f.model.turnInputs.back();
    const auto botEntry = [](const ModelInput& input, std::string_view text) {
        for (const auto& entry : input.history)
            if (entry["role"] == "bot" && entry["text"] == text) return entry;
        return Json(nullptr);
    };
    check(afterFirst.event["replyTo"]["suggestions"] == Json::array({"Juniper","Maple"}) &&
          afterSecond.event["replyTo"]["suggestions"] == Json::array({"Maple","Juniper"}) &&
          afterFirst.event["replyTo"]["inReplyTo"]["text"] == "Pick a name for one" &&
          botEntry(afterSecond,"Which one?")["suggestions"].is_array(),
          "a reply to a Bot message lost the order of the suggestions it offered");

    f.script("Pick a login fix",{suggest({"Fix login","Fix login"}),say("Which login fix: web or mobile?")});
    const auto duplicate = f.receive("duplicate","Pick a login fix");
    check(f.lastTurn()[0]["result"]["ok"] == false && !f.response(duplicate).contains("suggestions"),
          "duplicate suggestions were offered as separate choices");
}

void pausedTurnDoesNotHoldCompletion() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    auto limited = f.routing();
    limited.config["modelCallLimit"] = 1;
    std::string seen;
    Json latestResult;
    bool reportInHistory = false;
    bool foundResult = false, readResult = false;
    f.script("Check what is running, then change the criteria",{
        [](const ModelInput&) { return Decision{"find_tasks",Json::object(),"Look first"}; },
        [&](const ModelInput& input) {
            for (const auto& task : input.task.at("tasks"))
                if (task.at("taskId") == id) {
                    seen = task.at("state");
                    if (task.contains("latestResult")) latestResult = task.at("latestResult");
                }
            reportInHistory = std::ranges::any_of(input.history,[&](const Json& entry) {
                return entry.value("taskId",Id{0}) == id && entry.value("kind",std::string{}) == "report" &&
                       entry.value("text",std::string{}) == "Timing evidence";
            });
            return Decision{"find_tasks",Json::object(),"Find the current result"};
        },
        [&](const ModelInput& input) {
            const auto& found = input.event.at("turn").back().at("result").at("tasks");
            for (const auto& task : found)
                if (task.at("taskId") == id)
                    foundResult = task.at("latestResult").at("messageId") == latestResult.at("messageId");
            return Decision{"read_result",{{"messageId",latestResult.at("messageId")}},"Read the final report"};
        },
        [&](const ModelInput& input) {
            readResult = input.event.at("turn").back().at("result").at("text") == "Timing evidence";
            return Decision{"reply",{{"text","It already finished."}},"Report the current state"};
        }});
    const auto message = f.store->receive(localOwner,limited,"paused","Check what is running, then change the criteria");
    f.tick();
    f.tick();
    check(f.store->pausedConversations("alpha").size() == 1,
          "the conversation did not pause for model approval");
    f.t3.finish("run-1","Timing evidence");
    f.reach(id,"completed");
    Id reportId = 0;
    for (const auto& row : f.store->history(id))
        if (row.at("kind") == "completed") reportId = row.at("id").get<Id>();
    check(reportId > message.at("messageId").get<Id>(),"the fixture report did not arrive after the conversation message");
    for (int call = 0; call < 3; ++call) {
        const auto paused = f.store->pausedConversations("alpha").at(0);
        const Id pause = paused.at("messageId");
        f.store->decideInbox(localOwner,{"alpha",InboxKind::conversation_model_budget,paused.at("replyToMessageId"),pause,0,0,pause},"approve");
        f.tick();
    }
    check(f.store->pausedConversations("alpha").empty() && seen == "completed" &&
          latestResult.value("messageId",Id{0}) == reportId && latestResult.value("kind",std::string{}) == "completed" &&
          latestResult.value("excerpt",std::string{}) == "Timing evidence" && reportInHistory && foundResult && readResult,
          "the approved turn did not see the task's latest report");
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
    const auto source = lastTaskEvent(*f.store,old,"report").at("id");
    f.store->operate(localOwner,TaskTarget::from(f.store->task(old),0),"cancel");
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
    const std::string prefix = "Record all timing samples: ";
    const std::string constraint = "Keep the raw timing file.";
    const auto pending = prefix + std::string(9000 - prefix.size() - constraint.size(),'x') + constraint;
    f.script("Add timing requirements",{send(old,{{"instruction",pending}})});
    f.receive("pending-direction","Add timing requirements");
    check(f.task(old).at("pendingInstructions").back().at("text") == pending,
          "the full instruction was not pending at cancellation");
    f.store->operate(localOwner,TaskTarget::from(f.store->task(old),0),"cancel");
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
    const auto position = instruction.find(pending);
    check(position != instruction.npos && instruction.find(prefix) == position &&
          instruction.find(prefix,position + 1) == instruction.npos,
          "the resumed Session did not receive the full pending instruction exactly once");
    check(f.task(next)["doneWhen"] == f.task(old)["doneWhen"] && f.task(next)["sourceTaskId"] == old && next != old &&
          f.task(next)["environmentId"] == "cloud" &&
          instruction.find("Measure the reference dataset") != std::string::npos &&
          instruction.find("Report baseline latency") != std::string::npos &&
          instruction.find("Use a pinned regression fixture") != std::string::npos &&
          f.t3.sent.back().tool == "delegate",
          "resume omitted the original intent, criteria, or accepted directions");

    constexpr auto goal = "Measure the release dataset";
    constexpr auto criteria = "Report median throughput";
    constexpr auto trace = "attach the raw trace";
    constexpr auto revisionRequest = "Move this run to ローカルMac, measure the release dataset, "
                                     "report median throughput, and attach the raw trace";
    f.script(revisionRequest,{change(next,{{"intent",goal},{"doneWhen",criteria},{"environment","ローカルMac"}})});
    f.receive("revise-resumed",revisionRequest);
    check(f.task(next)["pendingDestination"]["environmentId"] == "desktop" && f.task(next)["intent"] == goal &&
          f.task(next)["doneWhen"] == criteria,"the combined move and work revision was not accepted");
    constexpr auto abandonedMove = "Use ローカルMac for the next run";
    f.script(abandonedMove,{change(next,{{"environment","ローカルMac"}})});
    f.receive("old-destination",abandonedMove);
    constexpr auto correctedCriteria = "Report p95 latency";
    constexpr auto correction = "Change the completion criteria to report p95 latency";
    f.script(correction,{change(next,{{"doneWhen",correctedCriteria}})});
    f.receive("correct-criteria",correction);
    f.store->operate(localOwner,TaskTarget::from(f.task(next),0),"cancel");
    f.t3.finish(f.task(next)["activeRun"]["runId"].get<std::string>(),"Resumed work settled");
    f.reach(next,"cancelled");
    Id anchor = 0;
    for (const auto& row : f.store->history(next))
        if (row["kind"] == "cancelled") anchor = row["id"];
    const auto secondRequest = std::string("Resume the measurement on クラウドMac and keep the flamegraph ") +
                               std::string(9000,'r');
    f.script(secondRequest,{start({{"request",secondRequest},{"sourceResultMessageId",anchor},
                                  {"resume",true},{"environment","クラウドMac"}})});
    f.receive("resume-again",secondRequest,anchor);
    const auto last = f.store->tasks("alpha").front().at("id").get<Id>();
    check(last != next && f.task(last)["sourceTaskId"] == next,"the second resume did not create fresh work");
    f.reach(last,"running");
    check(f.task(last)["intent"] == goal && f.task(last)["doneWhen"] == correctedCriteria &&
          f.task(last)["environmentId"] == "cloud","the second resume lost its current work contract");
    const auto& resumed = f.t3.sent.back().instruction;
    check(resumed.find(trace) != resumed.npos,"the second resume lost the accepted trace direction");
    check(f.t3.sent.back().environmentId == "cloud",
          "the second resume did not use its current destination");
    check(resumed.find(revisionRequest) != resumed.npos,"the second resume lost the revised mixed direction");
    check(resumed.find("Report baseline latency") == resumed.npos,"the second resume replayed superseded criteria");
    check(f.task(last)["remaining"] == 3,"the second resume charged for old work");

    constexpr auto localNote = "Record the CPU model";
    f.script(localNote,{send(last)});
    f.receive("cpu-model",localNote);
    f.t3.finish(f.task(last)["activeRun"]["runId"].get<std::string>(),"Measurement ready");
    f.advanceUntil([&] { return f.t3.sent.back().tool == "follow_up"; },
                   "the supplement never reached the Session");
    const auto& follow = f.t3.sent.back().instruction;
    check(follow.find(localNote) != follow.npos && follow.find("Measure the reference dataset") == follow.npos,
          "the follow-up omitted or replayed directions");

    constexpr auto move = "Continue the measurement on ローカルMac";
    f.script(move,{change(last,{{"environment","ローカルMac"}})});
    f.receive("move-resumed",move);
    f.t3.finish(f.task(last)["activeRun"]["runId"].get<std::string>(),"Cloud work settled");
    f.advanceUntil([&] { return f.t3.sent.back().environmentId == "desktop"; },
                   "the resumed task did not move");
    const auto& moved = f.t3.sent.back().instruction;
    check(moved.find(localNote) != moved.npos && moved.find(localNote) == moved.rfind(localNote) &&
          moved.find(trace) != moved.npos &&
          sessionPrompt(f.t3.sent.back().actionId,moved).size() <= maxSessionPromptBytes &&
          f.task(last)["remaining"] == 1,
          "the moved Session lost an applicable direction or spent extra allowance");
}

void liveMoveAndCriteriaCorrectionKeepCurrentContract() {
    Fixture f;
    const auto id = f.store->submit(f.routing("Measure the reference dataset","alpha","cloud"));
    f.reach(id,"running");
    constexpr auto goal = "Measure the release dataset";
    constexpr auto originalCriteria = "Report median throughput";
    constexpr auto correctedCriteria = "Report p95 latency";
    constexpr auto trace = "attach the raw trace";
    constexpr auto mixed = "Move this run to ローカルMac, measure the release dataset, report median throughput, "
                           "and attach the raw trace";
    f.script(mixed,{change(id,{{"intent",goal},{"doneWhen",originalCriteria},{"environment","ローカルMac"}})});
    f.receive("live-move-and-contract",mixed);
    constexpr auto correction = "Change the completion criteria to report p95 latency";
    f.script(correction,{change(id,{{"doneWhen",correctedCriteria}})});
    f.receive("live-criteria-correction",correction);
    check(f.task(id)["environmentId"] == "cloud" && f.task(id)["pendingDestination"]["environmentId"] == "desktop" &&
          f.task(id)["intent"] == goal && f.task(id)["doneWhen"] == correctedCriteria &&
          f.t3.sent.size() == 1,
          "the live route and criteria changes were not saved before delivery");

    f.t3.finish("run-1","Previous cloud work settled");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the live move was not delivered");
    const auto& command = f.t3.sent.back();
    const auto& instruction = command.instruction;
    check(command.tool == "delegate" && command.environmentId == "desktop" &&
          f.task(id)["environmentId"] == "desktop" && f.task(id)["intent"] == goal &&
          f.task(id)["doneWhen"] == correctedCriteria,
          "the live replan started with the wrong destination or work contract");
    check(instruction.find(trace) != instruction.npos && instruction.find(trace) == instruction.rfind(trace),
          "the live replan dropped or duplicated the trace direction");
    const auto intentHeading = instruction.find("Task intent:\n");
    const auto criteriaHeading = instruction.find("Completion criteria:\n");
    const auto intentValue = instruction.find(goal,intentHeading);
    const auto criteriaValue = instruction.find(correctedCriteria,criteriaHeading);
    check(intentHeading != instruction.npos && intentHeading == instruction.rfind("Task intent:\n") &&
          intentValue != instruction.npos && intentValue == instruction.rfind(goal) &&
          criteriaHeading != instruction.npos && criteriaHeading == instruction.rfind("Completion criteria:\n") &&
          criteriaValue != instruction.npos && criteriaValue == instruction.rfind(correctedCriteria),
          "the live replan did not state the current task intent and criteria once");
}

void environmentOnlyMoveKeepsWorkAcrossResume() {
    Fixture f;
    const std::string goal = "Measure " + std::string(4992,'g');
    const std::string criteria = "Complete " + std::string(3991,'c');
    constexpr auto create = "Start a large checksum measurement";
    f.script(create,{start({{"request",goal},{"doneWhen",criteria},{"environment","クラウドMac"}})});
    f.receive("large-contract",create);
    const auto original = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(original,"running");

    constexpr auto checksum = "sha256:8ed3f6ad685b959ead7022518e1af76cd816f8e8ec7ccdda1ed4018e8f2223f8";
    constexpr auto trace = "trace:latency-samples-42";
    const auto direction = std::string("Move this run to ローカルMac and include checksum ") + checksum +
                           " and attach trace " + trace;
    f.script(direction,{change(original,{{"environment","ローカルMac"}})});
    f.receive("move-with-checksum",direction);
    check(f.task(original)["pendingDestination"]["environmentId"] == "desktop",
          "the fixture did not create an environment-only move");

    f.t3.finish("run-1","Cloud work settled");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the move was not delivered to its new Session");
    const auto& moved = f.t3.sent.back();
    check(moved.environmentId == "desktop" && moved.instruction.find(checksum) != moved.instruction.npos &&
          moved.instruction.find(checksum) == moved.instruction.rfind(checksum) &&
          moved.instruction.find(trace) != moved.instruction.npos &&
          moved.instruction.find(trace) == moved.instruction.rfind(trace),
          "the move direction was not delivered to the new destination exactly once");
    f.store->operate(localOwner,TaskTarget::from(f.task(original),0),"cancel");
    f.t3.finish(f.task(original)["activeRun"]["runId"].get<std::string>(),"Desktop work settled");
    f.reach(original,"cancelled");

    const auto cancellation = lastTaskEvent(*f.store,original,"report").at("id");
    constexpr auto request = "Resume checksum validation on クラウドMac";
    f.script(request,{start({{"request",request},{"sourceResultMessageId",cancellation},{"resume",true},
                            {"environment","クラウドMac"}})});
    f.receive("resume-checksum",request,cancellation.get<Id>());
    const auto resumed = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(resumed,"running");
    const auto& command = f.t3.sent.back();
    const auto& instruction = command.instruction;
    check(command.environmentId == "cloud" && f.task(resumed)["environmentId"] == "cloud" &&
          f.task(resumed)["intent"] == goal && f.task(resumed)["doneWhen"] == criteria,
          "the resumed task did not use its current destination");
    check(instruction.find(checksum) != instruction.npos && instruction.find(checksum) == instruction.rfind(checksum) &&
          instruction.find(trace) != instruction.npos && instruction.find(trace) == instruction.rfind(trace),
          "the resumed Session dropped or duplicated the checksum or trace direction");
    const auto intentHeading = instruction.find("Task intent:\n");
    const auto criteriaHeading = instruction.find("Completion criteria:\n");
    const auto intentValue = instruction.find(goal);
    const auto criteriaValue = instruction.find(criteria);
    check(intentHeading != instruction.npos && intentHeading == instruction.rfind("Task intent:\n") &&
          intentValue != instruction.npos && intentValue == instruction.rfind(goal) && intentValue > intentHeading &&
          criteriaHeading != instruction.npos && criteriaHeading == instruction.rfind("Completion criteria:\n") &&
          criteriaValue != instruction.npos && criteriaValue == instruction.rfind(criteria) && criteriaValue > criteriaHeading,
          "the resumed prompt duplicated or lost its large task contract");
    check(sessionPrompt(command.actionId,instruction).size() <= maxSessionPromptBytes,
          "the resumed prompt exceeded the Session prompt cap");
}

void environmentOnlyMoveCancelledBeforeDeliveryKeepsWorkAcrossResume() {
    Fixture f;
    const auto original = f.store->submit(f.routing("Measure the checksum target","alpha","cloud"));
    f.reach(original,"running");

    constexpr auto checksum = "sha256:8ed3f6ad685b959ead7022518e1af76cd816f8e8ec7ccdda1ed4018e8f2223f8";
    constexpr auto direction = "Move this run to ローカルMac and include checksum "
                               "sha256:8ed3f6ad685b959ead7022518e1af76cd816f8e8ec7ccdda1ed4018e8f2223f8";
    f.script(direction,{change(original,{{"environment","ローカルMac"}})});
    f.receive("cancel-before-move-delivery",direction);
    check(f.task(original)["pendingDestination"]["environmentId"] == "desktop",
          "the fixture did not create an environment-only move");
    const auto activeRun = f.task(original)["activeRun"]["runId"].get<std::string>();
    f.store->operate(localOwner,TaskTarget::from(f.task(original),0),"cancel");
    f.t3.finish(activeRun,"Cloud work cancelled before the move was delivered");
    f.reach(original,"cancelled");
    check(f.t3.sent.size() == 1,"the environment-only move was delivered before cancellation");

    Id cancellation = 0;
    for (const auto& row : f.store->history(original))
        if (row["kind"] == "cancelled") cancellation = row["id"];
    check(cancellation > 0,"the cancelled task had no resume anchor");
    constexpr auto request = "Resume the checksum work on クラウドMac";
    f.script(request,{start({{"request",request},{"sourceResultMessageId",cancellation},{"resume",true},
                            {"environment","クラウドMac"}})});
    f.receive("resume-before-delivery",request,cancellation);
    const auto resumed = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(resumed,"running");
    const auto& command = f.t3.sent.back();
    const auto& instruction = command.instruction;
    check(command.environmentId == "cloud" && f.task(resumed)["environmentId"] == "cloud",
          "the resumed task did not use its current destination");
    check(instruction.find(checksum) != instruction.npos && instruction.find(checksum) == instruction.rfind(checksum),
          "the cancelled pending direction was dropped or duplicated");
}

void resumeBatchesOversizedPendingBacklog() {
    Fixture f;
    const auto old = f.store->submit(f.routing(std::string(16000,'i')));
    f.script("Set criteria",{change(old,{{"doneWhen",std::string(8000,'c')},{"instruction",std::string(8000,'d')}})});
    f.receive("criteria","Set criteria");
    f.reach(old,"running");

    std::vector<std::string> specs;
    for (int i = 1; i <= 9; ++i) {
        const auto marker = "SPEC-" + std::to_string(i);
        specs.push_back(marker + std::string(14000,'x') + "END-" + std::to_string(i));
        f.script(marker,{send(old,{{"instruction",specs.back()}})});
        f.receive(marker,marker);
    }
    f.store->operate(localOwner,TaskTarget::from(f.task(old),0),"cancel");
    f.t3.finish("run-1","Cancelled work settled");
    f.reach(old,"cancelled");
    Id anchor = 0;
    for (const auto& row : f.store->history(old))
        if (row["kind"] == "cancelled") anchor = row.at("id");
    f.script("Resume",{start({{"request",std::string(16000,'r')},{"sourceResultMessageId",anchor},{"resume",true}})});
    f.receive("resume","Resume",anchor);
    const auto next = f.store->tasks("alpha").front().at("id").get<Id>();
    std::vector<Json> shown;
    const auto recordBatch = [&](const ModelInput& input) {
        if (!input.task.contains("userInstructions")) return;
        shown.push_back(input.task.at("userInstructions"));
        if (shown.back().size() < input.task.at("pendingInstructionCount").get<std::size_t>())
            check(std::ranges::find(input.tools,"complete") == input.tools.end(),"completion could skip unseen instructions");
    };
    f.model.beforeIntake = recordBatch;
    f.model.beforeComplete = recordBatch;
    f.t3.loseNextSend = true;
    f.reach(next,"waiting");
    const auto inherited = f.task(next).at("pendingInstructions");
    check(inherited.size() == specs.size() && f.t3.sent.back().tool == "delegate",
          "unconfirmed delegate lost part of the inherited backlog");
    for (std::size_t i = 0; i < specs.size(); ++i)
        check(inherited[i].at("text") == specs[i],"unconfirmed delivery changed an inherited specification");

    const auto wait = f.task(next).at("wait");
    check(wait.at("reason") == "unknown_delivery","missing acknowledgement did not enter delivery recovery");
    f.store->operate(localOwner,TaskTarget::from(f.task(next),wait.at("id")),"reconcile");
    f.reach(next,"running");
    const auto remaining = f.task(next).at("pendingInstructions");
    check(!remaining.empty() && remaining.size() < inherited.size(),"delegate did not consume a bounded batch");
    f.restart();
    check(f.task(next).at("pendingInstructions") == remaining,"restart lost the outstanding batch");
    for (std::size_t batch = 0; batch < specs.size(); ++batch) {
        f.t3.finish(f.task(next).at("activeRun").at("runId").get<std::string>(),"Batch applied");
        if (f.task(next).at("pendingInstructions").empty()) break;
        f.reach(next,"deciding");
        f.reach(next,"running");
        check(f.t3.sent.back().tool == "follow_up","a subsequent batch did not continue the Session");
    }
    f.reach(next,"completed");
    check(f.task(next).value("pendingInstructions",Json::array()).empty(),"completed with inherited work pending");

    check(f.t3.sent.size() == shown.size() + 1,"expected one resumed Session command per model batch");
    for (std::size_t batch = 0; batch < shown.size(); ++batch)
        for (const auto& item : shown[batch])
            check(f.t3.sent[batch + 1].instruction.find(item.at("text").get<std::string>()) != std::string::npos,
                  "Session command differed from the batch shown to the model");
    std::string dispatched;
    for (const auto& cmd : f.t3.sent) {
        check(cmd.instruction.size() + 2 * cmd.actionId.size() + 36 <= 120000,"Session prompt exceeded T3 preflight");
        dispatched += cmd.instruction;
    }
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto pos = dispatched.find(specs[i],cursor);
        check(pos != dispatched.npos,"Session commands omitted, truncated or reordered a specification");
        cursor = pos + specs[i].size();
        for (const auto marker : {"SPEC-","END-"}) {
            const auto token = marker + std::to_string(i + 1);
            check(dispatched.find(token,dispatched.find(token) + 1) == dispatched.npos,"duplicated specification marker");
        }
    }
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

void settledInstructionsCanBeAcknowledgedWithoutAnotherRun() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.script("Confirm the existing criteria",{change(id,{{"doneWhen","Report baseline latency"}})});
    const auto receipt = f.receive("same-criteria","Confirm the existing criteria");
    check(!f.task(id).value("replanning",false) && f.task(id)["revisions"].size() == 1,
          "repeating a criterion triggered replanning");
    f.t3.finish("run-1","Baseline latency is 10 ms");
    int decisions = 0;
    f.model.onResult = [&](const ModelInput& input) {
        const auto& item = input.task.at("userInstructions").at(0);
        check(item.at("messageId") == receipt.at("messageId") && input.event.at("report") == "Baseline latency is 10 ms",
              "settled decision missed the instruction identity or evidence");
        Json args = {{"report","Baseline latency is 10 ms"}};
        if (++decisions > 1)
            args["acknowledgedInstructions"] = Json::array({{{"messageId",item.at("messageId")},{"step",item.at("step")}}});
        return Decision{"complete",args,"The existing report satisfies the repeated criterion"};
    };
    f.reach(id,"completed");
    check(decisions == 2 && f.t3.sent.size() == 1 && f.event(id,"decision").at("args").contains("acknowledgedInstructions"),
          "completion ignored pending instructions or forced another Run");
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

void cancellationTakesOverRevisionStop() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    f.script("Change criteria",{change(id,{{"doneWhen","Include timings"}})});
    f.receive("revision","Change criteria");
    f.advanceUntil([&] { return !f.t3.interrupted.empty(); },"revision did not interrupt its Run");
    const auto stop = f.task(id).at("runStop");
    check(stop.at("intent") == "replan" && stop.at("run") == f.task(id).at("activeRun"),"wrong revision stop target");
    f.store->operate(localOwner,TaskTarget::from(f.task(id),0),"cancel");
    f.restart();
    f.tick();
    check(f.task(id).at("runStop").at("intent") == "cancel" && f.t3.interrupted.size() == 1,
          "cancellation issued a second interrupt before its retry time");
    f.now += 5;
    f.tick();
    check(f.t3.interrupted.size() == 2 && f.t3.interrupted[0] == f.t3.interrupted[1],"stop retry changed its Run");
    f.t3.finish("run-1","Interrupted",false);
    f.tick();
    check(f.task(id)["state"] == "cancelling" && f.task(id)["activeRun"] == stop.at("run"),
          "interrupt request released unsettled delegated work");
    f.t3.finish("run-1","Interrupted");
    f.reach(id,"cancelled");
    check(f.t3.sent.size() == 1 && !f.task(id).contains("runStop") &&
          f.event(id,"interrupt_settled")["intent"] == "cancel","settlement resumed superseded work");
}

void deliveryWaitsForEveryTarget() {
    Fixture f;
    const auto running = f.store->submit(f.routing("First measurement"));
    const auto queued = f.store->submit(f.routing("Second measurement"));
    f.reach(running,"running");
    constexpr auto text = "Include CPU time in both measurements";
    f.script(text,{send(running),send(queued)});
    const Id message = f.receive("both-measurements",text).at("messageId");
    f.t3.finish("run-1","First measurement reached a safe point");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the first direction did not reach T3");
    check(f.conversationMessage(message)["deliveredToT3"] == false,
          "the first delivery marked the whole message as sent");
    f.t3.finish(f.task(running).at("activeRun").at("runId").get<std::string>(),"First measurement complete");
    f.reach(queued,"running");
    check(f.t3.sent.back().instruction.find(text) != std::string::npos &&
          f.conversationMessage(message)["deliveredToT3"] == true,
          "the message was not marked as sent after the second delivery");
}

void unknownSendThenScopeChange() {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    f.reach(id,"sending");
    f.t3.loseNextSend = true;
    f.reach(id,"waiting");
    const auto before = f.task(id);
    f.script("Use the updated specification",{change(id,{{"intent","Measure startup latency"},{"doneWhen","p95 レイテンシを報告"}})});
    const auto change = f.receive("revision","Use the updated specification");
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
    f.store->operate(localOwner,TaskTarget::from(f.store->task(id),f.task(id)["wait"]["id"]),"reconcile");
    f.reach(id,"running");
    check(f.t3.sent.size() == 2 && f.t3.reconciled.back().actionId == replacementAction &&
          f.t3.sent.back().tool == "follow_up" &&
          f.t3.sent.back().instruction.find("Task intent:\nMeasure startup latency") != std::string::npos &&
          f.t3.sent.back().instruction.find("Completion criteria:\np95 レイテンシを報告") != std::string::npos &&
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
    const auto report = lastTaskEvent(*f.store,id,"report").at("id");
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
    ++f.now;
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

    const auto held = f.task(id);
    check(held.at("pendingDecision").at("eventId") == held.at("eventId") &&
          held.at("pendingDecision").at("generation") == held.at("generation") &&
          held.at("pendingDecision").at("tool") == "complete","deferred decision lost its task or event identity");
    f.restart();
    f.reach(id,"completed");
    check(!f.task(id).contains("pendingDecision") && f.event(id,"completed")["text"] == "Settled evidence" && completionCalls == 1,
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
    const auto later = f.store->receipt(localOwner,"alpha","arrived-later","status");
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
        writer.operate(localOwner,TaskTarget::from(writer.task(id),0),"cancel");
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
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (f.task(project)["state"] != "sending" || f.store->tasks("alpha").size() != 2))
        std::this_thread::sleep_for(5ms);
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
    f.store->receive(localOwner,routing,"compound-budget",text);
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
        try { writer.operate(localOwner,TaskTarget::from(writer.task(pausedId),waitId),"approve"); return true; }
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

void rememberCloudProject(Fixture& f) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open(f.store->path().c_str(),&raw);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    check(opened == SQLITE_OK && sqlite3_exec(db.get(),
          "INSERT INTO bot_t3_projects VALUES('alpha','cloud','cloud-project')",nullptr,nullptr,nullptr) == SQLITE_OK,
          "could not save the previously selected cloud project");
    f.t3.projectsByEnvironment["cloud"] = {{"cloud-project","Cloud fixture",{}}};
}

void canonicalMoveKeepsProjectAndGateway() {
    Fixture f;
    T3Connections(*f.store).connect("desktop",localOwner.id,48123);
    f.t3.projectsByEnvironment["cloud"] = {{"cloud-project","Cloud fixture",{}},
                                             {"other-project","Other fixture",{}}};
    const Json aliases = {{"cloud","staging"},
                          {"cloud-target",{{"environmentId","cloud"},{"projectId","cloud-project"},
                                            {"gatewayEnvironmentId","desktop"}}}};
    auto initial = f.routing("Start this work","alpha","desktop","project");
    initial.config["conversationEnvironments"] = aliases;
    const auto moving = f.store->submit(initial);
    f.reach(moving,"running");
    check(f.task(moving).at("projectId") == "project","the task did not start in its conversation's default project");

    const auto text = "Move this work to cloud";
    f.script(text,{change(moving,{{"environment","cloud"}})});
    auto intake = f.routing(text,"alpha","desktop","project");
    intake.config["conversationEnvironments"] = aliases;
    f.store->receive(localOwner,intake,"canonical-move",text);
    f.runtime->runConversations();
    check(f.task(moving)["pendingDestination"] ==
          Json{{"environmentId","cloud"},{"projectId","cloud-project"},
               {"gatewayEnvironmentId","desktop"},{"pinned",true}},
          "a canonical move did not reserve its selected project and gateway together");

    f.t3.finish("run-1","Old destination settled");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the moved task was not relaunched");
    const auto task = f.task(moving);
    const auto& command = f.t3.sent.back();
    check(task.at("state") == "running" && task.at("environmentId") == "cloud" &&
          task.at("projectId") == "cloud-project" &&
          task.at("gatewayEnvironmentId") == "desktop" && command.environmentId == "cloud" &&
          command.projectId == "cloud-project" && command.gatewayEnvironmentId == "desktop",
          "the next T3 instruction lost the moved target, project, or gateway");
}

void nonDefaultProjectMoveKeepsCanonicalGateway() {
    Fixture f;
    T3Connections(*f.store).connect("desktop",localOwner.id,48123);
    f.t3.projectsByEnvironment["cloud"] = {{"cloud-project","Cloud fixture",{}},
                                              {"other-project","Other fixture",{}}};
    const Json aliases = {{"cloud","staging"},
                          {"cloud-target",{{"environmentId","cloud"},{"projectId","cloud-project"},
                                            {"gatewayEnvironmentId","desktop"}}}};
    auto initial = f.routing("Start this work","alpha","desktop","task-project");
    initial.config["conversationEnvironments"] = aliases;
    const auto moving = f.store->submit(initial);
    f.reach(moving,"running");
    check(f.task(moving).at("projectId") == "task-project",
          "the task did not start in its non-default project");

    const auto text = "Move this work to cloud";
    f.script(text,{change(moving,{{"environment","cloud"}})});
    auto intake = f.routing(text,"alpha","desktop","conversation-project");
    intake.config["conversationEnvironments"] = aliases;
    f.store->receive(localOwner,intake,"non-default-canonical-move",text);
    f.runtime->runConversations();
    check(f.task(moving)["pendingDestination"] ==
          Json{{"environmentId","cloud"},{"projectId","cloud-project"},
               {"gatewayEnvironmentId","desktop"},{"pinned",true}},
          "the move from a non-default project did not reserve the registered route");

    f.t3.finish("run-1","Old destination settled");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the moved task was not relaunched");
    const auto& command = f.t3.sent.back();
    check(command.environmentId == "cloud" && command.projectId == "cloud-project" &&
          command.gatewayEnvironmentId == "desktop",
          "the next T3 instruction silently fell back to the target ID as gateway");
}

void cloudDestinationWaitsForItsProject() {
    Fixture f;
    rememberCloudProject(f);
    f.script("Set the desktop timeout",{[](const ModelInput&){
        return Decision{"update_bot_settings",{{"environmentId","desktop"},{"runTimeoutSeconds",123}},"Set timeout"};
    }});
    f.receive("set-timeout","Set the desktop timeout");
    const auto occupied = f.store->submit(f.routing("Cloud work","beta","cloud","cloud-project"));
    const auto moving = f.store->submit(f.routing());
    f.script("Include the table as CSV",{send(moving)});
    f.receive("format-before-launch","Include the table as CSV");
    f.reach(occupied,"running");
    f.reach(moving,"running");
    check(f.task(moving).at("deadline")==f.now+123,"desktop launch missed its own timeout");
    f.script("Run the measurement on the cloud Mac",{change(moving,{{"environment","クラウドMac"}})});
    f.receive("where","Run the measurement on the cloud Mac");
    const auto following = f.store->submit(f.routing("Cloud follower","beta","cloud","cloud-project"));
    const auto desktopFollowing = f.store->submit(f.routing("Desktop follower","beta"));
    f.tick(3);
    check(f.t3.interrupted.size() == 1 && f.t3.interrupted[0] == std::pair<std::string,std::string>{"desktop","run-2"} &&
          f.task(moving)["revisions"].size() == 1,
          "a confirmed environment change interrupted the wrong Run or revised the criteria");
    f.t3.finish("run-2","Old location is still settling",false);
    f.tick(2);
    check(f.task(desktopFollowing)["state"] == "queued" && f.task(desktopFollowing)["blockedBy"]["id"] == moving,
          "the desktop slot was released before the old Run settled");
    f.t3.finish("run-2","Old location stopped " + std::string(6000,'x'));
    bool sawHistoricalEnvironment = false;
    f.model.beforeComplete = [&](const ModelInput& input) {
        if (input.task["id"] != moving) return;
        for (const auto& row : input.history) {
            if (row.at("kind") == "report" && row.value("truncated",false))
                sawHistoricalEnvironment = row.at("source").at("environmentId") == "desktop";
        }
    };
    f.restart();
    f.reach(desktopFollowing,"running");
    check(f.task(following)["state"] == "queued" && f.t3.sent.size() == 3 &&
          f.t3.sent.back().environmentId == "desktop" && f.task(moving)["environmentId"] == "cloud" &&
          f.task(moving)["blockedBy"]["id"] == occupied,
          "work entered a Cloud project where another Task was still active");
    f.t3.finish("run-1","Cloud project complete");
    f.reach(occupied,"completed");
    f.reach(moving,"running");
    const auto task = f.task(moving);
    check(task.at("deadline")==f.now+1800,"moved task retained the old environment timeout");
    check(sawHistoricalEnvironment && f.t3.sent.size() == 4 && f.t3.sent.back().tool == "delegate" && f.t3.sent.back().environmentId == "cloud" &&
          f.t3.sent.back().projectId == "cloud-project" && f.t3.sent.back().instruction.find("as CSV") != std::string::npos &&
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
    rememberCloudProject(f);
    const auto local = f.store->submit(f.routing("Existing local work","beta"));
    const auto moving = f.store->submit(f.routing("Transfer this task"));
    const auto remote = f.store->submit(f.routing("Newer destination owner","beta","cloud","cloud-project"));
    f.reach(local,"running");
    f.reach(remote,"running");
    f.script("Transfer this task to cloud",{change(moving,{{"environment","cloud"}})});
    f.receive("move-queued","Transfer this task to cloud");
    f.restart();
    f.advanceUntil([&] { return f.task(moving)["projectId"] == "cloud-project"; },
                   "the queued transfer stayed behind unrelated work in its old queue");
    check(f.task(moving)["blockedBy"]["id"] == remote && f.t3.sent.size() == 2 && f.t3.interrupted.empty(),
          "the transferred Task bypassed the newer active destination owner");
    const auto page = f.store->tasks("alpha",0,1);
    const Json owner = {{"id",remote},{"botId","beta"},{"request","Newer destination owner"},{"state","running"}};
    check(page.size() == 1 && page[0].at("id") == moving && page[0].at("blockedBy") == owner,
          "the queued transfer's task page still pointed to its old environment");
    f.t3.finish(f.task(remote)["activeRun"]["runId"].get<std::string>(),"Destination settled");
    f.reach(moving,"running");
    check(f.t3.sent.size() == 3 && f.t3.sent.back().environmentId == "cloud" &&
          f.t3.sent.back().projectId == "cloud-project" && f.task(local)["state"] == "running",
          "the queued Task did not retain its route through restart");
}

void moveAsksForTheDestinationProjectAfterSettlement() {
    Fixture f;
    T3Connections connections(*f.store);
    connections.connect("desktop",localOwner.id,48123);
    connections.connect("cloud",localOwner.id,48123);
    rememberCloudProject(f);
    const auto owner = f.store->submit(f.routing("Existing cloud work","beta","cloud","cloud-release"));
    const Intake automatic{"alpha","","",""};
    constexpr auto request = "Start work on desktop";
    f.script(request,{start({{"environment","desktop"}})});
    f.store->receive(localOwner,automatic,"create-on-desktop",request);
    f.runtime->runConversations();
    const Id moving = f.store->tasks("alpha").front().at("id");
    f.reach(owner,"running");
    f.reach(moving,"running");
    f.script("Move the work to cloud",{change(moving,{{"environment","cloud"}})});
    f.store->receive(localOwner,automatic,"select-cloud-project","Move the work to cloud");
    f.runtime->runConversations();
    check(f.task(moving)["pendingDestination"] ==
          Json{{"environmentId","cloud"},{"projectId","cloud-project"},
               {"gatewayEnvironmentId","cloud"},{"pinned",false}},
          "the unpinned Bot did not provisionally reserve its remembered project");
    f.t3.finish("run-2","Old Run still settling",false);
    f.tick(3);
    check(f.task(moving).at("environmentId") == "desktop" && f.task(moving).at("projectId") == "project" &&
          !f.task(moving).at("activeRun").is_null() && f.t3.sent.size() == 2,
          "project selection moved work before its original Run settled");

    f.t3.projectsByEnvironment["cloud"] = {
        {"cloud-docs","Cloud Docs","docs"},{"cloud-release","Cloud Release","release"}};
    f.t3.finish("run-2","Old Run settled");
    f.reach(moving,"waiting");
    const auto waiting = f.task(moving);
    check(waiting.at("environmentId") == "cloud" && waiting.at("projectId") == "" &&
          waiting.at("wait").at("choices") == Json::array({"Cloud Docs","Cloud Release"}) &&
          !waiting.contains("pendingDestination") && f.t3.sent.size() == 2,
          "the move kept a project that had disappeared or skipped the new choice");
    const auto target = TaskTarget::from(waiting,waiting.at("wait").at("id"));
    f.restart();
    f.store->operate(localOwner,target,"answer",{{"text","Cloud Release"}});
    f.tick(3);
    check(f.task(moving).at("state") == "queued" && f.task(moving).at("blockedBy").at("id") == owner &&
          f.t3.sent.size() == 2,"a move with a new project choice bypassed the destination owner");
    f.t3.finish("run-1","The destination's existing Run settled");
    f.reach(moving,"running");
    check(f.t3.sent.size() == 3 && f.t3.sent.back().environmentId == "cloud" &&
          f.t3.sent.back().projectId == "cloud-release" && f.task(moving).at("runs").size() == 2,
          "the moved work did not start a fresh Run on the selected destination project");
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
    check(f.t3.sent[1].environmentId == "desktop","the structured destination did not use the latest patch");
    for (std::size_t run = 2; run <= f.t3.sent.size(); ++run) {
        check(run <= 4,"the backlog exceeded the additional work allowance");
        f.t3.finish("run-" + std::to_string(run),"Continuation settled");
        f.advanceUntil([&] { return f.task(id)["state"] == "completed" || f.t3.sent.size() > run; },
                       "the accepted backlog did not finish");
    }
    std::string sentInstructions;
    for (auto command = std::next(f.t3.sent.begin()); command != f.t3.sent.end(); ++command) {
        check(command->environmentId == "desktop",
              "a continuation was sent with a superseded destination");
        sentInstructions += command->instruction;
        sentInstructions += '\n';
    }
    check(sentInstructions.find("クラウドMacで") < sentInstructions.find(notes.front()) &&
          sentInstructions.find("ローカルMacで") > sentInstructions.find(notes.back()),
          "environment corrections dropped or reordered accepted directions");
    for (const auto& note : notes) {
        const auto first = sentInstructions.find(note);
        check(first != std::string::npos && sentInstructions.find(note,first + note.size()) == std::string::npos,
              "an accepted backlog instruction was lost, truncated, or sent twice");
    }
}

enum class UnsentAction { FirstDelegation, FollowUp };
void checkUnsentRevision(UnsentAction action);

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
    f.store->operate(localOwner,TaskTarget::from(f.store->task(id),wait["id"]),"approve");
    f.reach(id,"running");
    check(f.t3.sent.size() == 5 && f.task(id)["remaining"] == 0 && f.task(id)["additionalUsed"] == 4 &&
          f.store->tasks("alpha").size() == 1,"explicit budget approval did not permit precisely one extra Run");
    check(f.t3.sent.back().instruction.find("for fixture 4") != std::string::npos,
          "the approved Run lost the instruction that was held for approval");
}

void revisionOfProvablyUnsentAction() {
    checkUnsentRevision(UnsentAction::FirstDelegation);
    checkUnsentRevision(UnsentAction::FollowUp);
}

void checkUnsentRevision(UnsentAction action) {
    Fixture f;
    const auto id = f.store->submit(f.routing());
    if (action == UnsentAction::FollowUp) {
        f.model.onResult = [](const ModelInput&) {
            return Decision{"follow_up",{{"instruction","Verify the report"}},"The report needs verification"};
        };
        f.reach(id,"running");
        f.t3.finish("run-1","Preliminary report");
    }
    f.reach(id,"sending");
    f.script("完了条件はレポートを出す",{change(id,{{"doneWhen","レポートを出す"}})});
    f.receive("ready-revision","完了条件はレポートを出す");
    f.restart();
    f.reach(id,"running");
    const auto task = f.task(id);
    const unsigned charged = action == UnsentAction::FollowUp ? 1 : 0;
    check(f.t3.sent.size() == 1 + charged && task["remaining"] == 3 - charged && task["additionalUsed"] == charged &&
          f.t3.sent.back().instruction.find("レポートを出す") != std::string::npos,
          "replacing unsent work delivered the wrong criteria or charged the wrong budget");
    check(task["runs"].back()["revisionId"] == task["currentRevision"] &&
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

void conversationCanReadAndChangeTheFullCurrentContract() {
    Fixture f;
    f.script("Start a release audit",{start({{"doneWhen","Report on staging"}})});
    f.receive("create-audit","Start a release audit");
    const Id id = f.store->tasks("alpha").front().at("id");
    const std::string intent = std::string(999,'i') + "監査の対象";
    const std::string criteria = std::string(1599,'c') + "公開しない";
    f.script("Use the updated contract",{change(id,{{"intent",intent},{"doneWhen",criteria}})});
    const auto update = f.receive("updated-contract","Use the updated contract");
    const auto queued = f.task(id);
    const auto expectedPreview = [&](const Json& task) {
        return task.at("intent") == std::string(999,'i') && task.at("intentTruncated") == true &&
               task.at("doneWhen") == std::string(1599,'c') && task.at("doneWhenTruncated") == true &&
               task.at("doneWhenSource") == "requested" && task.at("criteriaSourceMessageId") == update.at("messageId") &&
               task.at("revisionId") == 0 && task.at("generation") == queued.at("generation");
    };
    const std::string addition = "\nInclude the audit date";
    Json context, search, detail;
    f.script("Also include the audit date",{
        [&](const ModelInput& input) {
            context = listed(input,id);
            return Decision{"find_tasks",{{"query","release audit"}},"Find the audit"};
        },
        [&](const ModelInput& input) {
            search = input.event.at("turn").back().at("result").at("tasks").at(0);
            return Decision{"find_tasks",{{"taskId",search.at("taskId")}},"Read its full contract"};
        },
        [&](const ModelInput& input) {
            detail = input.event.at("turn").back().at("result").at("tasks").at(0);
            return Decision{"change_task",{{"taskId",detail.at("taskId")},{"generation",detail.at("generation")},
                {"revisionId",detail.at("revisionId")},{"instruction","Add the audit date to the criteria"},
                {"doneWhen",detail.at("doneWhen").get<std::string>() + addition}},"Add the criterion"};
        },
    });
    const auto changeReceipt = f.receive("extend-contract","Also include the audit date");
    check(queued.at("state") == "queued" && expectedPreview(context) && expectedPreview(search),
          "the conversation lists hid the effective prelaunch criteria or clipped them without a marker");
    check(detail.at("taskId") == id && detail.at("revisionId") == 0 && detail.at("generation") == queued.at("generation") &&
          detail.at("intent") == intent && detail.at("doneWhen") == criteria &&
          !detail.value("intentTruncated",false) && !detail.value("doneWhenTruncated",false) &&
          f.response(changeReceipt).at("actions").size() == 1,
          "the targeted lookup did not provide a complete contract usable by change_task");
    f.reach(id,"running");
    const auto launched = f.task(id);
    check(launched.at("doneWhen") == criteria + addition && launched.at("intent") == intent &&
          launched.at("revisions").at(0).at("sourceMessageId") == changeReceipt.at("messageId"),
          "the launch lost part of the criteria when the user extended them");

    f.script("Read the current audit contract",{
        [id](const ModelInput&) { return Decision{"find_tasks",{{"taskId",id}},"Read the audit's current revision"}; },
    });
    f.receive("read-launched-contract","Read the current audit contract");
    const auto current = f.lastTurn().at(0).at("result").at("tasks").at(0);
    check(current.at("revisionId") == launched.at("currentRevision") && current.at("revisionId") != 0 &&
          current.at("generation") == launched.at("generation") && current.at("doneWhen") == criteria + addition &&
          current.at("intent") == intent && current.value("doneWhenSource","") != "requested",
          "the targeted lookup lost the identity or contract of the established revision");
}

void newSessionsReceivePersistedTaskDecisions() {
    for (const bool explicitFresh : {false,true}) {
        Fixture f([](std::string_view) { return "Use the performance runner"; });
        f.shareMemoryWithSessions = true;
        const auto intent = "Measure the cold-start fixture, including " + std::string(4800,'i') + " the cache-disabled case";
        const auto criteria = "Report " + std::string(4500,'c') + " p99 and raw samples";
        constexpr auto startText = "Measure the cold-start fixture";
        f.script(startText,{start({{"request",intent},{"doneWhen",criteria}})});
        f.receive("start-handoff",startText);
        const Id id = f.store->tasks("alpha").front().at("id");
        const auto direction = "Use the pinned input. " + std::string(5000,'d') + " Keep its checksum in the report.";
        f.script("Use the pinned input",{send(id,{{"instruction",direction}})});
        f.receive("pinned-input","Use the pinned input");
        f.model.questions[id] = {{"question","Which dataset?"}};
        f.reach(id,"waiting");
        const auto wait = f.task(id).at("wait");
        constexpr auto userAnswer = "Use dataset Cobalt-492";
        f.store->operate(localOwner,TaskTarget::from(f.task(id),wait.at("id")),"answer",{{"text",userAnswer}});
        f.reach(id,"running");

        if (explicitFresh) {
            f.script("Start with fresh context",{[&](const ModelInput&) {
                return Decision{"continue_fresh_session",{{"taskId",id},{"instruction","Use a clean checkout"}},
                                "The user requested a fresh Session"};
            }});
            f.receive("fresh-handoff","Start with fresh context");
        } else {
            f.model.onResult = [](const ModelInput&) {
                return Decision{"delegate",{{"instruction","Inspect this independently"}},"Use an independent Session"};
            };
        }
        f.t3.finish("run-1","First evidence");
        f.restart();
        f.reach(id,"sending");
        constexpr auto readyDirection = "Include the kernel version";
        f.script(readyDirection,{send(id)});
        f.receive("ready-direction",readyDirection);
        f.restart();
        f.reach(id,"running");
        const auto& command = f.t3.sent.back();
        const auto task = f.task(id);
        check(f.t3.sent.size() == 2 && command.tool == "delegate" && task["runs"].size() == 2 &&
              task["runs"][0]["threadId"] != task["runs"][1]["threadId"] &&
              task["runs"][0]["revisionId"] == task["runs"][1]["revisionId"],
              "the replacement did not start a separate Session for the same revision");
        for (const std::string_view text : {std::string_view(intent),std::string_view(criteria),
                std::string_view(direction),std::string_view(userAnswer),
                std::string_view(readyDirection),std::string_view("Use the performance runner")}) {
            const auto at = command.instruction.find(text);
            check(at != command.instruction.npos && at == command.instruction.rfind(text),
                  "the new Session lost, shortened or duplicated a task decision");
        }
        check(command.instruction.find(direction) < command.instruction.find(userAnswer),
              "the new Session reordered user decisions");

        f.model.onResult = [](const ModelInput&) {
            return Decision{"follow_up",{{"instruction","Check the last measurement again"}},"Verify in the same Session"};
        };
        f.t3.finish("run-2","Second evidence");
        f.reach(id,"deciding");
        f.reach(id,"running");
        const auto& follow = f.t3.sent.back();
        check(follow.tool == "follow_up" && task["runs"][1]["threadId"] == follow.target.threadId &&
              follow.instruction.find(direction) == follow.instruction.npos &&
              follow.instruction.find(userAnswer) == follow.instruction.npos,
              "the same-Thread follow-up replayed the new-Session handoff");
    }
}

void revisedSessionDoesNotReplaySupersededDecisions() {
    Fixture f;
    const auto id = f.store->submit(f.routing("Measure the retired dataset"));
    constexpr auto oldDirection = "Use the retired runner";
    f.script(oldDirection,{send(id)});
    f.receive("old-direction",oldDirection);
    f.model.questions[id] = {{"question","Which retired account?"}};
    f.reach(id,"waiting");
    const auto wait = f.task(id).at("wait");
    constexpr auto oldAnswer = "Use account Old-149";
    f.store->operate(localOwner,TaskTarget::from(f.task(id),wait.at("id")),"answer",{{"text",oldAnswer}});
    f.reach(id,"running");
    f.script("Use the replacement suite",{change(id,{{"intent","Measure the replacement dataset"},
        {"doneWhen","Report median throughput"},{"environment","クラウドMac"}})});
    f.receive("replace-contract","Use the replacement suite");
    f.restart();
    f.t3.finish("run-1","The prior Run settled");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the revised Session did not start");
    const auto& command = f.t3.sent.back();
    check(command.tool == "delegate" && command.environmentId == "cloud" &&
          command.instruction.find("Measure the replacement dataset") != command.instruction.npos &&
          command.instruction.find("Report median throughput") != command.instruction.npos &&
          command.instruction.find("Use the replacement suite") != command.instruction.npos,
          "the revised Session lost its current contract or instruction");
    for (const auto stale : {"Measure the retired dataset","Report baseline latency",oldDirection,oldAnswer})
        check(command.instruction.find(stale) == command.instruction.npos,
              "a superseded task decision reached the revised Session");
}

void authoritativeHandoffFitsBeforeMemory() {
    Fixture f;
    f.shareMemoryWithSessions = true;
    const std::string protectedValue = "fixture-private-key";
    std::string memory = "Use fixture credentials: " + protectedValue + "\n";
    while (memory.size() + std::string_view("界").size() <= 8192) memory += "界";
    f.store->memory(localOwner,"alpha","apply_memory_edit",{{"baseVersion",0},{"content",memory}});
    auto policy = f.store->policy();
    policy.submitSecret(localOwner,"alpha",policy.requestSecret("alpha",{"fixture-key"}),protectedValue);
    const auto id = f.store->submit(f.routing());
    f.reach(id,"running");
    const auto checkMemory = [&](const Command& command, std::size_t sectionBudget) {
        const auto& prompt = command.instruction;
        const auto notes = prompt.find("\n\nNotes from alpha's memory:\n");
        const auto marker = prompt.find(sessionPrompt(command.actionId,{}));
        check(notes < marker && marker - notes <= sectionBudget && prompt.size() <= maxSessionPromptBytes &&
              prompt.find("Use fixture credentials: [redacted]") != prompt.npos &&
              prompt.find(protectedValue) == prompt.npos && prompt.find("[Bot memory truncated]") < marker,
              "the Session memory exceeded its budget, lost its truncation notice, or contained a protected value");
        Json(prompt).dump();
        return notes;
    };
    checkMemory(f.t3.sent.front(),8192);
    std::vector<std::string> directions;
    for (int i = 0; i < 9; ++i) {
        const auto key = "large-direction-" + std::to_string(i);
        directions.push_back(key + std::string(14500,'x'));
        f.script(key,{send(id,{{"instruction",directions.back()}})});
    }
    for (int i = 0; i < 8; ++i) {
        const auto key = "large-direction-" + std::to_string(i);
        f.receive(key,key);
    }
    f.model.onResult = [](const ModelInput&) {
        return Decision{"delegate",{{"instruction","Start an independent Session"}},"Verify independently"};
    };
    f.t3.finish("run-1","Previous Run settled");
    f.advanceUntil([&] { return f.t3.sent.size() == 2; },"the instructions fit without the full memory section");
    const auto& tight = f.t3.sent.back();
    const auto notes = checkMemory(tight,4000);
    check(tight.instruction.starts_with("Start an independent Session"),"memory displaced the work instruction");
    for (std::size_t i = 0; i < 8; ++i)
        check(tight.instruction.find(directions[i]) < notes,"memory displaced an accepted instruction");

    f.model.onResult = [](const ModelInput&) {
        return Decision{"follow_up",{{"instruction","Check the baseline again"}},"Verify the prior report"};
    };
    f.t3.finish("run-2","Previous Run settled again");
    f.advanceUntil([&] { return f.t3.sent.size() == 3; },"the Session did not receive its follow-up");
    const auto& roomier = f.t3.sent.back();
    check(roomier.target.threadId == "session-2" && roomier.instruction.size() > 8192,
          "a shorter follow-up omitted the longer excerpt of the same memory version");
    checkMemory(roomier,8192);

    f.model.onResult = [](const ModelInput&) {
        return Decision{"delegate",{{"instruction","Start an independent Session"}},"Verify independently"};
    };
    for (int i = 0; i < 9; ++i) {
        const auto key = "large-direction-" + std::to_string(i);
        f.receive("next-" + key,key);
    }
    f.t3.finish("run-3","Previous Run settled once more");
    f.reach(id,"sending");
    f.restart();
    f.reach(id,"waiting");
    const auto task = f.task(id);
    check(f.t3.sent.size() == 3 && task["pendingInstructions"].size() == 9 &&
          task["wait"]["reason"] == "settings" &&
          task["wait"]["text"].get<std::string>().find("size limit") != std::string::npos,
          "a new Session silently received an incomplete authoritative handoff");
}

void intakeChangeScopesTheFirstSessionAnswers() {
    Fixture f;
    const auto id = f.store->submit(f.routing("Measure frontend startup"));
    f.model.questions[id] = {{"question","Which browser?"}};
    f.reach(id,"waiting");
    auto task = f.task(id);
    f.store->operate(localOwner,TaskTarget::from(task,task["wait"]["id"]),"answer",{{"text","Use Firefox Nightly"}});
    f.script("Measure worker startup instead",{change(id,{{"intent","Measure worker startup"}})});
    f.receive("replace-intake-intent","Measure worker startup instead");
    f.model.questions[id] = {{"question","Which worker?"}};
    f.reach(id,"waiting");
    task = f.task(id);
    f.store->operate(localOwner,TaskTarget::from(task,task["wait"]["id"]),"answer",{{"text","Use worker Green-32"}});
    f.restart();
    f.reach(id,"running");
    const auto& prompt = f.t3.sent.front().instruction;
    check(prompt.find("Measure worker startup") != prompt.npos && prompt.find("Use worker Green-32") != prompt.npos &&
          prompt.find("Use Firefox Nightly") == prompt.npos,
          "the first Session lost the current intake answer or replayed one from the previous intent");
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

void interruptNoOpsDoNotAcknowledgeARequest() {
    for (const auto outcome : {InterruptOutcome::AlreadyTerminal,InterruptOutcome::NoActiveRun}) {
        Fixture f;
        const auto id = f.store->submit(f.routing());
        const auto queued = f.store->submit(f.routing("Next project task"));
        f.reach(id,"running");
        const auto run = f.task(id).at("activeRun");
        f.t3.interruptOutcome = outcome;
        f.script("Pause that run",{interrupt(id)});
        const auto message = f.receive("interrupt","Pause that run");
        const auto result = f.lastTurn().at(0).at("result");
        check(result["ok"] == false &&
              result["outcome"] == (outcome == InterruptOutcome::AlreadyTerminal ? "already_terminal" : "no_active_run") &&
              f.response(message)["actions"].empty() && f.response(message)["status"] == "done" &&
              f.event(id,"interrupt_requested").is_null() && f.event(id,"cancel_requested").is_null() &&
              f.task(id)["state"] == "running" && f.task(id)["activeRun"] == run,
              "an interrupt no-op was recorded as a stop or changed the Task");

        f.store->operate(localOwner,TaskTarget::from(f.task(id),0),"cancel");
        f.advanceUntil([&] { return f.t3.interrupted.size() == 2; },"cancellation did not contact T3");
        check(f.task(id)["state"] == "cancelling" && f.task(id)["activeRun"] == run &&
              f.task(queued)["state"] == "queued" && f.event(id,"interrupt_requested").is_null(),
              "a no-op interrupt released the cancelling Task's queue slot or was journaled as acknowledged");
        f.t3.finish(run["runId"].get<std::string>(),"Settled");
        f.reach(id,"cancelled");
    }
}

void environmentsResolveByNameOrId() {
    Fixture f;
    auto routing = f.routing();
    auto& aliases = routing.config["conversationEnvironments"];
    aliases["cloud"] = "staging";
    aliases["クラウドMac"] = { {"environmentId","cloud"},{"projectId","cloud-project"},
                                {"gatewayEnvironmentId","desktop"} };
    T3Connections connector(*f.store);
    connector.connect("desktop",localOwner.id,48123);
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
    const auto message = f.store->receive(localOwner,routing,"environments",text);
    f.runtime->runConversations();
    const auto tasks = f.store->tasks("alpha");
    const auto cloudTask = tasks.size() > 1 ? f.task(tasks[1]["id"]) : Json::object();
    check(tasks.size() == 2 && cloudTask["environmentId"] == "cloud" &&
          cloudTask["projectId"] == "cloud-project" && cloudTask.value("gatewayEnvironmentId","") == "desktop" &&
          f.task(tasks[0]["id"])["environmentId"] == "executor-final" &&
          f.lastTurn()[2]["result"]["ok"] == false && f.response(message)["actions"].size() == 2,
          "a canonical ID was shadowed by an alias, an alias was missed, or an unknown environment was accepted");
}
void existingSessionStartsNormalWork() {
    Fixture f([](std::string_view) { return "Prefer integration tests."; });
    f.shareMemoryWithSessions = true;
    T3Connections connector(*f.store);
    connector.connect("desktop",localOwner.id,48123);
    connector.connect("cloud",localOwner.id,48123);

    f.script("Create the initial Flock task",{start()});
    f.receive("initial","Create the initial Flock task");
    const auto initial = f.store->tasks("alpha").front().at("id").get<Id>();
    f.reach(initial,"running");
    const auto firstRun = f.task(initial).at("activeRun");
    const auto firstRunId = firstRun.at("runId").get<std::string>();
    const auto firstThreadId = firstRun.at("threadId").get<std::string>();
    f.t3.finish(firstRunId,"Initial report");
    f.reach(initial,"completed");

    const auto runTitle = "Bot Session";
    const auto nativeTitle = "Native Session";
    f.t3.sessions[{"desktop",firstThreadId}] =
        {runTitle,"Bot Project","project","completed","2026-11-23T00:00:00Z","2026-11-23T00:00:00Z",{},false};
    f.t3.sessions[{"cloud","native-thread"}] =
        {nativeTitle,"Other Project","other-project","running","2026-11-23T00:00:00Z","2026-11-23T00:00:00Z",{},false};

    for (const auto title : {nativeTitle,runTitle}) {
        const auto request = std::string("Add tests in ") + title;
        f.script(request,{
            [title](const ModelInput&) {
                return Decision{"flock_session_search",{{"query",title}},"Find the chosen Session"};
            },
            [request](const ModelInput& input) {
                const auto& sessions = input.event.at("turn").back().at("result").at("sessions");
                check(sessions.size() == 1,"the chosen Session was not discoverable");
                return Decision{"start_task",{{"request",request},{"sessionRef",sessions.front().at("sessionRef")}},
                                "Start work in the chosen Session"};
            }
        });
        f.store->receive(localOwner,f.routing(request,"alpha","",""),request,request);
        f.runtime->runConversations();
        const auto taskId = f.store->tasks("alpha").front().at("id").get<Id>();
        f.reach(taskId,"running");
        const auto& sent = f.t3.sent.back();
        const bool native = title == nativeTitle;
        check(sent.tool == "start_in_session" && sent.target.runId.empty() &&
              sent.target.threadId == (native ? "native-thread" : firstThreadId) &&
              sent.environmentId == (native ? "cloud" : "desktop") &&
              sent.projectId == (native ? "other-project" : "project"),
              "work lost the selected Thread or used the Bot's launch route");
        check(sent.instruction.find("Prefer integration tests.") != sent.instruction.npos,
              "work in a Session found by search did not receive the Bot's memory");
        const auto assigned = f.task(taskId).at("activeRun");
        check(assigned.at("runId") != firstRunId && assigned.at("threadId") == sent.target.threadId,
              "Flock tracked earlier work instead of the assigned Run");
        f.t3.finish(assigned.at("runId").get<std::string>(),request);
        f.reach(taskId,"completed");
        const auto result = f.event(taskId,"completed");
        check(!result.is_null() && f.task(initial).at("state") == "completed",
              "the new Run did not complete as a normal task");
    }
    const auto sentBefore = f.t3.sent.size();
    f.script("Try a raw Thread ID",{start({{"sessionRef","native-thread"}})});
    f.receive("raw-thread","Try a raw Thread ID");
    f.tick();
    check(f.lastTurn().front().at("result").at("ok") == false && f.t3.sent.size() == sentBefore,
          "a raw Thread ID bypassed Session selection");
}

void sessionMemoryFollowsTheAcceptedVersion() {
    Fixture f([](std::string_view bot) { return bot == "alpha" ? "Use baseline A." : "Beta's private convention."; });
    f.shareMemoryWithSessions = true;
    f.store->memory(localOwner,"beta","read_memory",Json::object());
    f.store->memory(localOwner,"alpha","apply_memory_edit",
        {{"docId","team/notes.md"},{"baseVersion",0},{"content","Shared playbook."}});
    f.t3.deferNextRunId = true;
    f.t3.deliveryVisible = false;
    auto routing = f.routing();
    routing.config["botDisplayName"] = "Release";
    const auto id = f.store->submit(routing);
    f.advanceUntil([&] { return f.t3.sent.size() == 1; },"the initial Session did not receive work");
    const auto first = f.t3.sent.front().instruction;
    check(first.find("\n\nNotes from Release's memory:\nUse baseline A.") != first.npos &&
          first.find("Beta's private convention.") == first.npos && first.find("Shared playbook.") == first.npos,
          "the first prompt did not include only the owning Bot's private memory");
    check(f.task(id).at("pendingAction").dump().find("Use baseline A.") == first.npos,
          "the pending work details exposed the memory snapshot");

    f.restart();
    f.t3.deliveryVisible = true;
    f.reach(id,"running");
    check(f.t3.sent.size() == 1 && !f.t3.reconciled.empty() && f.t3.reconciled.back().instruction == first,
          "recovery did not match the original prompt containing memory");

    f.model.onResult = [](const ModelInput&) {
        return Decision{"follow_up",{{"instruction","Retest with the fixture"}},"Verify the prior report"};
    };
    const auto follow = [&](int run) {
        f.t3.finish("run-" + std::to_string(run),"Prior evidence");
        f.advanceUntil([&] { return f.t3.sent.size() == static_cast<std::size_t>(run + 1); },
                       "the Session did not receive its follow-up");
        const auto& next = f.t3.sent.back();
        check(next.tool == "follow_up" && next.target.threadId == "session-1","work left the existing Session");
        return next.instruction;
    };
    const auto unchanged = follow(1);
    check(unchanged.find("Notes from") == unchanged.npos,"unchanged seed memory was repeated");

    f.store->memory(localOwner,"alpha","apply_memory_edit",{{"baseVersion",0},{"content","Use baseline B."}});
    const auto updated = follow(2);
    check(updated.find("Notes from Release's memory:\nUse baseline B.") != updated.npos &&
          updated.find("Use baseline A.") == updated.npos,"the follow-up missed the updated memory");

    f.store->memory(localOwner,"alpha","apply_memory_edit",{{"baseVersion",1},{"content",""}});
    const auto cleared = follow(3);
    check(cleared.find("Notes from") == cleared.npos,"empty memory added a section to the follow-up");
}

void privateMemoryStaysInFlockByDefault() {
    Fixture f([](std::string_view) { return "Private convention in a project other Bots can read."; });
    auto routing = f.routing();
    routing.config.erase("shareMemoryWithSessions");
    f.reach(f.store->submit(routing),"running");
    check(f.t3.sent.size() == 1 && f.t3.sent.front().instruction.find("Private convention") == std::string::npos,
          "a Session received private memory without an explicit opt-in");

    f.t3.finish("run-1","Prior request completed");
    RuntimeOptions options;
    options.intake = [&](std::string_view, std::string_view) { return f.routing(); };
    f.runtime.reset();
    f.runtime = std::make_unique<Runtime>(*f.store,f.model,f.t3,Runtime::SoulLoader{},options);
    routing.config["shareMemoryWithSessions"] = true;
    f.reach(f.store->submit(routing),"running");
    check(f.t3.sent.size() == 2 && f.t3.sent.back().instruction.find("Private convention") == std::string::npos,
          "a queued task sent private memory after the Bot disabled sharing");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "non-default-project-move") {
            nonDefaultProjectMoveKeepsCanonicalGateway();
            std::cout << "Non-default-project move acceptance passed with simulated T3\n";
            return 0;
        }
        durableInboxAndTargetedRevision();
        existingSessionStartsNormalWork();
        sessionMemoryFollowsTheAcceptedVersion();
        privateMemoryStaysInFlockByDefault();
        linkedResultStartsANewSession();
        independentRequestQueuesSeparately();
        followUpCarriesTheReferencedReport();
        questionAnswerReachesTheBotBeforeContinuingWork();
        answersStayOnTheirQuestion();
        answersNeedTheirOwnReplyTargetAndAStableTask();
        exactOfferedAnswersUseTheModel();
        latestSuggestionsTakePrecedenceOverAnInheritedQuestion();
        attachedChoiceCanBeInspectedBeforeAnswering();
        resumedChoicesRecoverSavedSessionCalls();
        directChoiceRechecksWebAuthority();
        destinationClarificationContinuesFromConversationHistory();
        connectedEnvironmentSelectsProjectBeforeTakingItsQueueSlot();
        unansweredProjectChoiceDoesNotBlockAnotherBot();
        revisingWorkKeepsTheProjectChoice();
        directAnswersRemainInConversationHistory();
        queuedTurnsSeeEarlierReplies();
        pausedTurnDoesNotHoldCompletion();
        suggestionsStayWithTheBotMessage();
        cancelledReportCanBeReferencedWithoutRestarting();
        resumeRestoresCancelledTaskDirections();
        resumeBatchesOversizedPendingBacklog();
        liveMoveAndCriteriaCorrectionKeepCurrentContract();
        environmentOnlyMoveKeepsWorkAcrossResume();
        environmentOnlyMoveCancelledBeforeDeliveryKeepsWorkAcrossResume();
        appendWaitsForSafeBoundary();
        settledInstructionsCanBeAcknowledgedWithoutAnotherRun();
        twoInstructionsFromOneMessageAreTrackedSeparately();
        cancellationTakesOverRevisionStop();
        deliveryWaitsForEveryTarget();
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
        canonicalMoveKeepsProjectAndGateway();
        nonDefaultProjectMoveKeepsCanonicalGateway();
        moveAsksForTheDestinationProjectAfterSettlement();
        queuedMoveWaitsForTheNewerDestinationOwner();
        latestTargetPrecedesABatchedBacklog();
        continuationUsesTheCurrentBudget();
        revisionOfProvablyUnsentAction();
        queuedCriteriaAreUsedForFirstDelegation();
        startCriteriaAreUsedForFirstDelegation();
        conversationCanReadAndChangeTheFullCurrentContract();
        environmentsResolveByNameOrId();
        newSessionsReceivePersistedTaskDecisions();
        revisedSessionDoesNotReplaySupersededDecisions();
        authoritativeHandoffFitsBeforeMemory();
        intakeChangeScopesTheFirstSessionAnswers();
        interruptLeavesTheTaskOpen();
        interruptNoOpsDoNotAcknowledgeARequest();
        std::cout << "Conversation Runtime acceptance passed with simulated T3\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
