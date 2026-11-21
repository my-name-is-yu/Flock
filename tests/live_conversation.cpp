// Opt-in evaluation of conversation turns with a live model. It is not part of ctest.
// Usage: flock_live_conversation <FLOCK_HOME> <model-slug> [trials]
#include "flock/core.hpp"
#include "model.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <unistd.h>

using namespace flockv0;

namespace {
struct Directory {
    std::filesystem::path path;
    Directory() {
        char pattern[] = "/tmp/flock-live-conversation-XXXXXX";
        if (const auto* created = mkdtemp(pattern)) path = created;
        else throw std::runtime_error("Cannot create a test directory");
    }
    ~Directory() { std::filesystem::remove_all(path); }
};

// The conversation uses the live model. Task decisions stay scripted so each scenario starts from known state.
struct Mixed : Model {
    Model& live;
    std::map<std::string,Json> questions;
    Json trace = Json::array();
    explicit Mixed(Model& model) : live(model) {}
    Decision decide(const ModelInput& input) override {
        if (input.event.value("kind","") == "conversation_turn") {
            auto decision = live.decide(input);
            trace = input.event.at("turn");
            trace.push_back({{"tool",decision.tool},{"args",decision.args}});
            return decision;
        }
        const auto request = input.task.value("request","");
        if (const auto question = questions.find(request); question != questions.end() && !input.event.contains("userAnswer"))
            return {"ask_user",question->second,"The fixture needs a decision"};
        if (input.event.value("kind","") == "result")
            return {"complete",{{"report",input.event.value("report","")}},"The fixture report is the result"};
        Json args = {{"instruction",request}};
        if (input.task.value("doneWhen","").empty()) args["doneWhen"] = "Report the result with evidence";
        return {"delegate",args,"Run the fixture work"};
    }
};

struct Runs : T3 {
    std::map<std::string,Observation> runs;
    std::vector<Command> notes;
    std::vector<std::string> interrupted;
    std::string authGeneration(const std::string&, const Json&) override { return "live-eval"; }
    Delivery send(const Command& command) override {
        if (command.tool == "steer") {
            notes.push_back(command);
            return {Delivery::Kind::Accepted,command.target,{}};
        }
        const auto id = std::to_string(runs.size() + 1);
        runs["run-" + id] = {};
        return {Delivery::Kind::Accepted,{"thread-" + id,"run-" + id,{}},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return runs[run.runId]; }
    void interrupt(const std::string&, const Json&, const RunRef& run) override { interrupted.push_back(run.runId); }
    void finish(const std::string& run, std::string report) {
        runs[run] = {.kind = Observation::Kind::Finished, .status = "completed", .report = std::move(report),
                     .resultRunId = run, .itemId = "report-" + run, .settled = true};
    }
};

struct Scene {
    Directory directory;
    Store store;
    Mixed model;
    Runs t3;
    Runtime runtime;
    Json config;
    Scene(Model& live, const std::string& slug)
        : store(directory.path / "state.db"), model(live),
          runtime(store,model,t3,[](std::string_view) { return BotNotes{}; }),
          config({{"modelProvider","chatgpt"},{"model",slug},{"conversationEnvironments",{{"cloud Mac","cloud"}}}}) {}

    Intake intake(std::string request = {}, std::string project = "project") const {
        return {"release",std::move(request),"desktop",std::move(project),config,{},0};
    }
    Id task(std::string request, std::string project) {
        const auto id = store.submit(intake(std::move(request),std::move(project)));
        for (int i = 0; i < 12 && store.task(id).at("state") != "running" && store.task(id).at("state") != "waiting"; ++i)
            runtime.tick();
        return id;
    }
    Json send(const std::string& text, Id replyTo = 0) {
        const auto receipt = store.receive(intake(),"message-" + std::to_string(store.conversation("release",0,200).size()),
                                           text,replyTo);
        runtime.runConversations();
        for (const auto& row : store.conversation("release",0,200))
            if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == receipt["messageId"])
                return row["data"];
        throw std::runtime_error("The turn did not reply");
    }
};

int count(const Json& actions, std::string_view tool, Id task = 0) {
    return static_cast<int>(std::ranges::count_if(actions,[&](const Json& action) {
        return action.at("tool") == tool && (!task || action.at("taskId") == task);
    }));
}

using Scenario = std::function<std::string(Scene&)>;

const std::vector<std::pair<std::string,Scenario>> scenarios{
    {"new request on a named destination",[](Scene& s) -> std::string {
        const auto reply = s.send("Run the project's existing test suite on the cloud Mac and report any failures");
        const auto tasks = s.store.tasks("release");
        if (count(reply.at("actions"),"start_task") != 1 || tasks.size() != 1) return "expected one new task";
        return s.store.task(tasks[0]["id"]).at("environmentId") == "cloud" ? "" : "the task did not use the cloud Mac";
    }},
    {"a reply that matches an offered choice answers the question",[](Scene& s) -> std::string {
        s.model.questions["Prepare the release label"] = {{"question","Which label should I use?"},
                                                          {"choices",{"Cancel this task","Status"}}};
        const auto id = s.task("Prepare the release label","label");
        const auto question = s.store.task(id).at("wait").at("messageId").get<Id>();
        const auto reply = s.send("Cancel this task",question);
        if (count(reply.at("actions"),"cancel_task")) return "the offered answer cancelled the task";
        return count(reply.at("actions"),"answer_task_question",id) == 1 ? "" : "the question was not answered";
    }},
    {"a named stop cancels only that task",[](Scene& s) -> std::string {
        const auto notes = s.task("Write the release notes","notes");
        const auto benchmark = s.task("Benchmark the startup time","benchmark");
        const auto reply = s.send("Stop the benchmark");
        if (count(reply.at("actions"),"cancel_task",notes)) return "the release notes were cancelled";
        return count(reply.at("actions"),"cancel_task",benchmark) == 1 ? "" : "the benchmark was not cancelled";
    }},
    {"an ambiguous stop asks first",[](Scene& s) -> std::string {
        s.task("ログイン画面の修正","login-a");
        s.task("ログイン画面の修正","login-b");
        const auto reply = s.send("それ止めて");
        if (count(reply.at("actions"),"cancel_task")) return "a task was cancelled without asking";
        return reply.at("text").get<std::string>().empty() ? "the Bot did not ask which one" : "";
    }},
    {"new criteria change the task",[](Scene& s) -> std::string {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto reply = s.send("For the startup benchmark, change the completion criteria to reporting p95 latency");
        if (count(reply.at("actions"),"change_task",id) != 1) return "the task was not changed";
        return s.store.task(id).dump().find("p95") != std::string::npos ? "" : "the new criteria were lost";
    }},
    {"two requests in one message start two tasks",[](Scene& s) -> std::string {
        const auto reply = s.send("Write the release notes. Separately, audit the third-party licenses.");
        return count(reply.at("actions"),"start_task") == 2 && s.store.tasks("release").size() == 2
            ? "" : "expected two new tasks";
    }},
    {"explaining a result changes no work",[](Scene& s) -> std::string {
        const auto id = s.task("Compare three rollout plans","plans");
        s.t3.finish(s.store.task(id).at("activeRun").at("runId").get<std::string>(),
                    "1. Big bang release\n2. Canary to 5% of users for a day, then everyone\n3. Feature flag per team");
        for (int i = 0; i < 6 && s.store.task(id).at("state") != "completed"; ++i) s.runtime.tick();
        const auto before = s.store.tasks("release");
        const auto reply = s.send("Explain the second option in more detail");
        if (!reply.at("actions").empty() || s.store.tasks("release") != before) return "the explanation changed work";
        return reply.at("text").get<std::string>().find("5%") != std::string::npos ? "" : "the answer missed the second option";
    }},
    {"a note for running work reaches that task",[](Scene& s) -> std::string {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto reply = s.send("For the running benchmark, use the staging database from now on");
        if (s.store.tasks("release").size() != 1) return "a new task was started";
        return count(reply.at("actions"),"message_session",id) + count(reply.at("actions"),"send_to_task",id) == 1
            ? "" : "the note did not reach the benchmark";
    }},
};
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: flock_live_conversation <FLOCK_HOME> <model-slug> [trials]\n";
        return 2;
    }
    try {
        app::ChatGPTModel model(std::filesystem::path(argv[1]) / "credentials" / "chatgpt");
        const int trials = argc == 4 ? std::stoi(argv[3]) : 3;
        bool passed = true;
        for (const auto& [name,scenario] : scenarios) {
            int passes = 0;
            for (int trial = 1; trial <= trials; ++trial) {
                std::string failure;
                Json trace;
                try {
                    Scene scene(model,argv[2]);
                    failure = scenario(scene);
                    trace = scene.model.trace;
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                if (failure.empty()) ++passes;
                Json row = {{"scenario",name},{"trial",trial},{"pass",failure.empty()},{"failure",failure}};
                if (!failure.empty()) row["turn"] = trace;
                std::cout << row.dump() << '\n';
            }
            passed = passed && passes == trials;
            std::cout << Json{{"scenario",name},{"passes",passes},{"trials",trials}}.dump() << '\n';
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
