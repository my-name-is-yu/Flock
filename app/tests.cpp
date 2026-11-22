#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <fcntl.h>
#include <sys/wait.h>
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
    const Cron local("0 9 * * 1-5","Asia/Tokyo");
    check(local.matches(utc(2026,11,20,0,0)), "weekday in the configured IANA timezone");
    check(!local.matches(utc(2026,11,21,0,0)), "skip Saturday");
    const Cron listsAndSteps("5,10-22/4 2/3 * 11 7","Etc/UTC");
    check(listsAndSteps.matches(utc(2026,11,22,5,14)), "list/range/step/Sunday alias");
    check(!listsAndSteps.matches(utc(2026,11,22,5,15)), "minute step mismatch");
    const Cron eitherDay("0 0 21 * 1","Etc/UTC");
    check(eitherDay.matches(utc(2026,11,21,0,0)), "day-of-month alternative");
    check(eitherDay.matches(utc(2026,11,23,0,0)), "day-of-week alternative");
    check(!eitherDay.matches(utc(2026,11,22,0,0)), "neither eligible day");
    rejects<Conflict>([] { Cron("1 2 * * * *","Etc/UTC"); },"six fields accepted");
    rejects<Conflict>([] { Cron("*/0 2 * * *","Etc/UTC"); },"zero divisor accepted");
    rejects<Conflict>([] { Cron("0 24 * * *","Etc/UTC"); },"hour 24 accepted");
    rejects<Conflict>([] { Cron("1,,2 0 * * *","Etc/UTC"); },"empty list element accepted");
    rejects<Conflict>([] { Cron("* * * * *","Not/A_Zone"); },"unknown time zone accepted");
}

void cronCardsAndExecutionShareValidation() {
    Sandbox temp;
    const auto directory = temp.path / "bots" / "release";
    auto config = botConfig();
    config["avatar"] = "mug";
    config["cron"] = Json::array({
        {{"id","daily"},{"schedule","0 * * * *"},{"timezone","Etc/UTC"},{"request","Check release"}}
    });
    createBot(directory,config,"Release","");
    Bots bots(temp.path / "bots");
    check(bots.cards().at(0).at("cron") == config["cron"] && !bots.cards().at(0).contains("avatar"),"card must use canonical schedules and structured avatars");
    config["cron"].push_back(config["cron"].front());
    write(directory / "config.json",config.dump());
    Store store(temp.path / "state.db");
    unsigned failures = 0;
    Scheduler scheduler(bots,store,0,[&](auto,auto) { ++failures; });
    scheduler.tick(0);
    check(bots.cards().at(0).at("cron").empty() && failures == 1 && store.tasks().empty(),
          "duplicate schedule IDs must invalidate the same schedules for cards and the scheduler");
    config["cron"].erase(1);
    config["cron"][0]["timezone"] = "Invalid/Zone";
    write(directory / "config.json",config.dump());
    scheduler.tick(60);
    check(bots.cards().at(0).at("cron").empty() && failures == 2 && store.tasks().empty(),
          "an invalid timezone must be unavailable in cards and the scheduler");
    config["cron"] = Json::array();
    config["modelProvider"] = "unsupported";
    write(directory / "config.json",config.dump());
    rejects<Conflict>([&] { bots.get("release"); },"unsupported model provider loaded as executable config");
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
        Scheduler spring(bots,store,utc(2026,3,8,6,0),report);
        spring.tick(utc(2026,3,8,9,0));
        check(store.tasks().empty(),"nonexistent spring local time was submitted");
        Scheduler fall(bots,store,utc(2026,11,1,5,0),report);
        fall.tick(utc(2026,11,1,7,0));
        fall.tick(utc(2026,11,1,7,0));
        const auto tasks = store.tasks();
        check(tasks.size() == 2,"both fall-back instants should submit exactly once");
        check(tasks[0]["scheduledAt"] == utc(2026,11,1,6,30) &&
              tasks[1]["scheduledAt"] == utc(2026,11,1,5,30),"fall offsets did not produce separate UTC instants");
        check(tasks[0]["blockedBy"]["id"] == tasks[1]["id"],"cron must participate in the same project queue");
        Scheduler repeated(bots,store,utc(2026,11,1,6,30),report);
        repeated.tick(utc(2026,11,1,6,30));
        check(store.tasks().size() == 2,"duplicate scheduled UTC instant registered after restart");
    }
    {
        Store store(db);
        Scheduler resumed(bots,store,utc(2027,11,1,5,30,1),report);
        resumed.tick(utc(2027,11,1,5,35));
        check(store.tasks().size() == 2,"downtime schedule should be skipped and existing rows preserved");
    }
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
    bool finished = false, settingsError = false;
    std::string authGeneration(const std::string&,const Json&) override { return "test-auth"; }
    Delivery send(const Command& command) override {
        commands.push_back(command);
        if (settingsError) return {Delivery::Kind::Settings,{},"Invalid fixture session settings",true};
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
    const auto notes = [&](std::string_view bot) { return bots.notes(bot); };
    NotesModel model;
    NotesT3 t3;
    Id firstId, secondId, waitId;
    {
        Store store(db);
        Runtime runtime(store,model,t3,notes);
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
        Store store(db);
        Runtime runtime(store,model,t3,notes);
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
        Store store(db);
        Runtime runtime(store,model,t3,notes);
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
    Store store(temp.path / "tasks.db");
    NotesT3 t3;
    t3.finished = true;
    std::optional<std::string> noteReadError;
    Runtime runtime(store,model,t3,[&](std::string_view botId) {
        if (noteReadError) throw Conflict(*noteReadError);
        auto notes = bots.notes(botId);
        if (!notes.soul.empty()) notes.soul += std::string(64,'X');
        if (!notes.memory.empty()) notes.memory += std::string(64,'X');
        return notes;
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
    write(folder / "first" / "MEMORY.md",first.memory);
    checkResultAnswer(firstIntake,firstTask,edited,expectedNotes(first),std::string(explanation) + " " + conclusion);
    const auto deleted = askAboutResult(firstIntake,firstTask,"deleted-notes");
    std::filesystem::remove(folder / "first" / "SOUL.md");
    std::filesystem::remove(folder / "first" / "MEMORY.md");
    checkResultAnswer(firstIntake,firstTask,deleted,{},explanation);

    const auto beforeFailure = model.requests.size();
    const auto originalRequest = "Collect the missing release evidence";
    noteReadError = std::string("Unable to read SOUL.md: ") + static_cast<char>(0xff);
    store.receive(firstIntake,"unreadable-notes",originalRequest);
    runtime.runConversations();
    const auto failure = store.conversation(firstIntake.botId).back().at("data");
    check(model.requests.size() == beforeFailure && store.tasks(firstIntake.botId).size() == 1 &&
          failure.at("status") == "error" &&
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
                   "message_session","interrupt_session","read_result","find_tasks","reply"};
    input.event = {{"kind","conversation_turn"},{"text","Create a sales report in CSV format"},{"turn",Json::array()}};
    const auto turn = responseRequest(input);
    const auto turnTool = [&](std::string_view name) -> const Json& {
        for (const auto& function : turn.at("tools").at(0).at("tools"))
            if (function.at("name") == name) return function.at("parameters");
        throw std::runtime_error("Missing conversation tool: " + std::string(name));
    };
    check(turn.at("tools").at(0).at("tools").size() == 10 &&
          turnTool("message_session")["required"] == Json::array({"reason","taskId","generation","text"}) &&
          turnTool("start_task")["required"] == Json::array({"reason","request"}) &&
          turnTool("start_task")["properties"]["resume"]["type"] == "boolean" &&
          turnTool("change_task")["required"] == Json::array({"reason","taskId","generation","revisionId","instruction"}) &&
          turnTool("change_task")["properties"]["revisionId"]["minimum"] == 0 &&
          turnTool("send_to_task")["properties"]["generation"]["minimum"] == 0 &&
          turnTool("reply")["properties"]["suggestions"]["maxItems"] == 4,
          "conversation provider request must offer the conversation tools");
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
    const auto valid = responseDecision(Json::array({{{"type","reasoning"}},call}),{"complete"});
    check(valid.tool == "complete" && valid.reason == "Evidence provided" && valid.args == Json{{"report","Done"}},
        "Responses call did not map to core decision");
    rejects<ModelError>([&] { responseDecision(Json::array({call,call}),{"complete"}); },"multiple tools accepted");
    rejects<ModelError>([&] { responseDecision(Json::array({call}),{"follow_up"}); },"state-invalid tool accepted");
    rejects<ModelError>([] { responseDecision(Json::array(),{"complete"}); },"missing tool accepted");
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
    const auto notes = [](auto) { return BotNotes{}; };
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

void fixedDemoRequiresExplicitIntent() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    const auto demo = makeDemoModel();
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return BotNotes{}; });
    const Intake intake{"demo","Measure the result","local","project"};
    const auto send = [&](std::string_view key, std::string_view text, Id anchor = 0) {
        const auto message = store.receive(intake,key,text,anchor).at("messageId");
        runtime.runConversations();
        for (const auto& row : store.conversation("demo"))
            if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message) return row["data"];
        throw std::runtime_error("Missing demo reply");
    };
    const auto greeting = send("hello","Hello");
    check(greeting.at("suggestions") == Json::array({"Run a demo task"}) && store.tasks().empty(),
          "greeting created work or omitted the explicit demo action");
    send("start","Run a demo task",greeting.at("messageId"));
    const auto tasks = store.tasks();
    check(tasks.size() == 1,"the explicit demo action did not create one task");
    const auto task = tasks.front().at("id").get<Id>();
    for (int i = 0; i < 12 && store.task(task)["state"] != "completed"; ++i) runtime.tick();
    const auto result = store.latestTaskResult(task).at("id").get<Id>();
    const auto plain = send("thanks","Thanks",result);
    const auto explanation = send("explain","Explain this result",result);
    const auto other = send("wrong-target","Explain this result",plain.at("messageId"));
    check(store.tasks("demo").size() == 1 && plain.at("text") == greeting.at("text") &&
          other.at("actions").empty() && explanation.at("text") != plain.at("text"),
          "demo result explanation did not require intent and a result anchor");
}

void fixedDemoAnswersAnAnchoredQuestion() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    const auto demo = makeDemoModel("Which dataset should I use?");
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return BotNotes{}; });
    const Intake intake{"demo","Measure the result","local","project"};
    const auto task = store.submit(intake);
    for (int i = 0; i < 12 && store.task(task)["state"] != "waiting"; ++i) runtime.tick();
    check(store.task(task).at("wait").at("text") == "Which dataset should I use?",
          "the explicit demo question was not used");
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

    // Scheduler ignores Bots without cron schedules instead of failing on missing execution config.
    Store store(temp.path / "scheduler.db");
    Scheduler scheduler(bots,store,utc(2026,11,21,0,0),[](std::string_view id,std::string_view err) {
        throw std::runtime_error(std::string(id) + ": " + std::string(err));
    });
    scheduler.tick(utc(2026,11,21,0,2));

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
          loaded.projectId == "project-canonical",
          "editing profile altered existing Bot execution configuration");
    check(std::ranges::any_of(bots.cards(),[](const Json& card) {
        return card.at("id") == "release" && card.at("avatar").at("icon") == "compass";
    }),"edited Bot card did not retain the selected icon");

    // A name-only update keeps a hand-written avatar and leaves files the editor did not write.
    const auto customDir = temp.path / "bots" / "custom";
    std::filesystem::create_directories(customDir);
    const Json definition = {{"version",1},{"id","badge"},{"name","Badge"},
        {"body",{{"path","M10 10H90V90H10Z"}}},{"eyes",{{"x",50},{"y",50},{"spacing",20},{"scale",1}}}};
    const Json customAvatar = {{"kind","shape"},{"definition",definition},{"color","#123456"},{"eyeColor","#0000ff"}};
    { std::ofstream(customDir / "config.json") << Json{{"name","Custom"},{"avatar",customAvatar}}.dump(); }
    { std::ofstream(customDir / "avatar.png") << pngBytes; }
    const auto customRenamed = bots.updateProfile("custom",{{"name","Custom Two"}});
    check(customRenamed.at("name") == "Custom Two" && customRenamed.at("avatar") == customAvatar,
          "name-only update replaced a custom avatar");
    auto recolored = customAvatar;
    recolored["color"] = "#fedcba";
    check(bots.updateProfile("custom",{{"name","Custom Two"},{"avatar",recolored}}).at("avatar") == recolored,
          "recoloring a custom avatar replaced its definition");
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
void cliCommands(const std::filesystem::path& executable) {
    Sandbox home;
    const auto invoke = [&](std::initializer_list<std::string> cliArgs, int expectedStatus) {
        std::vector<std::string> args{executable.string(),"--home",home.path.string()};
        args.insert(args.end(),cliArgs);
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        const auto output = home.path / "cli-output";
        const auto child = fork();
        check(child >= 0,"could not start CLI fixture");
        if (child == 0) {
            const int fd = open(output.c_str(),O_WRONLY | O_CREAT | O_TRUNC,0600);
            if (fd < 0 || dup2(fd,STDOUT_FILENO) < 0 || dup2(fd,STDERR_FILENO) < 0) _exit(126);
            close(fd);
            execv(argv[0],argv.data());
            _exit(127);
        }
        int status = 0;
        check(waitpid(child,&status,0) == child,"could not collect CLI fixture");
        std::ifstream stream(output);
        std::ostringstream bytes;
        bytes << stream.rdbuf();
        const auto text = bytes.str();
        check(WIFEXITED(status) && WEXITSTATUS(status) == expectedStatus,"unexpected CLI result: " + text);
        return text;
    };
    const auto help = invoke({"--help"},0);
    check(help.find("--no-web") != std::string::npos && help.find("--headless") == std::string::npos &&
          invoke({"run","--fixed","--headless"},1).find("Unknown run option: --headless") != std::string::npos &&
          invoke({"run","--fixed","--no-web","--unexpected"},1).find("Unknown run option: --unexpected") != std::string::npos,
          "CLI should expose the canonical Web flag only");

    const auto directory = home.path / "bots" / "release";
    auto config = botConfig();
    createBot(directory,config,"","");
    const Bots bots(home.path / "bots");
    Store store(home.path / "flock.sqlite3");
    const auto first = store.submit(bots.get("release").intake("First"));
    const auto second = store.submit(bots.get("release").intake("Second"));
    check(Json::parse(invoke({"tasks","release","--before",std::to_string(second),"--limit","1"},0)).at(0).at("id") == first,
          "tasks did not apply its Bot filter and before cursor");
    const auto initial = store.history(first).at(0).at("id").get<Id>();
    check(Json::parse(invoke({"history",std::to_string(first),"--after",std::to_string(initial),"--limit","1"},0)).empty(),
          "history did not apply its exclusive after cursor");
    invoke({"tasks","--after","1"},1);
    invoke({"history",std::to_string(first),"--before","1"},1);

    NotesModel model;
    NotesT3 t3;
    t3.settingsError = true;
    Runtime runtime(store,model,t3,[](auto) { return BotNotes{}; });
    for (int i = 0; i < 8 && store.task(first).at("state") != "waiting"; ++i) runtime.tick();
    const auto waiting = store.task(first);
    const auto wait = waiting.at("wait").at("id").get<Id>();
    check(waiting.at("wait").at("reason") == "settings","CLI fixture did not reach an unsent settings wait");
    config["session"]["runtimeMode"] = "approval-required";
    write(directory / "config.json",config.dump());
    const auto stale = invoke({"reconcile",std::to_string(first),std::to_string(wait + 1)},1);
    check(stale.find("Waiting item is no longer current") != std::string::npos && store.task(first) == waiting,
          "CLI reconcile changed a stale wait");
    const auto reconciled = Json::parse(invoke({"reconcile",std::to_string(first),std::to_string(wait)},0));
    check(reconciled.at("config").at("session").at("runtimeMode") == "approval-required" &&
          reconciled.at("state") == "sending","CLI reconcile did not delegate settings refresh to the Store");
}

}

int main(int argc, char** argv) {
    try {
        cronSyntaxAndTimeZones();
        cronIntakeAndRestart();
        cronCardsAndExecutionShareValidation();
        notesAndRoutingOnRestart();
        notesAcrossConversationAndWork();
        directionalPeerConfiguration();
        responseContract();
        acceptanceRunner();
        fixedDemoWithUserCriteriaAndContinuation();
        fixedDemoAnswersAnAnchoredQuestion();
        fixedDemoRequiresExplicitIntent();
        botProfilesAndAvatars();
        const auto executable = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(argv[0]).parent_path() / "flock";
        cliCommands(executable);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
