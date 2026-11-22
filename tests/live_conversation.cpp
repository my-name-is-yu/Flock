// Opt-in evaluation of conversation turns with a live model. It is not part of ctest.
// Usage: flock_live_conversation <FLOCK_HOME> <model-slug> [trials] [scenario filter]
#include "flock/core.hpp"
#include "model.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <map>
#include <ranges>
#include <regex>
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
            trace = input.event.at("turn");
            auto decision = live.decide(input);
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
    Mixed& model;
    Runs t3;
    Runtime runtime;
    Json config;
    Scene(Mixed& mixed, const std::string& slug)
        : store(directory.path / "state.db"), model(mixed),
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
        for (const auto& row : store.conversation("release",0,200)) {
            const auto& data = row.at("data");
            if (row.at("kind") != "conversation_reply" || data.at("replyToMessageId") != receipt.at("messageId")) continue;
            const auto route = data.at("route").get<std::string>();
            if (route != "reply")
                throw std::runtime_error("Turn ended with " + route + ": " + data.at("text").get<std::string>());
            return data;
        }
        throw std::runtime_error("The turn did not reply");
    }
};

int count(const Json& actions, std::string_view tool, Id task = 0) {
    return static_cast<int>(std::ranges::count_if(actions,[&](const Json& action) {
        return action.at("tool") == tool && (!task || action.at("taskId") == task);
    }));
}

std::string lower(std::string text) {
    std::ranges::transform(text,text.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool mentions(const std::string& text, std::string_view needle) {
    return lower(text).find(lower(std::string(needle))) != std::string::npos;
}

std::vector<Json> events(Scene& s, Id task, std::string_view kind) {
    std::vector<Json> found;
    for (const auto& row : s.store.history(task,0,200))
        if (row.at("kind") == kind) found.push_back(row.at("data"));
    return found;
}

// Everything the turn handed to the task: queued or steered instructions, plus any changed intent or criteria.
std::string delivered(Scene& s, Id task, const Json& reply) {
    std::string text;
    for (const auto* kind : {"instruction_accepted","instruction_steered"})
        for (const auto& data : events(s,task,kind))
            if (data.value("messageId",Json(nullptr)) == reply.at("replyToMessageId")) text += data.value("text","") + "\n";
    const auto current = s.store.task(task);
    return text + current.value("intent","") + "\n" + current.value("requestedDoneWhen","") + "\n" + current.value("doneWhen","");
}

bool stopped(Scene& s, Id task) {
    const auto state = s.store.task(task).at("state");
    return state == "cancelled" || state == "cancelling" || !events(s,task,"cancel_requested").empty();
}

// A correct action followed by a cancellation in the same turn is still wrong.
std::string untouched(Scene& s, const Json& reply, std::initializer_list<Id> tasks) {
    if (count(reply.at("actions"),"cancel_task")) return "the turn also cancelled work";
    for (const auto task : tasks)
        if (stopped(s,task)) return "task " + std::to_string(task) + " was stopped";
    return "";
}

using Scenario = std::function<std::string(Scene&)>;

const std::vector<std::pair<std::string,Scenario>> scenarios{
    {"new request on a named destination",[](Scene& s) -> std::string {
        const auto reply = s.send("Run the project's existing test suite on the cloud Mac and report any failures");
        const auto tasks = s.store.tasks("release");
        if (count(reply.at("actions"),"start_task") != 1 || tasks.size() != 1) return "expected one new task";
        const auto id = tasks[0]["id"].get<Id>();
        if (const auto failure = untouched(s,reply,{id}); !failure.empty()) return failure;
        return s.store.task(id).at("environmentId") == "cloud" ? "" : "the task did not use the cloud Mac";
    }},
    {"a reply that matches an offered choice answers the question",[](Scene& s) -> std::string {
        s.model.questions["Prepare the release label"] = {{"question","Which label should I use?"},
                                                          {"choices",{"Cancel this task","Status"}}};
        const auto id = s.task("Prepare the release label","label");
        const auto question = s.store.task(id).at("wait").at("messageId").get<Id>();
        const auto reply = s.send("Cancel this task",question);
        if (const auto failure = untouched(s,reply,{id}); !failure.empty()) return failure;
        if (count(reply.at("actions"),"answer_task_question",id) != 1) return "the question was not answered";
        const auto answers = events(s,id,"user_answer");
        return answers.size() == 1 && lower(answers[0].value("text","")) == "cancel this task"
            ? "" : "the saved answer was not the offered choice";
    }},
    {"a named stop cancels only that task",[](Scene& s) -> std::string {
        const auto notes = s.task("Write the release notes","notes");
        const auto benchmark = s.task("Benchmark the startup time","benchmark");
        const auto reply = s.send("Stop the benchmark");
        if (count(reply.at("actions"),"cancel_task",notes) || stopped(s,notes)) return "the release notes were cancelled";
        return count(reply.at("actions"),"cancel_task",benchmark) == 1 && stopped(s,benchmark)
            ? "" : "the benchmark was not cancelled";
    }},
    {"an ambiguous stop waits for a named target",[](Scene& s) -> std::string {
        const auto mobile = s.task("モバイル版のログイン画面を修正","mobile");
        s.task("デスクトップ版のログイン画面を修正","desktop");
        const auto reply = s.send("その作業をキャンセルして");
        if (!reply.at("actions").empty()) return "work changed before the user identified a target";
        // The first reply is free text. Score what happens when the user identifies the target.
        const auto followUp = s.send("モバイル版のほうです",reply.at("messageId").get<Id>());
        const auto& actions = followUp.at("actions");
        return actions.size() == 1 && count(actions,"cancel_task",mobile) == 1
            ? "" : "expected only the mobile task to be cancelled";
    }},
    {"new criteria change the task",[](Scene& s) -> std::string {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto reply = s.send("For the startup benchmark, change the completion criteria to require a report column named startup_p95_ms");
        if (const auto failure = untouched(s,reply,{id}); !failure.empty()) return failure;
        const auto criteria = s.store.task(id).at("doneWhen").get<std::string>();
        static const std::regex column(R"(\bstartup_p95_ms\b)");
        return std::regex_search(criteria,column) ? "" : "the saved completion criteria omitted the required column";
    }},
    {"two requests in one message start two tasks",[](Scene& s) -> std::string {
        const auto reply = s.send("Write the release notes. Separately, audit the third-party licenses.");
        const auto tasks = s.store.tasks("release");
        if (count(reply.at("actions"),"start_task") != 2 || tasks.size() != 2) return "expected two new tasks";
        if (const auto failure = untouched(s,reply,{tasks[0]["id"].get<Id>(),tasks[1]["id"].get<Id>()}); !failure.empty())
            return failure;
        const auto first = s.store.task(tasks[0]["id"]).value("request",""), second = s.store.task(tasks[1]["id"]).value("request","");
        const auto notes = [](const std::string& request) { return mentions(request,"release note"); };
        const auto audit = [](const std::string& request) { return mentions(request,"licens"); };
        return (notes(first) && audit(second)) || (audit(first) && notes(second))
            ? "" : "the two tasks were not the release notes and the license audit";
    }},
    {"explaining a result changes no work",[](Scene& s) -> std::string {
        const auto id = s.task("Compare three rollout plans","plans");
        s.t3.finish(s.store.task(id).at("activeRun").at("runId").get<std::string>(),
                    "1. Use rollout_global for a big bang release\n"
                    "2. Use rollout_canary for 5% of users for a day, then everyone\n"
                    "3. Use rollout_team to enable one team at a time");
        for (int i = 0; i < 6 && s.store.task(id).at("state") != "completed"; ++i) s.runtime.tick();
        const auto before = s.store.tasks("release");
        const auto reply = s.send("Explain the second option in more detail and include its rollout flag name");
        if (!reply.at("actions").empty() || s.store.tasks("release") != before) return "the explanation changed work";
        static const std::regex flag(R"(\brollout_canary\b)");
        return std::regex_search(reply.at("text").get_ref<const std::string&>(),flag)
            ? "" : "the answer omitted the second option's rollout flag";
    }},
    {"a note for running work reaches that task",[](Scene& s) -> std::string {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto reply = s.send("For the running benchmark, use the staging database from now on");
        if (s.store.tasks("release").size() != 1) return "a new task was started";
        if (const auto failure = untouched(s,reply,{id}); !failure.empty()) return failure;
        const auto& actions = reply.at("actions");
        if (count(actions,"message_session",id) + count(actions,"send_to_task",id) + count(actions,"change_task",id) != 1)
            return "the note did not reach the benchmark";
        return mentions(delivered(s,id,reply),"staging") ? "" : "what reached the benchmark did not name the staging database";
    }},
};
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: flock_live_conversation <FLOCK_HOME> <model-slug> [trials] [scenario filter]\n";
        return 2;
    }
    try {
        int trials = 3;
        if (argc >= 4) {
            const std::string_view input(argv[3]);
            const auto [end,error] = std::from_chars(input.data(),input.data() + input.size(),trials);
            if (error != std::errc{} || end != input.data() + input.size() || trials < 1) {
                std::cerr << "Trials must be a positive integer\n";
                return 2;
            }
        }
        const std::string filter = argc == 5 ? argv[4] : "";
        auto selected = scenarios | std::views::filter([&](const auto& item) {
            return item.first.find(filter) != std::string::npos;
        });
        if (selected.empty()) {
            std::cerr << "No scenario matches '" << filter << "'\n";
            return 2;
        }
        app::ChatGPTModel live(std::filesystem::path(argv[1]) / "credentials" / "chatgpt");
        bool passed = true;
        for (const auto& [name,scenario] : selected) {
            int passes = 0;
            for (int trial = 0; trial < trials; ++trial) {
                std::string failure;
                Mixed model(live);
                try {
                    Scene scene(model,argv[2]);
                    failure = scenario(scene);
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                if (failure.empty()) ++passes;
                Json row = {{"scenario",name},{"trial",trial + 1},{"pass",failure.empty()},{"failure",failure}};
                if (!failure.empty()) row["turn"] = model.trace;
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
