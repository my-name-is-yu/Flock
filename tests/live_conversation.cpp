// Opt-in evaluation of conversation turns with a live model. It is not part of ctest.
// Usage: flock_live_conversation <FLOCK_HOME> <model-slug> [trials] [scenario filter]
// Offline check: flock_live_conversation --self-test
#include "flock/core.hpp"
#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <ranges>
#include <regex>
#include <set>
#include <unistd.h>

using namespace flockv0;

namespace {
void check(bool passed, const std::string& failure) {
    if (!passed) throw std::runtime_error(failure);
}

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
    std::map<Id,Json> questions;
    Json trace = Json::array();
    explicit Mixed(Model& model) : live(model) {}
    Decision decide(const ModelInput& input) override {
        const auto kind = input.event.value("kind","");
        if (kind == "conversation_turn") {
            trace = input.event.at("turn");
            auto decision = live.decide(input);
            trace.push_back({{"tool",decision.tool},{"args",decision.args}});
            return decision;
        }
        if (const auto question = questions.find(input.task.at("id").get<Id>());
            kind == "intake" && question != questions.end() && !input.event.contains("userAnswer"))
            return {"ask_user",question->second,"The fixture needs a decision"};
        if (kind == "result")
            return {"complete",{{"report",input.event.value("report","")}},"The fixture report is the result"};
        Json args = {{"instruction",input.task.value("request","")}};
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
        runs.at(run) = {.kind = Observation::Kind::Finished, .question = {}, .status = "completed",
                        .report = std::move(report), .resultRunId = run, .itemId = "report-" + run,
                        .settled = true, .detail = {}};
    }
};

using Tasks = std::map<Id,Json>;

struct Turn {
    Json reply;
    Tasks before, after;
    std::vector<Command> steers;
    std::size_t interrupts;

    Id message() const { return reply.at("replyToMessageId").get<Id>(); }
    Id replyMessage() const { return reply.at("messageId").get<Id>(); }
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
    void reach(Id id, std::string_view expected) {
        constexpr int tickLimit = 20;
        auto state = store.task(id).at("state").get<std::string>();
        for (int tick = 0; tick < tickLimit && state != expected; ++tick) {
            runtime.tick();
            state = store.task(id).at("state").get<std::string>();
        }
        check(state == expected, "fixture task " + std::to_string(id) + " expected state " +
              std::string(expected) + " after at most " + std::to_string(tickLimit) + " ticks, got " + state);
    }
    Id task(std::string request, std::string project, Json question = nullptr) {
        const auto id = store.submit(intake(std::move(request),std::move(project)));
        const bool asksQuestion = !question.is_null();
        if (asksQuestion) model.questions.emplace(id,std::move(question));
        reach(id,asksQuestion ? "waiting" : "running");
        if (asksQuestion) {
            const auto wait = store.task(id).at("wait");
            const auto& expected = model.questions.at(id);
            check(wait.at("reason") == "user_question" && wait.at("text") == expected.at("question") &&
                  wait.at("choices") == expected.at("choices"),
                  "fixture expected question " + expected.dump() + ", got wait " + wait.dump());
        }
        return id;
    }
    Tasks snapshot() const {
        Tasks tasks;
        for (auto& task : store.tasks("release",0,200)) {
            const auto id = task.at("id").get<Id>();
            tasks.emplace(id,std::move(task));
        }
        return tasks;
    }
    Turn send(const std::string& text, Id replyTo = 0) {
        auto before = snapshot();
        const auto steerCount = t3.notes.size();
        const auto interruptCount = t3.interrupted.size();
        const auto receipt = store.receive(intake(),"message-" + std::to_string(store.conversation("release",0,200).size()),
                                           text,replyTo);
        runtime.runConversations();
        for (const auto& row : store.conversation("release",0,200)) {
            const auto& data = row.at("data");
            if (row.at("kind") != "conversation_reply" || data.at("replyToMessageId") != receipt.at("messageId")) continue;
            const auto route = data.at("route").get<std::string>();
            if (route != "reply")
                throw std::runtime_error("Turn ended with " + route + ": " + data.at("text").get<std::string>());
            return {data,std::move(before),snapshot(),
                    {t3.notes.begin() + static_cast<std::ptrdiff_t>(steerCount),t3.notes.end()},
                    t3.interrupted.size() - interruptCount};
        }
        throw std::runtime_error("The turn did not reply");
    }
};

using Action = std::pair<std::string,Id>;

// Only applied, task-affecting tools appear in reply.actions. Read-only lookups are free.
void checkActions(const Turn& turn, std::initializer_list<Action> expected) {
    std::set<Action> remaining(expected);
    std::set<Id> touched, started;
    std::size_t steers = 0;
    for (const auto& [tool,id] : expected) {
        touched.insert(id);
        if (tool == "start_task") started.insert(id);
        if (tool == "message_session") ++steers;
    }
    for (const auto& action : turn.reply.at("actions")) {
        const auto tool = action.at("tool").get<std::string>();
        const auto id = action.at("taskId").get<Id>();
        check(remaining.erase({tool,id}) == 1, "unexpected action " + action.dump());
    }
    if (!remaining.empty()) {
        const auto& [tool,id] = *remaining.begin();
        throw std::runtime_error("missing " + tool + " for task " + std::to_string(id));
    }

    for (const auto& [id,task] : turn.before) {
        check(turn.after.contains(id), "task disappeared: " + std::to_string(id));
        if (!touched.contains(id))
            check(turn.after.at(id) == task, "untouched task changed: " + std::to_string(id));
    }
    std::set<Id> created;
    for (const auto& [id,task] : turn.after)
        if (!turn.before.contains(id)) created.insert(id);
    check(created == started,"the tasks created by the turn did not match its start actions");
    check(turn.steers.size() == steers && turn.interrupts == 0,"unexpected T3 steer or interrupt count");
}

std::string lower(std::string text) {
    std::ranges::transform(text,text.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool includes(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

bool rejectsStartupColumn(const std::string& text) {
    static const std::regex denial(
        R"((?:(?:\b(?:not|never)|n['’]t)\s+(?:include|report|provide|require|add|use))"
        R"(|\b(?:no|avoid|omit|exclude|skip|without(?:\s+(?:including|reporting|adding))?|instead of|rather than))"
        R"()\s+(?:(?:a|the|report|column|field|named|called)\s+)*`?\bstartup_p95_ms\b)"
        R"(|\bstartup_p95_ms\b`?(?:\s+(?:column|field))?\s+(?:is\s+(?:optional|unnecessary|not required)|(?:must|should)\s+not\s+be\s+(?:included|reported)))");
    return std::regex_search(text,denial);
}

std::vector<Json> turnEvents(const Scene& s, const Turn& turn, Id task, std::string_view kind) {
    std::vector<Json> found;
    for (const auto& row : s.store.history(task,turn.message(),200)) {
        if (row.at("id") >= turn.replyMessage()) break;
        if (row.at("kind") == kind) found.push_back(row.at("data"));
    }
    return found;
}

void checkCancellation(const Scene& s, const Turn& turn, Id id) {
    checkActions(turn,{{"cancel_task",id}});
    const auto& state = turn.after.at(id).at("state");
    check((state == "cancelling" || state == "cancelled") &&
          turnEvents(s,turn,id,"cancel_requested").size() == 1,"the requested task was not stopped by this turn");
}

bool asksWhichTask(const Json& reply) {
    bool mobile = false, desktop = false;
    for (const auto& option : reply.value("suggestions",Json::array())) {
        const auto& text = option.get_ref<const std::string&>();
        const bool namesMobile = includes(text,"モバイル");
        const bool namesDesktop = includes(text,"デスクトップ");
        mobile |= namesMobile && !namesDesktop;
        desktop |= namesDesktop && !namesMobile;
    }
    const auto text = lower(reply.at("text").get<std::string>());
    const bool asks = includes(text,"?") || includes(text,"？") || includes(text,"教えて") || includes(text,"ください");
    const bool identifiesChoice = includes(text,"どちら") || includes(text,"どの") || includes(text,"どっち") ||
        includes(text,"which") || (includes(text,"モバイル") && includes(text,"デスクトップ"));
    return (mobile && desktop) || (asks && identifiesChoice);
}

bool requiresStartupColumn(const std::string& criteria) {
    static const std::regex column(R"(\bstartup_p95_ms\b)");
    static const std::regex field(R"(\b(?:column|field)\b)");
    static const std::regex requirement(
        R"(\b(?:include[sd]?|contain[s]?|require[sd]?|add|must|report|provide|with)\b)");
    const auto text = lower(criteria);
    return std::regex_search(text,column) && std::regex_search(text,field) &&
        std::regex_search(text,requirement) && !rejectsStartupColumn(text);
}

void checkCriteria(const Scene& s, const Turn& turn, Id id) {
    checkActions(turn,{{"change_task",id}});
    const auto& before = turn.before.at(id);
    const auto& after = turn.after.at(id);
    const auto& criteria = after.at("doneWhen").get_ref<const std::string&>();
    const auto& revisions = before.at("revisions");
    const auto& savedRevisions = after.at("revisions");
    check(savedRevisions.size() == revisions.size() + 1,"expected one new saved revision");
    const auto& revision = savedRevisions.back();
    check(std::equal(revisions.begin(),revisions.end(),savedRevisions.begin()) &&
          revision.at("id") == after.at("currentRevision") &&
          revision.at("sourceMessageId") == turn.message() && revision.at("doneWhen") == criteria &&
          revision.at("intent") == before.at("intent") && after.at("intent") == before.at("intent") &&
          before.at("doneWhen") != criteria && requiresStartupColumn(criteria),
          "the new revision did not preserve the benchmark and require the startup_p95_ms report column");

    const auto accepted = turnEvents(s,turn,id,"instruction_accepted");
    check(turn.reply.at("actions").front().at("effect") == "replanning" && after.value("replanning",false) &&
          accepted.size() == 1 && accepted.front().at("messageId") == turn.message() &&
          after.at("pendingInstructions").back() == accepted.front() && accepted.front().at("conflicting") == true &&
          accepted.front().at("replaceKey") == "criteria" && accepted.front().at("revisionId") == revision.at("id") &&
          !rejectsStartupColumn(lower(accepted.front().at("text").get<std::string>())),
          "the turn did not queue a consistent criteria replan for its new revision");
}

bool explainsCanary(const std::string& reply) {
    const auto text = lower(reply);
    static const std::regex canary(R"(\brollout_canary\b)");
    static const std::regex directsAlternative(
        R"((?:(?:(?:^|[.!?;\n])\s*(?:please\s+)?|\b(?:but|then|and)\s+|\b(?:i|we|you)\s+(?:would|should)\s+))"
        R"((?:use|enable|choose|select|go with)|\b(?:i|we)\s+(?:would\s+)?(?:prefer|recommend(?:\s+(?:using|choosing|selecting))?)))"
        R"(\s+(?:the\s+)?`?\brollout_(?!canary\b)[a-z0-9_]+\b)");
    static const std::regex alternativeChoice(
        R"(\brollout_(?!canary\b)[a-z0-9_]+\b`?\s+(?:is\s+(?:recommended|preferred|selected)\b)"
        R"(|is\s+the\s+(?:recommended|preferred|selected|best)\s+(?:flag|option|choice)\b|should\s+be\s+used\b))");
    static const std::regex percentage(R"(\b[0-9]+(?:\.[0-9]+)?\s*(?:%|percent\b))");
    static const std::regex allowedPercentage(R"((?:5|100)\s*(?:%|percent))");
    static const std::regex duration(R"(\b(?:[0-9]+(?:\.[0-9]+)?|one|two|three|a)\s+(?:hours?|days?|weeks?)\b)");
    static const std::regex allowedDuration(R"(24\s+hours?|(?:1|one|a)\s+day)");
    static const std::regex wrongValue(
        R"(\b(?:not|never|instead of|rather than|avoid)\b(?!\s+(?:use\s+)?`?rollout_(?!canary\b)))"
        R"([^,;.!?\n]{0,40}\b(?:5\s*(?:%|percent)|rollout_canary|(?:one|a)\s+day|24\s+hours|everyone|all users))"
        R"(|\b(?:never|not)\s+(?:expand|roll out|enable))");
    static const std::regex rollout(
        R"(\b5\s*(?:%|percent)\s+(?:of\s+(?:the\s+)?)?(?:users|audience)\b)"
        R"([\s\S]{0,160}?\b(?:24\s+hours?|(?:1|one|a)\s+day)\b)"
        R"([\s\S]{0,160}?\b(?:then|after(?:ward|wards| that)?|before|followed by)\b)"
        R"([\s\S]{0,160}?\b(?:everyone|all users|(?:the\s+)?(?:entire|full)\s+(?:audience|user base)|100\s*(?:%|percent)|full rollout))");
    if (!std::regex_search(text,canary) || !std::regex_search(text,rollout) ||
        std::regex_search(text,wrongValue) || std::regex_search(text,directsAlternative) ||
        std::regex_search(text,alternativeChoice)) return false;
    for (auto it = std::sregex_iterator(text.begin(),text.end(),percentage); it != std::sregex_iterator(); ++it)
        if (!std::regex_match(it->str(),allowedPercentage)) return false;
    for (auto it = std::sregex_iterator(text.begin(),text.end(),duration); it != std::sregex_iterator(); ++it)
        if (!std::regex_match(it->str(),allowedDuration)) return false;
    return true;
}

enum class NoteTiming { Immediate, SafeBoundary };

void checkStagingNote(const Scene& s, const Turn& turn, Id id, NoteTiming timing) {
    const bool immediate = timing == NoteTiming::Immediate;
    checkActions(turn,{{immediate ? "message_session" : "send_to_task",id}});
    const auto& before = turn.before.at(id);
    const auto& after = turn.after.at(id);
    const auto kind = immediate ? "instruction_steered" : "instruction_accepted";
    const auto notes = turnEvents(s,turn,id,kind);
    check(notes.size() == 1 && notes.front().at("messageId") == turn.message(),
          "expected one new " + std::string(kind) + " event for this message");
    const auto& note = notes.front();
    const auto& instruction = note.at("text").get_ref<const std::string&>();
    static const std::regex staging(
        R"(\b(?:use|switch to|target|connect to|benchmark against|run against)\s+(?:the\s+)?staging\s+(?:database|db)\b)");
    static const std::regex rejectsStaging(
        R"((?:(?:\b(?:not|never)|n['’]t)\s+(?:use|switch to|target|connect to|benchmark against|run against))"
        R"(|\b(?:no|avoid|skip|without|instead of|rather than))\s+(?:the\s+)?staging\b)");
    static const std::regex otherDatabase(R"(\b(?:production|prod|development|dev)\s+(?:database|db)\b)");
    const auto text = lower(instruction);
    check(std::regex_search(text,staging) && !std::regex_search(text,rejectsStaging) &&
          !std::regex_search(text,otherDatabase),
          "the new instruction did not direct the benchmark to use the staging database");

    const auto& effect = turn.reply.at("actions").front().at("effect");
    const auto& run = before.at("activeRun");
    if (immediate) {
        const auto& steer = turn.steers.front();
        check(effect == "delivered" && note.at("run").at("runId") == run.at("runId") &&
              note.at("run").at("threadId") == run.at("threadId") &&
              steer.target.runId == run.at("runId").get<std::string>() &&
              steer.target.threadId == run.at("threadId").get<std::string>() && steer.instruction == instruction && after == before,
              "the steer did not reach the current run without changing the task");
    } else {
        auto pending = before.value("pendingInstructions",Json::array());
        pending.push_back(note);
        check(effect == "accepted" && note.at("conflicting") == false &&
              note.at("revisionId") == before.at("currentRevision") && after.at("pendingInstructions") == pending &&
              after.at("state") == before.at("state") && after.at("activeRun") == run &&
              after.at("currentRevision") == before.at("currentRevision") && after.at("revisions") == before.at("revisions") &&
              !after.value("replanning",false),"the note was not queued for the current revision's next safe boundary");
    }
}

using Scenario = std::function<void(Scene&)>;

const std::vector<std::pair<std::string,Scenario>> scenarios{
    {"new request on a named destination",[](Scene& s) {
        const auto turn = s.send("Run the project's existing test suite on the cloud Mac and report any failures");
        check(turn.after.size() == 1,"expected one new task");
        const auto& [id,task] = *turn.after.begin();
        checkActions(turn,{{"start_task",id}});
        check(task.at("environmentId") == "cloud","the task did not use the cloud Mac");
    }},
    {"a reply that matches an offered choice answers the question",[](Scene& s) {
        const auto id = s.task("Prepare the release label","label",
            {{"question","Which label should I use?"},{"choices",{"Cancel this task","Status"}}});
        const auto wait = s.store.task(id).at("wait");
        const auto turn = s.send("Cancel this task",wait.at("messageId").get<Id>());
        checkActions(turn,{{"answer_task_question",id}});
        const auto answers = turnEvents(s,turn,id,"user_answer");
        const auto& task = turn.after.at(id);
        check(answers.size() == 1 && answers.front().at("messageId") == turn.message() &&
              answers.front().at("waitId") == wait.at("id") &&
              lower(answers.front().at("text").get<std::string>()) == "cancel this task" &&
              task.at("state") == "deciding" && task.at("wait").is_null(),
              "the offered choice was not saved as the answer to the open question");
    }},
    {"a named stop cancels only that task",[](Scene& s) {
        s.task("Write the release notes","notes");
        const auto benchmark = s.task("Benchmark the startup time","benchmark");
        const auto turn = s.send("Stop the benchmark");
        checkCancellation(s,turn,benchmark);
    }},
    {"an ambiguous stop waits for a named target",[](Scene& s) {
        const auto mobile = s.task("モバイル版のログイン画面を修正","mobile");
        s.task("デスクトップ版のログイン画面を修正","desktop");
        const auto turn = s.send("その作業をキャンセルして");
        checkActions(turn,{});
        check(asksWhichTask(turn.reply),"the reply did not ask which task to cancel or offer both choices");
        const auto followUp = s.send("モバイル版のほうです",turn.replyMessage());
        checkCancellation(s,followUp,mobile);
    }},
    {"new criteria change the task",[](Scene& s) {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto turn = s.send("For the startup benchmark, change only the completion criteria to require a report column named startup_p95_ms");
        checkCriteria(s,turn,id);
    }},
    {"two requests in one message start two tasks",[](Scene& s) {
        const auto turn = s.send("Write the release notes. Separately, audit the third-party licenses.");
        check(turn.after.size() == 2,"expected two new tasks");
        const auto first = turn.after.begin();
        const auto second = std::next(first);
        checkActions(turn,{{"start_task",first->first},{"start_task",second->first}});
        static const std::regex notes(R"(\brelease[- ]notes?\b)");
        static const std::regex licenses(R"(\blicen[cs](?:e[sd]?|ing)\b)");
        static const std::regex audit(R"(\b(?:audit|review|check|inspect|verify|assess)\b)");
        static const std::regex denial(
            R"(\b(?:not|no|never|avoid|omit|exclude|skip|without|optional|unnecessary|instead of|rather than)\b|n['’]t\b)");
        const auto role = [&](const Json& task) {
            const auto request = lower(task.at("request").get<std::string>());
            const bool hasNotes = std::regex_search(request,notes);
            const bool hasLicenses = std::regex_search(request,licenses);
            if (hasNotes == hasLicenses || std::regex_search(request,denial)) return "";
            if (hasNotes) return "release notes";
            return std::regex_search(request,audit) ? "license audit" : "";
        };
        const std::set<std::string> roles{role(first->second),role(second->second)};
        check(roles == std::set<std::string>{"release notes","license audit"},
              "the tasks did not map one-to-one to release notes and a license audit");
    }},
    {"explaining a result changes no work",[](Scene& s) {
        const auto id = s.task("Compare three rollout plans","plans");
        s.t3.finish(s.store.task(id).at("activeRun").at("runId").get<std::string>(),
                    "1. Use rollout_global for a big bang release\n"
                    "2. Use rollout_canary for 5% of users for a day, then everyone\n"
                    "3. Use rollout_team to enable one team at a time");
        s.reach(id,"completed");
        const auto turn = s.send("Explain the second option in more detail and include its rollout flag name");
        checkActions(turn,{});
        check(explainsCanary(turn.reply.at("text").get_ref<const std::string&>()),
              "the reply did not explain rollout_canary's 5% one-day rollout followed by full release");
    }},
    {"a note for running work reaches that task immediately",[](Scene& s) {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto turn = s.send("Tell the running benchmark session right now to use the staging database from now on. Keep the run going.");
        checkStagingNote(s,turn,id,NoteTiming::Immediate);
    }},
    {"a note for running work waits for a safe boundary",[](Scene& s) {
        const auto id = s.task("Benchmark the startup time","benchmark");
        const auto turn = s.send("Leave a note for the benchmark task to use the staging database at its next safe point. Let the current run continue.");
        checkStagingNote(s,turn,id,NoteTiming::SafeBoundary);
    }},
};

struct Script : Model {
    std::vector<Decision> steps;
    std::size_t next = 0;

    explicit Script(std::vector<Decision> decisions) : steps(std::move(decisions)) {}
    Decision decide(const ModelInput& input) override {
        auto decision = steps.at(next++);
        if (!decision.args.contains("fixtureRequest")) return decision;
        const auto request = decision.args.at("fixtureRequest");
        decision.args.erase("fixtureRequest");
        const auto& tasks = input.task.at("tasks");
        const auto task = std::ranges::find_if(tasks,[&](const Json& item) { return item.at("request") == request; });
        check(task != tasks.end(),"scripted task was missing: " + request.dump());
        decision.args["taskId"] = task->at("taskId");
        if (decision.tool == "answer_task_question") {
            decision.args["waitId"] = task->at("wait").at("waitId");
        } else {
            decision.args["generation"] = task->at("generation");
            if (decision.tool == "change_task") decision.args["revisionId"] = task->at("revisionId");
        }
        return decision;
    }
};

void selfTest() {
    int passed = 0;
    const auto run = [&](std::string_view name, const Scenario& scenario, std::vector<Decision> steps,
                         std::string_view expectedFailure = {}) {
        Script script(std::move(steps));
        Mixed model(script);
        Scene scene(model,"offline");
        std::string failure;
        try {
            scenario(scene);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        check(script.next == script.steps.size() &&
              (expectedFailure.empty() ? failure.empty() : includes(failure,expectedFailure)),
              std::string(name) + " expected " + (expectedFailure.empty() ? "a pass" : std::string(expectedFailure)) +
              ", got " + (failure.empty() ? "a pass" : failure) + "; script steps " +
              std::to_string(script.next) + "/" + std::to_string(script.steps.size()));
        ++passed;
    };
    const auto test = [&](std::string_view prefix, std::vector<Decision> steps, std::string_view failure = {}) {
        const auto scenario = std::ranges::find_if(scenarios,[&](const auto& item) { return item.first.starts_with(prefix); });
        check(scenario != scenarios.end(),"missing offline scenario: " + std::string(prefix));
        run(scenario->first,scenario->second,std::move(steps),failure);
    };
    const auto say = [](const std::string& text) { return Decision{"reply",{{"text",text}},"Scripted reply"}; };
    const auto start = [](const std::string& request) {
        return Decision{"start_task",{{"request",request}},"Scripted start"};
    };
    const auto action = [](std::string tool, std::string_view task, Json args = Json::object()) {
        args["fixtureRequest"] = task;
        return Decision{std::move(tool),std::move(args),"Scripted action"};
    };
    const auto done = say("Done.");
    const auto notes = start("Write the release notes");
    const auto audit = start("Audit the third-party licenses");
    const auto answer = action("answer_task_question","Prepare the release label",{{"text","Cancel this task"}});
    constexpr auto benchmark = "Benchmark the startup time";
    const auto stop = action("cancel_task",benchmark);
    const auto stopMobile = action("cancel_task","モバイル版のログイン画面を修正");
    const auto clarify = say("モバイル版とデスクトップ版のどちらをキャンセルしますか？");
    auto choices = say("対象を選んでください。");
    choices.args["suggestions"] = {"モバイル版","デスクトップ版"};
    const auto criteria = [&](std::string_view text, std::string_view instruction = "Update the report criteria") {
        return action("change_task",benchmark,{{"doneWhen",text},{"instruction",instruction}});
    };
    const auto steer = [&](std::string_view text) { return action("message_session",benchmark,{{"text",text}}); };
    const auto queue = action("send_to_task",benchmark,{{"instruction","Use the staging database at the next safe point"}});

    auto cloud = start("Run the existing test suite");
    cloud.args["environment"] = "cloud Mac";
    test("new request",{cloud,done});
    test("a reply",{answer,done});
    test("a reply",{answer,notes,done},"unexpected action");
    test("a named stop",{stop,done});
    test("a named stop",{stop,action("send_to_task","Write the release notes",{{"instruction","Pause drafting"}}),done},
         "unexpected action");
    test("an ambiguous stop",{clarify,stopMobile,done});
    test("an ambiguous stop",{choices,stopMobile,done});
    test("an ambiguous stop",{say("キャンセルしました。")},"did not ask which task");
    test("new criteria",{criteria("Include a report column named startup_p95_ms"),done});
    constexpr auto columnConstraint = "Include a report column named startup_p95_ms without changing the benchmark";
    test("new criteria",{criteria(columnConstraint,columnConstraint),done});
    for (auto text : {"Do not include the startup_p95_ms column",
                      "Include a report column named startup_p95_ms. Omit startup_p95_ms from the final report.",
                      "Add a report column, but exclude startup_p95_ms."})
        test("new criteria",{criteria(text),done},"did not preserve the benchmark and require");
    test("new criteria",{criteria(columnConstraint,"Omit startup_p95_ms"),done},"did not queue a consistent criteria replan");
    test("two requests",{notes,audit,done});
    test("two requests",{start("Write the release notes and audit the third-party licenses"),audit,done},"one-to-one");
    test("explaining",{say("Use rollout_canary for 5% of users for 24 hours, then expand it to everyone.")});
    test("explaining",{say("Use rollout_canary for 5% of users for a day, then expand to everyone. "
                            "Unlike rollout_global, this limits initial exposure.")});
    test("explaining",{say("Use rollout_canary, not rollout_global, for 5% of users for a day, then expand to everyone.")});
    test("explaining",{say("Use rollout_canary for 5% of users for a day, then expand to everyone. Do not use rollout_global.")});
    test("explaining",{say("The first option is to use rollout_global. "
                            "For the second option, rollout_canary starts with 5% of users for a day, then expands to everyone.")});
    test("explaining",{say("Start with 5% of users for a day, then expand to everyone.")},"did not explain");
    test("explaining",{say("Use rollout_global for 5% of users for a day, then expand to everyone. "
                            "The other flag was rollout_canary.")},"did not explain");
    test("explaining",{say("rollout_canary starts with 5% of users for a day, then expands to everyone. "
                            "I recommend using rollout_immediate.")},"did not explain");
    test("explaining",{say("rollout_canary starts with 5% of users for a day, then expands to everyone. "
                            "rollout_immediate is the recommended option.")},"did not explain");
    test("explaining",{say("Use rollout_canary for 50% of users for a week, then everyone.")},"did not explain");
    test("explaining",{say("Use rollout_canary for 5% of users for a day, then everyone. Start with 50%.")},"did not explain");
    test("explaining",{say("Use rollout_canary for 5% of users for a day, then never expand it to everyone.")},"did not explain");
    test("a note for running work reaches",{steer("Use the staging database"),done});
    test("a note for running work reaches",{steer("Use the staging database; do not stop the current run"),done});
    test("a note for running work reaches",{queue,done},"unexpected action");
    test("a note for running work reaches",
         {steer("Do not use the staging database"),done},"did not direct");
    test("a note for running work reaches",
         {steer("Use the staging database for now. Do not use staging for the benchmark."),done},"did not direct");
    test("a note for running work waits",{queue,done});
    test("a note for running work waits",{steer("Use the staging database"),done},"unexpected action");
    test("a note for running work waits",
         {action("send_to_task",benchmark,{{"instruction","Avoid staging database"}}),done},"did not direct");

    constexpr auto stagingBenchmark = "Benchmark startup using the staging database";
    const auto queuedNote = [&](std::string text) {
        return action("send_to_task",stagingBenchmark,{{"instruction",std::move(text)}});
    };
    run("old intent and instruction cannot satisfy the new turn",[&](Scene& s) {
        const auto id = s.task(stagingBenchmark,"benchmark");
        const auto prior = s.send("Use the staging database");
        checkStagingNote(s,prior,id,NoteTiming::SafeBoundary);
        const auto turn = s.send("Use staging for the next batch too");
        checkStagingNote(s,turn,id,NoteTiming::SafeBoundary);
    },{queuedNote("Use the staging database"),done,queuedNote("Continue measuring"),done},"did not direct");

    for (const auto* field : {"state","currentRevision","pendingInstructions"}) {
        run("unreported change to an untouched task",[&](Scene& s) {
            const auto id = s.task(benchmark,"benchmark");
            auto turn = s.send("What's the status?");
            turn.after.at(id)[field] = "unexpected change";
            checkActions(turn,{});
        },{done},"untouched task changed");
    }
    std::cout << "Offline scoring self-test: " << passed << " cases passed\n";
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
        try {
            selfTest();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: flock_live_conversation <FLOCK_HOME> <model-slug> [trials] [scenario filter]\n"
                     "       flock_live_conversation --self-test\n";
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
                    scenario(scene);
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
