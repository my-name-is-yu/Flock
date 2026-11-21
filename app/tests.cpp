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
            if (event.at("kind") == "route_conversation") {
                const auto& reply = event.at("reply");
                Json route = {{"kind","create_task"}};
                if (reply.at("kind") == "result")
                    route = {{"kind","answer_result"},{"resultMessageId",reply.at("resultMessageId")}};
                if (reply.at("kind") == "clarification")
                    return {"route_conversation",{{"routes",Json::array({route})},{"resolution","resolved"},
                            {"clarificationMessageId",reply.at("messageId")}},"Retry the original request"};
                return {"route_conversation",{{"routes",Json::array({route})}},"Route the user request"};
            }
            if (event.at("kind") == "intake")
                return {"delegate",{{"instruction","Collect the required evidence"},{"doneWhen",criterion}},"Gather evidence"};

            const bool answer = event.at("kind") == "answer_conversation_result";
            check(answer || event.at("kind") == "result","unexpected model request");
            const auto& memory = context.at("memory").get_ref<const std::string&>();
            std::string text = answer ? explanation : "The Session returned the required evidence.";
            if (memory.starts_with("Lead with the conclusion.")) text = std::string(conclusion) + " " + text;
            if (memory.starts_with("Lead with the evidence.")) text += " " + std::string(conclusion);
            if (answer) return {"answer_conversation_result",{{"text",text}},"Explain the saved result"};
            return {"complete",{{"report",text}},"The Session evidence meets the criterion"};
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
        runtime.routeConversation();
        const auto tasks = store.tasks(bot.botId);
        check(tasks.size() == 1,"the request should create one Task for this Bot");
        const Id task = tasks.at(0).at("id");
        for (int step = 0; step < 10 && store.task(task).at("state") != "completed"; ++step) runtime.tick();
        const auto result = store.latestTaskResult(task);
        check(result.at("kind") == "completed" && result.at("data").at("text") == expectedReport &&
              result.at("data").at("doneWhen") == criterion && store.task(task).at("runs").size() == 1,
            "the Bot's completion report should apply its notes and preserve the saved criterion");
        check(model.requests.size() == start + 3,"expected routing, task intake, and completion requests");
        checkRequest(model.requests.at(start),bot,notes,"route_conversation");
        checkRequest(model.requests.at(start + 1),bot,notes,"intake");
        checkRequest(model.requests.at(start + 2),bot,notes,"result");
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
        const Id result = store.latestTaskResult(task).at("id");
        runtime.tick();
        const auto reply = store.conversation(bot.botId).back().at("data");
        check(reply.at("replyToMessageId") == question && reply.at("text") == expectedReply &&
              reply.at("source").at("messageId") == result,"the explanation should apply current notes to the selected result");
        check(model.requests.size() == start + 2,"expected routing and a separate explanation request");
        checkRequest(model.requests.at(start),bot,notes,"route_conversation");
        checkRequest(model.requests.at(start + 1),bot,notes,"answer_conversation_result");
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
    runtime.routeConversation();
    const auto clarifications = store.clarifications(firstIntake.botId);
    check(model.requests.size() == beforeFailure && store.tasks(firstIntake.botId).size() == 1 &&
          clarifications.size() == 1 &&
          clarifications.front().at("text").get_ref<const std::string&>().find("SOUL.md") != std::string::npos,
          "an unreadable note must prompt repair without calling the model or creating work");

    noteReadError.reset();
    store.receive(firstIntake,"repaired-notes","Please retry",clarifications.front().at("messageId"));
    runtime.routeConversation();
    const auto tasks = store.tasks(firstIntake.botId);
    check(model.requests.size() == beforeFailure + 1 && store.clarifications(firstIntake.botId).empty() &&
          tasks.size() == 2 && tasks.front().at("request").get_ref<const std::string&>().starts_with(originalRequest),
          "replying after note repair must retry the original request");
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

    input.tools = {"route_conversation"};
    input.event = {{"kind","route_conversation"},{"text","Create a sales report in CSV format"}};
    const auto routing = responseRequest(input);
    const auto& routingTool = routing["tools"][0]["tools"][0];
    check(routingTool["name"] == "route_conversation" &&
          routingTool["parameters"]["required"] == Json::array({"reason","routes"}),
          "conversation provider request must use the routing tool");
    const auto& routingPrompt = routing.at("instructions").get_ref<const std::string&>();
    check(routingPrompt.find("You are the Flock Bot routing") != std::string::npos &&
          routingPrompt.find("current SOUL (soul) and MEMORY (memory)") != std::string::npos &&
          routingPrompt.find("Only event.text") != std::string::npos,
          "routing instructions must scope Bot notes to the user's authorized request");
    check(routing.dump().find("must-stay-local") == std::string::npos,
          "conversation adapter config leaked into the prompt");
    input.tools = {"answer_conversation_result","clarify_conversation_result"};
    input.event = {{"kind","answer_conversation_result"},{"text","Explain the selected option"},
                   {"source",{{"text","Selected evidence"}}}};
    const auto resultAnswer = responseRequest(input);
    const auto& answerTool = resultAnswer["tools"][0]["tools"][0];
    check(answerTool["name"] == "answer_conversation_result" &&
          answerTool["parameters"]["required"] == Json::array({"reason","text"}),
          "result provider request must use the answer tool");
    const auto& clarificationTool = resultAnswer["tools"][0]["tools"][1];
    check(clarificationTool["name"] == "clarify_conversation_result" &&
          clarificationTool["parameters"]["required"] == Json::array({"reason","text"}),
          "result provider request must support clarification for insufficient evidence");
    const auto answerContext = requestContext(resultAnswer);
    check(answerContext["event"] == input.event,"result provider request lost the question or selected evidence");
    const auto& answerPrompt = resultAnswer.at("instructions").get_ref<const std::string&>();
    check(answerPrompt.find("Answer event.text in the user's language") != std::string::npos &&
          answerPrompt.find("current SOUL (soul) and MEMORY (memory)") != std::string::npos &&
          answerPrompt.find("Ground claims about the result in event.source.text") != std::string::npos &&
          resultAnswer.dump().find("must-stay-local") == std::string::npos,
          "result instructions must apply Bot notes within the selected evidence and keep adapter config private");

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
    struct ChangeRouter final : Model {
        Json route;
        Decision decide(const ModelInput&) override {
            return {"route_conversation",{{"routes",Json::array({route})}},"Apply the fixture's request"};
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
    {
        Runtime runtime(store,*demo,*t3,notes);
        store.receive(intake,"bare-status","progress");
        runtime.routeConversation();
        check(store.tasks("demo").empty() && store.conversation("demo").back()["data"]["route"] == "status",
              "bare progress started a new Task in the fixed demo");
    }
    const auto task = store.submit(intake);
    {
        Runtime routing(store,changes,*t3,notes);
        changes.route = {{"kind","revise_task"},{"taskId",task},{"doneWhen","Confirm the output"}};
        store.receive(intake,"criteria","Completion criteria: Confirm the output");
        routing.routeConversation();
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
        changes.route = {{"kind","append_instruction"},{"taskId",task}};
        store.receive(intake,"follow-up","Include the exact sample count");
        routing.routeConversation();
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

void fixedDemoCancelsAnAnchoredQuestion() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    const auto demo = makeDemoModel();
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return BotNotes{}; });
    const Intake intake{"demo","ask: Which dataset should I use?","local","project"};
    const auto task = store.submit(intake);
    for (int i = 0; i < 12 && store.task(task)["state"] != "waiting"; ++i) runtime.tick();
    const auto question = store.task(task).at("wait").at("messageId").get<Id>();

    store.receive(intake,"cancel-answer","Cancel this task",question);
    runtime.routeConversation();
    for (int i = 0; i < 12 && store.task(task)["state"] != "cancelled"; ++i) runtime.tick();
    const auto history = store.history(task);
    check(store.task(task)["state"] == "cancelled" && store.task(task)["runs"].empty() &&
          std::ranges::none_of(history,[](const Json& event) { return event["kind"] == "user_answer"; }),
          "the fixed demo answered its question instead of cancelling the task");
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
        notesAndRoutingOnRestart();
        notesAcrossConversationAndWork();
        directionalPeerConfiguration();
        responseContract();
        acceptanceRunner();
        fixedDemoWithUserCriteriaAndContinuation();
        fixedDemoCancelsAnAnchoredQuestion();
        botProfilesAndAvatars();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
