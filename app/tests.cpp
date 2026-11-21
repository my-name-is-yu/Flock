#include "bots.hpp"
#include "model.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
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

template <typename Condition>
void tickUntil(Runtime& runtime, Condition reached, std::string_view what) {
    for (int tick = 0; tick < 10 && !reached(); ++tick) runtime.tick();
    check(reached(),what);
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

    Json scheduledTasks;
    {
        Store store(db);
        Scheduler spring(bots,store,utc(2026,3,8,6,0),report);
        spring.tick(utc(2026,3,8,9,0));
        check(store.tasks().empty(),"nonexistent spring local time was submitted");
        Scheduler fall(bots,store,utc(2026,11,1,5,0),report);
        fall.tick(utc(2026,11,1,7,0));
        fall.tick(utc(2026,11,1,7,0));
        scheduledTasks = store.tasks();
        check(scheduledTasks.size() == 2,"both fall-back instants should submit exactly once");
        check(scheduledTasks[0]["scheduledAt"] == utc(2026,11,1,6,30) &&
              scheduledTasks[1]["scheduledAt"] == utc(2026,11,1,5,30),
              "fall offsets did not produce separate UTC instants");
        for (const auto& task : scheduledTasks) {
            check(task["botId"] == "release" && task["scheduleId"] == "fall" && task["request"] == "Overlap",
                "scheduler did not submit the configured request");
        }
        check(scheduledTasks[0]["blockedBy"]["id"] == scheduledTasks[1]["id"],
            "cron must participate in the same project queue");
    }
    {
        Store store(db);
        Scheduler repeated(bots,store,utc(2026,11,1,6,30),report);
        repeated.tick(utc(2026,11,1,6,30));
        const auto repeatedTasks = store.tasks();
        check(repeatedTasks.size() == 2 && repeatedTasks[0]["id"] == scheduledTasks[0]["id"] &&
            repeatedTasks[1]["id"] == scheduledTasks[1]["id"],
            "replayed schedule must preserve the original tasks after reopening the store");
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
        if (input.task["botId"] == "first") {
            if (!input.event.contains("userAnswer"))
                return {"ask_user",{{"question","Which branch?"}},"Only the user can select the branch."};
            const auto branch = input.event.at("userAnswer").get<std::string>();
            return {"delegate",{{"instruction","Verify branch " + branch},{"doneWhen","Evidence required before edit"}},
                "Use the branch the user selected."};
        }
        return {"delegate",{{"instruction","Verify the second Bot's scope"},{"doneWhen","Second Bot criterion"}},
            "Run verification in T3."};
    }
};

class NotesT3 final : public T3 {
public:
    std::vector<Command> commands;
    std::string finishedRunId;
    std::string authGeneration(const std::string&,const Json&) override { return "test-auth"; }
    Delivery send(const Command& command) override {
        commands.push_back(command);
        return {Delivery::Kind::Accepted,{"thread-" + command.actionId,"run-" + command.actionId,"link"},{}};
    }
    Delivery reconcile(const Command&) override { throw std::runtime_error("Unexpected reconcile"); }
    Observation observe(const std::string&,const Json&,const RunRef& run) override {
        Observation result;
        if (run.runId == finishedRunId) {
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
        tickUntil(runtime,[&] { return store.task(firstId)["state"] == "waiting"; },"first Bot did not ask a question");
        check(store.task(firstId)["wait"]["text"] == "Which branch?","wrong question shown to the user");
        waitId = store.task(firstId)["wait"]["id"];
        secondId = store.submit(bots.get("second").intake("Request for second Bot"));
        runtime.tick();
        check(store.task(secondId)["blockedBy"]["id"] == firstId && t3.commands.empty(),
            "same-project tasks must queue across Bots");
    }
    {
        Store store(db);
        Runtime runtime(store,model,t3,notes);
        store.operate(firstId,waitId,"answer",{{"text","release-branch"}});
        rejects<Conflict>([&] { store.operate(firstId,waitId,"answer",{{"text","old answer"}}); },"duplicate answer accepted");
        tickUntil(runtime,[&] { return store.task(firstId)["state"] == "running"; },"answer did not resume the original task");
        const auto active = store.task(firstId);
        check(active["doneWhen"] == "Evidence required before edit","initial delegation did not save its criterion");
        t3.finishedRunId = active["activeRun"]["runId"];
        check(t3.commands.size() == 1 && t3.commands[0].environmentId == "env-canonical" &&
              t3.commands[0].projectId == "project-canonical","intake must preserve canonical IDs");
        const auto& instruction = t3.commands[0].instruction;
        check(instruction.substr(0,instruction.find('\n')) == "Verify branch release-branch",
            "T3 did not receive the branch from the answer");
        check(t3.commands[0].config["session"]["model"] == "child-model" &&
              t3.commands[0].config["session"]["providerInstanceId"] == "child-provider" &&
              t3.commands[0].config["session"]["runtimeMode"] == "auto" &&
              t3.commands[0].config["session"]["workspaceStrategy"]["branch"] == "main" &&
              t3.commands[0].config["runTimeoutSeconds"] == 999,"child settings and timeout must survive intake");
    }

    createBot(folder / "first",botConfig("new-model"),"New SOUL would now ask for something different","First updated memory");
    {
        Store store(db);
        Runtime runtime(store,model,t3,notes);
        tickUntil(runtime,[&] { return store.task(firstId)["state"] == "completed"; },"first Bot did not complete");
        tickUntil(runtime,[&] { return store.task(secondId)["state"] == "running"; },"next task did not start after completion");
        check(t3.commands.size() == 2 &&
              t3.commands.back().instruction.starts_with("Verify the second Bot's scope"),
              "T3 did not receive the queued task exactly once");
        const auto afterRestart = std::find_if(model.inputs.begin(),model.inputs.end(),[&](const ModelInput& input) {
            return input.task.at("id") == firstId && input.event.at("kind") == "result";
        });
        check(afterRestart != model.inputs.end(),"model did not receive the first Bot's result");
        check(afterRestart->event.at("report") == "Returned evidence for the original criterion",
            "model did not receive the T3 report");
        check(afterRestart->task.at("doneWhen") == "Evidence required before edit",
            "stored criterion must survive restart and a SOUL edit");
        check(afterRestart->notes.soul == "New SOUL would now ask for something different" &&
              afterRestart->notes.memory == "First updated memory","existing task must read current notes");
        check(afterRestart->config.at("model") == "fixture-model","model setting must be snapshotted at intake");
        const auto containsFirstAnswer = [](const ModelInput& input) {
            return std::any_of(input.history.begin(),input.history.end(),[](const Json& item) {
                return item.at("kind") == "user_answer" && item.at("data").at("text") == "release-branch";
            });
        };
        check(containsFirstAnswer(*afterRestart),"first Bot did not get its saved answer in history");
        const auto second = std::find_if(model.inputs.begin(),model.inputs.end(),[&](const ModelInput& input) {
            return input.task.at("id") == secondId;
        });
        check(second != model.inputs.end() && second->notes.soul == "Second version" &&
              second->notes.memory == "Second-only memory" && !containsFirstAnswer(*second),
              "second Bot must get its own notes and no first-task answer");
    }
    rejects<Conflict>([&] { bots.get("../first"); },"path traversal accepted as Bot ID");
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
    check(request["tools"].size() == 1 && tool.at("type") == "namespace" && tool.at("name") == "flock",
        "Flock tools must be exposed through the flock namespace");
    const auto toolNames = [](const Json& functions) {
        std::set<std::string> names;
        for (const auto& function : functions) names.insert(function.at("name").get<std::string>());
        return names;
    };
    const auto delegateParameters = [](const Json& functions) -> const Json& {
        for (const auto& function : functions) {
            if (function.at("name") == "delegate") return function.at("parameters");
        }
        throw std::runtime_error("Responses request omitted the allowed delegate function");
    };
    const auto& functions = tool.at("tools");
    check(functions.size() == input.tools.size() &&
          toolNames(functions) == std::set<std::string>{"follow_up","delegate","complete","ask_user"},
          "only the allowed tools should be offered to Responses");
    check(!delegateParameters(functions).at("properties").contains("doneWhen"),"later delegate must omit doneWhen");
    check(request.dump().find("must-stay-local") == std::string::npos,"adapter config leaked into the prompt");
    const auto context = Json::parse(request["input"][0]["content"].get<std::string>());
    check(context["task"]["doneWhen"] == "An existing requirement" && context["soul"] == "New SOUL" &&
        context["memory"] == "Current MEMORY","missing model context");

    input.task["doneWhen"] = "";
    input.tools = {"delegate","ask_user"};
    const auto initial = responseRequest(input);
    const auto& initialFunctions = initial.at("tools").at(0).at("tools");
    check(initialFunctions.size() == 2 && toolNames(initialFunctions) == std::set<std::string>{"delegate","ask_user"},
        "only initial-event tools should be offered to Responses");
    const auto& parameters = delegateParameters(initialFunctions);
    const auto required = parameters.at("required").get<std::set<std::string>>();
    check(required == std::set<std::string>{"reason","instruction","doneWhen"} &&
          parameters.at("properties").at("doneWhen").at("type") == "string",
          "initial delegate requires a saved criterion");

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
    explicit AcceptanceModel(bool wrongAction) : wrongAction_(wrongAction) {}

    std::map<std::string,std::vector<ModelInput>> inputs;
    Decision decide(const ModelInput& input) override {
        const auto bot = input.task.at("botId").get<std::string>();
        auto& trials = inputs[bot];
        trials.push_back(input);
        const auto& criterion = input.task.at("doneWhen").get_ref<const std::string&>();
        const auto& report = input.event.at("report").get_ref<const std::string&>();
        check(criterion.find("existing test") != std::string::npos &&
              report.find("existing tests passed") != std::string::npos &&
              report.find("No regression test was added") != std::string::npos,
              "acceptance must present passing tests and a missing regression test to both Bots");
        if (criterion.find("regression test") != std::string::npos && !(wrongAction_ && trials.size() == 1)) {
            return {"follow_up",{{"instruction","Add the required test"}},"A regression test is missing."};
        }
        return {"complete",{{"report","Existing tests passed"}},"The required existing tests passed."};
    }
private:
    bool wrongAction_;
};

void acceptanceRunner() {
    for (const bool wrongAction : {false,true}) {
        AcceptanceModel model(wrongAction);
        std::stringstream output;
        check(runModelAcceptance(model,"model-slug",output) == !wrongAction,
            "acceptance must fail if even one trial returns the wrong action");
        check(model.inputs.size() == 2 && model.inputs.at("regression").size() == 3 &&
              model.inputs.at("existing-tests").size() == 3,"each Bot must be evaluated three times");
        const auto& regression = model.inputs.at("regression").front();
        const auto& existing = model.inputs.at("existing-tests").front();
        check(regression.event.at("report") == existing.event.at("report"),
              "the two Bots must evaluate the same report");

        std::map<std::string,std::set<unsigned>> recorded;
        std::string line;
        while (std::getline(output,line)) {
            const auto trial = Json::parse(line);
            const auto bot = trial.at("botId").get<std::string>();
            const auto number = trial.at("trial").get<unsigned>();
            check(number >= 1 && number <= 3 && recorded[bot].insert(number).second,
                "trial record is duplicated or has an invalid number");
            const auto& inputs = model.inputs.at(bot);
            const auto& input = inputs.at(number - 1);
            check(responseRequest(input) == responseRequest(inputs.front()) && input.config.at("model") == "model-slug",
                  "the complete Responses request must stay fixed for each Bot");
            check(trial.at("model") == "model-slug" && trial.at("doneWhen") == input.task.at("doneWhen") &&
                  trial.at("sessionReport") == input.event.at("report"),"record did not identify its actual trial input");
            const bool failed = wrongAction && bot == "regression" && number == 1;
            const bool followUp = bot == "regression" && !failed;
            const Json args = followUp ? Json{{"instruction","Add the required test"}} :
                Json{{"report","Existing tests passed"}};
            check(trial.at("expected") == (bot == "regression" ? "follow_up" : "complete") &&
                  trial.at("tool") == (followUp ? "follow_up" : "complete") &&
                  trial.at("args") == args && trial.at("pass") == !failed &&
                  trial.at("reason") == (followUp ? "A regression test is missing." : "The required existing tests passed."),
                  "record did not distinguish the actual decision from the expected action");
        }
        check(recorded.at("regression").size() == 3 && recorded.at("existing-tests").size() == 3,
            "one result must be recorded for each of the six trials");
    }
}
} // namespace

int main() {
    try {
        cronSyntaxAndTimeZones();
        cronIntakeAndRestart();
        notesAndRoutingOnRestart();
        responseContract();
        acceptanceRunner();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
