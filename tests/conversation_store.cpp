#include "flock/access.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>
#include <unistd.h>

using namespace flockv0;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path dir;

    Fixture() {
        char pattern[] = "/tmp/flock-conversation-XXXXXX";
        if (const auto* path = mkdtemp(pattern)) dir = path;
        else throw std::runtime_error("Cannot create fixture");
    }
    ~Fixture() { std::filesystem::remove_all(dir); }

    std::filesystem::path database() const { return dir / "state.db"; }
    static Intake bot(std::string id = "alpha", std::string project = "project") {
        return {std::move(id),"","env",std::move(project),Json::object()};
    }
    void sql(const std::string& statement) const {
        sqlite3* raw = nullptr;
        check(sqlite3_open(database().c_str(),&raw) == SQLITE_OK,"Cannot open the fixture database");
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
        check(sqlite3_exec(db.get(),statement.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,"The fixture SQL failed");
    }
    long long count(const std::string& statement) const {
        sqlite3* raw = nullptr;
        check(sqlite3_open(database().c_str(),&raw) == SQLITE_OK,"Cannot open the fixture database");
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
        long long result = -1;
        check(sqlite3_exec(db.get(),statement.c_str(),[](void* out,int,char** values,char**) {
            *static_cast<long long*>(out) = values[0] ? std::atoll(values[0]) : 0;
            return 0;
        },&result,nullptr) == SQLITE_OK,"The fixture query failed");
        return result;
    }
};

Id task(Store& store, std::string name, std::string bot = "alpha", std::string project = "project") {
    auto intake = Fixture::bot(std::move(bot),std::move(project));
    intake.request = std::move(name);
    return store.submit(intake);
}

Json reply(Store& store, Id message, std::string_view bot = "alpha") {
    for (const auto& row : store.conversation(bot,0,200))
        if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message)
            return row["data"];
    return nullptr;
}

Id receive(Store& store, std::string_view id, std::string_view text, Id anchor = 0, Intake intake = Fixture::bot()) {
    return store.receive(intake,id,text,anchor).at("messageId");
}

Decision say(std::string text, Json suggestions = nullptr) {
    Json args = {{"text",std::move(text)}};
    if (!suggestions.is_null()) args["suggestions"] = std::move(suggestions);
    return {"reply",std::move(args),"Reply"};
}

struct ScriptedModel : Model {
    std::function<Decision(const ModelInput&)> conversation;
    std::vector<ModelInput> turns;

    Decision decide(const ModelInput& input) override {
        if (input.event.value("kind","") == "conversation_turn") {
            turns.push_back(input);
            return conversation(input);
        }
        if (input.event.at("kind") == "intake")
            return {"delegate",{{"instruction","Run the fixture work"},{"doneWhen","Provide a fixture report"}},"Run the fixture"};
        check(input.event.at("kind") == "result","Unexpected execution fixture event");
        return {"complete",{{"report",input.event.at("report")}},"The fixture report exists"};
    }
};

struct SimulatedT3 : T3 {
    std::map<std::string,Observation> observations;
    unsigned launches = 0;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    Delivery send(const Command& command) override {
        check(command.tool == "delegate","Unexpected work command in the Store fixture");
        const auto suffix = std::to_string(++launches);
        return {Delivery::Kind::Accepted,{"thread-" + suffix,"run-" + suffix,{}},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        return observations[run.runId];
    }
    void interrupt(const std::string&, const Json&, const RunRef&) override {}
};

struct RuntimeFixture {
    ScriptedModel model;
    SimulatedT3 t3;
    std::function<BotNotes(std::string_view)> notes = [](std::string_view) { return BotNotes{}; };
    Runtime runtime;

    explicit RuntimeFixture(Store& store)
        : runtime(store,model,t3,[this](std::string_view bot) { return notes(bot); }) {
        for (auto bot : {"alpha","beta"})
            store.access().resourceGrant({"fixture",true},bot,{"environment","env","launch"},GrantChange::grant);
    }

    void reach(Store& store, Id id, std::string_view state) {
        for (int i = 0; i < 16 && store.task(id).at("state") != state; ++i) runtime.tick();
        check(store.task(id).at("state") == state,"The work did not reach the required fixture state");
    }

    void finish(Store& store, Id id, std::string result) {
        reach(store,id,"running");
        const auto run = store.task(id).at("activeRun").at("runId").get<std::string>();
        t3.observations[run] = {.kind = Observation::Kind::Finished, .status = "completed", .report = std::move(result),
            .resultRunId = run, .itemId = "report-" + run, .settled = true};
        reach(store,id,"completed");
    }
};

Json turn(const ModelInput& input) { return input.event.at("turn"); }

void retryAcrossConnections() {
    Fixture fixture;
    Store left(fixture.database()), right(fixture.database());
    Id a = 0, b = 0;
    std::exception_ptr first, second;
    std::thread one([&] {
        try { a = receive(left,"retry","Write a release note"); }
        catch (...) { first = std::current_exception(); }
    });
    std::thread two([&] {
        try { b = receive(right,"retry","Write a release note"); }
        catch (...) { second = std::current_exception(); }
    });
    one.join();
    two.join();
    if (first) std::rethrow_exception(first);
    if (second) std::rethrow_exception(second);
    check(a && a == b,"Concurrent delivery returned different receipts");
    check(left.tasks("alpha").empty(),"Receipt intake executed work before its turn");
    Store restarted(fixture.database());
    RuntimeFixture execution(restarted);
    unsigned calls = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        check(input.event.at("text") == "Write a release note","Restart lost the pending message");
        ++calls;
        if (turn(input).empty()) return Decision{"start_task",{{"request","Write a release note"}},"Start it"};
        return say("I queued your request.");
    };
    execution.runtime.runConversations();
    check(restarted.tasks("alpha").size() == 1 && reply(restarted,a)["actions"].size() == 1,
          "Delivery did not queue exactly the requested work");
    bool rejected = false;
    try { receive(right,"retry","Write a different release note"); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"Changed message text did not conflict");
    rejected = false;
    try { receive(right,"retry","Write a release note",reply(restarted,a).at("messageId")); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"Changed message reply target did not conflict");
    check(receive(restarted,"retry","Write a release note") == a,"Receipt was lost on restart");
    execution.runtime.runConversations();
    check(calls == 2 && restarted.tasks("alpha").size() == 1,"The retry ran an applied message again");
}

void savedDecisionIsAppliedWithoutAnotherModelCall() {
    Fixture fixture;
    Store store(fixture.database());
    const auto message = receive(store,"saved-decision","Write a release note");
    const auto id = std::to_string(message);
    fixture.sql("INSERT INTO conversation_turns(message_id,status) VALUES(" + id + ",'running')");
    fixture.sql("INSERT INTO conversation_steps(message_id,step,decision) VALUES(" + id +
                ",1,'{\"tool\":\"start_task\",\"args\":{\"request\":\"Write a release note\"},\"reason\":\"Saved\"}')");
    RuntimeFixture execution(store);
    unsigned calls = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        ++calls;
        check(turn(input).size() == 1 && turn(input)[0]["result"]["ok"] == true,"The saved step was not applied first");
        return say("Queued.");
    };
    execution.runtime.runConversations();
    check(calls == 1 && store.tasks("alpha").size() == 1 && reply(store,message)["text"] == "Queued.",
          "A saved decision was decided again or applied twice");
}

void sessionNoteIsNotResentAfterARestart() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto id = task(writer,"Benchmark startup");
    execution.reach(store,id,"running");
    const auto message = receive(store,"note","Use the staging database");
    const auto mid = std::to_string(message);
    const Json effect = {{"kind","steer"},{"taskId",id},{"environmentId","env"},{"projectId","project"},
                         {"config",Json::object()},{"run",store.task(id).at("activeRun")},{"text","Use the staging database"},
                         {"actionId","flock-turn-" + mid + "-1"}};
    fixture.sql("INSERT INTO conversation_turns(message_id,status) VALUES(" + mid + ",'running')");
    fixture.sql("INSERT INTO conversation_steps(message_id,step,decision,effect) VALUES(" + mid + ",1,'" +
                Json{{"tool","message_session"},{"args",{{"taskId",id},{"generation",0},{"text","x"}}},{"reason","Saved"}}.dump() +
                "','" + effect.dump() + "')");
    Json seen;
    execution.model.conversation = [&](const ModelInput& input) {
        seen = turn(input);
        return say("I may have sent that already.");
    };
    execution.runtime.runConversations();
    check(seen.size() == 1 && seen[0]["result"]["ok"] == false && seen[0]["result"]["delivery"] == "unknown" &&
          execution.t3.launches == 1,"a note interrupted by a restart was sent again or reported as not delivered");
}

void transientFailureResumesAfterTheLastFinishedStep() {
    Fixture fixture;
    Id message = 0;
    std::map<std::size_t,int> calls;
    {
        Store store(fixture.database());
        RuntimeFixture execution(store);
        execution.model.conversation = [&](const ModelInput& input) -> Decision {
            ++calls[turn(input).size()];
            if (turn(input).empty()) return {"start_task",{{"request","Write a release note"}},"Start it"};
            throw ModelError("The connection dropped",true);
        };
        message = receive(store,"transient","Write a release note");
        execution.runtime.runConversations();
        check(store.tasks("alpha").size() == 1 && reply(store,message).is_null(),
              "A transient failure lost the finished step or ended the turn");
    }
    Store restarted(fixture.database());
    RuntimeFixture execution(restarted);
    execution.model.conversation = [&](const ModelInput& input) {
        ++calls[turn(input).size()];
        check(turn(input).size() == 1,"The restarted turn did not continue after its finished step");
        return say("Queued.");
    };
    execution.runtime.runConversations();
    check(calls[0] == 1 && calls[1] == 2 && restarted.tasks("alpha").size() == 1 &&
          reply(restarted,message)["text"] == "Queued.","Restart repeated a finished step or lost the turn");
    check(fixture.count("SELECT count(*) FROM model_attempts WHERE message_id=" + std::to_string(message)) == 3,
          "The failed call was not counted against the message budget");
}

void modelFailureReportsWhatWasAlreadyDone() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        if (turn(input).empty()) return {"start_task",{{"request","Write a release note"}},"Start it"};
        throw ModelError("The response was malformed",false);
    };
    const auto message = receive(store,"failure","Write a release note and a changelog");
    execution.runtime.runConversations();
    const auto response = reply(store,message);
    check(response["route"] == "error" && response["status"] == "working" &&
          response["text"].get<std::string>().find("Already done for this message") != std::string::npos &&
          response["text"].get<std::string>().find("Write a release note") != std::string::npos &&
          store.tasks("alpha").size() == 1,"The error reply hid the work the turn had already started");
}

void notesFailureEndsTheTurnWithoutWork() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    execution.notes = [](std::string_view) -> BotNotes { throw std::runtime_error("SOUL.md is unreadable"); };
    execution.model.conversation = [](const ModelInput&) -> Decision {
        throw std::runtime_error("The model was called without notes");
    };
    const auto message = receive(store,"notes","Write a release note");
    execution.runtime.runConversations();
    const auto response = reply(store,message);
    check(response["route"] == "error" && response["text"].get<std::string>().find("SOUL.md is unreadable") != std::string::npos &&
          store.tasks("alpha").empty(),"A notes failure started work or hid the cause");
}

void toolErrorsReturnToTheModel() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto mine = task(writer,"Prepare the release");
    const auto foreign = task(writer,"Beta work","beta");
    const auto before = store.task(mine);
    const std::vector<Decision> steps{
        {"start_task",{{"request","Run it"},{"environment","mars"}},"Unknown environment"},
        {"change_task",{{"taskId",mine},{"generation",before.at("generation")},{"revisionId",0},
                        {"instruction","Change nothing"}},"Missing fields"},
        {"send_to_task",{{"taskId",foreign},{"generation",0},{"instruction","Hi"}},"Another Bot's task"},
        {"send_to_task",{{"taskId",999999},{"generation",0},{"instruction","Hi"}},"Missing task"},
        {"send_to_task",{{"taskId",mine},{"generation",before.at("generation").get<Id>() + 5},{"instruction","Hi"}},"Stale"},
        {"launch_rocket",Json::object(),"Unknown tool"},
        say("Pick one",Json::array({"a","b","c","d","e"})),
        say("Which release should I prepare?",Json::array({"1.4","1.5"})),
    };
    execution.model.conversation = [&](const ModelInput& input) { return steps.at(turn(input).size()); };
    const auto message = receive(store,"errors","Prepare it");
    execution.runtime.runConversations();
    const auto results = execution.model.turns.back().event.at("turn");
    check(results.size() == 7 && std::ranges::all_of(results,[](const Json& item) { return item["result"]["ok"] == false; }),
          "An invalid call was applied or not reported");
    check(results[2]["result"]["error"] == "No task with taskId " + std::to_string(foreign) &&
          results[3]["result"]["error"] == "No task with taskId 999999",
          "Another Bot's task was distinguishable from a missing one");
    check(results[4]["result"]["error"] == "stale" && results[4]["result"]["current"]["generation"] == before.at("generation"),
          "A stale call did not return the current version");
    check(store.task(mine) == before && store.task(foreign)["pendingInstructions"].is_null() &&
          store.tasks("alpha").size() == 1,"A failed call changed a task");
    const auto response = reply(store,message);
    check(response["route"] == "reply" && response["suggestions"] == Json::array({"1.4","1.5"}) &&
          response["status"] == "done","The reply or its suggestions were not saved");
}

void boundedContextAndOlderTasks() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    constexpr auto oldName = "古い計測計画";
    const auto old = task(writer,oldName);
    execution.finish(store,old,"Compare paired measurements using the same workload on both builds.");
    const auto original = store.latestTaskResult(old);
    for (int i = 0; i < 30; ++i) execution.finish(store,task(writer,"New completed plan " + std::to_string(i)),"Latest plan");
    const auto foreign = task(writer,"Beta hidden project fixture","beta");
    execution.finish(store,foreign,"Beta-only result fixture");
    for (int i = 0; i < 20; ++i) task(writer,"Outstanding plan " + std::to_string(i));
    const auto before = store.tasks("alpha",0,200);
    const auto launches = execution.t3.launches;

    Json found, read;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        const auto encoded = input.task.dump() + input.event.dump() + input.history.dump();
        check(encoded.find("Beta hidden project fixture") == std::string::npos &&
              encoded.find("Beta-only result fixture") == std::string::npos,"Another Bot leaked into the turn");
        check(input.task.at("tasks").size() <= 20 && input.task.at("moreTasks") == true && encoded.size() < 128 * 1024,
              "The turn context grew with the Bot's full history");
        const auto& steps = turn(input);
        if (steps.empty()) return {"find_tasks",{{"query","計測計画"}},"Find the older plan"};
        if (steps.size() == 1) {
            found = steps[0]["result"];
            return {"read_result",{{"messageId",found["tasks"][0]["latestResult"]["messageId"]}},"Read it"};
        }
        read = steps[1]["result"];
        return say("同じ負荷で比較する計画です。");
    };
    const auto message = receive(writer,"named","「古い計測計画」を要約して");
    execution.runtime.runConversations();
    check(found["tasks"].size() == 1 && found["tasks"][0]["taskId"] == old &&
          found["tasks"][0]["generation"] == store.task(old)["generation"] && found["tasks"][0].contains("revisionId") &&
          read["text"] == "Compare paired measurements using the same workload on both builds." &&
          read["end"] == true && read["currentRevision"] == true && original.at("id") == read["messageId"],
          "An older task outside the list could not be found and read");
    check(reply(store,message)["actions"].empty() && store.tasks("alpha",0,200) == before && execution.t3.launches == launches,
          "Reading an older result changed work");
}

void longResultsAreReadInChunks() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto id = task(store,"Long report");
    const std::string report = std::string(5990,'a') + "あいうえお" + std::string(1500,'b');
    execution.finish(store,id,report);
    const auto result = store.latestTaskResult(id).at("id");
    std::vector<Json> chunks;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        const auto& steps = turn(input);
        if (!steps.empty()) chunks.push_back(steps.back()["result"]);
        if (steps.empty()) return {"read_result",{{"messageId",result}},"Read"};
        if (steps.size() == 1) return {"read_result",{{"messageId",result},{"offset",steps[0]["result"]["nextOffset"]}},"More"};
        return say("Read it.");
    };
    receive(store,"chunks","Summarize the long report");
    execution.runtime.runConversations();
    check(chunks.size() == 2 && chunks[0]["end"] == false && chunks[1]["end"] == true &&
          chunks[0]["text"].get<std::string>() + chunks[1]["text"].get<std::string>() == report,
          "A long result was not readable in UTF-8 safe chunks");
}

void budgetPauseLetsLaterMessagesRunAndApprovalResumes() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    auto limited = Fixture::bot();
    limited.config["modelCallLimit"] = 1;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        if (input.event["text"] == "Thanks") return say("You're welcome.");
        if (turn(input).empty()) return {"find_tasks",Json::object(),"Look"};
        return say("Nothing yet.");
    };
    const auto paused = receive(store,"limited","What is running?",0,limited);
    const auto later = receive(store,"later","Thanks");
    execution.runtime.runConversations();
    const auto pauses = store.pausedConversations("alpha");
    check(pauses.size() == 1 && pauses[0]["route"] == "model_budget" && reply(store,paused)["route"] == "model_budget" &&
          reply(store,later)["text"] == "You're welcome.","A paused turn blocked a later message or was not reported");
    const auto waitMessage = pauses[0]["messageId"].get<Id>();
    store.approveMessageModelCall("alpha",waitMessage);
    bool rejected = false;
    try { store.approveMessageModelCall("alpha",waitMessage); }
    catch (const Conflict&) { rejected = true; }
    check(rejected && store.pausedConversations("alpha").empty(),"The same pause was approved twice");
    execution.runtime.runConversations();
    Json answered;
    for (const auto& row : store.conversation("alpha",0,200))
        if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == paused && row["data"]["route"] == "reply")
            answered = row["data"];
    check(answered["text"] == "Nothing yet." &&
          fixture.count("SELECT count(*) FROM model_attempts WHERE message_id=" + std::to_string(paused)) == 2 &&
          fixture.count("SELECT attempts FROM model_budgets WHERE id='message/" + std::to_string(paused) + "'") == 2,
          "The approved call did not resume the paused turn exactly once");
}

void replyTargetsReachTheTurn() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto id = task(writer,"Measure startup");
    execution.finish(store,id,"Startup took 100ms");
    const auto result = store.latestTaskResult(id).at("id").get<Id>();
    const auto foreignMessage = receive(writer,"foreign","Hello beta",0,Fixture::bot("beta"));
    Json seen = Json::array();
    execution.model.conversation = [&](const ModelInput& input) {
        if (input.event.at("text") != "Hello beta") seen.push_back(input.event.at("replyTo"));
        return say("Noted.");
    };
    const auto first = receive(writer,"about-result","Why so slow?",result);
    execution.runtime.runConversations();
    receive(writer,"about-reply","What do you mean?",reply(store,first).at("messageId"));
    receive(writer,"about-foreign","And this?",foreignMessage);
    execution.runtime.runConversations();
    check(seen.size() == 3 && seen[0]["kind"] == "result" && seen[0]["taskId"] == id &&
          seen[0]["text"] == "Startup took 100ms","A reply to a result did not reach the turn");
    check(seen[1]["kind"] == "bot_message" && seen[1]["text"] == "Noted." &&
          seen[1]["inReplyTo"]["text"] == "Why so slow?","A reply to a Bot message lost the message it answered");
    check(seen[2]["kind"] == "unavailable","A reply to another Bot's message was exposed");
}
}

int main() {
    try {
        retryAcrossConnections();
        savedDecisionIsAppliedWithoutAnotherModelCall();
        transientFailureResumesAfterTheLastFinishedStep();
        sessionNoteIsNotResentAfterARestart();
        modelFailureReportsWhatWasAlreadyDone();
        notesFailureEndsTheTurnWithoutWork();
        toolErrorsReturnToTheModel();
        boundedContextAndOlderTasks();
        longResultsAreReadInChunks();
        budgetPauseLetsLaterMessagesRunAndApprovalResumes();
        replyTargetsReachTheTurn();
        std::cout << "Conversation Store acceptance passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
