#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "live_conversation_judge.hpp"
#include "live_setup.hpp"
#include "model.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace flockv0;
using namespace flockv0::live_test;

namespace {
void check(bool passed, const std::string& failure) {
    if (!passed) throw std::runtime_error(failure);
}

void checkReply(const Json& data) {
    const auto outcome = data.at("route").get<std::string>();
    if (outcome != "reply") throw std::runtime_error("Turn ended with " + outcome + ": " + data.value("text", ""));
}

struct Mixed : Model {
    Model& live;
    std::map<Id, Json> questions;
    Json trace = Json::array();
    explicit Mixed(Model& model) : live(model) {}
    ModelTurn decide(const ModelInput& input) override {
        const auto kind = input.event.value("kind", "");
        if (kind == "conversation_turn") {
            trace = input.event.at("turn");
            auto turn = live.decide(input);
            for (const auto& decision : turn.calls) trace.push_back({{"tool", decision.tool}, {"args", decision.args}});
            return turn;
        }
        if (const auto question = questions.find(input.task.at("id").get<Id>());
            kind == "intake" && question != questions.end() && !input.event.contains("userAnswer"))
            return {{{"ask_user", question->second, "The fixture needs a decision"}}};
        if (kind == "result") return {{{"complete", {{"report", input.event.value("report", "")}}, "The fixture report is the result"}}};
        Json args = {{"instruction", input.task.value("request", "")}};
        if (input.task.value("doneWhen", "").empty()) args["doneWhen"] = "Report the result with evidence";
        return {{{"delegate", args, "Run the fixture work"}}};
    }
};

struct Runs : T3 {
    std::map<std::string, Observation> runs;
    std::vector<std::string> interrupted;
    std::string authGeneration(const std::string&, const Json&) override { return "live-eval"; }
    Delivery send(const Command&) override {
        const auto id = std::to_string(runs.size() + 1);
        runs["run-" + id] = {};
        return {Delivery::Kind::Accepted, {"thread-" + id, "run-" + id, {}}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return runs[run.runId]; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef& run) override {
        interrupted.push_back(run.runId);
        return InterruptOutcome::Requested;
    }
    void finish(const std::string& run, std::string report) {
        runs.at(run) = {.kind = Observation::Kind::Finished,
                        .question = {},
                        .status = "completed",
                        .report = std::move(report),
                        .resultRunId = run,
                        .itemId = "report-" + run,
                        .detail = {}};
    }
};

using Tasks = std::map<Id, Json>;

struct Turn {
    Json reply;
    Tasks before, after;

    Id message() const { return reply.at("replyToMessageId").get<Id>(); }
    Id replyMessage() const { return reply.at("messageId").get<Id>(); }
};

struct Scene {
    ScratchDirectory directory;
    Store store;
    Mixed& model;
    Runs t3;
    Runtime runtime;
    Judge judge;
    Json config;
    Scene(Mixed& mixed, const std::string& slug, Judge textJudge)
        : store(directory.path / "state.db"), model(mixed), runtime(store, model, t3, [](std::string_view) { return std::string{}; }),
          judge(std::move(textJudge)),
          config({{"modelProvider", "chatgpt"}, {"model", slug}, {"conversationEnvironments", {{"cloud Mac", "cloud"}}}}) {}

    Intake intake(std::string request = {}, std::string project = "project") const {
        return {"release", std::move(request), "desktop", std::move(project), config};
    }
    void reach(Id id, std::string_view expected) {
        constexpr int tickLimit = 20;
        auto state = std::string(taskStateName(store.task(id).task.state));
        for (int tick = 0; tick < tickLimit && state != expected; ++tick) {
            runtime.tick();
            state = std::string(taskStateName(store.task(id).task.state));
        }
        check(state == expected, "fixture task " + std::to_string(id) + " expected state " + std::string(expected) + " after at most " +
                                     std::to_string(tickLimit) + " ticks, got " + state);
    }
    Id task(std::string request, std::string project, Json question = nullptr) {
        const auto id = store.submit(intake(std::move(request), std::move(project)));
        const bool asksQuestion = !question.is_null();
        if (asksQuestion) model.questions.emplace(id, std::move(question));
        reach(id, asksQuestion ? "waiting" : "running");
        if (asksQuestion) {
            const auto wait = taskJson(store.task(id)).at("wait");
            const auto& expected = model.questions.at(id);
            check(wait.at("reason") == "user_question" && wait.at("text") == expected.at("question") &&
                      wait.at("choices") == expected.at("choices"),
                  "fixture expected question " + expected.dump() + ", got wait " + wait.dump());
        }
        return id;
    }
    Tasks snapshot() const {
        Tasks tasks;
        for (auto& task : taskJson(store.tasks("", 0, 200))) {
            const auto id = task.at("id").get<Id>();
            tasks.emplace(id, std::move(task));
        }
        return tasks;
    }
    Turn send(const std::string& text, Id replyTo = 0) {
        auto before = snapshot();
        const auto runCount = t3.runs.size();
        const auto interruptCount = t3.interrupted.size();
        const auto receipt =
            store.receive(localOwner, intake(), "message-" + std::to_string(store.conversation("release", 0, 200).size()), text, replyTo);
        runtime.runConversations();
        check(t3.runs.size() == runCount && t3.interrupted.size() == interruptCount, "unexpected T3 send or interrupt");
        for (const auto& row : store.conversation("release", 0, 200)) {
            const auto& data = row.at("data");
            if (row.at("kind") != "conversation_reply" || data.at("replyToMessageId") != receipt.at("messageId")) continue;
            checkReply(data);
            return {data, std::move(before), snapshot()};
        }
        throw std::runtime_error("The turn did not reply");
    }
};

struct Action {
    std::string_view tool;
    Id task;
    std::string_view effect;
};

std::vector<Json> checkEvents(const Scene& s, const Turn& turn, Id task, std::initializer_list<std::string_view> expected) {
    std::vector<std::string> kinds;
    std::vector<Json> payloads;
    for (const auto& row : s.store.history(task, turn.message(), 200)) {
        if (row.at("id") >= turn.replyMessage()) break;
        kinds.push_back(row.at("kind").get<std::string>());
        payloads.push_back(row.at("data"));
    }
    check(std::ranges::equal(kinds, expected),
          "task " + std::to_string(task) + " expected journal events " + Json(expected).dump() + ", got " + Json(kinds).dump());
    return payloads;
}

void checkActions(const Scene& s, const Turn& turn, const std::vector<Action>& expected) {
    auto actions = Json::array();
    for (const auto& [tool, task, effect] : expected) actions.push_back({{"tool", tool}, {"taskId", task}, {"effect", effect}});
    const auto& applied = turn.reply.at("actions");
    check(applied.is_array() && std::ranges::is_permutation(applied, actions),
          "expected applied actions " + actions.dump() + ", got " + applied.dump());

    for (const auto& [id, task] : turn.before) {
        check(turn.after.contains(id), "task disappeared: " + std::to_string(id));
        if (std::ranges::any_of(expected, [id](const Action& action) { return action.task == id; })) continue;
        check(turn.after.at(id) == task, "untouched task changed: " + std::to_string(id));
        checkEvents(s, turn, id, {});
    }
    for (const auto& [id, task] : turn.after) {
        if (turn.before.contains(id)) continue;
        check(std::ranges::any_of(expected, [id](const Action& action) { return action.task == id && action.tool == "start_task"; }),
              "unreported task creation: " + std::to_string(id));
    }
}

std::vector<Id> checkStarts(const Scene& s, const Turn& turn, std::size_t count) {
    std::vector<Id> created;
    std::vector<Action> actions;
    for (const auto& [id, task] : turn.after) {
        if (turn.before.contains(id)) continue;
        created.push_back(id);
        actions.push_back({"start_task", id, "started"});
    }
    check(created.size() == count, "expected " + std::to_string(count) + " new tasks");
    checkActions(s, turn, actions);
    for (const auto id : created) {
        const auto& task = turn.after.at(id);
        const auto request = checkEvents(s, turn, id, {"request"}).front();
        check(task.at("state") == "queued" && task.at("sourceMessageId") == turn.message() && request.at("messageId") == turn.message() &&
                  request.at("source") == "conversation" && request.at("text") == task.at("request"),
              "new task is missing its queued conversation request");
    }
    return created;
}

void checkCancellation(const Scene& s, const Turn& turn, Id id) {
    checkActions(s, turn, {{"cancel_task", id, "cancelling"}});
    checkEvents(s, turn, id, {"cancel_requested"});
    check(turn.after.at(id).at("state") == "cancelling", "the requested task did not enter cancellation");
}

void checkQueuedInstruction(const Turn& turn, Id id, const Json& note, bool conflicting) {
    auto pending = turn.before.at(id).value("pendingInstructions", Json::array());
    pending.push_back(note);
    const auto& after = turn.after.at(id);
    check(note.at("messageId") == turn.message() && note.at("conflicting") == conflicting &&
              note.at("revisionId") == after.at("currentRevision") && after.at("pendingInstructions") == pending &&
              after.value("replanning", false) == conflicting,
          "the turn did not queue one instruction for its current revision");
}

void checkMeaning(const Scene& s, Criterion criterion, std::string_view text, std::string_view failure) {
    const auto judgment = s.judge(criterion, text);
    check(judgment.verdict == Verdict::Match, std::string(failure) + "; judge quoted " + Json(judgment.evidence).dump());
}

void checkCriteria(const Scene& s, const Turn& turn, Id id) {
    checkActions(s, turn, {{"change_task", id, "replanning"}});
    const auto events = checkEvents(s, turn, id, {"task_revision", "instruction_accepted"});
    const auto& before = turn.before.at(id);
    const auto& after = turn.after.at(id);
    const auto& criteria = after.at("doneWhen").get_ref<const std::string&>();
    const auto& revision = after.at("revisions").back();
    const auto& revisionEvent = events.front();
    auto expectedRevisions = before.at("revisions");
    expectedRevisions.push_back(revision);
    check(after.at("revisions") == expectedRevisions && revision.at("id") == after.at("currentRevision") &&
              revision.at("id") != before.at("currentRevision") && revisionEvent.at("revisionId") == revision.at("id") &&
              revisionEvent.at("sourceMessageId") == turn.message() && revision.at("sourceMessageId") == turn.message() &&
              revision.at("doneWhen") == criteria && before.at("doneWhen") != criteria && revision.at("intent") == before.at("intent"),
          "expected one journaled criteria revision retaining the original intent");
    for (const auto* field :
         {"intent", "request", "environmentId", "projectId", "gatewayEnvironmentId", "pendingDestination", "state", "activeRun"})
        check(before.value(field, Json()) == after.value(field, Json()), "a criteria-only change altered " + std::string(field));

    const auto& note = events.back();
    checkQueuedInstruction(turn, id, note, true);
    checkMeaning(s, Criterion::CompletionColumn, criteria, "saved criteria did not require the startup_p95_ms column");
    checkMeaning(s, Criterion::CriteriaInstruction, note.at("text").get_ref<const std::string&>(),
                 "the queued instruction conflicted with the updated criteria");
}

void checkStagingNote(const Scene& s, const Turn& turn, Id id) {
    checkActions(s, turn, {{"send_to_task", id, "accepted"}});
    const auto note = checkEvents(s, turn, id, {"instruction_accepted"}).front();
    checkQueuedInstruction(turn, id, note, false);

    auto before = turn.before.at(id);
    auto after = turn.after.at(id);
    for (const auto* field : {"pendingInstructions", "generation"}) {
        before.erase(field);
        after.erase(field);
    }
    check(after == before, "the queued note altered the running task");
    checkMeaning(s, Criterion::StagingInstruction, note.at("text").get_ref<const std::string&>(),
                 "the new instruction did not direct the benchmark to use the staging database");
}

using Scenario = std::function<void(Scene&)>;

const std::vector<std::pair<std::string, Scenario>> scenarios{
    {"new request on a named destination",
     [](Scene& s) {
         s.task("Inventory test fixtures", "inventory");
         const auto turn = s.send("Run the project's existing test suite on the cloud Mac and report any failures");
         const auto id = checkStarts(s, turn, 1).front();
         check(turn.after.at(id).at("environmentId") == "cloud", "the task did not use the cloud Mac");
     }},
    {"a reply that matches an offered choice answers the question",
     [](Scene& s) {
         s.task("Inventory test fixtures", "inventory");
         const auto id = s.task("Prepare the release label", "label",
                                {{"question", "Which label should I use?"}, {"choices", {"Cancel this task", "Status"}}});
         const auto wait = taskJson(s.store.task(id)).at("wait");
         const auto turn = s.send("Cancel this task", wait.at("messageId").get<Id>());
         checkActions(s, turn, {{"answer_task_question", id, "answered"}});
         const auto events = checkEvents(s, turn, id, {"user_answer", "wait_closed", "inbox_decision"});
         const auto& answer = events[0];
         const auto& closure = events[1];
         const auto& task = turn.after.at(id);
         check(answer.at("messageId") == turn.message() && answer.at("waitId") == wait.at("id") && answer.at("conversation") == true &&
                   answer.at("text") == "Cancel this task" && closure.at("waitId") == wait.at("id") &&
                   closure.at("resolution") == "answer" && task.at("state") == "deciding" && task.at("wait").is_null(),
               "the offered choice was not saved as the answer to the open question");
     }},
    {"a named stop cancels only that task",
     [](Scene& s) {
         s.task("Write the release notes", "notes");
         const auto benchmark = s.task("Benchmark the startup time", "benchmark");
         checkCancellation(s, s.send("Stop the benchmark"), benchmark);
     }},
    {"an ambiguous stop waits for a named target",
     [](Scene& s) {
         const auto mobile = s.task("モバイル版のログイン画面を修正", "mobile");
         s.task("デスクトップ版のログイン画面を修正", "desktop");
         const auto turn = s.send("その作業をキャンセルして");
         checkActions(s, turn, {});
         auto clarification = turn.reply.at("text").get<std::string>();
         for (const auto& option : turn.reply.value("suggestions", Json::array()))
             clarification += "\n" + option.get_ref<const std::string&>();
         checkMeaning(s, Criterion::CancellationTarget, clarification, "the reply did not ask which task to cancel");
         checkCancellation(s, s.send("モバイル版のほうです", turn.replyMessage()), mobile);
     }},
    {"new criteria change the task",
     [](Scene& s) {
         s.task("Inventory test fixtures", "inventory");
         const auto id = s.task("Benchmark the startup time", "benchmark");
         const auto turn =
             s.send("For the startup benchmark, change only the completion criteria to require a report column named startup_p95_ms");
         checkCriteria(s, turn, id);
     }},
    {"two requests in one message start two tasks",
     [](Scene& s) {
         s.task("Inventory test fixtures", "inventory");
         const auto turn = s.send("Write the release notes. Separately, audit the third-party licenses.");
         const auto ids = checkStarts(s, turn, 2);
         const auto classify = [&](Id id) {
             return s.judge(Criterion::SplitRequest, turn.after.at(id).at("request").get_ref<const std::string&>());
         };
         const auto first = classify(ids[0]);
         const auto second = classify(ids[1]);
         check((first.verdict == Verdict::ReleaseNotes && second.verdict == Verdict::LicenseAudit) ||
                   (first.verdict == Verdict::LicenseAudit && second.verdict == Verdict::ReleaseNotes),
               "the tasks did not map one-to-one to release notes and a license audit; judge quoted " +
                   Json::array({first.evidence, second.evidence}).dump());
     }},
    {"explaining a result changes no work",
     [](Scene& s) {
         s.task("Inventory test fixtures", "inventory");
         const auto id = s.task("Compare three rollout plans", "plans");
         s.t3.finish(s.store.task(id).task.activeRun.value().ref.runId, "1. Use rollout_global for a big bang release\n"
                                                                        "2. Use rollout_canary for 5% of users for a day, then everyone\n"
                                                                        "3. Use rollout_team to enable one team at a time");
         s.reach(id, "completed");
         const auto turn = s.send("Explain the second option in more detail and include its rollout flag name");
         checkActions(s, turn, {});
         checkMeaning(s, Criterion::CanaryExplanation, turn.reply.at("text").get_ref<const std::string&>(),
                      "the reply did not explain rollout_canary's 5% one-day rollout followed by full release");
     }},
    {"a note for running work waits for a safe boundary",
     [](Scene& s) {
         s.task("Inventory test fixtures", "inventory");
         const auto id = s.task("Benchmark the startup time", "benchmark");
         const auto turn = s.send(
             "Leave a note for the benchmark task to use the staging database at its next safe point. Let the current run continue.");
         checkStagingNote(s, turn, id);
     }},
};

struct Script : Model {
    std::vector<Decision> steps;
    std::size_t next = 0;

    explicit Script(std::vector<Decision> decisions) : steps(std::move(decisions)) {}
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        auto decision = steps.at(next++);
        if (!decision.args.contains("fixtureRequest")) return decision;
        const auto request = decision.args.at("fixtureRequest");
        decision.args.erase("fixtureRequest");
        const auto& tasks = input.task.at("tasks");
        const auto task = std::ranges::find_if(tasks, [&](const Json& item) { return item.at("request") == request; });
        check(task != tasks.end(), "scripted task was missing: " + request.dump());
        decision.args["taskId"] = task->at("taskId");
        decision.args["generation"] = task->at("generation");
        if (decision.tool == "answer_task_question") decision.args["waitId"] = task->at("wait").at("waitId");
        if (decision.tool == "change_task" || decision.tool == "answer_task_question") decision.args["revisionId"] = task->at("revisionId");
        return decision;
    }
};

struct RecordedJudgment {
    Criterion criterion;
    std::string_view text;
    std::string_view verdict = "match";
};

void selfTest() {
    using enum Criterion;
    judgeSelfTest();
    int passed = 0;
    const auto run = [&](std::string_view name, const Scenario& scenario, std::vector<Decision> steps,
                         std::vector<RecordedJudgment> judgments, std::string_view expectedFailure) {
        Script script(std::move(steps));
        Mixed model(script);
        std::size_t nextJudgment = 0;
        Scene scene(model, "offline", [&](Criterion criterion, std::string_view text) {
            const auto& recorded = judgments.at(nextJudgment++);
            check(criterion == recorded.criterion && text == recorded.text, "unexpected text sent to the judge: " + std::string(text));
            return parseJudgment(criterion, text, Json{{"verdict", recorded.verdict}, {"evidence", recorded.text}}.dump());
        });
        std::string failure;
        try {
            scenario(scene);
        } catch (const std::exception& error) {
            failure = error.what();
        }
        check(script.next == script.steps.size() && nextJudgment == judgments.size() &&
                  (expectedFailure.empty() ? failure.empty() : failure.find(expectedFailure) != std::string::npos),
              std::string(name) + " expected " + (expectedFailure.empty() ? "a pass" : std::string(expectedFailure)) + ", got " +
                  (failure.empty() ? "a pass" : failure) + "; script steps " + std::to_string(script.next) + "/" +
                  std::to_string(script.steps.size()) + "; judgments " + std::to_string(nextJudgment) + "/" +
                  std::to_string(judgments.size()));
        ++passed;
    };
    const auto test = [&](std::string_view prefix, std::vector<Decision> steps, std::vector<RecordedJudgment> judgments = {},
                          std::string_view failure = {}) {
        const auto scenario = std::ranges::find_if(scenarios, [&](const auto& item) { return item.first.starts_with(prefix); });
        check(scenario != scenarios.end(), "missing offline scenario: " + std::string(prefix));
        run(scenario->first, scenario->second, std::move(steps), std::move(judgments), failure);
    };
    const auto say = [](std::string_view text) { return Decision{"reply", {{"text", text}}, "Scripted reply"}; };
    const auto start = [](std::string_view request) { return Decision{"start_task", {{"request", request}}, "Scripted start"}; };
    const auto action = [](std::string tool, std::string_view task, Json args = Json::object()) {
        args["fixtureRequest"] = task;
        return Decision{std::move(tool), std::move(args), "Scripted action"};
    };
    const auto done = say("Done.");
    constexpr auto notes = "Draft the notes for this release.";
    constexpr auto audit = "Review licensing for the external dependencies.";
    constexpr auto benchmark = "Benchmark the startup time";
    constexpr auto mobile = "モバイル版のログイン画面を修正";
    constexpr auto clarify = "モバイル版とデスクトップ版のどちらをキャンセルしますか？";
    constexpr auto column = "The report must have a startup_p95_ms column.";
    constexpr auto update = "Update the report criteria.";
    constexpr auto staging = "Use the staging database at the next safe point.";
    constexpr auto explain = "Use rollout_canary for one in twenty users for a day, then expand to everyone.";
    const auto stop = action("cancel_task", benchmark);
    const auto stopMobile = action("cancel_task", mobile);
    const auto criteria = action("change_task", benchmark, {{"doneWhen", column}, {"instruction", update}});
    const auto queue = [&](std::string_view text) { return action("send_to_task", benchmark, {{"instruction", text}}); };

    auto cloud = start("Run the existing test suite");
    cloud.args["environment"] = "cloud Mac";
    test("new request", {cloud, done});
    test("new request", {start("Run the existing test suite"), done}, {}, "did not use the cloud Mac");
    test("a reply", {action("answer_task_question", "Prepare the release label", {{"text", "Cancel this task"}}), done});
    test("a reply", {action("answer_task_question", "Prepare the release label", {{"text", "Status"}}), done}, {},
         "not saved as the answer");
    test("a named stop", {stop, done});
    test("a named stop", {action("cancel_task", "Write the release notes"), done}, {}, "expected applied actions");
    test("an ambiguous stop", {say(clarify), stopMobile, done}, {{CancellationTarget, clarify}});
    test("an ambiguous stop", {stopMobile, done}, {}, "expected applied actions");
    constexpr auto acknowledgement = "I will check.";
    test("an ambiguous stop", {say(acknowledgement)}, {{CancellationTarget, acknowledgement, "ambiguous"}}, "did not ask which task");

    test("new criteria", {criteria, done}, {{CompletionColumn, column}, {CriteriaInstruction, update}});
    auto changedIntent = criteria;
    changedIntent.args["intent"] = "Measure memory usage instead.";
    test("new criteria", {changedIntent, done}, {}, "retaining the original intent");
    auto movedBenchmark = criteria;
    movedBenchmark.args["environment"] = "cloud Mac";
    test("new criteria", {movedBenchmark, done}, {}, "altered pendingDestination");
    constexpr auto omitColumn = "Do not include startup_p95_ms in the final report.";
    auto conflicting = criteria;
    conflicting.args["instruction"] = omitColumn;
    test("new criteria", {conflicting, done}, {{CompletionColumn, column}, {CriteriaInstruction, omitColumn, "mismatch"}},
         "instruction conflicted");

    test("two requests", {start(audit), start(notes), done},
         {{SplitRequest, audit, "license_audit"}, {SplitRequest, notes, "release_notes"}});
    test("two requests", {start(notes), done}, {}, "expected 2 new tasks");
    test("two requests", {start(notes), start(notes), done},
         {{SplitRequest, notes, "release_notes"}, {SplitRequest, notes, "release_notes"}}, "one-to-one");
    test("explaining", {say(explain)}, {{CanaryExplanation, explain}});
    constexpr auto contradictory = "Use rollout_canary for 5% for a day, then expand to everyone. Never expand beyond 5%.";
    test("explaining", {say(contradictory)}, {{CanaryExplanation, contradictory, "mismatch"}}, "did not explain");
    test("a note for running work", {queue(staging), done}, {{StagingInstruction, staging}});

    constexpr auto stagingBenchmark = "Benchmark startup using the staging database";
    constexpr auto oldInstruction = "Use the staging database.";
    constexpr auto newInstruction = "Continue measuring as before.";
    const auto queuedNote = [&](std::string_view text) { return action("send_to_task", stagingBenchmark, {{"instruction", text}}); };
    run(
        "old intent and instruction cannot satisfy the new turn",
        [&](Scene& s) {
            const auto id = s.task(stagingBenchmark, "benchmark");
            checkStagingNote(s, s.send("Use the staging database"), id);
            checkStagingNote(s, s.send("Use staging for the next batch too"), id);
        },
        {queuedNote(oldInstruction), done, queuedNote(newInstruction), done},
        {{StagingInstruction, oldInstruction}, {StagingInstruction, newInstruction, "mismatch"}}, "did not direct");

    for (const auto* field : {"state", "currentRevision", "pendingInstructions"}) {
        run(
            "unreported change to an untouched task",
            [&](Scene& s) {
                const auto id = s.task(benchmark, "benchmark");
                auto turn = s.send("What's the status?");
                turn.after.at(id)[field] = "unexpected change";
                checkActions(s, turn, {});
            },
            {done}, {}, "untouched task changed");
    }
    std::cout << "Offline scoring self-test: " << passed << " cases passed\n";
}

void usage(std::ostream& out) {
    out << "Usage:\n"
        << "  flock_live_conversation --live [--trials N]\n"
        << "  flock_live_conversation <FLOCK_HOME> <model-slug> [trials] [scenario filter]\n"
        << "  flock_live_conversation --self-test\n\n"
        << "--live reads connection settings from the environment. --self-test checks scoring offline.\n"
        << chatGPTSetup << "\n"
        << "The default is one trial per scenario. The optional filter matches a substring of the scenario name.\n"
        << "Scenarios:\n";
    for (const auto& [name, scenario] : scenarios) out << "  " << name << '\n';
}
}

int main(int argc, char** argv) {
    try {
        const std::string_view operation = argc > 1 ? argv[1] : "";
        if (argc == 2 && operation == "--help") {
            usage(std::cout);
            return 0;
        }
        if (argc == 2 && operation == "--self-test") {
            selfTest();
            return 0;
        }
        const bool fromEnvironment = operation == "--live";
        if (fromEnvironment ? argc != 2 && (argc != 4 || std::string_view(argv[2]) != "--trials")
                            : argc < 3 || argc > 5 || operation.starts_with("--")) {
            usage(std::cerr);
            return 2;
        }
        int trials = 1;
        if (argc >= 4) {
            const std::string_view input(argv[3]);
            const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), trials);
            if (error != std::errc{} || end != input.data() + input.size() || trials < 1) {
                std::cerr << "Trials must be a positive integer\n";
                return 2;
            }
        }
        const std::string filter = argc == 5 ? argv[4] : "";
        auto selected = scenarios | std::views::filter([&](const auto& item) { return item.first.find(filter) != std::string::npos; });
        if (selected.empty()) {
            std::cerr << "No scenario matches '" << filter << "'\n";
            return 2;
        }
        const auto home = fromEnvironment ? environment("FLOCK_HOME") : argv[1];
        const auto slug = fromEnvironment ? environment("FLOCK_LIVE_MODEL") : argv[2];
        if (home.empty()) return skip("FLOCK_HOME is unset", chatGPTSetup);
        if (slug.empty()) return skip("FLOCK_LIVE_MODEL is unset", chatGPTSetup);
        const auto credentials = std::filesystem::path(home) / "credentials" / "chatgpt";
        if (!hasChatGPTProfile(credentials)) return skip("No ChatGPT sign-in in FLOCK_HOME", chatGPTSetup);
        app::ChatGPTModel live(credentials);
        const auto status = live.status();
        if (!status.at("connected").get<bool>()) return skip("No ChatGPT sign-in in FLOCK_HOME", chatGPTSetup);
        check(status.at("planUsage").get<bool>(),
              "ChatGPT plan usage is unavailable. Run flock model login --reconsent in the same FLOCK_HOME.");
        bool passed = true;
        for (const auto& [name, scenario] : selected) {
            int passes = 0;
            for (int trial = 0; trial < trials; ++trial) {
                std::string failure;
                Mixed model(live);
                try {
                    Scene scene(model, slug, liveJudge(live, slug));
                    scenario(scene);
                } catch (const std::exception& error) {
                    failure = error.what();
                }
                if (failure.empty()) ++passes;
                Json row = {{"scenario", name}, {"trial", trial + 1}, {"pass", failure.empty()}, {"failure", failure}};
                if (!failure.empty()) row["turn"] = model.trace;
                std::cout << row.dump() << '\n';
            }
            passed = passed && passes == trials;
            std::cout << Json{{"scenario", name}, {"passes", passes}, {"trials", trials}}.dump() << '\n';
        }
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
