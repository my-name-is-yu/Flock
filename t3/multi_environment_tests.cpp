#include "flock/conversation_intake.hpp"
#include "flock/task_control.hpp"
#include "flock/task_intake.hpp"
#include "flock/task_list.hpp"
#include "internal.hpp"
#include "flock/tasks.hpp"
#include "flock/board_inbox.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_connection.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace flockv0;
using flockv0::t3detail::http;

namespace {
constexpr auto bot = "release";
constexpr auto request = "Verify the release fixture";
constexpr auto holdLocal = "Hold the local project";
constexpr auto threadId = "shared-thread";
constexpr auto runId = "shared-run";

void check(bool pass, std::string_view detail) {
    if (!pass) throw std::runtime_error(std::string(detail));
}

struct Endpoint {
    std::string name, origin;

    std::string id() const { return "env-" + name; }
    std::string project() const { return "project-" + name; }
    std::string link() const { return origin + "/" + id() + "/" + threadId; }

    void configure(const Json& settings) const {
        check(http(origin + "/fixture", "POST", {{"Content-Type", "application/json"}}, settings.dump()).status == 200,
              "could not configure the " + name + " fixture");
    }

    Json effects(const char* tool) const {
        const auto response = http(origin + "/fixture/effects", "GET");
        check(response.status == 200, "could not read the " + name + " effects");
        return Json::parse(response.body).value(tool, Json::array());
    }

    std::vector<std::string> routedTargets() const {
        const auto response = http(origin + "/fixture/routes", "GET");
        check(response.status == 200, "could not read routed target IDs");
        return Json::parse(response.body).get<std::vector<std::string>>();
    }

    void login(const std::filesystem::path& directory) const {
        const auto signedIn =
            loginT3(directory, id(), origin + "/mcp", [&](const auto& authorization) { configure({{"authorization", authorization}}); });
        check(signedIn.environmentId == id() && signedIn.operate, "did not connect " + name);
    }
};

struct Script final : Model {
    explicit Script(std::string sha) : commit(std::move(sha)) {}

    std::string commit;
    Json startResult;
    Json sendResult;
    Json wrongGatewaySendResult;
    Json ambiguousResult;
    Json conflictingResult;
    Json cancelResult;
    Json repeatCancelResult;
    Id followUpTask = 0;
    Id followUpGeneration = 0;
    Id cancelTask = 0;
    Id cancelGeneration = 0;
    bool parallel = true;
    bool linkedOnly = false;
    bool nativeLinked = false;
    bool nativeGatewayOptionsSeen = false;
    std::string selectedEnvironment;
    std::string nativeTargetEnvironment;
    std::string nativeProject;
    std::string nativeGateway;
    std::string nativeChoice;
    Json nativeEnvironmentOptions = Json::array();
    Json nativeEnvironmentIssues = Json::array();

    ModelTurn decide(const ModelInput& input) override {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") {
            const auto& text = input.event.value("text", "");
            if (input.event.at("turn").empty() &&
                (text == "Send instruction to Mini" || text == "Send linked instruction" ||
                 text == "Send linked instruction through other gateway" || text == "Send instruction without destination" ||
                 text == "Send instruction with conflicting selectors")) {
                Json args{{"taskId", followUpTask}, {"generation", followUpGeneration}, {"instruction", "Check the Mini result"}};
                if (text == "Send instruction to Mini") args["destination"] = "mini";
                if (text == "Send linked instruction" || text == "Send linked instruction through other gateway") {
                    args["environmentId"] = nativeTargetEnvironment;
                    args["projectId"] = nativeProject;
                    args["gatewayEnvironmentId"] = text == "Send linked instruction" ? nativeGateway : nativeTargetEnvironment;
                }
                if (text == "Send instruction with conflicting selectors") {
                    args["destination"] = "mini";
                    args["environmentId"] = "env-local";
                    args["projectId"] = "project-local";
                }
                return {{{"send_to_task", std::move(args), "Send to the selected destination"}}};
            }
            if (input.event.at("turn").empty() && text == "Stop linked target")
                return {{{"cancel_task",
                          {{"taskId", cancelTask},
                           {"generation", cancelGeneration},
                           {"environmentId", nativeTargetEnvironment},
                           {"projectId", nativeProject},
                           {"gatewayEnvironmentId", nativeGateway}},
                          "Stop only the selected linked target"}}};
            if (input.event.at("turn").empty() && (text == "Stop Mini" || text == "Stop Mini again"))
                return {{{"cancel_task",
                          {{"taskId", cancelTask}, {"generation", cancelGeneration}, {"destination", "mini"}},
                          "Stop only Mini"}}};
            const auto& turn = input.event.at("turn");
            if (turn.empty() && input.event.value("text", "") == request) {
                Json args{{"request", request}};
                if (!selectedEnvironment.empty()) args["environment"] = selectedEnvironment;
                if (nativeLinked) {
                    nativeEnvironmentOptions = input.task.value("environments", Json::array());
                    nativeEnvironmentIssues = input.task.value("linkedEnvironmentIssues", Json::array());
                    std::string localGatewayName;
                    bool localGateway = false;
                    bool secondGateway = false;
                    for (const auto& choice : input.task.at("environments")) {
                        if (choice.value("linked", false) && choice.value("environmentId", "") == nativeTargetEnvironment) {
                            const auto gateway = choice.value("gatewayEnvironmentId", "");
                            if (gateway == nativeGateway) {
                                localGatewayName = choice.at("name");
                                localGateway = true;
                            }
                            if (gateway == nativeTargetEnvironment) secondGateway = true;
                        }
                    }
                    nativeGatewayOptionsSeen = localGateway && secondGateway && !localGatewayName.empty();
                    nativeChoice = localGatewayName;
                }
                if (parallel) {
                    args["environments"] = nativeLinked ? Json::array({nativeChoice})
                                           : linkedOnly ? Json::array({"mini"})
                                                        : Json::array({"local", "mini"});
                    if (!commit.empty()) args["commit"] = commit;
                }
                return {{{"start_task", std::move(args), "Run the release verification"}}};
            }
            if (!turn.empty()) {
                if (text == "Send instruction to Mini" || text == "Send linked instruction")
                    sendResult = turn.back().at("result");
                else if (text == "Send linked instruction through other gateway")
                    wrongGatewaySendResult = turn.back().at("result");
                else if (text == "Send instruction without destination")
                    ambiguousResult = turn.back().at("result");
                else if (text == "Send instruction with conflicting selectors")
                    conflictingResult = turn.back().at("result");
                else if (text == "Stop Mini" || text == "Stop linked target")
                    cancelResult = turn.back().at("result");
                else if (text == "Stop Mini again")
                    repeatCancelResult = turn.back().at("result");
                else
                    startResult = turn.back().at("result");
            }
            return {{{"reply", {{"text", "I have the fixture request."}}, "End this turn"}}};
        }
        if (kind == "intake" && input.task.at("request") == holdLocal)
            return {{{"ask_user", {{"question", "Can the project be released?"}}, "Hold the local slot"}}};
        if (kind == "intake")
            return {{{"delegate", {{"instruction", request}, {"doneWhen", "The checks have a result"}}, "Run the checks"}}};
        if (kind == "result") return {{{"complete", {{"report", input.event.at("report")}}, "Return the checks"}}};
        throw std::logic_error("unexpected model event: " + input.event.dump());
    }
};

struct Scenario {
    const Endpoint& local;
    const Endpoint& mini;
    Script model;
    Store store;
    std::unique_ptr<T3> adapter;
    std::atomic<Seconds> now{unixTime()};
    Runtime runtime;
    Id taskId = 0;

    Scenario(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory, const std::string& commit,
             std::string_view name)
        : local(local), mini(mini), model(commit), store(directory.parent_path() / (std::string(name) + ".db")),
          adapter(makeT3Mcp(directory)), runtime(store, model, *adapter, {}, RuntimeOptions{[&] { return now.load(); }}) {}

    void start(bool parallel = true, bool loseMiniReply = false) {
        model.parallel = parallel;
        model.selectedEnvironment.clear();
        local.configure({{"multiReset", true}, {"multiSingle", !parallel}, {"multiCommit", model.commit}});
        mini.configure({{"multiReset", true}, {"multiHoldLaunchReply", loseMiniReply}, {"multiCommit", model.commit}});
        const Intake routing{bot,
                             {},
                             local.id(),
                             local.project(),
                             {{"conversationEnvironments",
                               {{"local", {{"environmentId", local.id()}, {"projectId", local.project()}}},
                                {"mini", {{"environmentId", mini.id()}, {"projectId", mini.project()}}}}}}};
        ConversationIntake(store).receive(localOwner, routing, "start-fixture", request);
        runtime.runConversations();
        check(model.startResult.is_object() && model.startResult.value("ok", false),
              "conversation could not start the verification: " + model.startResult.dump());
        taskId = model.startResult.at("taskId");
    }

    void startLinkedMini(bool forwardingUnavailable = false) {
        model.parallel = true;
        model.linkedOnly = true;
        model.selectedEnvironment.clear();
        T3Connections(store).connect(local.id(), localOwner.id, 3210);
        local.configure({{"multiReset", true},
                         {"multiSingle", true},
                         {"multiCommit", model.commit},
                         {"multiExpectedTarget", mini.id()},
                         {"multiForwardingUnavailable", forwardingUnavailable}});
        mini.configure({{"multiReset", true}, {"multiHoldLaunchReply", false}, {"multiCommit", model.commit}});
        const Intake routing{
            bot,
            {},
            local.id(),
            local.project(),
            {{"conversationEnvironments",
              {{"local", {{"environmentId", local.id()}, {"projectId", local.project()}}},
               {"mini", {{"environmentId", mini.id()}, {"projectId", local.project()}, {"gatewayEnvironmentId", local.id()}}}}}}};
        ConversationIntake(store).receive(localOwner, routing, "linked-fixture", request);
        runtime.runConversations();
        check(model.startResult.is_object() && model.startResult.value("ok", false),
              "conversation could not start linked-environment work: " + model.startResult.dump());
        taskId = model.startResult.at("taskId");
    }

    void startNativeLinked(bool dropLaunchReply = false) {
        model.parallel = true;
        model.nativeLinked = true;
        model.nativeTargetEnvironment = mini.id();
        model.nativeProject = mini.project();
        model.nativeGateway = local.id();
        T3Connections connections(store);
        connections.connect(local.id(), localOwner.id, 3210);
        connections.connect(mini.id(), localOwner.id, 3210);
        local.configure(
            {{"multiReset", true},
             {"multiSingle", true},
             {"multiCommit", model.commit},
             {"multiExpectedTarget", mini.id()},
             {"multiProject", mini.project()},
             {"multiDropLaunchReply", dropLaunchReply},
             {"multiLinkedEnvironments", Json::array({{{"environmentId", mini.id()}, {"label", "Mini"}, {"status", "reachable"}}})}});
        mini.configure(
            {{"multiReset", true},
             {"multiSingle", true},
             {"multiCommit", model.commit},
             {"multiLinkedEnvironments", Json::array({{{"environmentId", mini.id()}, {"label", "Mini"}, {"status", "reachable"}}})}});
        const Intake routing{bot, {}, local.id(), local.project(), Json::object()};
        ConversationIntake(store).receive(localOwner, routing, "native-linked-fixture", request);
        runtime.runConversations();
        check(model.startResult.is_object() && model.startResult.value("ok", false) && model.nativeGatewayOptionsSeen,
              "the model must receive distinct gateway choices for the same linked target: " + model.nativeEnvironmentOptions.dump() +
                  " issues=" + model.nativeEnvironmentIssues.dump() + " result=" + model.startResult.dump());
        taskId = model.startResult.at("taskId");
    }

    TaskView task() { return Tasks(store).task(taskId); }

    static Json result(const TaskView& task, const Endpoint& endpoint) {
        for (const auto& run : taskJson(task).value("runResults", Json::array()))
            if (run.value("environmentId", "") == endpoint.id()) return run;
        return Json::object();
    }

    template <class Predicate> void advanceUntil(Predicate done, std::string_view detail) {
        using namespace std::chrono_literals;
        const auto end = std::chrono::steady_clock::now() + 8s;
        for (int tick = 0; std::chrono::steady_clock::now() < end; ++tick) {
            const auto snapshot = task();
            if (done(snapshot)) return;
            if (tick % 10 == 0) ++now;
            runtime.tickConcurrent();
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error(std::string(detail) + ": " + taskJson(task()).dump());
    }

    void awaitBothRunning() {
        advanceUntil(
            [&](const TaskView& snapshot) {
                return result(snapshot, local).value("status", "") == "running" && result(snapshot, mini).value("status", "") == "running";
            },
            "both environments must start before either Run settles");
        checkLaunches();
    }

    void checkLaunches() {
        const auto localLaunches = local.effects("t3_thread_launch");
        const auto miniLaunches = mini.effects("t3_thread_launch");
        check(localLaunches.size() == 1 && miniLaunches.size() == 1 &&
                  localLaunches.at(0).at("clientRequestId") != miniLaunches.at(0).at("clientRequestId") &&
                  local.effects("t3_thread_send").empty() && mini.effects("t3_thread_send").empty(),
              "each environment must accept exactly one distinct action");
    }

    void checkResult(const Endpoint& endpoint, const Json& outcome, std::string_view status, std::string_view commit) {
        check(outcome.at("name") == endpoint.name && outcome.at("projectId") == endpoint.project() && outcome.at("status") == status &&
                  outcome.at("commit") == commit && outcome.at("threadId") == threadId && outcome.at("runId") == runId &&
                  outcome.at("link") == endpoint.link(),
              "incorrect per-environment Run outcome: " + outcome.dump());
        if (status == "completed") check(outcome.at("report") == endpoint.id() + " verification completed", "a success report was lost");
    }

    void cancel() { TaskControl(store).operate(localOwner, TaskTarget::from(task().task, 0), "cancel"); }
};

void normal(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.mini.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; }, "both Runs should complete");
    const auto task = fixture.task();
    check(task.task.runResults().size() == 2 && task.task.runOutcome() == RunOutcome::all_completed &&
              TaskList(fixture.store).tasks(bot).size() == 1,
          "the original request must contain both outcomes");
    fixture.checkResult(fixture.local, Scenario::result(task, fixture.local), "completed", fixture.model.commit);
    fixture.checkResult(fixture.mini, Scenario::result(task, fixture.mini), "completed", fixture.model.commit);
}

void ordinaryMulti(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "ordinary-multi");
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.mini.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "ordinary multi-environment work should complete without a requested commit");
    const auto task = fixture.task();
    check(!task.task.requestedCommit && task.task.runOutcome() == RunOutcome::all_completed && task.task.runResults().size() == 2,
          "ordinary work must aggregate both destinations without SHA policy");
    fixture.checkResult(fixture.local, Scenario::result(task, fixture.local), "completed", "");
    fixture.checkResult(fixture.mini, Scenario::result(task, fixture.mini), "completed", "");
}

void linkedTargetUsesGateway(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "linked-target");
    fixture.startLinkedMini();
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, fixture.mini).value("status", "") == "running"; },
                         "the linked target should launch through its gateway");
    const auto expectedTargetLink = local.origin + "/" + fixture.mini.id() + "/" + threadId;
    check(Scenario::result(fixture.task(), fixture.mini).value("link", "") == expectedTargetLink,
          "a linked launch URL must use the target environment, not its credential gateway");
    fixture.model.followUpTask = fixture.taskId;
    fixture.model.followUpGeneration = fixture.task().task.generation;
    const Intake routing{
        bot,
        {},
        local.id(),
        local.project(),
        {{"conversationEnvironments",
          {{"local", {{"environmentId", local.id()}, {"projectId", local.project()}}},
           {"mini", {{"environmentId", mini.id()}, {"projectId", local.project()}, {"gatewayEnvironmentId", local.id()}}}}}}};
    ConversationIntake(fixture.store).receive(localOwner, routing, "linked-send-fixture", "Send instruction to Mini");
    fixture.runtime.runConversations();
    check(fixture.model.sendResult.value("ok", false) && local.effects("t3_thread_send").size() == 1 &&
              mini.effects("t3_thread_send").empty() &&
              Scenario::result(fixture.task(), fixture.mini).value("link", "") == expectedTargetLink,
          "a saved linked-target follow-up must use the gateway credential and preserve the target Session URL");
    fixture.model.cancelTask = fixture.taskId;
    fixture.model.cancelGeneration = fixture.task().task.generation;
    ConversationIntake(fixture.store).receive(localOwner, routing, "linked-stop-fixture", "Stop Mini");
    fixture.runtime.runConversations();
    fixture.advanceUntil([&](const TaskView&) { return local.effects("t3_thread_interrupt").size() == 1; },
                         "the linked target stop should dispatch through the gateway");
    check(fixture.model.cancelResult.value("ok", false) && local.effects("t3_thread_interrupt").size() == 1 &&
              mini.effects("t3_thread_interrupt").empty(),
          "a saved linked-target stop must interrupt through the gateway without relaunching: " + fixture.model.cancelResult.dump());
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "the linked target stop should settle");
    const auto task = fixture.task();
    const auto result = Scenario::result(task, fixture.mini);
    const auto routed = local.routedTargets();
    check(result.value("gatewayEnvironmentId", "") == local.id() && result.value("environmentId", "") == mini.id() &&
              result.at("status") == "interrupted" && local.effects("t3_thread_launch").size() == 1 &&
              mini.effects("t3_thread_launch").empty() && local.effects("t3_thread_send").size() == 1 &&
              mini.effects("t3_thread_send").empty() && local.effects("t3_thread_interrupt").size() == 1 &&
              mini.effects("t3_thread_interrupt").empty() && !routed.empty() &&
              std::all_of(routed.begin(), routed.end(), [&](const auto& target) { return target == mini.id(); }),
          "linked launch, follow-up, and stop must use gateway credentials while preserving the target ID");
}

void resumeNativeLinkedRoute(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario resumed(local, mini, directory, {}, "native-linked-restart");
    const auto tasks = TaskList(resumed.store).tasks(bot);
    check(tasks.size() == 1, "the native linked task must survive process restart");
    resumed.taskId = tasks.front().task.id;
    resumed.model.nativeTargetEnvironment = mini.id();
    resumed.model.nativeProject = mini.project();
    resumed.model.nativeGateway = local.id();
    resumed.model.followUpTask = resumed.taskId;
    resumed.model.followUpGeneration = resumed.task().task.generation;
    const Intake changedContext{bot, {}, mini.id(), mini.project(), Json::object()};
    ConversationIntake(resumed.store)
        .receive(localOwner, changedContext, "native-linked-wrong-gateway", "Send linked instruction through other gateway");
    resumed.runtime.runConversations();
    check(!resumed.model.wrongGatewaySendResult.value("ok", true) && local.effects("t3_thread_send").empty() &&
              mini.effects("t3_thread_send").empty(),
          "a saved target selector with a different gateway must fail without switching credentials");
    ConversationIntake(resumed.store).receive(localOwner, changedContext, "native-linked-restart-followup", "Send linked instruction");
    resumed.runtime.runConversations();
    check(resumed.model.sendResult.value("ok", false) && local.effects("t3_thread_send").size() == 1 &&
              mini.effects("t3_thread_send").empty(),
          "after restart and a changed client context, follow-up must use the saved gateway and target");
    resumed.model.cancelTask = resumed.taskId;
    resumed.model.cancelGeneration = resumed.task().task.generation;
    ConversationIntake(resumed.store).receive(localOwner, changedContext, "native-linked-restart-stop", "Stop linked target");
    resumed.runtime.runConversations();
    resumed.advanceUntil([&](const TaskView&) { return local.effects("t3_thread_interrupt").size() == 1; },
                         "the saved linked Run should be interrupted through its original gateway");
    resumed.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "the linked stop should settle after process restart");
    const auto task = resumed.task();
    const auto result = Scenario::result(task, mini);
    const auto expectedTargetLink = local.origin + "/" + mini.id() + "/" + result.value("threadId", "");
    const auto routed = local.routedTargets();
    check(resumed.model.cancelResult.value("ok", false) && task.task.state == TaskState::completed &&
              task.task.runOutcome() == RunOutcome::failed && result.value("status", "") == "interrupted" &&
              result.value("gatewayEnvironmentId", "") == local.id() && result.value("link", "") == expectedTargetLink &&
              local.effects("t3_thread_launch").size() == 1 && mini.effects("t3_thread_launch").empty() &&
              local.effects("t3_thread_send").size() == 1 && mini.effects("t3_thread_send").empty() &&
              local.effects("t3_thread_interrupt").size() == 1 && mini.effects("t3_thread_interrupt").empty() && !routed.empty() &&
              std::all_of(routed.begin(), routed.end(), [&](const auto& target) { return target == mini.id(); }),
          "restart recovery must retain the exact gateway and target without credential overwrite or rerouting");
}

void nativeLinkedUnknownAcceptance(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "native-linked-unknown-acceptance");
    fixture.startNativeLinked(true);
    fixture.advanceUntil(
        [&](const TaskView& task) {
            const auto result = Scenario::result(task, fixture.mini);
            return result.value("status", "") == "confirmation_pending" && result.value("gatewayEnvironmentId", "") == fixture.local.id();
        },
        "a linked launch with a lost reply should remain uncertain on its saved gateway");
    fixture.model.cancelTask = fixture.taskId;
    fixture.model.cancelGeneration = fixture.task().task.generation;
    const Intake changedContext{bot, {}, mini.id(), mini.project(), Json::object()};
    ConversationIntake(fixture.store).receive(localOwner, changedContext, "native-linked-unknown-stop", "Stop linked target");
    fixture.runtime.runConversations();
    check(fixture.model.cancelResult.value("ok", false), "an uncertain linked destination must accept a local stop intent");
    const auto authGeneration = fixture.adapter->authGeneration(local.id(), Json::object());
    local.configure({{"accessToken", "fixture.rotated-signed-token"}});
    local.login(directory);
    check(fixture.adapter->authGeneration(local.id(), Json::object()) != authGeneration,
          "the fixture must change gateway auth generation before reconciling the linked launch");
    for (int tick = 0; tick < 70; ++tick) {
        ++fixture.now;
        fixture.runtime.tickConcurrent();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto task = fixture.task();
    const auto result = Scenario::result(task, mini);
    const auto expectedTargetLink = local.origin + "/" + mini.id() + "/" + result.value("threadId", "");
    const auto routed = local.routedTargets();
    check(task.task.state == TaskState::completed && result.value("status", "") == "interrupted" &&
              result.value("gatewayEnvironmentId", "") == local.id() && result.value("link", "") == expectedTargetLink &&
              local.effects("t3_thread_launch").size() == 1 && mini.effects("t3_thread_launch").empty() &&
              local.effects("t3_thread_interrupt").size() == 1 && mini.effects("t3_thread_interrupt").empty() && !routed.empty() &&
              std::all_of(routed.begin(), routed.end(), [&](const auto& target) { return target == mini.id(); }),
          "linked unknown acceptance must reconcile read-only after gateway auth changes and never relaunch on another gateway");
}

void linkedForwardingUnavailable(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "linked-forwarding-unavailable");
    fixture.startLinkedMini(true);
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, mini).value("status", "") == "failed"; },
                         "a disabled linked-environment gate should fail the target action");
    const auto routed = local.routedTargets();
    check(taskJson(fixture.task()).dump().find("This T3 connection cannot forward to linked environments") != std::string::npos &&
              local.effects("t3_thread_launch").size() == 1 && mini.effects("t3_thread_launch").empty() && !routed.empty() &&
              std::all_of(routed.begin(), routed.end(), [&](const auto& target) { return target == mini.id(); }),
          "a forwarding gate must be actionable and must not retry or route to another target: " + taskJson(fixture.task()).dump() +
              " routed=" + Json(routed).dump());
}

void sendToOneDestination(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "send-one-destination");
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, fixture.local).value("status", "") == "completed"; },
                         "the local destination must complete before messaging Mini");
    const auto original = Scenario::result(fixture.task(), fixture.local);
    fixture.model.followUpTask = fixture.taskId;
    fixture.model.followUpGeneration = fixture.task().task.generation;
    const Intake routing{bot,
                         {},
                         local.id(),
                         local.project(),
                         {{"conversationEnvironments",
                           {{"local", {{"environmentId", local.id()}, {"projectId", local.project()}}},
                            {"mini", {{"environmentId", mini.id()}, {"projectId", mini.project()}}}}}}};
    ConversationIntake(fixture.store)
        .receive(localOwner, routing, "conflicting-follow-up-fixture", "Send instruction with conflicting selectors");
    fixture.runtime.runConversations();
    check(!fixture.model.conflictingResult.value("ok", true) &&
              fixture.model.conflictingResult.value("error", "").find("either by friendly name") != std::string::npos &&
              local.effects("t3_thread_send").empty() && mini.effects("t3_thread_send").empty(),
          "contradictory selector forms must be rejected without contacting either destination");
    ConversationIntake(fixture.store).receive(localOwner, routing, "ambiguous-follow-up-fixture", "Send instruction without destination");
    fixture.runtime.runConversations();
    check(!fixture.model.ambiguousResult.value("ok", true) &&
              fixture.model.ambiguousResult.value("error", "").find("Choose a destination") != std::string::npos &&
              local.effects("t3_thread_send").empty() && mini.effects("t3_thread_send").empty(),
          "an omitted selector must be rejected as ambiguous without contacting either destination");
    const auto miniTarget = Resource::sessionRead(mini.id(), threadId).target;
    Policy{fixture.store}.changeNeverRule(localOwner, Action::t3_send, miniTarget, true);
    ConversationIntake(fixture.store).receive(localOwner, routing, "policy-denied-follow-up-fixture", "Send instruction to Mini");
    fixture.runtime.runConversations();
    check(!fixture.model.sendResult.value("ok", true) && mini.effects("t3_thread_send").empty(),
          "saved-target follow-up must still obey the T3 send policy");
    Policy{fixture.store}.changeNeverRule(localOwner, Action::t3_send, miniTarget, false);
    ConversationIntake(fixture.store).receive(localOwner, routing, "follow-up-fixture", "Send instruction to Mini");
    fixture.runtime.runConversations();
    check(fixture.model.sendResult.value("ok", false),
          "send_to_task did not deliver the selected destination message: " + fixture.model.sendResult.dump());
    const auto task = fixture.task();
    const auto miniMessages = mini.effects("t3_thread_send");
    check(local.effects("t3_thread_send").empty() && miniMessages.size() == 1 &&
              miniMessages.front().at("message").get<std::string>().find("Check the Mini result") != std::string::npos &&
              local.effects("t3_thread_launch").size() == 1 && mini.effects("t3_thread_launch").size() == 1 &&
              TaskList(fixture.store).tasks(bot).size() == 1 && task.task.id == fixture.taskId &&
              Scenario::result(task, fixture.local) == original,
          "the instruction must go only to Mini without relaunching or changing Local's successful result");
}

void cancelOneDestination(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "cancel-one-destination");
    fixture.start();
    fixture.awaitBothRunning();
    fixture.mini.configure({{"multiDisconnected", true}, {"multiHoldSettlement", true}});
    fixture.model.cancelTask = fixture.taskId;
    const Intake routing{bot,
                         {},
                         local.id(),
                         local.project(),
                         {{"conversationEnvironments",
                           {{"local", {{"environmentId", local.id()}, {"projectId", local.project()}}},
                            {"mini", {{"environmentId", mini.id()}, {"projectId", mini.project()}}}}}}};
    const auto miniTarget = Resource::sessionRead(mini.id(), threadId).target;
    Policy{fixture.store}.changeNeverRule(localOwner, Action::t3_send, miniTarget, true);
    fixture.model.cancelGeneration = fixture.task().task.generation;
    ConversationIntake(fixture.store).receive(localOwner, routing, "cancel-mini-denied-fixture", "Stop Mini");
    fixture.runtime.runConversations();
    check(!fixture.model.cancelResult.value("ok", true) && fixture.mini.effects("t3_thread_interrupt").empty(),
          "selected cancellation must obey the saved Run's T3 policy");
    Policy{fixture.store}.changeNeverRule(localOwner, Action::t3_send, miniTarget, false);
    fixture.model.cancelGeneration = fixture.task().task.generation;
    ConversationIntake(fixture.store).receive(localOwner, routing, "cancel-mini-fixture", "Stop Mini");
    fixture.runtime.runConversations();
    check(fixture.model.cancelResult.value("ok", false) && fixture.model.cancelResult.value("effect", "") == "stop_requested",
          "selected cancellation should persist one destination stop intent");
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil(
        [&](const TaskView& task) {
            return Scenario::result(task, fixture.local).value("status", "") == "completed" &&
                   Scenario::result(task, fixture.mini).value("status", "") == "confirmation_pending";
        },
        "Mini should retain stop uncertainty while Local continues to completion");
    check(fixture.task().task.state == TaskState::running &&
              Scenario::result(fixture.task(), fixture.mini).value("status", "") == "confirmation_pending" &&
              fixture.local.effects("t3_thread_interrupt").empty() && fixture.mini.effects("t3_thread_interrupt").empty() &&
              TaskList(fixture.store).tasks(bot).size() == 1,
          "selected cancellation must preserve the sibling and original request while settlement is pending");
    fixture.model.cancelGeneration = fixture.task().task.generation;
    ConversationIntake(fixture.store).receive(localOwner, routing, "cancel-mini-repeat-fixture", "Stop Mini again");
    fixture.runtime.runConversations();
    check(fixture.model.repeatCancelResult.value("ok", false) &&
              fixture.model.repeatCancelResult.value("effect", "") == "already_requested" &&
              fixture.mini.effects("t3_thread_interrupt").empty(),
          "repeating selected cancellation must be idempotent");
    fixture.mini.configure({{"multiDisconnected", false}, {"multiHoldSettlement", true}});
    fixture.advanceUntil([&](const TaskView&) { return fixture.mini.effects("t3_thread_interrupt").size() == 1; },
                         "Mini should be interrupted after reconnecting");
    const auto stopping = fixture.task();
    check(stopping.task.state == TaskState::running && !stopping.task.runOutcome(),
          "the selected destination must keep the task open after dispatching the stop and before settlement");
    fixture.mini.configure({{"multiHoldSettlement", false}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "the parent should settle after Mini's stop is confirmed");
    const auto task = fixture.task();
    check(task.task.request == request && Scenario::result(task, fixture.local).at("status") == "completed" &&
              Scenario::result(task, fixture.mini).at("status") == "interrupted" && TaskList(fixture.store).tasks(bot).size() == 1,
          "one stopped destination must not recreate the request or erase its successful sibling");
    const auto expected = Json::array({{{"threadId", threadId}, {"runId", runId}}});
    check(fixture.local.effects("t3_thread_interrupt").empty() && fixture.mini.effects("t3_thread_interrupt") == expected,
          "selected cancellation must interrupt only Mini's exact Run");
}

void cancelUnknownAcceptance(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory) {
    Scenario fixture(local, mini, directory, {}, "cancel-unknown-acceptance");
    fixture.start(true, true);
    fixture.advanceUntil(
        [&](const TaskView& task) {
            return Scenario::result(task, fixture.local).value("status", "") == "running" &&
                   Scenario::result(task, fixture.mini).value("status", "") == "confirmation_pending";
        },
        "the held Mini launch reply should remain uncertain");
    fixture.model.cancelTask = fixture.taskId;
    fixture.model.cancelGeneration = fixture.task().task.generation;
    const Intake routing{bot,
                         {},
                         local.id(),
                         local.project(),
                         {{"conversationEnvironments",
                           {{"local", {{"environmentId", local.id()}, {"projectId", local.project()}}},
                            {"mini", {{"environmentId", mini.id()}, {"projectId", mini.project()}}}}}}};
    ConversationIntake(fixture.store).receive(localOwner, routing, "cancel-unknown-mini-fixture", "Stop Mini");
    fixture.runtime.runConversations();
    check(fixture.model.cancelResult.value("ok", false), "an uncertain saved destination must accept a local stop intent");
    const auto authGeneration = fixture.adapter->authGeneration(mini.id(), Json::object());
    mini.configure({{"accessToken", "fixture.rotated-signed-token"}});
    mini.login(directory);
    check(fixture.adapter->authGeneration(mini.id(), Json::object()) != authGeneration,
          "the fixture must change auth generation before reconciling the uncertain launch");
    fixture.local.configure({{"multiStatus", "completed"}});
    for (int tick = 0; tick < 70; ++tick) {
        ++fixture.now;
        fixture.runtime.tickConcurrent();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto task = fixture.task();
    check(Scenario::result(task, fixture.local).at("status") == "completed" &&
              Scenario::result(task, fixture.mini).at("status") == "interrupted" && task.task.state == TaskState::completed &&
              mini.effects("t3_thread_launch").size() == 1 && mini.effects("t3_thread_interrupt").size() == 1 &&
              TaskList(fixture.store).tasks(bot).size() == 1,
          "unknown acceptance must reconcile read-only after auth changes, stop the known Run, and never relaunch");
}

void independentSlots(Scenario& fixture) {
    const auto blocker = TaskIntake(fixture.store).submit({"owner", holdLocal, fixture.local.id(), fixture.local.project()});
    fixture.start();
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, fixture.mini).value("status", "") == "running"; },
                         "the free route must start while the other project is blocked");
    check(Tasks(fixture.store).task(blocker).task.state == TaskState::waiting && fixture.local.effects("t3_thread_launch").empty() &&
              Scenario::result(fixture.task(), fixture.local).at("status") == "queued",
          "the blocked Run was dispatched before its project became free");
    fixture.mini.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, fixture.mini).value("status", "") == "completed"; },
                         "the free route must settle independently of the blocked sibling");
    TaskControl(fixture.store).operate(localOwner, TaskTarget::from(Tasks(fixture.store).task(blocker).task, 0), "cancel");
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, fixture.local).value("status", "") == "running"; },
                         "the waiting route must launch once its project is free");
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "the batch must complete once both Runs settle");
    fixture.checkLaunches();
}

void deadlines(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    const auto started = fixture.task();
    const auto local = Scenario::result(started, fixture.local);
    const auto mini = Scenario::result(started, fixture.mini);
    fixture.now = std::max(local.at("deadline").get<Seconds>(), mini.at("deadline").get<Seconds>()) + 1;
    fixture.advanceUntil([](const TaskView& task) { return task.wait && task.wait->reason() == WaitReason::deadline; },
                         "an expired Run must notify the user");
    const auto waiting = fixture.task();
    const auto& wait = waiting.wait.value();
    const auto* verification = wait.verification();
    check(verification, "the deadline must identify the expired Run");
    const bool localExpiredFirst = verification->actionId == local.at("actionId").get<std::string>();
    const auto& overdue = localExpiredFirst ? fixture.local : fixture.mini;
    const auto& sibling = localExpiredFirst ? fixture.mini : fixture.local;
    check(wait.run && wait.run->environmentId == overdue.id() && wait.run->run.link == overdue.link(),
          "the deadline must point to its own Run and environment");
    sibling.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, sibling).value("status", "") == "completed"; },
                         "a deadline wait must not stop another Run from settling");
    const auto stillWaiting = fixture.task();
    const auto waitId = wait.id;
    check(stillWaiting.wait && stillWaiting.wait->id == waitId && stillWaiting.task.generation != waiting.task.generation,
          "the sibling did not advance the task while preserving the original deadline");
    const auto extension = fixture.now.load() + 120;
    TaskControl(fixture.store).operate(localOwner, TaskTarget::from(waiting.task, waitId), "extend", {{"deadline", extension}});
    const auto extended = fixture.task();
    check(!extended.wait && Scenario::result(extended, overdue).at("deadline") == extension &&
              Scenario::result(extended, sibling).at("deadline") == (localExpiredFirst ? mini : local).at("deadline"),
          "extending changed the wrong deadline");
    overdue.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "the extended Run must finish normally");
    fixture.checkLaunches();
}

void partial(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    const auto follower =
        TaskIntake(fixture.store).submit({bot, "Queued behind the local verification", fixture.local.id(), fixture.local.project()});
    check(Tasks(fixture.store).task(follower).blockedBy.value().id == fixture.taskId, "the active Run should hold its project slot");
    fixture.mini.configure({{"multiDisconnected", true}});
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil(
        [&](const TaskView& task) {
            return Scenario::result(task, fixture.local).value("status", "") == "completed" &&
                   Scenario::result(task, fixture.mini).value("status", "") == "confirmation_pending";
        },
        "disconnected Mini must not suppress the local report");
    auto task = fixture.task();
    const auto success = Scenario::result(task, fixture.local);
    check(task.task.state != TaskState::completed, "a disconnected Run is not terminal");
    fixture.checkResult(fixture.local, success, "completed", fixture.model.commit);
    const auto unblocked = Tasks(fixture.store).task(follower);
    check(!unblocked.blockedBy, "an unrelated task should proceed in the settled local project's queue");
    TaskControl(fixture.store).operate(localOwner, TaskTarget::from(unblocked.task, 0), "cancel");
    fixture.mini.configure({{"multiDisconnected", false}, {"multiStatus", "failed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; }, "both outcomes should settle");
    task = fixture.task();
    check(task.task.runOutcome() == RunOutcome::partial_failure && Scenario::result(task, fixture.local) == success,
          "a later Mini failure changed the local success or the aggregate outcome");
    fixture.checkResult(fixture.mini, Scenario::result(task, fixture.mini), "failed", fixture.model.commit);
    fixture.checkLaunches();
}

void mismatch(Scenario& fixture) {
    constexpr auto otherCommit = "fedcba9876543210fedcba9876543210fedcba98";
    fixture.start();
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.mini.configure({{"multiStatus", "completed"}, {"multiActualCommit", otherCommit}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; }, "both outcomes should settle");
    const auto task = fixture.task();
    check(task.task.runOutcome() == RunOutcome::partial_failure, "a different verified commit must fail the Mini check");
    fixture.checkResult(fixture.local, Scenario::result(task, fixture.local), "completed", fixture.model.commit);
    fixture.checkResult(fixture.mini, Scenario::result(task, fixture.mini), "failed", otherCommit);
}

void commitEvidence(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    const Json config{{"commitVerification", {{"projectId", fixture.local.project()}, {"expectedCommit", fixture.model.commit}}}};
    const RunRef run{threadId, runId, fixture.local.link()};
    for (const auto* mode : {"missing-post", "duplicate-pre", "unmarked", "reversed", "foreign-post", "failed-post", "truncated-post",
                             "post-then-work", "missing-report", "foreign-report", "empty-final-report"}) {
        fixture.local.configure({{"multiStatus", "completed"}, {"multiEvidence", mode}});
        const auto observation = fixture.adapter->observe(fixture.local.id(), config, run);
        check(observation.kind == Observation::Kind::Finished && observation.status == "completed" && observation.checkedCommit.empty() &&
                  observation.commitEvidenceUnavailable,
              std::string("incomplete or unattributed commit evidence was accepted: ") + mode);
    }
    const std::string wrongCommit(40, 'e');
    fixture.local.configure({{"multiEvidence", "rechecked-mismatch"}, {"multiActualCommit", wrongCommit}});
    check(fixture.adapter->observe(fixture.local.id(), config, run).checkedCommit == wrongCommit,
          "a later matching check must not overwrite an attributed mismatch");
    auto legacy = config;
    legacy["commitVerification"]["markedChecks"] = false;
    fixture.local.configure({{"multiEvidence", "unmarked"}, {"multiActualCommit", fixture.model.commit}});
    check(fixture.adapter->observe(fixture.local.id(), legacy, run).checkedCommit == fixture.model.commit,
          "a saved unmarked prompt still needs two distinct Git commands");
    fixture.local.configure({{"multiEvidence", "unmarked-missing-post"}});
    check(fixture.adapter->observe(fixture.local.id(), legacy, run).checkedCommit.empty(),
          "legacy recovery must require the final command");
    fixture.local.configure({{"multiEvidence", "truncated-report"}});
    const auto longReport = fixture.adapter->observe(fixture.local.id(), config, run);
    check(longReport.truncated && longReport.checkedCommit == fixture.model.commit && !longReport.commitEvidenceUnavailable,
          "truncating the written report must preserve separately verified commit evidence");
    fixture.local.configure({{"multiEvidence", "restarted"}});
    const auto observation = fixture.adapter->observe(fixture.local.id(), config, run);
    check(observation.kind == Observation::Kind::Finished && observation.status == "completed" &&
              observation.checkedCommit == fixture.model.commit && !observation.commitEvidenceUnavailable &&
              observation.resultRunId == "continued-run",
          "a pre-check and a post-check in the attributable restart must establish the commit");
}

void supervised(Scenario& fixture) {
    Policy{fixture.store}.setMode(localOwner, "supervised");
    fixture.start();
    auto card = [&](const Endpoint& endpoint) {
        for (const auto& item : Inbox(fixture.store).inbox(localOwner, bot))
            if (item.at("target") == endpoint.id() && item.at("status") == "pending") return item;
        return Json(nullptr);
    };
    auto approve = [&](const Json& item) {
        Inbox(fixture.store)
            .decideInbox(localOwner,
                         {item.at("botId"), InboxKind::policy_approval, item.at("itemId"), item.at("waitId"), item.at("taskId"),
                          item.at("targetRevision"), item.at("targetGeneration")},
                         "allow_once");
    };
    fixture.advanceUntil([&](const TaskView&) { return !card(fixture.local).is_null() && !card(fixture.mini).is_null(); },
                         "both dispatches should request separate approvals");
    check(fixture.local.effects("t3_thread_launch").empty() && fixture.mini.effects("t3_thread_launch").empty(),
          "a Run started before its own approval");
    const auto miniCard = card(fixture.mini);
    approve(card(fixture.local));
    fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, fixture.local).value("status", "") == "running"; },
                         "the approved local Run should start while the Mini still awaits approval");
    check(fixture.mini.effects("t3_thread_launch").empty() && fixture.task().task.generation > miniCard.at("targetGeneration"),
          "one approval launched both Runs or failed to advance the request");
    approve(miniCard);
    fixture.awaitBothRunning();
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.mini.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; }, "both approved Runs should settle");
    check(fixture.task().task.runOutcome() == RunOutcome::all_completed, "approving both Runs must preserve the verification results");
}

void stop(Scenario& fixture) {
    fixture.start();
    fixture.awaitBothRunning();
    fixture.mini.configure({{"multiDisconnected", true}, {"multiHoldSettlement", true}});
    fixture.local.configure({{"multiHoldSettlement", true}});
    fixture.cancel();
    fixture.advanceUntil(
        [&](const TaskView& task) {
            return fixture.local.effects("t3_thread_interrupt").size() == 1 &&
                   Scenario::result(task, fixture.mini).value("status", "") == "confirmation_pending";
        },
        "the connected Run should be interrupted while the disconnected Run awaits confirmation");
    check(fixture.task().task.state == TaskState::cancelling && fixture.mini.effects("t3_thread_interrupt").empty(),
          "stop released a disconnected Run or interrupted the wrong environment");
    fixture.local.configure({{"multiHoldSettlement", false}});
    fixture.mini.configure({{"multiDisconnected", false}});
    fixture.advanceUntil([&](const TaskView&) { return fixture.mini.effects("t3_thread_interrupt").size() == 1; },
                         "the Mini should be interrupted after reconnecting");
    check(fixture.task().task.state == TaskState::cancelling, "stop released work before the delegated settlement");
    fixture.mini.configure({{"multiHoldSettlement", false}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::cancelled; },
                         "the original request should cancel after both Runs settle");
    const auto task = fixture.task();
    check(Scenario::result(task, fixture.local).at("status") == "interrupted" &&
              Scenario::result(task, fixture.mini).at("status") == "interrupted",
          "the original request should retain each stop outcome");
    const auto interrupts = Json::array({{{"threadId", threadId}, {"runId", runId}}});
    check(fixture.local.effects("t3_thread_interrupt") == interrupts && fixture.mini.effects("t3_thread_interrupt") == interrupts,
          "stop must target exactly the assigned Run in each saved environment");
    fixture.checkLaunches();
}

void single(const Endpoint& local, const Endpoint& mini, const std::filesystem::path& directory, const std::string& commit,
            bool stopEarly) {
    const std::string name = stopEarly ? "single-stop" : "single";
    Id taskId;
    {
        Scenario fixture(local, mini, directory, commit, name);
        fixture.start(false);
        fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::running; }, "single Run did not start");
        taskId = fixture.taskId;
    }
    Scenario fixture(local, mini, directory, commit, name);
    fixture.taskId = taskId;
    if (stopEarly)
        fixture.cancel();
    else
        fixture.local.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil([&](const TaskView& task) { return task.task.state == (stopEarly ? TaskState::cancelled : TaskState::completed); },
                         "single Run did not settle");
    check(fixture.local.effects("t3_thread_launch").size() == 1 && fixture.mini.effects("t3_thread_launch").empty() &&
              fixture.local.effects("t3_thread_interrupt").size() == (stopEarly ? 1u : 0u),
          "existing start_task must use only the default environment");
}

void cancelledRunWaitsForDelegatedWork(Scenario& fixture) {
    fixture.start(false);
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::running; }, "single Run did not start");
    const auto next = TaskIntake(fixture.store).submit({bot, "Work after cancellation", fixture.local.id(), fixture.local.project()});
    fixture.local.configure({{"multiStatus", "cancelled"}, {"multiHoldSettlement", true}});
    fixture.cancel();
    fixture.advanceUntil(
        [](const TaskView& task) {
            return task.task.state == TaskState::cancelled ||
                   task.task.cancellationDetail == "Waiting for the remaining work on this request to finish";
        },
        "Flock did not observe the cancelled parent Run");
    check(fixture.task().task.state == TaskState::cancelling && Tasks(fixture.store).task(next).blockedBy.value().id == fixture.taskId,
          "a cancelled parent freed the project's slot before its delegated work settled");

    fixture.local.configure({{"multiHoldSettlement", false}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::cancelled; },
                         "the project should be released once delegated work settles");
    check(!Tasks(fixture.store).task(next).blockedBy, "settled cancellation kept the next request blocked");
}

void recover(Scenario& fixture) {
    const auto originals = TaskList(fixture.store).tasks(bot);
    check(originals.size() == 1, "a restart must keep the original request");
    fixture.taskId = originals.at(0).task.id;
    fixture.mini.configure({{"multiHideLaunch", true}});
    fixture.advanceUntil(
        [&](const TaskView& task) {
            const auto local = Scenario::result(task, fixture.local);
            const auto mini = Scenario::result(task, fixture.mini);
            return local.value("status", "") == "running" && local.contains("deadline") &&
                   mini.value("status", "") == "confirmation_pending" && mini.contains("deadline");
        },
        "recovered assigned and unconfirmed Runs both need deadlines");
    fixture.local.configure({{"multiStatus", "completed"}});
    fixture.advanceUntil(
        [&](const TaskView& task) {
            return Scenario::result(task, fixture.local).value("status", "") == "completed" &&
                   Scenario::result(task, fixture.mini).value("status", "") == "confirmation_pending";
        },
        "only the Mini should await confirmation after restart");
    check(Scenario::result(fixture.task(), fixture.mini).value("runId", "").empty(), "Flock attributed an unconfirmed launch");
    fixture.checkLaunches();
    const auto pending = Scenario::result(fixture.task(), fixture.mini);
    fixture.now = pending.at("deadline").get<Seconds>() + 1;
    fixture.advanceUntil(
        [&](const TaskView& task) {
            const auto* verification = task.wait ? task.wait->verification() : nullptr;
            return verification && verification->actionId == pending.at("actionId").get<std::string>();
        },
        "a lost launch receipt must surface its deadline during reconciliation");
    fixture.mini.configure({{"multiHideLaunch", false}, {"multiStatus", "completed"}});
    fixture.advanceUntil([](const TaskView& task) { return task.task.state == TaskState::completed; },
                         "restart must adopt the saved Mini Run");
    const auto task = fixture.task();
    fixture.checkResult(fixture.local, Scenario::result(task, fixture.local), "completed", fixture.model.commit);
    fixture.checkResult(fixture.mini, Scenario::result(task, fixture.mini), "completed", fixture.model.commit);
    fixture.checkLaunches();
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 6, "expected two fixture URLs, credential directory, commit and phase");
        const Endpoint local{"local", argv[1]}, mini{"mini", argv[2]};
        const std::filesystem::path directory = argv[3];
        const std::string commit = argv[4];
        const std::string_view phase = argv[5];
        if (phase == "linked-routes") {
            local.login(directory);
            mini.login(directory);
            linkedTargetUsesGateway(local, mini, directory);
            nativeLinkedUnknownAcceptance(local, mini, directory);
            return 0;
        }
        if (phase == "native-start") {
            Scenario fixture(local, mini, directory, commit, "native-linked-restart");
            fixture.startNativeLinked();
            fixture.advanceUntil([&](const TaskView& task) { return Scenario::result(task, mini).value("status", "") == "running"; },
                                 "the selected native linked target should start through its gateway");
            const auto selected = Scenario::result(fixture.task(), mini);
            check(selected.value("environmentId", "") == mini.id() && selected.value("gatewayEnvironmentId", "") == local.id() &&
                      selected.value("projectId", "") == mini.project(),
                  "the selected linked destination must persist its exact gateway, target, and project: " + selected.dump());
        } else if (phase == "native-resume") {
            resumeNativeLinkedRoute(local, mini, directory);
        } else if (phase == "native-unknown") {
            nativeLinkedUnknownAcceptance(local, mini, directory);
        } else if (phase == "normal") {
            local.login(directory);
            mini.login(directory);
            {
                Scenario fixture(local, mini, directory, commit, "normal");
                normal(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "independent");
                independentSlots(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "deadlines");
                deadlines(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "partial");
                partial(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "mismatch");
                mismatch(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "commit-evidence");
                commitEvidence(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "supervised");
                supervised(fixture);
            }
            {
                Scenario fixture(local, mini, directory, commit, "stop");
                stop(fixture);
            }
            ordinaryMulti(local, mini, directory);
            linkedTargetUsesGateway(local, mini, directory);
            linkedForwardingUnavailable(local, mini, directory);
            sendToOneDestination(local, mini, directory);
            cancelOneDestination(local, mini, directory);
            cancelUnknownAcceptance(local, mini, directory);
            single(local, mini, directory, commit, false);
            single(local, mini, directory, commit, true);
            {
                Scenario fixture(local, mini, directory, commit, "delegated-stop");
                cancelledRunWaitsForDelegatedWork(fixture);
            }
        } else {
            Scenario fixture(local, mini, directory, commit, "recovery");
            if (phase == "crash") {
                fixture.start(true, true);
                fixture.advanceUntil([](const TaskView&) { return false; }, "expected Flock to be killed before launch reply");
            } else {
                check(phase == "recover", "unknown fixture phase");
                recover(fixture);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
