#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"
#include "tests/fixtures.hpp"
#include "../tests/task_history.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using namespace flockv0;
using namespace flockv0::app;
using namespace flockv0::app::test;
using flockv0::test::lastTaskEvent;

namespace flockv0::app {
void sessionConversationDispatch(Store&, const Json&);
}

namespace {
class NotesModel final : public Model {
public:
    std::vector<ModelInput> inputs;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        inputs.push_back(input);
        if (input.event["kind"] == "result")
            return {"complete",{{"report","Original evidence confirmed"}},"The report meets the original acceptance criterion."};
        if (input.history.empty() && input.task["botId"] == "first")
            return {"ask_user",{{"question","Which branch?"}},"Only the user can select the branch."};
        const auto doneWhen = input.task["botId"] == "first" ? "Evidence required before edit" : "Second Bot criterion";
        return {"delegate",{{"instruction","Verify the agreed scope"},{"doneWhen",doneWhen}},"Run verification in T3."};
    }
};

class NotesT3 : public T3 {
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
        }
        return result;
    }
    InterruptOutcome interrupt(const std::string&,const Json&,const RunRef&) override { return InterruptOutcome::Requested; }
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
        store.operate(localOwner,TaskTarget::from(store.task(firstId),waitId),"answer",{{"text","main"}});
        rejects<Conflict>([&] { store.operate(localOwner,TaskTarget::from(store.task(firstId),waitId),"answer",{{"text","old answer"}}); },"duplicate answer accepted");
        runtime.tick();
        runtime.tick();
        check(store.task(firstId)["state"] == "running" && store.task(firstId)["doneWhen"] == "Evidence required before edit",
            "answer must reach the old task and initialize saved doneWhen");
        check(t3.commands.size() == 1 && t3.commands[0].environmentId == "env-canonical" &&
              t3.commands[0].projectId == "project-canonical","intake must preserve canonical IDs");
        check(t3.commands[0].config["session"]["model"] == "child-model" &&
              t3.commands[0].config["session"]["providerInstanceId"] == "child-provider" &&
              t3.commands[0].config["session"]["runtimeMode"] == "auto" &&
              t3.commands[0].config["session"]["workspaceStrategy"]["baseRef"] == "origin/main" &&
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
    return Json::parse(request.at("input").at(0).at("content").at(0).at("text").get<std::string>());
}

void notesAcrossConversationAndWork() {
    static constexpr auto criterion = "Confirm the required evidence";
    static constexpr auto conclusion = "The evidence meets the saved criterion.";
    static constexpr auto explanation = "The Session supplied the evidence required for completion.";
    struct ModelFixture final : Model {
        std::vector<Json> requests;
        ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
        Decision choose(const ModelInput& input) {
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
        store.receive(localOwner,bot,"new-work","Collect the validation evidence");
        runtime.runConversations();
        const auto tasks = store.tasks(bot.botId);
        check(tasks.size() == 1,"the request should create one Task for this Bot");
        const Id task = tasks.at(0).at("id");
        for (int step = 0; step < 10 && store.task(task).at("state") != "completed"; ++step) runtime.tick();
        const auto result = lastTaskEvent(store,task,"completed");
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
        const Id result = lastTaskEvent(store,task,"completed").at("id");
        return store.receive(localOwner,bot,key,"Why does that evidence support the result?",result).at("messageId");
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
    store.receive(localOwner,firstIntake,"unreadable-notes",originalRequest);
    runtime.runConversations();
    const auto failure = store.conversation(firstIntake.botId).back().at("data");
    check(model.requests.size() == beforeFailure && store.tasks(firstIntake.botId).size() == 1 &&
          failure.at("route") == "error" &&
          failure.at("text").get_ref<const std::string&>().find("SOUL.md") != std::string::npos,
          "an unreadable note must prompt repair without calling the model or creating work");

    noteReadError.reset();
    store.receive(localOwner,firstIntake,"repaired-notes","Please retry",failure.at("messageId"));
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
    check(bots.peers("verifier").empty(),"peer configuration must be directional and default to empty");

    release["allowedBots"] = Json::array();
    write(temp.path / "release" / "config.json",release.dump());
    check(bots.peers("release").empty(),"resolver must observe peer configuration changes");
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
        Json::array({"reason","instruction"}),"saved requested criteria must not require model authorship");
    check(requestContext(initial).at("task").at("requestedDoneWhen") == "Use independently verified measurements" &&
          initial.at("instructions").get_ref<const std::string&>().find("requestedDoneWhen") != std::string::npos,
          "the initial work prompt must direct the Bot to use the requested completion criteria");

    input.tools = {"start_task","send_to_task","change_task","answer_task_question","cancel_task",
                   "record_task_blocker","withdraw_task_question",
                   "message_session","interrupt_session","read_result","find_tasks","reply","start_workflow","workflow_status",
                   "instruct_workflow_stage","stop_workflow_stage","stop_workflow","compact_conversation",
                   "expand_context","continue_fresh_session",
                   "list_secrets","request_secret_input","list_connections",
                   "check_environment_readiness","update_bot_settings","request_bot_change",
                   "read_memory","propose_memory_edit","apply_memory_edit","revert_memory_edit","memory_history",
                   "create_schedule","list_schedules","remove_schedule","create_reminder","create_trigger",
                   "flock_session_search","flock_session_events"};
    input.event = {{"kind","conversation_turn"},{"text","Create a sales report in CSV format"},{"turn",Json::array()}};
    const auto turn = responseRequest(input);
    const auto turnTool = [&](std::string_view name) -> const Json& {
        for (const auto& function : turn.at("tools").at(0).at("tools"))
            if (function.at("name") == name) return function.at("parameters");
        throw std::runtime_error("Missing conversation tool: " + std::string(name));
    };
    check(turn.at("tools").at(0).at("tools").size() == input.tools.size() &&
          turnTool("start_workflow")["properties"]["stages"]["items"]["required"] == Json::array({"stageId","role"}) &&
          turnTool("workflow_status")["required"].back() == "workflowId" &&
          turnTool("instruct_workflow_stage")["required"].back() == "instruction" &&
          turnTool("stop_workflow_stage")["required"].back() == "stageId" &&
          turnTool("stop_workflow")["required"].back() == "workflowId" &&
          turnTool("compact_conversation")["required"].back() == "summary" &&
          turnTool("expand_context")["properties"].contains("compactionId") &&
          turnTool("continue_fresh_session")["required"].back() == "taskId" &&
          turnTool("request_secret_input")["required"] == Json::array({"reason","name"}) &&
          turnTool("message_session")["required"] == Json::array({"reason","taskId","generation","text"}) &&
          turnTool("start_task")["required"] == Json::array({"reason","request"}) &&
          turnTool("start_task")["properties"]["sessionRef"]["type"] == "string" &&
          turnTool("start_task")["properties"]["resume"]["type"] == "boolean" &&
          turnTool("change_task")["required"] == Json::array({"reason","taskId","generation","revisionId","instruction"}) &&
          turnTool("change_task")["properties"]["revisionId"]["minimum"] == 0 &&
          turnTool("find_tasks")["required"] == Json::array({"reason"}) &&
          turnTool("find_tasks")["properties"]["taskId"]["type"] == "integer" &&
          turnTool("send_to_task")["properties"]["generation"]["minimum"] == 0 &&
          turnTool("reply")["properties"]["suggestions"]["maxItems"] == 4,
          "conversation provider request must offer the conversation tools");
    check(turnTool("record_task_blocker")["required"] == Json::array({"reason","taskId","generation","blockerReason"}) &&
          turnTool("withdraw_task_question")["required"] == Json::array({"reason","taskId","generation","waitId"}) &&
          turnTool("withdraw_task_question")["properties"]["generation"]["minimum"] == 0,
          "blocker and withdrawal schemas must expose their exact task targets");
    for (auto name : {"list_secrets","list_connections","check_environment_readiness"})
        check(turnTool(name).at("required")==Json::array({"reason"}),"unexpected mandatory access-tool parameter");
    check(turnTool("update_bot_settings").at("required")==Json::array({"reason","environmentId","runTimeoutSeconds"}) &&
          turnTool("update_bot_settings").at("properties").at("runTimeoutSeconds").at("maximum")==2147483647 &&
          turnTool("request_bot_change").at("required")==Json::array({"reason","operation","targetBotId","name"}) &&
          turnTool("request_bot_change").at("properties").at("operation").at("enum")==Json::array({"create_bot","rename_bot"}),
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
    const auto& search = turnTool("flock_session_search");
    const auto& events = turnTool("flock_session_events");
    check(search.at("required") == Json::array({"reason"}) && search.at("properties").size() == 8 &&
          events.at("required") == Json::array({"reason","sessionRef","operation"}) &&
          events.at("properties").size() == 11 &&
          events.at("properties").at("operation").at("enum") ==
              Json::array({"list","search","read_event","counts","get_attachment"}),
          "Session search and event schemas do not match their contracts");
    for (const auto* schema : {&search,&events}) {
        const auto& fields = schema->at("properties");
        check(fields.at("limit").at("minimum") == 1 && fields.at("limit").at("maximum") == sessionPageLimit &&
              fields.at("fromTime").at("type") == "string" && fields.at("toTime").at("type") == "string" &&
              !fields.contains("botId"),"Session tool bounds or authenticated identity changed");
    }
    const auto modelCall = [](std::string_view tool, const Json& args) {
        return Json::array({{{"type","function_call"},{"name",tool},{"namespace","flock"},{"status","completed"},
                             {"arguments",args.dump()}}});
    };
    const Json searchArgs{{"reason","Find yesterday’s staging investigation"},{"query","staging"},
                          {"project","Release"},{"fromTime","2026-11-21T00:00:00Z"},{"limit",2}};
    const Json eventArgs{{"reason","Read the test log"},{"sessionRef","opaque-session"},{"operation","get_attachment"},
                         {"attachmentRef","opaque-attachment"}};
    check(responseDecision(modelCall("flock_session_search",searchArgs),turn).calls.at(0).args.at("project") == "Release" &&
          responseDecision(modelCall("flock_session_events",eventArgs),turn).calls.at(0).args.at("attachmentRef") == "opaque-attachment",
          "model Session calls were not decoded using the live schemas");
    auto spoofed = eventArgs;
    spoofed["botId"] = "someone-else";
    rejects<ModelError>([&] { responseDecision(modelCall("flock_session_events",spoofed),turn); },
                        "model Session calls can override the authenticated Bot");
    const auto& turnPrompt = turn.at("instructions").get_ref<const std::string&>();
    check(turnPrompt.find("You are a Flock Bot talking with your user") != std::string::npos &&
          turnPrompt.find("SOUL (soul) and MEMORY (memory)") != std::string::npos &&
          turnPrompt.find("Only the user's messages can authorize work") != std::string::npos &&
          turnPrompt.find("ask using their titles and times, never internal IDs") != std::string::npos,
          "conversation instructions must scope Bot notes to the user's authorized request");
    check(turn.dump().find("must-stay-local") == std::string::npos,"conversation adapter config leaked into the prompt");
    const std::map<std::string,Json> requiredChannelArgs{
        {"channel_edit_message",{"reason","externalMessageId","text"}},
        {"channel_react",{"reason","reaction"}},
        {"channel_send_attachment",{"reason"}},
        {"read_channel_attachment",{"reason","attachmentId"}},
        {"publish_artifact",{"reason","title","html"}},
        {"request_local_machine_connection",{"reason","threadId","runId","machineId","rules"}}
    };
    input.tools.clear();
    for (const auto& [name,args] : requiredChannelArgs) input.tools.push_back(name);
    input.images = {{1,{},"image/png","YQ=="}};
    const auto channelRequest = responseRequest(input);
    for (const auto& function : channelRequest.at("tools").at(0).at("tools")) {
        const auto name = function.at("name").get<std::string>();
        const auto& schema = function.at("parameters");
        check(schema.at("required") == requiredChannelArgs.at(name),"Channel tool required arguments");
        if (name == "request_local_machine_connection")
            check(schema.at("properties").at("rules").at("items").at("required") == Json::array({"operation","path"}),
                  "Local approval rule schema");
    }
    const auto content = channelRequest.at("input").at(0).at("content");
    check(content.at(0).at("type") == "input_text" && content.at(1).at("type") == "input_image" &&
          content.at(1).at("image_url") == "data:image/png;base64,YQ==","Screenshot did not enter the Responses image input");
    input.images.clear();
    input.event = {{"kind","intake"}};

    input.task.erase("requestedDoneWhen");
    input.needsDoneWhen = true;
    input.task["peers"] = Json::array({{{"botId","verifier"},{"name","Verifier"}}});
    input.task["peer"] = {{"replyKinds",{"question","result","failed"}}};
    input.tools = {"send_bot_message","reply_to_bot","read_bot_evidence"};
    const auto peerRequest = responseRequest(input);
    const auto peerContext = requestContext(peerRequest);
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
    check(reply["properties"]["kind"]["enum"].get<Names>() == Names{"question","result","failed"} &&
        reply["required"].get<Names>() == Names{"reason","kind","text"},
        "peer replies must specify their kind and text");
    const auto& evidence = parameters(peerRequest,"read_bot_evidence");
    check(evidence["properties"]["messageId"]["type"] == "string" &&
        evidence["required"].get<Names>() == Names{"reason","messageId"},
        "peer evidence must identify the message that carries it");
    input.needsDoneWhen = false;
    input.task["doneWhen"] = "Saved work acceptance";
    const auto nextPeerRequest = responseRequest(input);
    check(!parameters(nextPeerRequest,"send_bot_message")["properties"].contains("doneWhen"),
        "later peer work must keep the saved criterion");
    input.canForwardUserMessage = true;
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
    const auto valid = responseDecision(Json::array({{{"type","reasoning"}},call}),completeRequest).calls.at(0);
    check(valid.tool == "complete" && valid.reason == "Evidence provided" && valid.args == Json{{"report","Done"}},
        "Responses call did not map to core decision");
    for (const auto* key : {"namespace","status"}) {
        auto incomplete = call;
        incomplete.erase(key);
        rejects<ModelError>([&] { responseDecision(Json::array({incomplete}),completeRequest); },"missing provider qualifier");
        incomplete[key] = nullptr;
        rejects<ModelError>([&] { responseDecision(Json::array({incomplete}),completeRequest); },"null provider qualifier");
    }
    rejects<ModelError>([&] { responseDecision(Json::array({call,call}),completeRequest); },"multiple tools accepted");
    rejects<ModelError>([&] { responseDecision(Json::array({call}),forwardRequest); },"state-invalid tool accepted");
    rejects<ModelError>([&] { responseDecision(Json::array(),completeRequest); },"missing tool accepted");
}

void oversizedRequestStopsBeforeOpeningBridge() {
    Sandbox temp;
    const auto credentials = temp.path / "credentials";
    ChatGPTModel model(credentials);
    const ModelInput input{
        Json::object(),{{"kind","conversation_turn"},{"text",std::string(1'250'000,'\x01')}},
        Json::array(),{},{"reply"}, {}, Json::array(),botConfig()
    };
    rejects<ModelError>([&] { model.decide(input); },"Oversized escaped request reached the transport");
    check(!std::filesystem::exists(credentials),"The bridge was opened for a request over its byte limit");
}

class AcceptanceModel final : public Model {
public:
    std::vector<Json> requests;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        requests.push_back(responseRequest(input));
        if (input.task["botId"] == "regression") {
            return {"follow_up",{{"instruction","Cover the reported bug with a new automated check"}},"A regression test is missing."};
        }
        return {"complete",{{"report","All existing checks succeeded"}},"The required existing tests passed."};
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
        ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
        Decision choose(const ModelInput& input) {
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
        store.receive(localOwner,intake,"criteria","Completion criteria: Confirm the output");
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
        store.receive(localOwner,intake,"follow-up","Include the exact sample count");
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
    const auto demo = makeDemoModel();
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return std::string{}; });
    const Intake intake{"demo","Measure the result","local","project"};
    const auto task = store.submit(intake);
    for (int i = 0; i < 12 && store.task(task)["state"] != "completed"; ++i) runtime.tick();
    const auto result = lastTaskEvent(store,task,"completed").at("id").get<Id>();
    const auto replyTo = [&](Id message) {
        for (const auto& row : store.conversation("demo"))
            if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message) return row["data"];
        return Json(nullptr);
    };
    const auto question = store.receive(localOwner,intake,"explain","What did it find?",result).at("messageId").get<Id>();
    runtime.runConversations();
    const auto explanation = replyTo(question);
    const auto again = store.receive(localOwner,intake,"again","Explain again",explanation.at("messageId").get<Id>())
        .at("messageId").get<Id>();
    runtime.runConversations();
    check(store.tasks("demo").size() == 1 && replyTo(again).at("text") == explanation.at("text"),
          "a follow-up to the fixed demo's explanation started work instead of reading the same result");
}

void fixedDemoAnswersAnAnchoredQuestion() {
    Sandbox fixture;
    Store store(fixture.path / "state.db");
    const auto demo = makeDemoModel();
    const auto t3 = makeDemoT3();
    Runtime runtime(store,*demo,*t3,[](auto) { return std::string{}; });
    const Intake intake{"demo","ask: Which dataset should I use?","local","project"};
    const auto task = store.submit(intake);
    for (int i = 0; i < 12 && store.task(task)["state"] != "waiting"; ++i) runtime.tick();
    const auto question = store.task(task).at("wait").at("messageId").get<Id>();

    store.receive(localOwner,intake,"dataset-answer","Use the release dataset",question);
    runtime.runConversations();
    for (int i = 0; i < 12 && store.task(task)["state"] != "completed"; ++i) runtime.tick();
    const auto history = store.history(task);
    check(store.task(task)["state"] == "completed" &&
          std::ranges::any_of(history,[](const Json& event) {
              return event["kind"] == "user_answer" && event["data"]["text"] == "Use the release dataset";
          }),"the fixed demo did not pass the anchored answer to its task");
}

void botCreationCollisionIsRecoverable() {
    struct CreationModel final : Model {
        Json result;
        ModelTurn decide(const ModelInput& input) override {
            const auto& turn = input.event.at("turn");
            if (!turn.empty()) {
                result = turn.back().at("result");
                return {{{"reply",{{"text","Finished"}},{}}}};
            }
            return {{{"request_bot_change",{{"operation","create_bot"},{"targetBotId","Target"},
                                           {"name","Target Bot"}},{}}}};
        }
    } model;
    Sandbox temp;
    const auto directory = temp.path / "bots";
    const auto target = directory / "Target";
    std::filesystem::create_directories(target);
    Bots bots(directory);
    Store store(temp.path / "state.db");
    const Person creator{"creator",false};
    store.policy().setOwner(creator,"Source");
    NotesT3 t3;
    RuntimeOptions options;
    options.applyBotChange = [&](const BotChange& change) { bots.apply(change); };
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },options);
    int sequence = 0;
    const auto create = [&] {
        store.receive(creator,Intake{"Source","","env","project"},std::to_string(++sequence),"Create a Bot");
        runtime.runConversations();
        return model.result;
    };
    for (const auto config : {"{",R"({"creationRequest":7})",R"({"creationRequest":"different"})"}) {
        std::ofstream(target / "config.json") << config;
        const auto result = create();
        check(result.at("ok") == false && result.dump().find("Target Bot already exists") != std::string::npos,
              "Existing Bot did not produce a collision tool error");
    }
    std::filesystem::remove_all(target);
    check(create().at("status") == "applied","Collision left a creation intent blocking the next request");
}

void botProfilesAndAvatars() {
    Sandbox temp;
    Bots bots(temp.path / "bots");

    {
        Store store(temp.path / "creation.db");
        auto policy = store.policy();
        const Person creator{"bot-creator",false};
        const Json profile{{"name","Blue Lake"},
                           {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}}};
        policy.submitSecret(localOwner,"secrets",policy.requestSecret("secrets",{"creation-fixture"}),"blue-lake");
        bool rejected = false;
        try {
            policy.createOwnedBot(creator,[&](const auto& publish) {
                return bots.create(profile,std::nullopt,publish);
            });
        } catch (const Conflict&) {
            rejected = true;
        }
        check(rejected && bots.cards().empty() &&
              !std::filesystem::exists(temp.path/"bots"/".flock-issued-ids"/"blue-lake") &&
              !policy.allows(creator,Action::bot_settings,"blue-lake"),
              "Rejected owner registration left a profile, issued ID, or owner");
    }

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
    },AvatarImage{pngBytes});
    check(uploaded.at("name") == "Scout Prime" && uploaded.at("avatar").at("kind") == "upload" &&
          uploaded.at("avatar").at("filename") == "photo.png" &&
          !uploaded.at("avatar").at("version").get<std::string>().empty(),
          "uploaded PNG avatar metadata was not saved");
    const auto savedPng = bots.avatarImage("scout");
    check(savedPng && savedPng->format().contentType == "image/png" && savedPng->bytes() == pngBytes,
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
    },AvatarImage{gifBytes});
    check(replaced.at("avatar").at("version") != uploaded.at("avatar").at("version") &&
          !std::filesystem::exists(temp.path / "bots" / "scout" / "avatar.png") &&
          std::filesystem::exists(temp.path / "bots" / "scout" / "avatar.gif"),
          "replacing uploaded avatar did not clean up previous image file");
    const auto savedGif = bots.avatarImage("scout");
    check(savedGif && savedGif->format().contentType == "image/gif" && savedGif->bytes() == gifBytes,
          "replaced GIF bytes did not match");

    // Switching back to shape removes uploaded file.
    const auto switched = bots.updateProfile("scout",{
        {"name","Scout Renamed"},
        {"avatar",{{"kind","shape"},{"example","barbell"},{"color","#ee8d85"}}},
    });
    check(switched.at("avatar").at("kind") == "shape" && !bots.avatarImage("scout").has_value() &&
          !std::filesystem::exists(temp.path / "bots" / "scout" / "avatar.gif"),
          "switching from upload to shape did not remove stored image file");

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
    }),"updated Bot card did not retain its icon");

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
    const auto customImage = pngBytes + "new upload";
    bots.updateProfile("custom",{{"name","Custom Two"},{"avatar",{{"kind","upload"}}}},AvatarImage{customImage});
    const auto reopened = Bots(temp.path / "bots").avatarImage("custom");
    std::ifstream spare(customDir / "avatar.png",std::ios::binary);
    check(reopened && reopened->bytes() == customImage &&
          std::string(std::istreambuf_iterator<char>(spare),{}) == pngBytes,
          "upload failed to persist the new image and preserve the unrelated image");

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
        configuredSchedules();
        notesAndRoutingOnRestart();
        notesAcrossConversationAndWork();
        directionalPeerConfiguration();
        responseContract();
        oversizedRequestStopsBeforeOpeningBridge();
        {
            Sandbox temp;
            Store store(temp.path/"state.db");
            sessionConversationDispatch(store,botConfig());
        }
        acceptanceRunner();
        fixedDemoWithUserCriteriaAndContinuation();
        fixedDemoAnswersAnAnchoredQuestion();
        fixedDemoFollowUpKeepsTheExplainedResult();
        botCreationCollisionIsRecoverable();
        botProfilesAndAvatars();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
