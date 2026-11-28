#include "flock/bot_lifecycle.hpp"
#include "support.hpp"
#include "flock/conversation_intake.hpp"
#include "flock/task_control.hpp"
#include "flock/task_history.hpp"
#include "flock/task_intake.hpp"
#include "flock/task_list.hpp"
#include "flock/notifications.hpp"
#include "flock/secret_ref.hpp"
#include "flock/tasks.hpp"
#include "flock/automations.hpp"
#include "flock/conversations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"
#include "task_history.hpp"
#include "flock/command.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/person.hpp"
#include "flock/bot_access.hpp"
#include "flock/policy_rules.hpp"
#include "flock/secret_requests.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/store.hpp"
#include "flock/store_options.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <sqlite3.h>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

using namespace flockv0;
using flockv0::test::lastTaskEvent;

namespace {
Decision converse(const ModelInput& input, const std::string& tool, Id taskId, Json args) {
    if (!input.event.at("turn").empty()) return {"reply", {{"text", "Done."}}, "Report the change"};
    if (taskId) {
        for (const auto& task : input.task.at("tasks")) {
            if (task.at("taskId") != taskId) continue;
            args["taskId"] = taskId;
            args["generation"] = task.at("generation");
            if (tool == "change_task" || tool == "answer_task_question") args["revisionId"] = task.at("revisionId");
        }
    }
    const auto text = tool == "start_task" ? "request" : "instruction";
    if (!args.contains(text)) args[text] = input.event.at("text");
    return {tool, std::move(args), "Scripted turn"};
}
using flockv0::test::check;
void executeSql(const std::filesystem::path& databaseFile, const char* sql) {
    sqlite3* database = nullptr;
    const auto opened = sqlite3_open(databaseFile.c_str(), &database);
    if (opened != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "Could not open the test database";
        if (database) sqlite3_close(database);
        throw std::runtime_error(message);
    }
    char* error = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    const std::string message = error ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    sqlite3_close(database);
    if (result != SQLITE_OK) throw std::runtime_error(message);
}
constexpr auto Evidence = "report and independent verification";

Decision work(const char* tool = "delegate", bool initial = true) {
    auto args = Json{{"instruction", "Check the fixture and report"}};
    if (initial) args["doneWhen"] = Evidence;
    return {tool, args, "Need the fixture report"};
}
Decision complete() {
    return {"complete", {{"report", "Verified the fixture"}}, "The report includes the required evidence"};
}
Decision ask() {
    return {"ask_user", {{"question", "Which branch?"}}, "The branch needs user confirmation"};
}

struct Script : Model {
    std::function<Decision(const ModelInput&)> script;
    int calls = 0;
    Json usage = Json::object();
    bool authenticated = false;
    bool hasAuthentication() const override { return authenticated; }
    ModelTurn decide(const ModelInput& input) override {
        ++calls;
        return {{script(input)}, usage};
    }
};

struct SimulatedT3 : T3 {
    int launches = 0, follows = 0, selectedStarts = 0, replies = 0, interrupts = 0, reconciles = 0, reads = 0, polls = 0;
    std::string generation = "same-oauth-session";
    bool loseAcknowledgement = false;
    bool delayLaunchRunId = false;
    bool failNextAnswerRead = false;
    bool hasReconciledRun = true;
    Delivery unconfirmedDelivery;
    std::string expectedBranch = "main";
    std::map<std::string, Observation> runs;
    std::map<std::string, RunRef> assignments;
    std::vector<Command> seen, recovery;
    std::vector<ReadRequest> readRequests;
    std::vector<Excerpt> historyPages;
    std::function<void()> beforeRead;
    std::function<RunEvidence(const RunRef&, std::string_view)> recoverEvidence;
    std::vector<RunRef> interruptedRuns;
    std::map<std::string, std::vector<T3Project>, std::less<>> projectsByEnvironment;
    std::function<void()> onPreflight;
    std::function<bool(const Command&)> rejectWork;
    std::function<void()> beforeWorkAcknowledgement;
    std::vector<T3Project> projects(std::string_view environment) override {
        const auto found = projectsByEnvironment.find(environment);
        return found == projectsByEnvironment.end() ? std::vector<T3Project>{{"p", "Fixture project", {}}} : found->second;
    }
    std::string authGeneration(const std::string&, const Json&) override { return generation; }
    std::optional<Delivery> preflight(const Command&) override {
        if (onPreflight) onPreflight();
        return std::nullopt;
    }
    Delivery send(const Command& cmd) override {
        seen.push_back(cmd);
        check(cmd.authGeneration == generation, "OAuth generation missing from committed send");
        if (cmd.tool == CommandTool::answer_session_question) {
            if (std::exchange(failNextAnswerRead, false)) return {Delivery::Kind::Unknown, {}, "T3 question read failed", true};
            ++replies;
            check(cmd.questionId == "branch-question" && cmd.answers["branch"] == expectedBranch, "wrong T3 answer");
            runs.at(cmd.target.runId) = {};
            if (std::exchange(loseAcknowledgement, false)) return {};
            return {Delivery::Kind::Accepted, {}, {}};
        }
        check(cmd.instruction.find("[Flock action ID: " + cmd.actionId + "]") != std::string::npos, "missing full action marker");
        if (rejectWork && rejectWork(cmd))
            return {Delivery::Kind::NotAccepted, {}, "The provisioned specialist worktree does not match the saved commit", true};
        const auto serial = launches + follows + selectedStarts + 1;
        RunRef run{"thread-" + std::to_string(launches + 1), "run-" + std::to_string(serial), "https://t3.test/thread"};
        if (cmd.tool == CommandTool::delegate)
            ++launches;
        else {
            if (cmd.tool == CommandTool::start_in_session) {
                check(!cmd.target.threadId.empty() && cmd.target.runId.empty(), "invalid selected Session target");
                ++selectedStarts;
            } else {
                check(cmd.tool == CommandTool::follow_up && !cmd.target.runId.empty(), "invalid follow-up target");
                ++follows;
            }
            run.threadId = cmd.target.threadId;
        }
        runs.emplace(run.runId, Observation{});
        check(assignments.emplace(cmd.actionId, run).second, "duplicated action");
        if (auto callback = std::exchange(beforeWorkAcknowledgement, {})) callback();
        if (std::exchange(loseAcknowledgement, false)) return {};
        if (cmd.tool == CommandTool::delegate && std::exchange(delayLaunchRunId, false))
            return {Delivery::Kind::Unknown, {run.threadId, {}, run.link}, "Run ID pending"};
        return {Delivery::Kind::Accepted, run, {}};
    }
    Delivery reconcile(const Command& cmd) override {
        ++reconciles;
        recovery.push_back(cmd);
        if (cmd.tool == CommandTool::answer_session_question && runs.at(cmd.target.runId).kind == Observation::Kind::Running)
            return {Delivery::Kind::Accepted, {}, {}};
        if (!hasReconciledRun || !assignments.contains(cmd.actionId)) return unconfirmedDelivery;
        return {Delivery::Kind::Accepted, assignments.at(cmd.actionId), {}};
    }
    Observation observe(const std::string&, const Json&, const RunRef& ref) override {
        ++polls;
        return runs.at(ref.runId);
    }
    RunEvidence readRunEvidence(const std::string& env, const Json& config, const RunRef& ref, std::string_view status) override {
        return recoverEvidence ? recoverEvidence(ref, status) : T3::readRunEvidence(env, config, ref, status);
    }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef& ref) override {
        check(runs.contains(ref.runId), "interrupted unrelated Run");
        interruptedRuns.push_back(ref);
        ++interrupts;
        return InterruptOutcome::Requested;
    }
    Excerpt read(const ReadRequest& req) override {
        const bool reported =
            std::any_of(runs.begin(), runs.end(), [&](const auto& entry) { return entry.second.resultRunId == req.runId; });
        const bool knownSession = req.wholeSession && req.runId.empty() && std::ranges::any_of(assignments, [&](const auto& entry) {
                                      return entry.second.threadId == req.threadId;
                                  });
        check(knownSession || (!req.runId.empty() && (runs.contains(req.runId) || reported)), "read unrelated Session or Run");
        readRequests.push_back(req);
        ++reads;
        if (beforeRead) beforeRead();
        if (!historyPages.empty()) return historyPages.at(reads - 1);
        return {reads == 1 ? "commit wanted, updates enabled" : "RSS 25 MiB",
                req.threadId,
                req.runId,
                "item-" + std::to_string(reads),
                "next",
                100,
                reads == 1};
    }
    void question(std::string run) {
        runs.at(run).kind = Observation::Kind::Question;
        runs.at(run).question = {"branch-question",
                                 {{.id = "branch",
                                   .prompt = "Which branch?",
                                   .freeform = false,
                                   .options = {{"Main branch", "main"}, {"Development branch", "dev"}}}}};
    }
    void external(std::string run) {
        runs.at(run).kind = Observation::Kind::ExternalInput;
        runs.at(run).status = "Approval is waiting in T3";
    }
    void finish(std::string run, std::string status = "completed", bool settled = true) {
        auto& observation = runs.at(run);
        observation.kind = settled ? Observation::Kind::Finished : Observation::Kind::Settling;
        observation.status = std::move(status);
        observation.report = "checked";
        observation.resultRunId = observation.status == "failed" ? run : run + "-result";
        observation.itemId = "final-answer";
    }
};

struct Fixture {
    std::filesystem::path directory;
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    SimulatedT3 t3;
    Script model;
    Seconds now = unixTime();
    bool requireT3Connection;
    std::map<std::string, BotNotes> notes{{"alpha", {"Initial SOUL", "ALPHA_ONLY"}}, {"beta", {"Second SOUL", "BETA_ONLY"}}};
    std::map<std::string, std::vector<PeerBot>, std::less<>> peers;

    explicit Fixture(bool requireConnection = false) : requireT3Connection(requireConnection) {
        char temp[] = "/tmp/flock-acceptance-XXXXXX";
        const auto* path = mkdtemp(temp);
        if (!path) throw std::runtime_error("Cannot create test directory");
        directory = path;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(directory);
    }
    void restart() {
        runtime.reset();
        store = std::make_unique<Store>(directory / "state.db",
                                        StoreOptions{.memorySeed = [&](std::string_view id) { return notes.at(std::string(id)).memory; }});
        RuntimeOptions options{[&] { return now; }};
        options.requireT3Connection = requireT3Connection;
        options.peers = [&](std::string_view source) {
            const auto found = peers.find(source);
            return found == peers.end() ? std::vector<PeerBot>{} : found->second;
        };
        runtime = std::make_unique<Runtime>(
            *store, model, t3, [&](std::string_view id) { return notes.at(std::string(id)).soul; }, std::move(options));
    }
    Id submit(std::string request = "inspect", std::string bot = "alpha", std::string project = "p") {
        return TaskIntake(*store).submit({bot, request, "e", project});
    }
    Json task(Id id) { return taskJson(Tasks(*store).task(id)); }
    void reach(Id id, std::string state) {
        for (int i = 0; i < 12; ++i) {
            if (task(id)["state"] == Json(state)) return;
            runtime->tick();
        }
        throw std::runtime_error("Could not reach " + state + " from " + task(id)["state"].get<std::string>());
    }
    Id wait(Id id, const char* reason) {
        reach(id, "waiting");
        const auto value = task(id)["wait"];
        check(value["reason"] == reason, "wrong waiting reason");
        return value["id"].get<Id>();
    }
    void rejects(Id id, Id waitId, const char* operation, Json data = Json::object()) {
        try {
            TaskControl(*store).operate(localOwner, TaskTarget::from(Tasks(*store).task(id).task, waitId), operation, data);
        } catch (const Conflict&) {
            return;
        }
        throw std::runtime_error("Expected stale or unsupported operation rejection");
    }
    int logs(Id id, std::string kind) {
        auto log = TaskHistory(*store).history(id, 0, 200);
        return static_cast<int>(std::count_if(log.begin(), log.end(), [&](const Json& j) { return j["kind"] == Json(kind); }));
    }
    std::vector<Json> botMessages(std::string_view bot) {
        std::vector<Json> messages;
        for (const auto& item : Conversations(*store).conversation(bot))
            if (item["kind"] == "bot_message") messages.push_back(item["data"]);
        return messages;
    }
};

void runtimeDatabaseLockIsExclusive() {
    Fixture f;
    bool excluded = false;
    try {
        Runtime other(*f.store, f.model, f.t3, [](std::string_view) { return std::string{}; });
    } catch (const Conflict&) {
        excluded = true;
    }
    check(excluded, "Two Runtimes acquired the same database lock");
    f.restart();
}

Id seedSelectedTask(Fixture& fixture) {
    T3Connections(*fixture.store).connect("e", localOwner.id, 4173);
    fixture.model.script = [](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "conversation_turn")
            return {"reply", {{"text", "Work in the selected Session."}}, "Acknowledge the request"};
        if (input.event.at("kind") == "policy_outcome") return ask();
        return work();
    };
    ConversationIntake(*fixture.store).receive(localOwner, {"alpha", "", "e", "p"}, "select-thread", "Continue in the selected Session");
    fixture.runtime->runConversations();
    const auto id = fixture.submit();
    executeSql(fixture.directory / "state.db", R"SQL(
UPDATE tasks SET source_message_id=(SELECT id FROM messages WHERE bot_id='alpha'),
    selected_environment_id='e',selected_project_id='p',selected_thread_id='selected-thread'
WHERE id=(SELECT max(id) FROM tasks);
)SQL");
    return id;
}

void selectedWorkSurvivesLostReceiptAndStopsOnlyAssignedRun() {
    Fixture f;
    const auto task = seedSelectedTask(f);
    f.t3.runs["other-run"] = {};
    f.t3.loseAcknowledgement = true;
    f.reach(task, "sending");
    f.runtime->tick();
    check(f.t3.selectedStarts == 1 && f.t3.launches == 0, "Selected work created another Thread");
    f.t3.runs.at("run-1").status = "queued";
    f.restart();
    f.now += 5;
    f.reach(task, "running");
    f.now += 2 * 86400;
    f.runtime->tick();
    check(f.t3.selectedStarts == 1 && f.t3.reconciles == 1 && f.task(task).at("state") == "running",
          "Restart changed the queued Run or duplicated the send");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(task).task, 0), "cancel");
    f.runtime->tick();
    check(f.t3.interruptedRuns.size() == 1 && f.t3.interruptedRuns.front().runId == "run-1" &&
              f.t3.interruptedRuns.front().threadId == "selected-thread",
          "Cancellation targeted unrelated work");
    f.t3.finish("run-1", "cancelled", false);
    f.restart();
    f.runtime->tick();
    check(f.task(task).at("state") == "cancelling", "A terminal Run with outstanding delegated work was marked cancelled after restart");
    f.t3.finish("run-1", "cancelled");
    f.reach(task, "cancelled");
}

void selectedWorkRechecksConnectionAfterApproval() {
    Fixture f;
    const auto task = seedSelectedTask(f);
    PolicyRules{*f.store}.setMode(localOwner, "supervised");
    f.wait(task, "policy_approval");
    const auto item = Inbox(*f.store).inbox(localOwner).at(0);
    check(item.at("requestTitle").get<std::string>().starts_with("t3.send"), "Selected work used the new-Thread policy");
    Inbox(*f.store).decideInbox(localOwner,
                                {item.at("botId"), InboxKind::policy_approval, item.at("itemId"), item.at("waitId"), item.at("taskId"),
                                 item.at("targetRevision"), item.at("targetGeneration")},
                                "allow_once");
    f.t3.onPreflight = [&] { T3Connections(*f.store).disconnect("e"); };
    f.wait(task, "user_question");
    check(f.t3.seen.empty() && f.logs(task, "policy_action_closed") == 1,
          "A previously approved selected Session sent work after its connection was revoked");
}

void disconnectBlocksSendingAndRetry() {
    {
        Fixture f(true);
        Store settingsStore(f.directory / "state.db");
        T3Connections connections(settingsStore);
        connections.connect("e", localOwner.id, 4173);
        const auto id = f.submit();
        f.reach(id, "sending");
        f.t3.onPreflight = [&] { connections.disconnect("e"); };
        const auto wait = f.wait(id, "auth");
        const auto sent = 0u;
        check(f.t3.seen.size() == sent && f.logs(id, "send_started") == sent, "A disconnect during preflight admitted a new T3 send");
        f.restart();
        f.t3.onPreflight = [] { throw std::runtime_error("A disconnected environment reached T3 preflight"); };
        f.now += 5;
        f.runtime->tick();
        check(f.wait(id, "auth") == wait, "A connection check repeated the sign-in notice");
        check(f.t3.seen.size() == sent && f.t3.reconciles == 0, "Retrying an unsent action after disconnect contacted T3");
        f.t3.onPreflight = {};
        connections.connect("e", localOwner.id, 4173);
        f.now += 5;
        f.reach(id, "running");
        check(f.t3.seen.size() == sent + 1 && f.t3.reconciles == 0, "Reconnecting did not permit one send of the waiting action");
    }
}

void pauseDuringPreflightPreventsSend() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(id, "sending");
    f.t3.onPreflight = [&] {
        Store settings(f.directory / "state.db");
        BotLifecycle(settings).setBotPaused(localOwner, "alpha", true);
        BotLifecycle(settings).setBotPaused(localOwner, "alpha", false, [&] { return f.now; });
    };
    f.reach(id, "cancelled");
    check(f.t3.seen.empty() && f.logs(id, "send_started") == 0, "A send in preflight survived pause and resume");
}

void disconnectKeepsOwnedRunRecovery() {
    Fixture f(true);
    Store settingsStore(f.directory / "state.db");
    T3Connections connections(settingsStore);
    connections.connect("e", localOwner.id, 4173);
    const auto id = f.submit();
    const auto queued = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.t3.loseAcknowledgement = true;
    f.reach(id, "sending");
    f.runtime->tick();
    connections.disconnect("e");
    f.now += 5;
    f.reach(id, "running");
    f.runtime->tick();
    check(f.t3.launches == 1 && f.t3.reconciles == 1 && f.t3.polls > 0,
          "Disconnect blocked reconciliation or observation of already sent work");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
    f.runtime->tick();
    check(f.t3.interruptedRuns.size() == 1 && f.t3.interruptedRuns.front().runId == "run-1",
          "Disconnect blocked interrupting the exact owned Run");
    f.t3.finish("run-1", "cancelled");
    f.reach(id, "cancelled");
    f.wait(queued, "auth");
    check(f.t3.launches == 1 && f.logs(queued, "send_started") == 0,
          "Previously queued work launched after the environment was disconnected");
}

void disconnectWaitsForInFlightSend() {
    using namespace std::chrono_literals;
    Fixture f(true);
    Store settingsStore(f.directory / "state.db");
    T3Connections connections(settingsStore);
    connections.connect("e", localOwner.id, 4173);
    const auto id = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(id, "sending");
    std::promise<void> sendEntered, releaseSend;
    auto release = releaseSend.get_future();
    f.t3.beforeWorkAcknowledgement = [&] {
        sendEntered.set_value();
        release.wait();
    };
    auto tick = std::async(std::launch::async, [&] { f.runtime->tick(); });
    if (sendEntered.get_future().wait_for(2s) != std::future_status::ready) {
        releaseSend.set_value();
        tick.get();
        throw std::runtime_error("The test did not reach the in-flight T3 send");
    }
    std::promise<void> disconnectEntered;
    auto disconnect = std::async(std::launch::async, [&] {
        disconnectEntered.set_value();
        return connections.disconnect("e");
    });
    disconnectEntered.get_future().wait();
    const bool waited = disconnect.wait_for(100ms) == std::future_status::timeout;
    releaseSend.set_value();
    tick.get();
    check(disconnect.get() && waited && f.task(id)["activeRun"]["runId"] == "run-1",
          "A separate Store disconnected during an in-flight send or lost its accepted Run");
}

void resumeSkipsOccurrencesWhileWaitingForDispatch() {
    using namespace std::chrono_literals;
    Fixture f;
    Store settings(f.directory / "state.db");
    const auto fireAt = f.now + 1;
    Automations(settings).createAutomation(
        localOwner, {"alpha", "Check staging", "e", "p", Json::object()},
        {{"kind", "reminder"}, {"timezone", "Etc/UTC"}, {"fireAt", fireAt}, {"request", "Check staging"}}, f.now);
    BotLifecycle(settings).setBotPaused(localOwner, "alpha", true);

    const auto id = f.submit("Other work", "beta");
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(id, "sending");
    std::promise<void> sendEntered, releaseSend;
    auto release = releaseSend.get_future();
    f.t3.beforeWorkAcknowledgement = [&] {
        sendEntered.set_value();
        release.wait();
    };
    auto tick = std::async(std::launch::async, [&] { f.runtime->tick(); });
    if (sendEntered.get_future().wait_for(2s) != std::future_status::ready) {
        releaseSend.set_value();
        tick.get();
        throw std::runtime_error("The test did not reach the in-flight T3 send");
    }

    std::atomic<Seconds> clock{f.now};
    std::promise<void> resumeEntered;
    auto resume = std::async(std::launch::async, [&] {
        resumeEntered.set_value();
        BotLifecycle(settings).setBotPaused(localOwner, "alpha", false, [&] { return clock.load(); });
    });
    resumeEntered.get_future().wait();
    const bool waited = resume.wait_for(100ms) == std::future_status::timeout;
    clock = fireAt;
    releaseSend.set_value();
    tick.get();
    resume.get();

    Automations(settings).fireDueAutomations(fireAt);
    check(waited && !BotLifecycle(settings).botPaused("alpha") && TaskList(settings).tasks("alpha").empty(),
          "Resume replayed a reminder that became due while dispatch held the lock");
}

void longRunApprovalAndCancel() {
    Fixture f;
    const auto id = f.submit();
    const auto queued = f.submit("Next");
    f.reach(id, "running");
    f.t3.external("run-1");
    f.wait(id, "t3_input");
    f.runtime->tick();
    check(f.t3.interrupts == 0 && f.t3.launches == 1, "automatic work during T3 approval");
    f.t3.runs.at("run-1") = {};
    f.reach(id, "running");
    f.runtime->tick();
    const auto before = Tasks(*f.store).task(id);
    f.now += 2 * 86400;
    f.restart();
    f.runtime->tick();
    const auto after = Tasks(*f.store).task(id);
    check(after.task.state == TaskState::running && after.task.generation == before.task.generation && !after.wait &&
              Inbox(*f.store).inbox(localOwner, "alpha").empty(),
          "a silent two-day Run interrupted the request");
    check(f.t3.interrupts == 0 && f.t3.launches == 1, "a silent Run triggered more work");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(before.task, 0), "cancel");
    f.runtime->tick();
    check(f.t3.interrupts == 1 && f.task(id)["state"] == "cancelling" && f.task(queued)["state"] == "queued",
          "cancel released project before confirmation");
    f.restart();
    const auto cursor = Notifications(*f.store).notificationEventsSince(localOwner, {"alpha"}, std::nullopt, {});
    f.t3.finish("run-1", "interrupted", false);
    f.runtime->tick();
    check(f.task(id)["state"] == "cancelling" && f.t3.interrupts == 1, "cancel ignored delegated work");
    f.t3.finish("run-1", "interrupted");
    f.reach(id, "cancelled");
    f.reach(queued, "running");
    check(f.t3.launches == 2 && f.logs(id, "report") == 1, "cancel re-delivered work");
    check(Notifications(*f.store).notificationEventsSince(localOwner, {"alpha"}, cursor.through, cursor.scope).events.empty(),
          "a cancelled request emitted a failure alert for its interrupted Run");
}

void waitsExpireWithReality() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) {
        if (i.event["kind"] == "intake") return work();
        if (i.event["kind"] == "question") return ask();
        return complete();
    };
    f.reach(id, "running");
    f.t3.question("run-1");
    const auto question = f.wait(id, "user_question");
    f.t3.runs.at("run-1") = {};
    f.reach(id, "running");
    f.rejects(id, question, "answer", {{"text", "main"}});
    f.t3.finish("run-1");
    f.reach(id, "completed");
    check(f.t3.replies == 0 && f.logs(id, "user_answer") == 0 && f.t3.seen.size() == 1, "stale waiting operation executed");
}

void connectionRecoveryRestoresSessionQuestion() {
    for (const auto* outage : {"auth", "settings"}) {
        Fixture f;
        const auto id = f.submit();
        f.reach(id, "running");
        f.t3.question("run-1");
        const auto old = f.wait(id, "user_question");
        f.t3.runs.at("run-1") = {.kind = Observation::Kind::Unavailable, .status = outage};
        f.runtime->tick();
        const auto connection = f.wait(id, outage);
        check(f.task(id).at("question").at("suspendedWaitId") == old, "connection wait lost the suspended question");
        check(Inbox(*f.store).inbox(localOwner, "alpha").empty(), "question remained actionable during the outage");
        f.rejects(id, old, "answer", {{"text", "dev"}});
        f.restart();
        f.t3.question("run-1");
        f.runtime->tick();
        const auto restored = f.wait(id, "user_question");
        const auto inbox = Inbox(*f.store).inbox(localOwner, "alpha");
        check(restored != old && restored != connection && inbox.size() == 1 && inbox[0]["waitId"] == restored &&
                  inbox[0]["prompt"] == "Which branch?" && inbox[0]["choices"] == Json::array({"Main branch", "Development branch"}) &&
                  f.model.calls == 0,
              "the live question did not return with its clarification and a fresh target");
        f.rejects(id, old, "answer", {{"text", "dev"}});
        TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, restored), "answer", {{"text", "main"}});
        f.reach(id, "running");
        check(f.t3.replies == 1 && f.t3.seen.back().target.runId == "run-1" && f.t3.launches == 1 && f.logs(id, "session_question") == 1 &&
                  f.logs(id, "user_answer") == 1,
              "recovered question did not resume the original Run exactly once");
    }
}

void connectionRecoveryDiscardsExpiredSessionQuestion() {
    for (const bool replaced : {false, true}) {
        Fixture f;
        const auto id = f.submit();
        f.reach(id, "running");
        f.t3.question("run-1");
        const auto old = f.wait(id, "user_question");
        f.t3.runs.at("run-1") = {.kind = Observation::Kind::Unavailable, .status = "auth"};
        f.runtime->tick();
        f.wait(id, "auth");
        f.restart();
        if (replaced) {
            f.t3.question("run-1");
            f.t3.runs.at("run-1").question.requestId = "replacement";
            f.t3.runs.at("run-1").question.fields.front().prompt = "Which new branch?";
            f.runtime->tick();
            f.wait(id, "user_question");
            const auto inbox = Inbox(*f.store).inbox(localOwner, "alpha");
            check(inbox.size() == 1 && inbox[0]["prompt"] == "Which new branch?", "recovery restored an eclipsed clarification");
        } else {
            f.t3.runs.at("run-1") = {};
            f.reach(id, "running");
            check(Inbox(*f.store).inbox(localOwner, "alpha").empty(), "recovery restored a remotely resolved question");
        }
        f.rejects(id, old, "answer", {{"text", "main"}});
        check(f.t3.replies == 0 && f.logs(id, "session_question_closed") == 1, "a closed question was answered or remained open");
    }
}

void failedPreflightIsUnsent() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.t3.onPreflight = [] { throw ModelError("Missing T3 settings", false); };
    f.wait(id, "settings");
    check(f.t3.seen.empty() && f.logs(id, "send_started") == 0, "failed preflight started a delivery");
    f.restart();
    f.t3.onPreflight = {};
    f.now += 5;
    f.reach(id, "running");
    check(f.t3.launches == 1 && f.t3.reconciles == 0 && f.logs(id, "send_started") == 1,
          "preflight failure did not leave one unsent action");
}

void projectDiscoveryResumesWhenT3Responds() {
    Fixture f;
    const auto id = f.submit("Check the default project", "alpha", "");
    f.t3.projectsByEnvironment["e"] = {};
    f.model.script = [](const ModelInput&) { return work(); };
    const auto wait = f.wait(id, "settings");
    f.now += 5;
    f.runtime->tick();
    check(f.wait(id, "settings") == wait && f.t3.seen.empty(), "An empty T3 project list repeated its notice or started work");
    f.restart();
    f.t3.projectsByEnvironment["e"] = {{"p", "Available project", {}}};
    f.now += 5;
    f.reach(id, "running");
    check(f.t3.launches == 1 && f.model.calls == 0 && f.t3.seen.front().projectId == "p" && f.task(id)["wait"].is_null(),
          "The original task did not start when a T3 project became available");
}

void recoveredDelivery() {
    Fixture f;
    const auto id = f.submit();
    f.reach(id, "sending");
    f.restart();
    f.t3.loseAcknowledgement = true;
    f.runtime->tick();
    f.restart();
    f.t3.generation = "fresh-oauth-session";
    f.now += 5;
    f.reach(id, "running");
    check(f.t3.reconciles == 1 && f.t3.seen.size() == 1 && f.model.calls == 0 &&
              f.t3.recovery.back().authGeneration == "same-oauth-session",
          "Automatic delivery check changed the saved send, called a model, or lost the original sign-in identity");
    f.restart();
    f.t3.finish("run-1");
    f.reach(id, "completed");
    f.runtime->tick();
    check(f.t3.launches == 1 && f.logs(id, "report") == 1, "duplicate Run or report");
}

void launchReceiptSurvivesRecovery() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(id, "sending");
    f.t3.delayLaunchRunId = true;
    f.runtime->tick();
    check(f.logs(id, "run_id_pending") == 1, "launch receipt did not persist");

    f.restart();
    f.now += 5;
    f.reach(id, "running");
    check(f.t3.recovery.back().receipt && f.t3.recovery.back().receipt->threadId == "thread-1" && f.t3.launches == 1,
          "reconciliation lost the launch receipt or sent another launch");
}

void unconfirmedDeliveryAcrossRestart() {
    Fixture f;
    const auto id = f.submit();
    f.reach(id, "sending");
    f.t3.delayLaunchRunId = true;
    f.t3.hasReconciledRun = false;
    f.t3.unconfirmedDelivery = {Delivery::Kind::Auth, {}, "T3 sign-in expired"};
    f.runtime->tick();
    const auto original = f.t3.seen.front();
    f.restart();
    f.runtime->tick();
    check(f.t3.seen.size() == 1 && f.t3.reconciles == 0, "Restart sent the original action or skipped the delivery check interval");
    f.now += 5;
    f.runtime->tick();
    f.wait(id, "auth");
    check(f.t3.seen.size() == 1 && f.t3.reconciles == 1 && f.model.calls == 0,
          "A sign-in error during a check retried the send or called a model");

    f.restart();
    f.t3.unconfirmedDelivery = {Delivery::Kind::Unknown, {}, "T3 history does not identify the Run", true};
    f.now += 5;
    f.runtime->tick();
    const auto unresolved = Tasks(*f.store).task(id);
    check(f.t3.seen.size() == 1 && f.t3.reconciles == 2 && unresolved.pendingAction &&
              unresolved.pendingAction->phase == OutboxPhase::started && unresolved.task.state == TaskState::deciding &&
              Inbox(*f.store).inbox(localOwner, "alpha").empty(),
          "An inconclusive check reopened the send or put the task in Needs you");

    f.restart();
    f.now += 5;
    f.reach(id, "waiting");
    check(f.t3.seen.size() == 1 && Tasks(*f.store).task(id).wait->reason() == WaitReason::user_question && f.model.calls == 0,
          "An unconfirmed delivery was sent again or did not ask the person what to do");
}

void unsentAnswerRecoversAutomatically() {
    Fixture f;
    const auto id = f.submit();
    f.reach(id, "running");
    const auto run = f.task(id).at("activeRun");
    f.t3.question("run-1");
    const auto asked = f.wait(id, "user_question");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, asked), "answer", {{"text", "main"}});
    f.reach(id, "sending");
    f.t3.failNextAnswerRead = true;
    f.runtime->tick();
    const auto answer = f.t3.seen.back();
    const auto pending = Tasks(*f.store).task(id).pendingAction;
    check(f.t3.replies == 0 && pending && pending->phase == OutboxPhase::ready, "An answer blocked before sending lost its saved action");
    f.restart();
    f.now += 5;
    f.reach(id, "running");
    const auto delivered = f.t3.seen.back();
    check(f.t3.replies == 1 && f.t3.launches == 1 && f.t3.reconciles == 0 && f.task(id)["activeRun"] == run &&
              f.logs(id, "session_answered") == 1 && delivered.actionId == answer.actionId && delivered.questionId == answer.questionId,
          "The saved answer was not sent exactly once to its original Run and question");
}

void cancelDuringDelivery() {
    Fixture f;
    Store cli(f.directory / "state.db");
    const auto independent = f.submit("Other project work", "beta", "other");
    f.reach(independent, "running");
    const auto id = f.submit("Work being cancelled");
    const auto queued = f.submit("Next project task");
    f.t3.beforeWorkAcknowledgement = [&] { TaskControl(cli).operate(localOwner, TaskTarget::from(Tasks(cli).task(id).task, 0), "cancel"); };
    for (int i = 0; i < 12 && f.t3.launches < 2; ++i) f.runtime->tick();
    f.t3.beforeWorkAcknowledgement = {};
    f.restart();

    const auto cancelling = f.task(id);
    check(cancelling["state"] == "cancelling" && cancelling["activeRun"]["runId"] == "run-2",
          "cancellation lost the Run acknowledged by T3");
    for (int i = 0; i < 12 && f.t3.interruptedRuns.empty(); ++i) f.runtime->tick();
    check(f.t3.interruptedRuns.size() == 1 && f.t3.interruptedRuns[0].threadId == "thread-2" && f.t3.interruptedRuns[0].runId == "run-2",
          "cancelling delivery did not interrupt the Run that T3 accepted");
    check(f.task(queued)["state"] == "queued" && f.task(independent)["state"] == "running",
          "cancelling delivery changed the project queue or unrelated work");

    f.t3.finish("run-2", "interrupted");
    f.reach(id, "cancelled");
    f.reach(queued, "running");
    check(f.t3.launches == 3 && f.logs(id, "report") == 1, "settling cancelled delivery duplicated a Run or its result");
}

void settleWhileAnswerDeliveryIsUnknown() {
    for (const bool cancel : {false, true}) {
        Fixture f;
        const auto id = f.submit();
        f.reach(id, "running");
        f.t3.question("run-1");
        const auto asked = f.wait(id, "user_question");
        f.t3.loseAcknowledgement = true;
        TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, asked), "answer", {{"text", "main"}});
        f.reach(id, "sending");
        f.runtime->tick();
        f.t3.question("run-1");
        f.restart();

        if (cancel) {
            TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
            for (int i = 0; i < 12 && f.t3.interruptedRuns.empty(); ++i) f.runtime->tick();
            check(f.task(id)["state"] == "cancelling" && f.t3.interruptedRuns.size() == 1 &&
                      f.t3.interruptedRuns[0].threadId == "thread-1" && f.t3.interruptedRuns[0].runId == "run-1",
                  "unknown answer delivery blocked interruption of the known Run");
        }
        f.t3.finish("run-1", cancel ? "interrupted" : "completed");
        f.reach(id, cancel ? "cancelled" : "completed");
        check(f.logs(id, "report") == 1 && f.t3.replies == 1 && f.t3.launches == 1, "settled Run duplicated its answer or report");
    }
}

void settledReportCanRecoverAfterRestart() {
    for (const bool available : {false, true}) {
        Fixture f;
        const std::string secret = "private-report-diagnostic";
        SecretRequests secretRequests{*f.store};
        secretRequests.submitSecret(localOwner, "alpha", secretRequests.requestSecret("alpha", {"diagnostic"}), secret);
        const auto id = f.submit();
        f.reach(id, "running");
        f.t3.finish("run-1");
        auto& observation = f.t3.runs.at("run-1");
        observation.evidence = RunEvidence::Availability::ReportUnavailable;
        observation.report.clear();
        observation.itemId.clear();
        observation.detail = secret;
        f.runtime->tick();
        const auto history = TaskHistory(*f.store).history(id);
        check(f.task(id)["activeRun"].is_null() && f.task(id)["lastRun"]["runId"] == "run-1" &&
                  std::ranges::any_of(history,
                                      [](const Json& entry) {
                                          return entry.at("kind") == "run_settled" && entry.at("data").at("status") == "completed";
                                      }) &&
                  history.dump().find(secret) == std::string::npos,
              "missing evidence kept the Run active or leaked diagnostics");
        const auto polls = f.t3.polls;
        int attempts = 0;
        f.t3.recoverEvidence = [&](const RunRef& source, std::string_view status) {
            check(source.threadId == "thread-1" && source.runId == "run-1-result" && status == "completed",
                  "recovery used an unproven evidence source");
            ++attempts;
            if (available) return RunEvidence{RunEvidence::Availability::Available, "Recovered report", "recovered-item", "", false};
            return RunEvidence{RunEvidence::Availability::ReportUnavailable, "", "", secret, false};
        };
        f.restart();
        f.runtime->tick();
        check(f.model.calls == 0 && attempts == 0, "recovery made a model call or retried before the scheduled time");
        f.now += 5;
        f.runtime->tick();
        if (!available) {
            f.now += 10;
            f.runtime->tick();
        }
        f.reach(id, "completed");
        check(attempts == (available ? 1 : 2) && f.t3.polls == polls && f.logs(id, "report") == 1 && f.logs(id, "completed") == 1 &&
                  lastTaskEvent(*f.store, id, "completed").at("data").at("text") ==
                      (available ? "Recovered report" : "The Session finished without a readable report."),
              "evidence retry repeated Run observation or closed the task with the wrong report");
    }
}

void unavailableReportStillClosesTheTask() {
    Fixture f;
    const auto id = f.submit();
    int reportReads = 0;
    f.t3.recoverEvidence = [&](const RunRef& source, std::string_view status) {
        check(source.threadId == "thread-1" && source.runId == "run-1-result" && status == "completed",
              "report recovery used a different Run");
        ++reportReads;
        return RunEvidence{RunEvidence::Availability::ReportUnavailable, "", "", "Unavailable", false};
    };
    f.reach(id, "running");
    f.t3.finish("run-1");
    f.t3.runs.at("run-1").evidence = RunEvidence::Availability::ReportUnavailable;
    for (int i = 0; i < 6 && f.task(id).at("state") != "completed"; ++i) {
        f.now += 10;
        f.runtime->tick();
    }
    check(reportReads >= 1 && f.task(id).at("state") == "completed" &&
              lastTaskEvent(*f.store, id, "completed").at("data").at("text") == "The Session finished without a readable report.",
          "an unreadable report kept the task open or was retried without end");
}

void missingReportDoesNotBlockCancellation() {
    for (const bool settledBeforeCancel : {false, true}) {
        Fixture f;
        const auto id = f.submit();
        const auto next = f.submit("Next task");
        f.reach(id, "running");
        f.t3.finish("run-1", "interrupted");
        auto& observation = f.t3.runs.at("run-1");
        observation.evidence = RunEvidence::Availability::ReportUnavailable;
        observation.report.clear();
        if (settledBeforeCancel) f.runtime->tick();
        f.restart();
        TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
        f.reach(id, "cancelled");
        f.reach(next, "running");
        check(f.t3.interrupts == 0 && f.t3.launches == 2 && f.logs(id, "report") == 1 && f.task(id)["activeRun"].is_null(),
              "missing report held cancellation or the next task's project slot");
    }
}

void taskPagesShowProjectOwners() {
    Fixture f;
    const auto desktop = f.submit("Verify main", "beta");
    f.reach(desktop, "running");
    const auto cloud = TaskIntake(*f.store).submit({"beta", "Check cloud", "cloud", "p"});
    const auto desktopNext = f.submit("Desktop follow-up");
    const auto independent = f.submit("Separate release", "alpha", "another-project");
    const auto cancelled = f.submit("Withdrawn release");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(cancelled).task, 0), "cancel");
    const auto cloudNext = TaskIntake(*f.store).submit({"alpha", "Cloud follow-up", "cloud", "p"});
    f.restart();

    const auto newest = taskJson(TaskList(*f.store).tasks("alpha", 0, 2));
    const Json cloudOwner = {{"id", cloud}, {"botId", "beta"}, {"request", "Check cloud"}, {"state", "queued"}};
    check(newest.size() == 2 && newest[0].at("id") == cloudNext && newest[0].at("blockedBy") == cloudOwner &&
              newest[1].at("id") == cancelled && newest[1].at("state") == "cancelled" && newest[1].at("blockedBy").is_null(),
          "the newest page lost the off-page cloud owner or blocked cancelled work");
    const auto older = taskJson(TaskList(*f.store).tasks("alpha", cancelled, 2));
    const Json desktopOwner = {{"id", desktop}, {"botId", "beta"}, {"request", "Verify main"}, {"state", "running"}};
    check(older.size() == 2 && older[0].at("id") == independent && older[0].at("blockedBy").is_null() && older[1].at("id") == desktopNext &&
              older[1].at("blockedBy") == desktopOwner && TaskList(*f.store).tasks("alpha", desktopNext, 2).empty(),
          "the older page skipped a task, repeated the cursor or mixed up project owners");
    const auto active = TaskList(*f.store).activeTasks("alpha");
    check(active.size() == 3 && active.front().task.id == cloudNext && active.front().blockedBy && active.front().blockedBy->id == cloud &&
              active.front().blockedBy->botId == "beta" && active.front().blockedBy->request == "Check cloud" &&
              active.front().blockedBy->state == TaskState::queued && active.back().task.id == desktopNext && active.back().blockedBy &&
              active.back().blockedBy->id == desktop && active.back().blockedBy->botId == "beta" &&
              active.back().blockedBy->request == "Verify main" && active.back().blockedBy->state == TaskState::running,
          "the active task list lost the owners of work in separate environments");

    const auto history = taskJson(TaskList(*f.store).tasks({}, cloud, 1));
    check(history.size() == 1 && history[0].at("id") == desktop && history[0].at("blockedBy").is_null() &&
              history[0].at("runs").size() == 1 && history[0].at("runs")[0].at("link") == "https://t3.test/thread" &&
              history[0].at("revisions").size() == 1 && history[0].at("runs")[0].at("revisionId") == history[0].at("revisions")[0].at("id"),
          "the unfiltered task page lost the running owner's Session link or revision after restart");
}

void conversationTaskLaunchesItsRequest() {
    Fixture f;
    const auto receipt =
        ConversationIntake(*f.store).receive(localOwner, {"alpha", "", "e", "p"}, "work-with-criteria", "Check the release");
    f.model.script = [&](const ModelInput& input) -> Decision {
        check(input.event.at("kind") == "conversation_turn", "a conversation task asked the task loop to decide its launch");
        if (!input.event.at("turn").empty()) return {"reply", {{"text", "Started."}}, "Tell the person"};
        return converse(input, "start_task", 0, {{"doneWhen", "Confirm the signature"}});
    };
    f.runtime->runConversations();
    const Id task = TaskList(*f.store).tasks("alpha").at(0).task.id;
    f.reach(task, "running");
    const auto saved = f.task(task);
    check(saved.at("doneWhen") == "Confirm the signature" && saved.at("revisions").at(0).at("sourceMessageId") == receipt.at("messageId") &&
              f.t3.seen.size() == 1 && f.t3.seen.front().instruction.find("Check the release") != std::string::npos,
          "the conversation task did not launch its request with the saved criterion and source");
}

void wakeTurnInstructionBecomesOneFollowUp() {
    Fixture f;
    ConversationIntake(*f.store).receive(localOwner, {"alpha", "", "e", "p"}, "work", "Check the release");
    Id task = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event.contains("sessionWake")) return converse(input, "send_to_task", task, {{"instruction", "Run the tests too"}});
        return converse(input, "start_task", 0, {});
    };
    f.runtime->runConversations();
    task = TaskList(*f.store).tasks("alpha").at(0).task.id;
    f.reach(task, "running");
    int refusals = 3;
    f.t3.rejectWork = [&](const Command& cmd) { return cmd.tool == CommandTool::follow_up && refusals-- > 0; };
    f.t3.finish("run-1");
    for (int i = 0; i < 20 && f.t3.follows == 0; ++i) {
        f.now += 5;
        f.runtime->tick();
        f.runtime->runConversations();
    }
    check(f.t3.follows == 1 && refusals < 0 && f.task(task)["state"] == "running" &&
              f.t3.seen.back().instruction.find("Run the tests too") != std::string::npos,
          "an instruction sent from the wake turn did not reach the Session as one follow-up");
    check(f.logs(task, "send_started") == 2, "each retry of a refused follow-up wrote another send_started entry");
}

void sessionQuestionAcceptsTheShownChoice() {
    Fixture f;
    const auto id = f.submit();
    f.reach(id, "running");
    f.t3.question("run-1");
    const auto asked = f.wait(id, "user_question");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, asked), "answer", {{"text", "Main branch"}});
    f.reach(id, "running");
    check(f.t3.replies == 1, "the choice Flock showed was not sent to the Session as its value");

    f.t3.runs.at("run-1").kind = Observation::Kind::Question;
    f.t3.runs.at("run-1").question = {"two-fields", {{.id = "branch", .prompt = "Which branch?"}, {.id = "tag", .prompt = "Which tag?"}}};
    f.wait(id, "t3_input");
}

void replyToInterruptedTaskContinuesIt() {
    Fixture f;
    const auto id = f.submit();
    f.reach(id, "running");
    f.t3.finish("run-1", "interrupted");
    const auto asked = f.wait(id, "user_question");
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, asked), "answer", {{"text", "Carry on"}});
    f.reach(id, "running");
    check(f.t3.follows == 1 && f.t3.seen.back().instruction.find("Carry on") != std::string::npos,
          "the reply to an interrupted task did not reach its Session");
}

void recoveredRunAcceptsQuestion() {
    Fixture f;
    const auto id = TaskIntake(*f.store).submit({"alpha", "Recover and answer", "e", "p"});
    f.reach(id, "sending");
    f.t3.loseAcknowledgement = true;
    f.runtime->tick();
    f.now += 5;
    f.reach(id, "running");
    check(f.model.calls == 0 && f.t3.reconciles == 1 && f.t3.launches == 1, "delivery recovery used a model call or resent the Run");
    f.t3.question("run-1");
    const auto asked = f.wait(id, "user_question");
    f.restart();
    TaskControl(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, asked), "answer", {{"text", "main"}});
    f.reach(id, "running");
    check(f.model.calls == 0 && f.t3.replies == 1, "delivery recovery lost the original question");
}

}

int main() {
    for (const auto& [name, run] : std::vector<std::pair<const char*, void (*)()>>{
             {"exclusive Runtime database lock", runtimeDatabaseLockIsExclusive},
             {"failed preflight", failedPreflightIsUnsent},
             {"project discovery recovers automatically", projectDiscoveryResumesWhenT3Responds},
             {"long Run, approval and cancel", longRunApprovalAndCancel},
             {"stale waits", waitsExpireWithReality},
             {"recover live question across auth and settings waits", connectionRecoveryRestoresSessionQuestion},
             {"discard expired question after connection recovery", connectionRecoveryDiscardsExpiredSessionQuestion},
             {"delivery recovery", recoveredDelivery},
             {"unconfirmed delivery across restart", unconfirmedDeliveryAcrossRestart},
             {"launch receipt recovery", launchReceiptSurvivesRecovery},
             {"retry unsent Session answer", unsentAnswerRecoversAutomatically},
             {"cancel during delivery", cancelDuringDelivery},
             {"selected work reconciles and cancels only its own queued Run", selectedWorkSurvivesLostReceiptAndStopsOnlyAssignedRun},
             {"selected work rechecks its T3 connection after approval", selectedWorkRechecksConnectionAfterApproval},
             {"disconnect blocks new work and retries", disconnectBlocksSendingAndRetry},
             {"pause during preflight prevents a send", pauseDuringPreflightPreventsSend},
             {"resume skips reminders while waiting for dispatch", resumeSkipsOccurrencesWhileWaitingForDispatch},
             {"disconnect preserves owned Run recovery", disconnectKeepsOwnedRunRecovery},
             {"disconnect fences sends across Stores", disconnectWaitsForInFlightSend},
             {"Run settlement with unknown answer delivery", settleWhileAnswerDeliveryIsUnknown},
             {"settled report recovery across restart", settledReportCanRecoverAfterRestart},
             {"late report recovery after retries exhaust", unavailableReportStillClosesTheTask},
             {"cancellation with unavailable report", missingReportDoesNotBlockCancellation},
             {"project owners on task pages", taskPagesShowProjectOwners},
             {"Conversation task launches its request", conversationTaskLaunchesItsRequest},
             {"Recovered Run accepts its original question", recoveredRunAcceptsQuestion},
             {"wake turn instruction becomes one follow-up", wakeTurnInstructionBecomesOneFollowUp},
             {"Session question accepts the shown choice", sessionQuestionAcceptsTheShownChoice},
             {"reply to an interrupted task continues it", replyToInterruptedTaskContinuesIt},
             {"Pending conversation approval leaves an unrelated task free to complete",
              [] {
                  Fixture f;
                  f.model.script = [](const ModelInput& input) {
                      if (input.event.at("kind") == "intake") return work();
                      if (input.event.at("kind") == "conversation_turn")
                          return Decision{"request_bot_change",
                                          {{"operation", "create_bot"}, {"targetBotId", "new-bot"}, {"name", "New Bot"}},
                                          "Create"};
                      return complete();
                  };
                  const auto task = f.submit();
                  f.reach(task, "running");
                  PolicyRules{*f.store}.setMode(localOwner, "supervised");
                  ConversationIntake(*f.store).receive(localOwner, {"alpha", "", "e", "separate"}, "unrelated", "Create another Bot");
                  f.runtime->runConversations();
                  check(Inbox(*f.store).inbox(localOwner).size() == 1, "The unrelated turn did not await approval");
                  f.t3.finish("run-1");
                  f.reach(task, "completed");
                  check(Inbox(*f.store).inbox(localOwner).size() == 1, "Task completion settled an unrelated approval");
              }},
             {"Never blocks ordinary launch with a T3 connection",
              [] {
                  Fixture f;
                  PolicyRules{*f.store}.changeNeverRule(localOwner, Action::environment_launch, "e", true);
                  T3Connections(*f.store).connect("e", localOwner.id, 4173);
                  f.wait(f.submit(), "settings");
                  check(f.t3.launches == 0, "the connector allowed a blocked Bot launch");
              }},
         }) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
            return 1;
        }
    }
}
