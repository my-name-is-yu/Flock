#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <unistd.h>

using namespace flockv0;
using namespace flockv0::app;

namespace {
class Sandbox {
public:
    Sandbox() {
        auto pattern = (std::filesystem::temp_directory_path() / "flock-app-XXXXXX").string();
        const char* made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Temporary directory setup failed");
        path = made;
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(path,error);
    }
    std::filesystem::path path;
};

void check(bool condition, std::string_view what) {
    if (!condition) throw std::runtime_error(std::string(what));
}

template <typename Error, typename Function>
void rejects(Function run, std::string_view what) {
    try { run(); }
    catch (const Error&) { return; }
    throw std::runtime_error(std::string(what));
}

Seconds utc(int year, unsigned month, unsigned day, int hour, int minute, int second = 0) {
    using namespace std::chrono;
    return duration_cast<seconds>((sys_days{std::chrono::year{year} / month / day} +
        hours{hour} + minutes{minute} + seconds{second}).time_since_epoch()).count();
}

void write(const std::filesystem::path& file, const std::string& data) {
    std::ofstream output(file);
    output << data;
    if (!output) throw std::runtime_error("Fixture write failed");
}

Json botConfig(std::string model = "fixture-model") {
    return {{"environmentId","env-canonical"},{"projectId","project-canonical"},{"modelProvider","chatgpt"},
        {"model",model},{"runTimeoutSeconds",999},
        {"session",{{"model","child-model"},{"providerInstanceId","child-provider"},{"runtimeMode","auto"},
            {"workspaceStrategy",{{"kind","worktree"},{"remote","origin"},{"branch","main"}}}}},
        {"cron",Json::array()}};
}

void createBot(const std::filesystem::path& path, const Json& config, std::string soul, std::string memory) {
    std::filesystem::create_directories(path);
    write(path / "config.json",config.dump());
    write(path / "SOUL.md",soul);
    write(path / "MEMORY.md",memory);
}

void cronSyntaxAndTimeZones() {
    const auto matches = [](const Cron& cron, Seconds when) { return cron.next(when - 1) == when; };
    const Cron local("0 9 * * 1-5","Asia/Tokyo");
    check(matches(local,utc(2026,11,20,0,0)), "weekday in the configured IANA timezone");
    check(!matches(local,utc(2026,11,21,0,0)), "skip Saturday");
    const Cron listsAndSteps("5,10-22/4 2/3 * 11 7","Etc/UTC");
    check(matches(listsAndSteps,utc(2026,11,22,5,14)), "list/range/step/Sunday alias");
    check(!matches(listsAndSteps,utc(2026,11,22,5,15)), "minute step mismatch");
    const Cron eitherDay("0 0 21 * 1","Etc/UTC");
    check(matches(eitherDay,utc(2026,11,21,0,0)), "day-of-month alternative");
    check(matches(eitherDay,utc(2026,11,23,0,0)), "day-of-week alternative");
    check(!matches(eitherDay,utc(2026,11,22,0,0)), "neither eligible day");
    rejects<Conflict>([] { Cron("1 2 * * * *","Etc/UTC"); },"six fields accepted");
    rejects<Conflict>([] { Cron("*/0 2 * * *","Etc/UTC"); },"zero divisor accepted");
    rejects<Conflict>([] { Cron("0 24 * * *","Etc/UTC"); },"hour 24 accepted");
    rejects<Conflict>([] { Cron("1,,2 0 * * *","Etc/UTC"); },"empty list element accepted");
    rejects<Conflict>([] { Cron("* * * * *","Not/A_Zone"); },"unknown time zone accepted");
    rejects<Conflict>([] { Cron("* * * * *","/usr/share/zoneinfo/UTC"); },"non-IANA zone path accepted");
    const Cron leap("0 0 29 2 *","Etc/UTC");
    check(leap.next(utc(2096,2,29,0,0)) == utc(2104,2,29,0,0),"century leap year was not skipped");
    const Cron futureDST("30 2 * 3 0","America/New_York");
    check(futureDST.next(utc(2040,3,11,0,0)) == utc(2040,3,18,6,30),"future DST gap was not skipped");
    const Cron halfHourFold("45 1 * * *","Australia/Lord_Howe");
    const auto firstFold = utc(2026,4,4,14,45), secondFold = utc(2026,4,4,15,15);
    const auto folded = halfHourFold.between(firstFold,secondFold);
    check(folded.first == firstFold && folded.last == secondFold && folded.count == 2,"half-hour fall-back lost an instant");
    const Cron always("* * * * *","Etc/UTC");
    check(always.between(0,utc(2026,1,1,0,0)).count == utc(2026,1,1,0,0) / 60 + 1,
          "multi-decade catch-up lost occurrences");
}

void cronIntakeAndRestart() {
    Sandbox temp;
    const auto folder = temp.path / "bots" / "release";
    auto config = botConfig();
    config["cron"] = Json::array({
        {{"id","spring"},{"schedule","30 2 8 3 *"},{"timezone","America/New_York"},{"request","Gap"}},
        {{"id","fall"},{"schedule","30 1 1 11 *"},{"timezone","America/New_York"},{"request","Overlap"}},
    });
    createBot(folder,config,"SOUL","MEMORY");
    Bots bots(temp.path / "bots");
    const auto db = temp.path / "tasks.db";
    const auto report = [](std::string_view id,std::string_view error) {
        throw std::runtime_error(std::string(id) + ": " + std::string(error));
    };

    {
        Store store(db);
        bots.syncSchedules(store,utc(2026,3,8,6,0),report);
        store.fireDueAutomations(utc(2026,3,8,9,0));
        check(store.tasks().empty(),"nonexistent spring local time was submitted");
        store.fireDueAutomations(utc(2026,11,1,5,30));
        store.fireDueAutomations(utc(2026,11,1,6,30));
        const auto tasks = store.tasks();
        check(tasks.size() == 2,"both fall-back instants should submit exactly once");
        check(tasks[0]["trigger"]["targetTime"] == utc(2026,11,1,6,30) &&
              tasks[1]["trigger"]["targetTime"] == utc(2026,11,1,5,30),"fall offsets did not produce separate UTC instants");
        check(tasks[0]["blockedBy"]["id"] == tasks[1]["id"],"cron must participate in the same project queue");
    }
    {
        Store store(db);
        bots.syncSchedules(store,utc(2026,11,1,6,30),report);
        store.fireDueAutomations(utc(2026,11,1,6,30));
        check(store.tasks().size() == 2,"duplicate scheduled UTC instant registered after restart");
        store.fireDueAutomations(utc(2027,11,1,5,35));
        check(store.tasks().size() == 4,"configured schedules did not retain their due time across restart");
    }
}

void configuredScheduleLifecycle() {
    Sandbox temp;
    const auto home = temp.path / "bots";
    const auto folder = home / "release";
    auto config = botConfig();
    const Json entry{{"id","daily"},{"schedule","0 9 * * *"},{"timezone","Asia/Tokyo"},{"request","Daily check"}};
    config["cron"] = Json::array({entry});
    createBot(folder,config,"SOUL","MEMORY");
    createBot(home / "other",botConfig(),"","");
    const Bots bots(home);
    const Person owner{"fixture",true};
    Store store(temp.path / "tasks.db");
    std::vector<std::string> errors;
    const auto sync = [&](Seconds now) {
        bots.syncSchedules(store,now,[&](std::string_view, std::string_view error) { errors.emplace_back(error); });
    };
    const auto schedule = [&] { return store.automations(owner,"release").at("automations").front(); };
    const auto start = utc(2026,1,1,12,0);
    const auto next = utc(2026,1,2,0,0);
    sync(start);
    const auto id = schedule().at("id").get<Id>();
    config["projectId"] = "";
    write(folder / "config.json",config.dump());
    sync(next);
    store.fireDueAutomations(next);
    check(errors.size() == 1 && schedule().at("status") == "inactive" && store.tasks().empty(),
          "invalid configuration left its previously stored route active");

    config["projectId"] = "repaired-project";
    write(folder / "config.json",config.dump());
    sync(next);
    store.fireDueAutomations(next);
    check(store.tasks("release").size() == 1 && store.tasks("release").at(0).at("projectId") == "repaired-project",
          "repaired config did not restore the schedule's identity and route");

    config["cron"] = Json::array();
    write(folder / "config.json",config.dump());
    sync(next);
    check(schedule().at("status") == "inactive","omitted schedule was not deactivated");
    config["cron"] = Json::array({entry});
    write(folder / "config.json",config.dump());
    sync(next);
    store.fireDueAutomations(next);
    check(schedule().at("id") == id && schedule().at("status") == "active" && store.tasks().size() == 1,
          "reintroduced configuration duplicated a firing or lost its schedule ID");
    store.cancelAutomation(owner,"release",id);
    sync(next);
    check(schedule().at("status") == "canceled","sync reactivated an explicit cancellation");

    const auto routing = bots.get("release").intake("");
    store.createAutomation(owner,routing,{{"kind","reminder"},{"timezone","Asia/Tokyo"},{"fireAt",next+1},
        {"request","Check again"}},next);
    store.createAutomation(owner,routing,{{"kind","trigger"},{"source","deploy"},{"request","Deploy check"}},next);
    std::filesystem::remove_all(folder);
    sync(next);
    store.fireDueAutomations(next+1);
    rejects<AccessDenied>([&] { store.receiveWebhook(owner,"deploy","event-1",Json::object(),next+1); },
                          "webhook was delivered for a removed Bot");
    check(store.tasks().size() == 1,"automation for a removed Bot queued work");
}

class NotesModel final : public Model {
public:
    std::vector<ModelInput> inputs;
    Decision decide(const ModelInput& input) override {
        inputs.push_back(input);
        if (input.event["kind"] == "result")
            return {"complete",{{"report","Original evidence confirmed"}},"The report meets the original acceptance criterion."};
        if (input.history.empty() && input.task["botId"] == "first")
            return {"ask_user",{{"question","Which branch?"}},"Only the user can select the branch."};
        const auto doneWhen = input.task["botId"] == "first" ? "Evidence required before edit" : "Second Bot criterion";
        return {"delegate",{{"instruction","Verify the agreed scope"},{"doneWhen",doneWhen}},"Run verification in T3."};
    }
};

class NotesT3 final : public T3 {
public:
    std::vector<Command> commands;
    bool finished = false;
    std::string authGeneration(const std::string&,const Json&) override { return "test-auth"; }
    Delivery send(const Command& command) override {
        commands.push_back(command);
        return {Delivery::Kind::Accepted,{"thread-" + command.actionId,"run-" + command.actionId,"link"},{}};
    }
    Delivery reconcile(const Command&) override { throw std::runtime_error("Unexpected reconcile"); }
    Observation observe(const std::string&,const Json&,const RunRef&) override {
        Observation result;
        if (finished) {
            result.kind = Observation::Kind::Finished;
            result.status = "completed";
            result.report = "Returned evidence for the original criterion";
            result.settled = true;
        }
        return result;
    }
    void interrupt(const std::string&,const Json&,const RunRef&) override {}
};

void notesAndRoutingOnRestart() {
    Sandbox temp;
    const auto folder = temp.path / "bots";
    const auto db = temp.path / "tasks.db";
    createBot(folder / "first",botConfig(),"First version","First memory");
    createBot(folder / "second",botConfig(),"Second version","Second-only memory");
    const Bots bots(folder);
    const auto soul = [&](std::string_view bot) { return bots.soul(bot); };
    const auto seed = [&](std::string_view bot) { return bots.initialMemory(bot); };
    NotesModel model;
    NotesT3 t3;
    Id firstId, secondId, waitId;
    {
        Store store(db,seed);
        Runtime runtime(store,model,t3,soul);
        for (auto bot : {"first","second"})
            store.access().resourceGrant({"fixture",true},bot,{"environment","env-canonical","launch"},GrantChange::grant);
        firstId = store.submit(bots.get("first").intake("Request for first Bot"));
        runtime.tick();
        runtime.tick();
        check(store.task(firstId)["state"] == "waiting","fixture did not reach user question");
        waitId = store.task(firstId)["wait"]["id"];
        secondId = store.submit(bots.get("second").intake("Request for second Bot"));
        runtime.tick();
        check(store.task(secondId)["blockedBy"]["id"] == firstId && t3.commands.empty(),
            "same-project tasks must queue across Bots");
    }
    {
        Store store(db,seed);
        Runtime runtime(store,model,t3,soul);
        store.operate(firstId,waitId,"answer",{{"text","main"}});
        rejects<Conflict>([&] { store.operate(firstId,waitId,"answer",{{"text","old answer"}}); },"duplicate answer accepted");
        runtime.tick();
        runtime.tick();
        check(store.task(firstId)["state"] == "running" && store.task(firstId)["doneWhen"] == "Evidence required before edit",
            "answer must reach the old task and initialize saved doneWhen");
        check(t3.commands.size() == 1 && t3.commands[0].environmentId == "env-canonical" &&
              t3.commands[0].projectId == "project-canonical","intake must preserve canonical IDs");
        check(t3.commands[0].config["session"]["model"] == "child-model" &&
              t3.commands[0].config["session"]["providerInstanceId"] == "child-provider" &&
              t3.commands[0].config["session"]["runtimeMode"] == "auto" &&
              t3.commands[0].config["session"]["workspaceStrategy"]["branch"] == "main" &&
              t3.commands[0].config["runTimeoutSeconds"] == 999,"child settings and timeout must survive intake");
    }

    createBot(folder / "first",botConfig("new-model"),"New SOUL would now ask for something different","First updated memory");
    t3.finished = true;
    {
        Store store(db,seed);
        store.memory({"fixture",true},"first","apply_memory_edit",{{"baseVersion",0},{"content","First updated memory"}});
        Runtime runtime(store,model,t3,soul);
        for (int i = 0; i < 4; ++i) runtime.tick();
        check(store.task(firstId)["state"] == "completed","first Bot did not complete with its original criterion");
        check(store.task(secondId)["state"] == "sending","next task did not begin when the first task closed");
        check(model.inputs.size() == 4,"unexpected model calls");
        const auto& afterRestart = model.inputs[2];
        check(afterRestart.task["id"] == firstId && afterRestart.task["doneWhen"] == "Evidence required before edit",
            "stored criterion must survive restart and a SOUL edit");
        check(afterRestart.notes.soul == "New SOUL would now ask for something different" &&
              afterRestart.notes.memory == "First updated memory","existing task must read current notes");
        check(afterRestart.config["model"] == "fixture-model","model setting must be snapshotted at intake");
        const auto& second = model.inputs[3];
        check(second.task["id"] == secondId && second.notes.memory == "Second-only memory" && second.history.empty(),
            "second Bot's history and notes must remain independent");
    }
    rejects<Conflict>([&] { bots.get("../first"); },"path traversal accepted as Bot ID");
}

Json requestContext(const Json& request) {
    return Json::parse(request.at("input").at(0).at("content").get<std::string>());
}

void notesAcrossConversationAndWork() {
    static constexpr auto criterion = "Confirm the required evidence";
    static constexpr auto conclusion = "The evidence meets the saved criterion.";
    static constexpr auto explanation = "The Session supplied the evidence required for completion.";
    struct ModelFixture final : Model {
        std::vector<Json> requests;
        Decision decide(const ModelInput& input) override {
            requests.push_back(responseRequest(input));
            const auto context = requestContext(requests.back());
            const auto& event = context.at("event");
            const auto& memory = context.at("memory").get_ref<const std::string&>();
            const auto styled = [&](std::string text) {
                if (memory.starts_with("Lead with the conclusion.")) text = std::string(conclusion) + " " + text;
                if (memory.starts_with("Lead with the evidence.")) text += " " + std::string(conclusion);
                return text;
            };
            if (event.at("kind") == "conversation_turn") {
                const auto& turn = event.at("turn");
                if (!turn.empty()) {
                    if (turn.back().at("tool") == "read_result")
                        return {"reply",{{"text",styled(explanation)}},"Explain the saved result"};
                    return {"reply",{{"text","I queued your request."}},"Confirm the request"};
                }
                const auto& reply = event.at("replyTo");
                if (reply.is_object() && reply.at("kind") == "result")
                    return {"read_result",{{"messageId",reply.at("messageId")}},"Read the referenced result"};
                std::string request = event.at("text");
                if (request == "Please retry") {
                    for (const auto& entry : context.at("history"))
                        if (entry.at("role") == "user") request = entry.at("text");
                }
                return {"start_task",{{"request",request}},"Start the requested work"};
            }
            if (event.at("kind") == "intake")
                return {"delegate",{{"instruction","Collect the required evidence"},{"doneWhen",criterion}},"Gather evidence"};
            check(event.at("kind") == "result","unexpected model request");
            return {"complete",{{"report",styled("The Session returned the required evidence.")}},
                    "The Session evidence meets the criterion"};
        }
    } model;

    Sandbox temp;
    const auto folder = temp.path / "bots";
    BotNotes first{"Verify evidence before reporting.\n","Lead with the conclusion.\n"};
    first.soul.resize(8191,'s');
    first.memory.resize(8192,'m');
    const BotNotes second{"Verify independently.","Use a short explanation."};
    createBot(folder / "first",botConfig("first-model"),first.soul + "界tail",first.memory + "tail");
    createBot(folder / "second",botConfig("second-model"),second.soul,second.memory);
    const Bots bots(folder);
    Store store(temp.path / "tasks.db",[&](std::string_view bot) {
        auto memory = bots.initialMemory(bot);
        if (!memory.empty()) memory += std::string(64,'X');
        return memory;
    });
    for (auto bot : {"first","second"})
        store.access().resourceGrant({"fixture",true},bot,{"environment","env-canonical","launch"},GrantChange::grant);
    NotesT3 t3;
    t3.finished = true;
    std::optional<std::string> noteReadError;
    Runtime runtime(store,model,t3,[&](std::string_view botId) {
        if (noteReadError) throw Conflict(*noteReadError);
        auto soul = bots.soul(botId);
        if (!soul.empty()) soul += std::string(64,'X');
        return soul;
    });
    const auto expectedNotes = [](BotNotes notes) {
        if (!notes.soul.empty()) notes.soul += std::string(64,'X');
        if (!notes.memory.empty()) notes.memory += std::string(64,'X');
        notes.soul = bounded(notes.soul,8192);
        notes.memory = bounded(notes.memory,8192);
        return notes;
    };

    const auto checkRequest = [](const Json& request, const Intake& bot, const BotNotes& notes, std::string_view kind) {
        const auto context = requestContext(request);
        check(request.at("model") == bot.config.at("model") && context.at("event").at("kind") == kind,
            "unexpected configured model request");
        check(context.at("soul") == notes.soul && context.at("memory") == notes.memory,
            "model request must contain only the selected Bot's current bounded notes");
    };
    const auto completeTask = [&](const Intake& bot, const BotNotes& notes, std::string_view expectedReport) -> Id {
        const auto start = model.requests.size();
        store.receive(bot,"new-work","Collect the validation evidence");
        runtime.runConversations();
        const auto tasks = store.tasks(bot.botId);
        check(tasks.size() == 1,"the request should create one Task for this Bot");
        const Id task = tasks.at(0).at("id");
        for (int step = 0; step < 10 && store.task(task).at("state") != "completed"; ++step) runtime.tick();
        const auto result = store.latestTaskResult(task);
        check(result.at("kind") == "completed" && result.at("data").at("text") == expectedReport &&
              result.at("data").at("doneWhen") == criterion && store.task(task).at("runs").size() == 1,
            "the Bot's completion report should apply its notes and preserve the saved criterion");
        check(model.requests.size() == start + 4,"expected two conversation steps, task intake, and completion requests");
        checkRequest(model.requests.at(start),bot,notes,"conversation_turn");
        checkRequest(model.requests.at(start + 1),bot,notes,"conversation_turn");
        checkRequest(model.requests.at(start + 2),bot,notes,"intake");
        checkRequest(model.requests.at(start + 3),bot,notes,"result");
        return task;
    };
    const auto askAboutResult = [&](const Intake& bot, Id task, std::string_view key) -> Id {
        const Id result = store.latestTaskResult(task).at("id");
        return store.receive(bot,key,"Why does that evidence support the result?",result).at("messageId");
    };
    const auto checkResultAnswer = [&](const Intake& bot, Id task, Id question,
                                       const BotNotes& notes, std::string_view expectedReply) {
        const auto before = store.task(task);
        const auto taskCount = store.tasks().size();
        const auto commands = t3.commands.size();
        const auto start = model.requests.size();
        runtime.tick();
        const auto reply = store.conversation(bot.botId).back().at("data");
        check(reply.at("replyToMessageId") == question && reply.at("text") == expectedReply,
              "the explanation should apply current notes to the selected result");
        check(model.requests.size() == start + 2,"expected a read and a reply step");
        checkRequest(model.requests.at(start),bot,notes,"conversation_turn");
        checkRequest(model.requests.at(start + 1),bot,notes,"conversation_turn");
        check(store.tasks().size() == taskCount && t3.commands.size() == commands && store.task(task) == before,
            "explaining a result must preserve the Task and its Runs without starting any new work");
    };

    const auto firstIntake = bots.get("first").intake("");
    const auto firstTask = completeTask(firstIntake,expectedNotes(first),std::string(conclusion) + " The Session returned the required evidence.");
    checkResultAnswer(firstIntake,firstTask,askAboutResult(firstIntake,firstTask,"original-notes"),
                      expectedNotes(first),std::string(conclusion) + " " + explanation);
    const auto secondIntake = bots.get("second").intake("");
    const auto secondTask = completeTask(secondIntake,expectedNotes(second),"The Session returned the required evidence.");
    checkResultAnswer(secondIntake,secondTask,askAboutResult(secondIntake,secondTask,"second-bot"),expectedNotes(second),explanation);

    const auto edited = askAboutResult(firstIntake,firstTask,"edited-notes");
    first = {"Use the verified report.","Lead with the evidence."};
    write(folder / "first" / "SOUL.md",first.soul);
    const auto stored = expectedNotes(first);
    store.memory({"fixture",true},"first","apply_memory_edit",{{"baseVersion",0},{"content",stored.memory}});
    checkResultAnswer(firstIntake,firstTask,edited,stored,std::string(explanation) + " " + conclusion);
    const auto deleted = askAboutResult(firstIntake,firstTask,"deleted-notes");
    std::filesystem::remove(folder / "first" / "SOUL.md");
    std::filesystem::remove(folder / "first" / "MEMORY.md");
    std::filesystem::create_directory(folder / "first" / "MEMORY.md");
    checkResultAnswer(firstIntake,firstTask,deleted,{"",stored.memory},std::string(explanation) + " " + conclusion);

    const auto beforeFailure = model.requests.size();
    const auto originalRequest = "Collect the missing release evidence";
    noteReadError = std::string("Unable to read SOUL.md: ") + static_cast<char>(0xff);
    store.receive(firstIntake,"unreadable-notes",originalRequest);
    runtime.runConversations();
    const auto failure = store.conversation(firstIntake.botId).back().at("data");
    check(model.requests.size() == beforeFailure && store.tasks(firstIntake.botId).size() == 1 &&
          failure.at("route") == "error" &&
          failure.at("text").get_ref<const std::string&>().find("SOUL.md") != std::string::npos,
          "an unreadable note must prompt repair without calling the model or creating work");

    noteReadError.reset();
    store.receive(firstIntake,"repaired-notes","Please retry",failure.at("messageId"));
    runtime.runConversations();
    const auto tasks = store.tasks(firstIntake.botId);
    check(model.requests.size() == beforeFailure + 2 &&
          tasks.size() == 2 && tasks.front().at("request").get_ref<const std::string&>().starts_with(originalRequest),
          "replying after note repair must let the Bot retry the original request");
}

void directionalPeerConfiguration() {
    Sandbox temp;
    const Bots bots(temp.path);
    auto release = botConfig();
    release["allowedBots"] = Json::array({"verifier","missing"});
    createBot(temp.path / "release",release,"Own releases","Release-only memory");
    auto verifier = botConfig("verification-model");
    verifier["name"] = "Verifier";
    verifier["environmentId"] = "verification-environment";
    verifier["projectId"] = "verification-project";
    verifier["session"]["runtimeMode"] = "approval-required";
    createBot(temp.path / "verifier",verifier,"Verify changes","Verifier-only memory");

    const auto peers = bots.peers("release");
    check(peers.size() == 1 && peers[0].name == "Verifier" && peers[0].intake.botId == "verifier",
        "allowed peers must resolve to configured destinations");
    const auto& intake = peers[0].intake;
    check(intake.environmentId == "verification-environment" && intake.projectId == "verification-project" &&
        intake.config["model"] == "verification-model" && intake.config["session"]["runtimeMode"] == "approval-required",
        "receiver must use its own route and settings");
    check(bots.peers("verifier").empty(),"grant must be directional and default to empty");

    release["allowedBots"] = Json::array();
    write(temp.path / "release" / "config.json",release.dump());
    check(bots.peers("release").empty(),"resolver must observe changes to grants");
    release["allowedBots"] = Json::array({"../verifier"});
    write(temp.path / "release" / "config.json",release.dump());
    rejects<Conflict>([&] { bots.get("release"); },"invalid peer ID accepted");
}

void responseContract() {
    ModelInput input{
        {{"doneWhen","An existing requirement"}},
        {{"kind","result"}},Json::array(),{"New SOUL","Current MEMORY"},
        {"follow_up","delegate","complete","ask_user"},"",Json::array(),
        {{"modelProvider","chatgpt"},{"model","fixture-slug"},{"secret","must-stay-local"}},
    };
    const auto request = responseRequest(input);
    check(request["model"] == "fixture-slug" && request["store"] == false && request["stream"] == true &&
        request["tool_choice"] == "required" && request["parallel_tool_calls"] == false,"request policy");
    const auto& tool = request["tools"].at(0);
    check(tool["type"] == "namespace" && tool["name"] == "flock" && tool["tools"].size() == 4,"only allowed tools grouped in namespace");
    check(!tool["tools"][1]["parameters"]["properties"].contains("doneWhen"),"later delegate must omit doneWhen");
    check(request.dump().find("must-stay-local") == std::string::npos,"adapter config leaked into the prompt");
    const auto context = requestContext(request);
    check(context["task"]["doneWhen"] == "An existing requirement" && context["soul"] == "New SOUL" &&
        context["memory"] == "Current MEMORY","missing model context");
    check(request.at("instructions").get<std::string>().find("when writing your report") != std::string::npos,
          "the task provider prompt must apply the notes to the Bot's own report");

    input.task["doneWhen"] = "";
    input.task["requestedDoneWhen"] = "Use independently verified measurements";
    input.tools = {"delegate","ask_user"};
    const auto initial = responseRequest(input);
    check(initial["tools"][0]["tools"][0]["parameters"]["required"] ==
        Json::array({"reason","instruction","doneWhen"}),"initial delegate requires a saved criterion");
    check(requestContext(initial).at("task").at("requestedDoneWhen") == "Use independently verified measurements" &&
          initial.at("instructions").get_ref<const std::string&>().find("requestedDoneWhen") != std::string::npos,
          "the initial work prompt must direct the Bot to use the requested completion criteria");

    input.tools = {"start_task","send_to_task","change_task","answer_task_question","cancel_task",
                   "message_session","interrupt_session","read_result","find_tasks","reply","list_grants",
                   "request_access","list_secrets","request_secret_input","list_connections",
                   "check_environment_readiness","update_bot_settings","request_bot_change",
                   "read_memory","propose_memory_edit","apply_memory_edit","revert_memory_edit","memory_history",
                   "create_schedule","list_schedules","remove_schedule","create_reminder","create_trigger"};
    input.event = {{"kind","conversation_turn"},{"text","Create a sales report in CSV format"},{"turn",Json::array()}};
    const auto turn = responseRequest(input);
    const auto turnTool = [&](std::string_view name) -> const Json& {
        for (const auto& function : turn.at("tools").at(0).at("tools"))
            if (function.at("name") == name) return function.at("parameters");
        throw std::runtime_error("Missing conversation tool: " + std::string(name));
    };
    check(turn.at("tools").at(0).at("tools").size() == 28 &&
          turnTool("request_access")["required"] == Json::array({"reason","resourceKind","resourceId","operation"}) &&
          turnTool("request_secret_input")["required"] == Json::array({"reason","name"}) &&
          turnTool("message_session")["required"] == Json::array({"reason","taskId","generation","text"}) &&
          turnTool("start_task")["required"] == Json::array({"reason","request"}) &&
          turnTool("start_task")["properties"]["resume"]["type"] == "boolean" &&
          turnTool("change_task")["required"] == Json::array({"reason","taskId","generation","revisionId","instruction"}) &&
          turnTool("change_task")["properties"]["revisionId"]["minimum"] == 0 &&
          turnTool("send_to_task")["properties"]["generation"]["minimum"] == 0 &&
          turnTool("reply")["properties"]["suggestions"]["maxItems"] == 4,
          "conversation provider request must offer the conversation tools");
    for (auto name : {"list_grants","list_secrets","list_connections","check_environment_readiness"})
        check(turnTool(name).at("required")==Json::array({"reason"}),"unexpected mandatory access-tool parameter");
    check(turnTool("update_bot_settings").at("required")==Json::array({"reason","environmentId","runTimeoutSeconds"}) &&
          turnTool("update_bot_settings").at("properties").at("runTimeoutSeconds").at("maximum")==2147483647 &&
          turnTool("request_bot_change").at("required")==Json::array({"reason","operation","targetBotId","name"}) &&
          turnTool("request_bot_change").at("properties").at("operation").at("enum")==Json::array({"create_bot","rename_bot"}) &&
          turnTool("request_access").at("properties").at("resourceKind").at("enum").size()==resourceOperations().size(),
          "access request and settings schemas do not match dispatch");
    const std::map<std::string,Json> memoryAutomationRequired{
        {"read_memory",{"reason"}}, {"memory_history",{"reason"}},
        {"propose_memory_edit",{"reason","baseVersion","content","rationale"}},
        {"apply_memory_edit",{"reason","baseVersion"}},
        {"revert_memory_edit",{"reason","baseVersion","revisionId"}},
        {"create_schedule",{"reason","request","cron","timezone"}}, {"list_schedules",{"reason"}},
        {"remove_schedule",{"reason","id"}}, {"create_reminder",{"reason","request","fireAt","timezone"}},
        {"create_trigger",{"reason","request","source"}}};
    for (const auto& [name,required] : memoryAutomationRequired)
        check(turnTool(name).at("required") == required,"memory or automation schema does not match dispatch");
    check(turnTool("apply_memory_edit")["properties"]["baseVersion"]["minimum"] == 0 &&
          turnTool("create_trigger")["properties"]["eventFilter"]["type"] == "object","incorrect schema constraints");
    const auto& turnPrompt = turn.at("instructions").get_ref<const std::string&>();
    check(turnPrompt.find("You are a Flock Bot talking with your user") != std::string::npos &&
          turnPrompt.find("SOUL (soul) and MEMORY (memory)") != std::string::npos &&
          turnPrompt.find("Only the user's messages can authorize work") != std::string::npos,
          "conversation instructions must scope Bot notes to the user's authorized request");
    check(turn.dump().find("must-stay-local") == std::string::npos,"conversation adapter config leaked into the prompt");
    input.event = {{"kind","intake"}};

    input.task["peers"] = Json::array({{{"botId","verifier"},{"name","Verifier"}}});
    input.tools = {"send_bot_message","reply_to_bot","read_bot_evidence"};
    const auto peerRequest = responseRequest(input);
    const auto peerContext = Json::parse(peerRequest["input"][0]["content"].get<std::string>());
    const auto& destinations = peerContext.at("task").at("peers");
    check(destinations.size() == 1 && destinations.at(0).at("botId") == "verifier" &&
        destinations.at(0).at("name") == "Verifier",
        "the model did not receive the allowed peer destination");
    check(peerRequest.at("tools").at(0).at("tools").size() == 3,"unavailable tools entered the peer request");
    const auto parameters = [](const Json& request, std::string_view name) -> const Json& {
        for (const auto& function : request.at("tools").at(0).at("tools"))
            if (function.at("name") == name) return function.at("parameters");
        throw std::runtime_error("Missing model tool: " + std::string(name));
    };
    using Names = std::multiset<std::string>;
    const auto& send = parameters(peerRequest,"send_bot_message");
    check(send["properties"]["kind"]["enum"].get<Names>() == Names{"consult","work"} &&
        send["properties"]["doneWhen"]["type"] == "string" &&
        !send["properties"].contains("forwardUserMessage") &&
        send["required"].get<Names>() == Names{"reason","botId","kind","text"},
        "first peer work may set a criterion while consultation only needs a question");
    const auto& reply = parameters(peerRequest,"reply_to_bot");
    check(reply["properties"]["kind"]["enum"].get<Names>() == Names{"question","answer","result","failed"} &&
        reply["required"].get<Names>() == Names{"reason","kind","text"},
        "peer replies must specify their kind and text");
    const auto& evidence = parameters(peerRequest,"read_bot_evidence");
    check(evidence["properties"]["messageId"]["type"] == "string" &&
        evidence["required"].get<Names>() == Names{"reason","messageId"},
        "peer evidence must identify the message that carries it");
    input.task["doneWhen"] = "Saved work acceptance";
    const auto nextPeerRequest = responseRequest(input);
    check(!parameters(nextPeerRequest,"send_bot_message")["properties"].contains("doneWhen"),
        "later peer work must keep the saved criterion");
    input.task["forwardableUserMessage"] = {{"text","Saved user message"},{"time",123}};
    const auto forwardRequest = responseRequest(input);
    const auto& forward = parameters(forwardRequest,"send_bot_message");
    check(forward["properties"]["forwardUserMessage"]["type"] == "boolean" &&
          forward["required"].get<Names>() == Names{"reason","botId","kind"},
          "forwarding a saved user message must support an optional Bot note");

    const Json call = {{"type","function_call"},{"name","complete"},{"namespace","flock"},{"status","completed"},
        {"arguments",R"({"reason":"Evidence provided","report":"Done"})"}};
    input.tools = {"complete"};
    const auto completeRequest = responseRequest(input);
    const auto valid = responseDecision(Json::array({{{"type","reasoning"}},call}),completeRequest);
    check(valid.tool == "complete" && valid.reason == "Evidence provided" && valid.args == Json{{"report","Done"}},
        "Responses call did not map to core decision");
    rejects<ModelError>([&] { responseDecision(Json::array({call,call}),completeRequest); },"multiple tools accepted");
    rejects<ModelError>([&] { responseDecision(Json::array({call}),forwardRequest); },"state-invalid tool accepted");
    rejects<ModelError>([&] { responseDecision(Json::array(),completeRequest); },"missing tool accepted");
}

class AcceptanceModel final : public Model {
public:
    std::vector<Json> requests;
    Decision decide(const ModelInput& input) override {
        requests.push_back(responseRequest(input));
        if (input.task["botId"] == "regression") {
            return {"follow_up",{{"instruction","Add the required test"}},"A regression test is missing."};
        }
        return {"complete",{{"report","Existing tests passed"}},"The required existing tests passed."};
    }
};

void acceptanceRunner() {
    AcceptanceModel model;
    std::stringstream output;
    check(runModelAcceptance(model,"model-slug",output),"expected six successful trials");
    check(model.requests.size() == 6 && model.requests[0] == model.requests[1] && model.requests[1] == model.requests[2] &&
        model.requests[3] == model.requests[4] && model.requests[4] == model.requests[5],"trial inputs should be fixed per Bot");
    std::string line;
    unsigned count = 0;
    while (std::getline(output,line)) {
        const auto trial = Json::parse(line);
        ++count;
        check(trial["pass"] == true && !trial["reason"].get<std::string>().empty() &&
            trial["sessionReport"] == "All existing tests passed. No regression test was added.",
            "each trial must record the same report and its rationale");
    }
    check(count == 6,"six trial rows were not recorded");
}

void fixedDemoWithUserCriteriaAndContinuation() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    store.access().resourceGrant({"fixture",true},"demo",{"environment","local","launch"},GrantChange::grant);
    struct ChangeModel final : Model {
        std::string tool;
        Json args;
        Decision decide(const ModelInput& input) override {
            if (!input.event.at("turn").empty()) return {"reply",{{"text","Saved."}},"Confirm"};
            auto call = args;
            for (const auto& task : input.task.at("tasks")) {
                if (task.at("taskId") != call.at("taskId")) continue;
                call["generation"] = task.at("generation");
                if (tool == "change_task") call["revisionId"] = task.at("revisionId");
            }
            call["instruction"] = input.event.at("text");
            return {tool,call,"Apply the fixture's request"};
        }
    } changes;
    auto t3 = makeDemoT3();
    const auto demo = makeDemoModel();
    const auto notes = [](auto) { return std::string{}; };
    const Intake intake{"demo","Measure the result","local","project"};
    const auto tickUntil = [](Runtime& runtime,const auto& ready,std::string_view message) {
        for (int step = 0; step < 12 && !ready(); ++step) runtime.tick();
        check(ready(),message);
    };
    const auto task = store.submit(intake);
    {
        Runtime routing(store,changes,*t3,notes);
        changes.tool = "change_task";
        changes.args = {{"taskId",task},{"doneWhen","Confirm the output"}};
        store.receive(intake,"criteria","Completion criteria: Confirm the output");
        routing.runConversations();
    }
    check(store.task(task)["doneWhen"] == "" && store.task(task)["requestedDoneWhen"] == "Confirm the output",
          "the requested criteria was prematurely marked as fixed");
    {
        Runtime runtime(store,*demo,*t3,notes);
        tickUntil(runtime,[&] { return store.task(task)["state"] == "running"; },"the fixed demo never started");
    }
    const auto first = store.task(task);
    check(first["doneWhen"] == "Confirm the output" && first["remaining"] == 3 && first["runs"].size() == 1,
          "the fixed demo failed to delegate with the user's provisional criteria");
    {
        Runtime routing(store,changes,*t3,notes);
        changes.tool = "send_to_task";
        changes.args = {{"taskId",task}};
        store.receive(intake,"follow-up","Include the exact sample count");
        routing.runConversations();
    }
    {
        Runtime runtime(store,*demo,*t3,notes);
        tickUntil(runtime,[&] { return store.task(task)["state"] == "completed"; },
                  "the fixed demo did not finish its continuation");
    }
    const auto runs = store.task(task).at("runs");
    check(runs.size() == 2 && runs[0]["threadId"] == runs[1]["threadId"] && runs[0]["runId"] != runs[1]["runId"],
          "the fixed demo follow-up started another Session");
    Command followUp;
    followUp.tool = "follow_up";
    followUp.actionId = "persisted-demo-action";
    followUp.target = {runs[0]["threadId"],runs[0]["runId"],""};
    const auto sent = t3->send(followUp);
    const auto reconciled = t3->reconcile(followUp);
    check(sent.run.threadId == followUp.target.threadId && sent.run.runId == reconciled.run.runId &&
          sent.run.threadId == reconciled.run.threadId,"fixed demo reconciliation selected a different Run");
}

void fixedDemoFollowUpKeepsTheExplainedResult() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    store.access().resourceGrant({"fixture",true},"demo",{"environment","local","launch"},GrantChange::grant);
    const auto demo = makeDemoModel();
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return std::string{}; });
    const Intake intake{"demo","Measure the result","local","project"};
    const auto task = store.submit(intake);
    for (int i = 0; i < 12 && store.task(task)["state"] != "completed"; ++i) runtime.tick();
    const auto result = store.latestTaskResult(task).at("id").get<Id>();
    const auto replyTo = [&](Id message) {
        for (const auto& row : store.conversation("demo"))
            if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message) return row["data"];
        return Json(nullptr);
    };
    const auto question = store.receive(intake,"explain","What did it find?",result).at("messageId").get<Id>();
    runtime.runConversations();
    const auto explanation = replyTo(question);
    const auto again = store.receive(intake,"again","Explain again",explanation.at("messageId").get<Id>())
        .at("messageId").get<Id>();
    runtime.runConversations();
    check(store.tasks("demo").size() == 1 && replyTo(again).at("text") == explanation.at("text"),
          "a follow-up to the fixed demo's explanation started work instead of reading the same result");
}

void fixedDemoAnswersAnAnchoredQuestion() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    store.access().resourceGrant({"fixture",true},"demo",{"environment","local","launch"},GrantChange::grant);
    const auto demo = makeDemoModel();
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return std::string{}; });
    const Intake intake{"demo","ask: Which dataset should I use?","local","project"};
    const auto task = store.submit(intake);
    for (int i = 0; i < 12 && store.task(task)["state"] != "waiting"; ++i) runtime.tick();
    const auto question = store.task(task).at("wait").at("messageId").get<Id>();

    store.receive(intake,"dataset-answer","Use the release dataset",question);
    runtime.runConversations();
    for (int i = 0; i < 12 && store.task(task)["state"] != "completed"; ++i) runtime.tick();
    const auto history = store.history(task);
    check(store.task(task)["state"] == "completed" &&
          std::ranges::any_of(history,[](const Json& event) {
              return event["kind"] == "user_answer" && event["data"]["text"] == "Use the release dataset";
          }),"the fixed demo did not pass the anchored answer to its task");
}

void botProfilesAndAvatars() {
    Sandbox temp;
    Bots bots(temp.path / "bots");

    // Create a Bot with role template and shape avatar without execution settings.
    const auto created = bots.create({
        {"name","Scout"},
        {"role","reviewer"},
        {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}},
    });
    check(created.at("id") == "scout" && created.at("name") == "Scout" && created.at("role") == "reviewer" &&
          created.at("avatar").at("kind") == "shape" && created.at("avatar").at("example") == "sprout" &&
          created.at("avatar").at("color") == "#69c9ac" && !created.at("soul").get<std::string>().empty(),
          "created Bot did not persist name, role starter SOUL, and shape avatar");

    // Duplicate name generates a unique slug ID; non-ASCII name falls back safely.
    const auto duplicate = bots.create({
        {"name","Scout"},
        {"role","custom"},
        {"avatar",{{"kind","icon"},{"icon","sparkles"},{"color","#929dee"}}},
    });
    check(duplicate.at("id") == "scout-2" && duplicate.at("soul").get<std::string>().empty() &&
          duplicate.at("avatar").at("kind") == "icon" && duplicate.at("avatar").at("icon") == "sparkles",
          "duplicate Bot slug or icon avatar failed");

    const auto japanese = bots.create({
        {"name","レビュー担当"},
        {"role","builder"},
        {"avatar",{{"kind","shape"},{"example","mug"},{"color","#e1dfdf"}}},
    });
    check(japanese.at("id") == "bot" && japanese.at("name") == "レビュー担当",
          "non-ASCII Bot name did not produce a valid ID");

    // Cards retain profile details even before execution settings are configured.
    const auto cards = bots.cards();
    check(cards.size() == 3,"all created Bots should appear in cards()");

    Store store(temp.path / "scheduler.db");
    bots.syncSchedules(store,utc(2026,11,21,0,0),[](std::string_view id,std::string_view err) {
        throw std::runtime_error(std::string(id) + ": " + std::string(err));
    });
    store.fireDueAutomations(utc(2026,11,21,0,2));

    // Upload a valid PNG avatar, preserve it on name-only edit, replace with GIF, then switch to icon.
    const std::string pngBytes = std::string("\x89PNG\r\n\x1a\n",8) + std::string(16,'\x01');
    const std::string gifBytes("GIF89a\x01\x00\x01\x00\x00\x00\x00;",14);
    const auto uploaded = bots.updateProfile("scout",{
        {"name","Scout Prime"},
        {"avatar",{{"kind","upload"},{"filename","../dir/photo.png"},{"color","#75b8e8"}}},
    },AvatarImage{"image/png",pngBytes});
    check(uploaded.at("name") == "Scout Prime" && uploaded.at("avatar").at("kind") == "upload" &&
          uploaded.at("avatar").at("filename") == "photo.png" &&
          !uploaded.at("avatar").at("version").get<std::string>().empty(),
          "uploaded PNG avatar metadata was not saved");
    const auto savedPng = bots.avatarImage("scout");
    check(savedPng && savedPng->contentType == "image/png" && savedPng->bytes == pngBytes,
          "stored PNG bytes did not match upload");

    // Rename without re-uploading keeps the existing image.
    const auto renamed = bots.updateProfile("scout",{
        {"name","Scout Renamed"},
        {"avatar",{{"kind","upload"},{"color","#75b8e8"}}},
    },std::nullopt);
    check(renamed.at("name") == "Scout Renamed" &&
          renamed.at("avatar").at("version") == uploaded.at("avatar").at("version"),
          "renaming Bot lost existing uploaded image");

    // Replacing with GIF deletes old PNG file and updates version hash.
    const auto replaced = bots.updateProfile("scout",{
        {"name","Scout Renamed"},
        {"avatar",{{"kind","upload"},{"filename","anim.gif"},{"color","#75b8e8"}}},
    },AvatarImage{"image/gif",gifBytes});
    check(replaced.at("avatar").at("version") != uploaded.at("avatar").at("version") &&
          !std::filesystem::exists(temp.path / "bots" / "scout" / "avatar.png") &&
          std::filesystem::exists(temp.path / "bots" / "scout" / "avatar.gif"),
          "replacing uploaded avatar did not clean up previous image file");
    const auto savedGif = bots.avatarImage("scout");
    check(savedGif && savedGif->contentType == "image/gif" && savedGif->bytes == gifBytes,
          "replaced GIF bytes did not match");

    // Switching back to shape removes uploaded file.
    const auto switched = bots.updateProfile("scout",{
        {"name","Scout Renamed"},
        {"avatar",{{"kind","shape"},{"example","barbell"},{"color","#ee8d85"}}},
    });
    check(switched.at("avatar").at("kind") == "shape" && !bots.avatarImage("scout").has_value() &&
          !std::filesystem::exists(temp.path / "bots" / "scout" / "avatar.gif"),
          "switching from upload to shape did not remove stored image file");

    // Reject SVG masquerading as image, unknown shape/icon, and empty name.
    rejects<Conflict>([&] {
        bots.updateProfile("scout",{
            {"name","Scout"},
            {"avatar",{{"kind","upload"}}},
        },AvatarImage{"image/svg+xml","<svg xmlns='http://www.w3.org/2000/svg'></svg>"});
    },"SVG payload accepted as avatar image");
    rejects<Conflict>([&] {
        bots.create({{"name","   "},{"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}}});
    },"blank Bot name accepted");
    rejects<Conflict>([&] {
        bots.create({{"name","Bad"},{"avatar",{{"kind","icon"},{"icon","unknown"},{"color","#69c9ac"}}}});
    },"unknown avatar icon accepted");

    // Existing execution config stays intact when editing profile.
    createBot(temp.path / "bots" / "release",botConfig(),"Release soul","Release memory");
    bots.updateProfile("release",{
        {"name","Release Bot"},
        {"avatar",{{"kind","icon"},{"icon","compass"},{"color","#eec767"}}},
    });
    const auto loaded = bots.get("release");
    check(loaded.name == "Release Bot" && loaded.environmentId == "env-canonical" &&
          loaded.projectId == "project-canonical" && loaded.avatar.at("icon") == "compass",
          "editing profile altered existing Bot execution configuration");

    // A name-only update keeps a hand-written avatar and leaves files the editor did not write.
    const auto customDir = temp.path / "bots" / "custom";
    std::filesystem::create_directories(customDir);
    const Json customAvatar = {{"definition",{{"id","badge"},{"body",{{"path","M0 0h10v10z"}}}}},{"color","#123456"},{"eyeColor","#0000ff"}};
    { std::ofstream(customDir / "config.json") << Json{{"name","Custom"},{"avatar",customAvatar}}.dump(); }
    { std::ofstream(customDir / "avatar.png") << pngBytes; }
    const auto customRenamed = bots.updateProfile("custom",{{"name","Custom Two"}});
    check(customRenamed.at("name") == "Custom Two" && customRenamed.at("avatar") == customAvatar,
          "name-only update replaced a custom avatar");
    bots.updateProfile("custom",{
        {"name","Custom Two"},
        {"avatar",{{"kind","shape"},{"example","mug"},{"color","#69c9ac"}}},
    });
    check(std::filesystem::exists(customDir / "avatar.png"),"shape save deleted an image the avatar did not reference");

    // A save that would exceed the config read limit fails before the file changes.
    const auto bigDir = temp.path / "bots" / "big";
    std::filesystem::create_directories(bigDir);
    const auto bigText = Json{{"name","Big"},{"notes",std::string(128 * 1024 - 40,'x')}}.dump();
    { std::ofstream(bigDir / "config.json") << bigText; }
    rejects<Conflict>([&] {
        bots.updateProfile("big",{{"name","Big"},{"avatar",{{"kind","icon"},{"icon","compass"},{"color","#eec767"}}}});
    },"oversized config save accepted");
    std::ifstream bigFile(bigDir / "config.json");
    check(std::string(std::istreambuf_iterator<char>(bigFile),{}) == bigText,"rejected save changed config.json");
}
}

int main() {
    try {
        cronSyntaxAndTimeZones();
        cronIntakeAndRestart();
        configuredScheduleLifecycle();
        notesAndRoutingOnRestart();
        notesAcrossConversationAndWork();
        directionalPeerConfiguration();
        responseContract();
        acceptanceRunner();
        fixedDemoWithUserCriteriaAndContinuation();
        fixedDemoAnswersAnAnchoredQuestion();
        fixedDemoFollowUpKeepsTheExplainedResult();
        botProfilesAndAvatars();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
