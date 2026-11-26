#include "flock/notifications.hpp"
#include "flock/secret_ref.hpp"
#include "flock/decision_errors.hpp"
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
#include "flock/policy.hpp"
#include "flock/run_ref.hpp"
#include "flock/run_report.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
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
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
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
void setSessionReadJournalFailure(const std::filesystem::path& databaseFile, bool active) {
    executeSql(databaseFile, active ? "CREATE TRIGGER fail_session_read_journal BEFORE INSERT ON journal "
                                      "WHEN NEW.kind='session_read' BEGIN SELECT RAISE(ABORT,'injected journal failure'); END"
                                    : "DROP TRIGGER fail_session_read_journal");
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
Decision reply() {
    Json answers = {{"branch", "main"}};
    return {"answer_session_question", {{"requestId", "branch-question"}, {"answers", answers}}, "Use the branch specified by the user"};
}
Decision botRequest(std::string bot, const char* kind, const char* message) {
    auto args = Json{{"botId", std::move(bot)}, {"kind", kind}, {"text", message}};
    if (std::string_view(kind) == "work") args["doneWhen"] = Evidence;
    return {"send_bot_message", std::move(args), "Ask the specialist"};
}
Decision botReply(const char* kind, std::string_view message) {
    return {"reply_to_bot", {{"kind", kind}, {"text", message}}, "Respond to the current exchange"};
}
bool hasTool(const ModelInput& input, std::string_view tool) {
    return std::find(input.tools.begin(), input.tools.end(), tool) != input.tools.end();
}

struct Script : Model {
    std::function<Decision(const ModelInput&)> script;
    int calls = 0;
    Json usage = Json::object();
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
    bool failBeforeWorkSend = false;
    bool failNextAnswerRead = false;
    bool hasReconciledRun = true;
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
        if (std::exchange(failBeforeWorkSend, false)) return {};
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
        if (!hasReconciledRun || !assignments.contains(cmd.actionId)) return {};
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
        check(!req.runId.empty() && (runs.contains(req.runId) || reported), "read unrelated Run");
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
        runs.at(run).question = {
            "branch-question", {{.id = "branch", .freeform = false, .options = {{"Main branch", "main"}, {"Development branch", "dev"}}}}};
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
    std::string read = "off";
    bool requireT3Connection;
    std::map<std::string, BotNotes> notes{{"alpha", {"Initial SOUL", "ALPHA_ONLY"}}, {"beta", {"Second SOUL", "BETA_ONLY"}}};
    std::map<std::string, std::vector<PeerBot>, std::less<>> peers;

    explicit Fixture(bool requireConnection = false) : requireT3Connection(requireConnection) {
        char temp[] = "/tmp/flock-acceptance-XXXXXX";
        const auto* path = mkdtemp(temp);
        if (!path) throw std::runtime_error("Cannot create test directory");
        directory = path;
        restart();
        for (auto bot : {"alpha", "beta"}) {
        }
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        std::filesystem::remove_all(directory);
    }
    void restart() {
        runtime.reset();
        store = std::make_unique<Store>(directory / "state.db", [&](std::string_view id) { return notes.at(std::string(id)).memory; });
        RuntimeOptions options{[&] { return now; }, read};
        options.requireT3Connection = requireT3Connection;
        options.peers = [&](std::string_view source) {
            const auto found = peers.find(source);
            return found == peers.end() ? std::vector<PeerBot>{} : found->second;
        };
        runtime = std::make_unique<Runtime>(
            *store, model, t3, [&](std::string_view id) { return notes.at(std::string(id)).soul; }, std::move(options));
    }
    void allowBeta() { peers["alpha"] = {{"Verifier", {"beta", "", "e", "p", {{"runTimeoutSeconds", 10}, {"model", "beta-model"}}}}}; }
    Id submit(std::string request = "inspect", std::string bot = "alpha", std::string project = "p") {
        return Tasks(*store).submit({bot, request, "e", project, {{"runTimeoutSeconds", 10}}});
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
            Tasks(*store).operate(localOwner, TaskTarget::from(Tasks(*store).task(id).task, waitId), operation, data);
        } catch (const Conflict&) {
            return;
        }
        throw std::runtime_error("Expected stale or unsupported operation rejection");
    }
    int logs(Id id, std::string kind) {
        auto log = Tasks(*store).history(id, 0, 200);
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
    Conversations(*fixture.store).receive(localOwner, {"alpha", "", "e", "p"}, "select-thread", "Continue in the selected Session");
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
    f.wait(task, "unknown_delivery");
    check(f.t3.selectedStarts == 1 && f.t3.launches == 0, "Selected work created another Thread");
    f.t3.runs.at("run-1").status = "queued";
    f.restart();
    f.reach(task, "running");
    f.now += 30;
    f.runtime->tick();
    check(f.t3.selectedStarts == 1 && f.t3.reconciles == 1 && f.task(task).at("state") == "running",
          "Restart duplicated the send or a confirmed queued Run reached a work deadline");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(task).task, 0), "cancel");
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
    Policy{*f.store}.setMode(localOwner, "supervised");
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
    for (const bool followUp : {false, true}) {
        Fixture f(true);
        Store settingsStore(f.directory / "state.db");
        T3Connections connections(settingsStore);
        connections.connect("e", localOwner.id, 4173);
        const auto id = f.submit();
        f.model.script = [](const ModelInput& input) { return input.event.at("kind") == "intake" ? work() : work("follow_up", false); };
        if (followUp) {
            f.reach(id, "running");
            f.t3.finish("run-1");
        }
        f.reach(id, "sending");
        f.t3.onPreflight = [&] { connections.disconnect("e"); };
        auto wait = f.wait(id, "auth");
        const auto sent = followUp ? 1u : 0u;
        check(f.t3.seen.size() == sent && f.logs(id, "send_started") == sent, "A disconnect during preflight admitted a new T3 send");
        f.restart();
        f.t3.onPreflight = [] { throw std::runtime_error("A disconnected environment reached T3 preflight"); };
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile");
        wait = f.wait(id, "auth");
        check(f.t3.seen.size() == sent && f.t3.reconciles == 0, "Retrying an unsent action after disconnect contacted T3");
        f.t3.onPreflight = {};
        connections.connect("e", localOwner.id, 4173);
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile");
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
        settings.setBotPaused(localOwner, "alpha", true);
        settings.setBotPaused(localOwner, "alpha", false, [&] { return f.now; });
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
    const auto wait = f.wait(id, "unknown_delivery");
    connections.disconnect("e");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile");
    f.reach(id, "running");
    f.runtime->tick();
    check(f.t3.launches == 1 && f.t3.reconciles == 1 && f.t3.polls > 0,
          "Disconnect blocked reconciliation or observation of already sent work");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
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
    settings.setBotPaused(localOwner, "alpha", true);

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
        settings.setBotPaused(localOwner, "alpha", false, [&] { return clock.load(); });
    });
    resumeEntered.get_future().wait();
    const bool waited = resume.wait_for(100ms) == std::future_status::timeout;
    clock = fireAt;
    releaseSend.set_value();
    tick.get();
    resume.get();

    Automations(settings).fireDueAutomations(fireAt);
    check(waited && !settings.botPaused("alpha") && Tasks(settings).tasks("alpha").empty(),
          "Resume replayed a reminder that became due while dispatch held the lock");
}

void invalidDecisions() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [&](const ModelInput& input) -> Decision {
        check(input.event["kind"] == "intake", "wrong decision event");
        if (f.model.calls == 1) return complete();
        if (f.model.calls == 2) return {"invented", Json::object(), "unknown tool"};
        throw ModelError("The model returned two tool calls", false);
    };
    f.runtime->tick();
    f.runtime->tick();
    f.restart();
    const auto wait = f.wait(id, "model_failure");
    check(f.model.calls == 5 && f.logs(id, "model_failure") == 3 && f.logs(id, "tool_error") == 2 && f.t3.launches == 0 &&
              f.logs(id, "decision") == 0,
          "invalid initial action executed");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "retry");
    f.rejects(id, wait, "retry");
    f.model.script = [](const ModelInput& input) {
        check(input.feedback.empty() && input.event.at("decisionFeedback").empty(), "retry kept prior errors");
        return work();
    };
    f.reach(id, "running");
    f.t3.question("run-1");
    f.t3.runs.at("run-1").question.fields.push_back({.id = "checks", .multiple = true, .options = {{"Lint", "lint"}}});
    f.model.script = [](const ModelInput&) { return work(); };
    const auto questionWait = f.wait(id, "model_failure");
    check(f.t3.launches == 1 && f.t3.replies == 0 && f.task(id)["doneWhen"] == Evidence, "invalid in-question action executed");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, questionWait), "retry");
    int answerAttempts = 0;
    f.model.script = [&](const ModelInput& input) {
        auto answer = reply();
        if (++answerAttempts == 1)
            answer.args["answers"]["checks"] = Json::array({"lint", "another check"});
        else {
            check(!input.event.at("decisionFeedback").empty(), "model did not receive feedback on its invalid answer");
            answer.args["answers"]["checks"] = "another check";
        }
        return answer;
    };
    f.reach(id, "running");
    check(answerAttempts == 2 && f.t3.replies == 1 && f.t3.seen.back().answers.at("checks") == "another check",
          "T3 received the invalid answer instead of the model's correction");
    f.t3.finish("run-1");
    f.model.script = [](const ModelInput&) { return work(); };
    f.wait(id, "model_failure");
    check(f.t3.launches == 1 && f.task(id)["doneWhen"] == Evidence, "doneWhen overwritten");
}

void correctInvalidTaskArguments() {
    Fixture f;
    const auto id = f.submit();
    const Json badArgs = {{"question", "Which branch?"}, {"choices", 12}};
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (f.model.calls == 1) return {"ask_user", badArgs, "Need a branch"};
        const auto& feedback = input.event.at("decisionFeedback");
        check(feedback.size() == 1 && feedback[0].at("tool") == "ask_user" && feedback[0].at("args") == badArgs &&
                  feedback[0].at("result").at("ok") == false &&
                  feedback[0].at("result").at("error") == "Expected up to twelve short textual choices",
              "model did not receive the rejected arguments and validation result");
        return ask();
    };
    f.reach(id, "deciding");
    f.runtime->tick();
    f.restart();
    const auto wait = f.wait(id, "user_question");
    check(f.model.calls == 2 && !f.logs(id, "model_failure") && f.logs(id, "decision") == 1,
          "correcting an invalid tool call consumed a model-failure strike");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "answer", {{"text", "main"}});
    f.model.script = [](const ModelInput& input) {
        check(input.event.at("decisionFeedback").empty(), "validation errors leaked into the user's answer event");
        return work();
    };
    f.reach(id, "running");
}

void replayQuestionDecision(std::string_view outage = {}) {
    for (const bool stillOpen : {true, false}) {
        Fixture f;
        const auto id = Tasks(*f.store).submit({"alpha", "Use main", "e", "p", {{"modelCallLimit", 2}}});
        f.model.script = [&](const ModelInput& input) {
            if (input.event.at("kind") == "intake") return work();
            // Lose T3 visibility at the post-inference liveness check, before any reply can commit.
            f.t3.runs.at("run-1") = {.kind = Observation::Kind::Unavailable, .status = std::string(outage)};
            return reply();
        };
        f.reach(id, "running");
        f.t3.question("run-1");
        f.reach(id, "deciding");
        f.runtime->tick();
        check(f.model.calls == 2 && f.task(id)["state"] == (outage.empty() ? "deciding" : "waiting") && !f.t3.replies,
              "reply did not stop between inference and effect");
        f.restart();
        if (outage == "settings") {
            const auto task = Tasks(*f.store).task(id);
            Tasks(*f.store).operate(localOwner, TaskTarget::from(task.task, task.wait.value().id), "reconcile");
        }
        if (stillOpen)
            f.t3.question("run-1");
        else
            f.t3.runs.at("run-1") = {};
        if (outage == "settings") f.runtime->tick();
        f.reach(id, "running");
        check(f.model.calls == 2 && f.t3.replies == int(stillOpen) && !f.task(id).contains("pendingDecision"),
              "restart repeated inference or applied a stale question reply");
    }
}

void ambiguityAndFailure() {
    Fixture f;
    const auto id = f.submit("Unclear branch");
    const auto next = f.submit("Queued", "beta");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == next) {
            check(input.notes.memory == "BETA_ONLY" && input.history.empty(), "Other Bot or task data leaked");
            return ask();
        }
        check(input.notes.memory == "ALPHA_ONLY", "wrong initial memory");
        if (input.event["kind"] == "intake" && !input.event.contains("userAnswer")) return ask();
        if (input.event["kind"] == "intake") {
            check(input.event["userAnswer"] == "main", "lost clarification");
            return work();
        }
        check(input.task["doneWhen"] == Evidence && input.notes.soul == "Edited SOUL", "did not reload notes or preserve conditions");
        if (input.event["status"] == "failed") return work("follow_up", false);
        return complete();
    };
    const auto wait = f.wait(id, "user_question");
    check(f.t3.launches == 0 && f.task(next)["state"] == "queued" && f.task(next)["blockedBy"]["id"] == id,
          "ambiguous task delegated or did not hold project order");
    f.restart();
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "answer", {{"text", "main"}});
    f.rejects(id, wait, "answer", {{"text", "dev"}});
    f.reach(id, "running");
    f.notes.at("alpha").soul = "Edited SOUL";
    f.t3.finish("run-1", "failed");
    f.restart();
    f.reach(id, "deciding");
    f.reach(id, "running");
    check(f.t3.follows == 1 && f.t3.seen.back().target.runId == "run-1", "did not follow up after failure");
    f.t3.finish("run-2");
    f.reach(id, "completed");
    f.wait(next, "user_question");
    check(f.logs(id, "report") == 2 && f.logs(id, "user_answer") == 1, "bad task history");
}

void deadlineApprovalAndCancel() {
    Fixture f;
    const auto id = f.submit();
    const auto queued = f.submit("Next");
    const auto independent = f.submit("Independent", "beta", "other");
    f.model.script = [=](const ModelInput& input) { return input.task["id"] == id ? work() : ask(); };
    f.reach(id, "running");
    f.wait(independent, "user_question");
    f.t3.external("run-1");
    f.wait(id, "t3_input");
    const auto calls = f.model.calls;
    f.runtime->tick();
    check(f.model.calls == calls && f.t3.interrupts == 0 && f.t3.launches == 1, "automatic work during T3 approval");
    f.t3.runs.at("run-1") = {};
    f.reach(id, "running");
    f.now += 20;
    f.wait(id, "deadline");
    f.runtime->tick();
    check(f.model.calls == calls && f.t3.interrupts == 0 && f.t3.launches == 1, "automatic work at deadline");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
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
    f.wait(queued, "user_question");
    check(f.t3.launches == 1 && f.logs(id, "report") == 1, "cancel re-delivered work");
    check(Notifications(*f.store).notificationEventsSince(localOwner, {"alpha"}, cursor.through, cursor.scope).events.empty(),
          "a cancelled request emitted a failure alert for its interrupted Run");
}

void budgetAndStaleWaits() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [&](const ModelInput& input) {
        if (input.event["kind"] == "intake") return work();
        if (input.event["kind"] == "question") return reply();
        const auto used = input.task["additionalUsed"].get<int>();
        if (used == 3 && !input.event.contains("userAnswer") && f.logs(id, "user_answer") == 0) return ask();
        return work(used == 1 ? "delegate" : "follow_up", false);
    };
    f.reach(id, "running");
    for (int i = 1; i <= 3; ++i) {
        f.t3.finish("run-" + std::to_string(i));
        f.reach(id, "deciding");
        f.reach(id, "running");
    }
    check(f.t3.launches == 2 && f.t3.follows == 2 && f.task(id)["remaining"] == 0, "wrong shared work budget");
    f.t3.finish("run-4");
    const auto question = f.wait(id, "user_question");
    f.rejects(id, question, "approve");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, question), "answer", {{"text", "keep going"}});
    const auto budget = f.wait(id, "budget");
    f.rejects(id, budget, "answer", {{"text", "yes"}});
    check(f.task(id)["wait"]["attempted"] == "follow_up" && f.task(id)["remaining"] == 0 && f.t3.seen.size() == 4,
          "normal answer permitted an extra follow-up");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, budget), "approve");
    f.rejects(id, budget, "approve");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
    f.reach(id, "cancelled");
    f.rejects(id, budget, "approve");
    f.runtime->tick();
    check(f.task(id)["remaining"] == 3 && f.logs(id, "budget_granted") == 1 && f.t3.seen.size() == 4,
          "replayed permit or late work after cancel");
}

void sessionQuestions() {
    for (const bool needsUser : {false, true}) {
        Fixture f;
        const auto id = f.submit(needsUser ? "check code" : "check main");
        f.model.script = [&](const ModelInput& input) -> Decision {
            if (input.event["kind"] == "intake") return work();
            if (input.event["kind"] == "question") {
                if (needsUser && !input.event.contains("userAnswer")) return ask();
                return reply();
            }
            if (input.task["additionalUsed"] == 0) return work("follow_up", false);
            return complete();
        };
        f.reach(id, "running");
        f.t3.question("run-1");
        f.reach(id, "deciding");
        if (needsUser) {
            const auto wait = f.wait(id, "user_question");
            const auto task = f.task(id);
            check(task["question"]["target"] ==
                      Json{{"waitId", wait}, {"eventId", task["eventId"]}, {"run", task["activeRun"]}, {"requestId", "branch-question"}},
                  "user wait lost its Session target");
            executeSql(f.store->path(), "UPDATE tasks SET data=json_set(data,'$.question.target.eventId',0) WHERE state='waiting'");
            f.rejects(id, wait, "answer", {{"text", "main"}});
            check(f.logs(id, "user_answer") == 0, "answer for a stale question target was recorded");
            executeSql(f.store->path(), "UPDATE tasks SET data=json_set(data,'$.question.target.eventId',"
                                        "json_extract(data,'$.eventId')) WHERE state='waiting'");
            f.restart();
            Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "answer", {{"text", "main"}});
            const auto answered = f.task(id);
            check(answered["question"]["target"]["waitId"].is_null() && answered["question"]["target"]["eventId"] == answered["eventId"] &&
                      answered["eventId"] != task["eventId"],
                  "answer did not advance the question target");
        }
        f.reach(id, "running");
        check(f.t3.replies == 1 && f.t3.launches == 1 && f.t3.follows == 0 && f.task(id)["additionalUsed"] == 0 &&
                  f.logs(id, "user_answer") == needsUser,
              "wrong branch reply behavior");
        f.t3.finish("run-1");
        f.reach(id, "deciding");
        f.reach(id, "running");
        check(f.t3.follows == 1 && f.task(id)["additionalUsed"] == 1, "follow-up did not add one Run and one unit");
        f.t3.finish("run-2");
        f.reach(id, "completed");
    }
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
    f.now += 20;
    const auto deadline = f.wait(id, "deadline");
    f.t3.finish("run-1");
    f.reach(id, "deciding");
    f.rejects(id, deadline, "extend", {{"deadline", f.now + 100}});
    f.reach(id, "completed");
    check(f.t3.replies == 0 && f.logs(id, "user_answer") == 0 && f.t3.seen.size() == 1, "stale waiting operation executed");
}

void connectionRecoveryRestoresSessionQuestion() {
    for (const auto* outage : {"auth", "settings"}) {
        for (const bool manual : {false, true}) {
            Fixture f;
            const auto id = f.submit();
            f.model.script = [](const ModelInput& i) {
                if (i.event.at("kind") == "intake") return work();
                if (i.event.contains("userAnswer")) return reply();
                auto question = ask();
                question.args["choices"] = {"main", "dev"};
                return question;
            };
            f.reach(id, "running");
            f.t3.question("run-1");
            const auto old = f.wait(id, "user_question");
            f.t3.runs.at("run-1") = {.kind = Observation::Kind::Unavailable, .status = outage};
            f.runtime->tick();
            const auto connection = f.wait(id, outage);
            check(f.task(id).at("question").at("suspendedWaitId") == old, "connection wait lost the suspended question");
            check(Inbox(*f.store).inbox(localOwner, "alpha").empty(), "question remained actionable during the outage");
            f.rejects(id, old, "answer", {{"text", "dev"}});
            if (manual) Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, connection), "reconcile");
            f.restart();
            f.t3.question("run-1");
            f.runtime->tick();
            const auto restored = f.wait(id, "user_question");
            const auto inbox = Inbox(*f.store).inbox(localOwner, "alpha");
            check(restored != old && restored != connection && inbox.size() == 1 && inbox[0]["waitId"] == restored &&
                      inbox[0]["prompt"] == "Which branch?" && inbox[0]["choices"] == Json::array({"main", "dev"}) && f.model.calls == 2,
                  "the live question did not return with its clarification and a fresh target");
            f.rejects(id, old, "answer", {{"text", "dev"}});
            f.rejects(id, connection, "reconcile");
            Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, restored), "answer", {{"text", "main"}});
            f.reach(id, "running");
            check(f.t3.replies == 1 && f.t3.seen.back().target.runId == "run-1" && f.t3.launches == 1 &&
                      f.logs(id, "session_question") == 1 && f.logs(id, "user_answer") == 1,
                  "recovered question did not resume the original Run exactly once");
        }
    }
}

void connectionRecoveryDiscardsExpiredSessionQuestion() {
    for (const bool replaced : {false, true}) {
        Fixture f;
        const auto id = f.submit();
        f.model.script = [](const ModelInput& i) {
            if (i.event.at("kind") == "intake") return work();
            if (i.event.at("question").at("requestId") == "replacement")
                return Decision{"ask_user", {{"question", "Which new branch?"}}, "Confirm the new question"};
            return ask();
        };
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

void connectionRecoveryReconsidersNewDirections() {
    Fixture f;
    const auto id = f.submit("Check main initially");
    f.model.script = [&](const ModelInput& input) {
        if (input.event.at("kind") == "intake") return work();
        f.t3.runs.at("run-1") = {.kind = Observation::Kind::Unavailable, .status = "auth"};
        return reply();
    };
    f.reach(id, "running");
    f.t3.question("run-1");
    f.wait(id, "auth");
    check(f.t3.replies == 0, "the original answer escaped before recovery");

    f.model.script = [&](const ModelInput& input) {
        if (input.event.at("kind") == "conversation_turn") return converse(input, "send_to_task", id, Json::object());
        check(input.task.at("userInstructions").dump().find("Use dev") != std::string::npos, "question inference lost the new direction");
        auto answer = reply();
        answer.args["answers"]["branch"] = "dev";
        return answer;
    };
    Conversations(*f.store).receive(localOwner, {"alpha", "", "e", "p"}, "new-direction-during-outage", "Use dev");
    f.runtime->runConversations();
    f.restart();
    f.t3.expectedBranch = "dev";
    f.t3.question("run-1");
    f.runtime->tick();
    f.reach(id, "running");
    check(f.t3.replies == 1 && f.t3.seen.back().answers.at("branch") == "dev", "connection recovery applied the old Session answer");
}

void connectionRecoveryRestoresQuestionModelApproval() {
    Fixture f;
    const auto id = Tasks(*f.store).submit({"alpha", "Check main", "e", "p", {{"modelCallLimit", 1}}});
    f.model.script = [](const ModelInput& i) { return i.event.at("kind") == "intake" ? work() : reply(); };
    f.reach(id, "running");
    f.t3.question("run-1");
    const auto old = f.wait(id, "model_budget");
    for (const auto* outage : {"auth", "settings"}) {
        f.t3.runs.at("run-1") = {.kind = Observation::Kind::Unavailable, .status = outage};
        f.runtime->tick();
        f.wait(id, outage);
    }
    f.restart();
    f.t3.question("run-1");
    f.runtime->tick();
    const auto restored = f.wait(id, "model_budget");
    check(restored != old && f.model.calls == 1, "recovery bypassed the pending model approval");
    f.rejects(id, old, "approve");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, restored), "approve");
    f.reach(id, "running");
    check(f.model.calls == 2 && f.t3.replies == 1, "restored approval could not resume the open question");
}

void failedPreflightIsUnsent() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput&) { return work(); };
    f.t3.onPreflight = [] { throw ModelError("Missing T3 settings", false); };
    const auto wait = f.wait(id, "settings");
    check(f.t3.seen.empty() && f.logs(id, "send_started") == 0, "failed preflight started a delivery");
    f.restart();
    f.t3.onPreflight = {};
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile");
    f.reach(id, "running");
    check(f.t3.launches == 1 && f.t3.reconciles == 0 && f.logs(id, "send_started") == 1,
          "preflight failure did not leave one unsent action");
}

void recoveredDelivery() {
    for (const bool launch : {true, false}) {
        Fixture f;
        const auto id = f.submit();
        f.model.script = [](const ModelInput& i) {
            if (i.event["kind"] == "intake") return work();
            if (i.task["additionalUsed"] == 0) return work("follow_up", false);
            return complete();
        };
        if (!launch) {
            f.reach(id, "running");
            f.t3.finish("run-1");
        }
        f.reach(id, "sending");
        f.restart();
        f.t3.loseAcknowledgement = true;
        auto waiting = f.wait(id, "unknown_delivery");
        f.restart();
        f.t3.generation = "fresh-oauth-session";
        f.t3.hasReconciledRun = false;
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, waiting), "reconcile");
        waiting = f.wait(id, "unknown_delivery");
        check(f.t3.seen.size() == (launch ? 1ul : 2ul), "unmatched delivery was resent");
        f.t3.hasReconciledRun = true;
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, waiting), "reconcile");
        f.reach(id, "running");
        check(f.t3.reconciles == 2 && f.t3.recovery.back().authGeneration == "same-oauth-session",
              "recovery lost original authentication generation");
        f.restart();
        if (launch) {
            f.t3.finish("run-1");
            f.reach(id, "deciding");
            f.reach(id, "running");
        }
        f.t3.finish("run-2");
        f.reach(id, "completed");
        f.runtime->tick();
        check(f.t3.launches == 1 && f.t3.follows == 1 && f.logs(id, "report") == 2, "duplicate Run or report");
    }
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) { return i.event["kind"] == "intake" ? work() : reply(); };
    f.reach(id, "running");
    f.t3.question("run-1");
    f.reach(id, "sending");
    f.t3.loseAcknowledgement = true;
    const auto wait = f.wait(id, "unknown_delivery");
    f.restart();
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile");
    f.reach(id, "running");
    check(f.t3.replies == 1 && f.t3.launches == 1 && f.t3.reconciles == 1 && f.task(id)["additionalUsed"] == 0,
          "recovered answer created duplicate work");
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
    f.t3.hasReconciledRun = false;
    const auto wait = f.wait(id, "unknown_delivery");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile");
    f.t3.hasReconciledRun = true;
    f.reach(id, "running");
    check(f.t3.recovery.back().launchReceiptThreadId == "thread-1" && !f.t3.recovery.back().candidate && f.t3.launches == 1,
          "reconciliation lost the launch receipt or sent another launch");
}

void unsentAnswerRemainsRetryable() {
    Fixture f;
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) { return i.event["kind"] == "intake" ? work() : reply(); };
    f.reach(id, "running");
    const auto run = f.task(id).at("activeRun");
    f.t3.question("run-1");
    f.reach(id, "sending");
    f.t3.failNextAnswerRead = true;
    const auto wait = f.wait(id, "unknown_delivery");
    const auto answer = f.t3.seen.back();
    check(f.t3.replies == 0 && f.task(id)["wait"]["text"].get<std::string>().find("was not sent") != std::string::npos,
          "the user was not told the Session answer was unsent");

    f.restart();
    f.rejects(id, wait, "reconcile", {{"questionId", "another-question"}});
    check(f.task(id).at("wait").at("id") == wait && f.t3.replies == 0,
          "a mismatched reconciliation hint allowed an unsent answer to proceed");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "reconcile",
                            {{"questionId", answer.questionId}});
    f.reach(id, "running");
    const auto retry = f.t3.seen.back();
    check(f.t3.replies == 1 && f.t3.launches == 1 && f.t3.reconciles == 0 && f.task(id)["activeRun"] == run &&
              f.logs(id, "session_answered") == 1 && retry.actionId == answer.actionId && retry.questionId == answer.questionId,
          "the unsent Session answer was not retried once for the same Run and question");
}

void cancelDuringDelivery() {
    Fixture f;
    Store cli(f.directory / "state.db");
    const auto independent = f.submit("Other project work", "beta", "other");
    f.model.script = [](const ModelInput&) { return work(); };
    f.reach(independent, "running");
    const auto id = f.submit("Work being cancelled");
    const auto queued = f.submit("Next project task");
    f.model.script = [=](const ModelInput& i) { return i.task["id"] == queued ? ask() : work(); };
    f.t3.beforeWorkAcknowledgement = [&] { Tasks(cli).operate(localOwner, TaskTarget::from(Tasks(cli).task(id).task, 0), "cancel"); };
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
    f.wait(queued, "user_question");
    check(f.t3.launches == 2 && f.logs(id, "report") == 1, "settling cancelled delivery duplicated a Run or its result");
}

void settleWhileAnswerDeliveryIsUnknown() {
    for (const bool cancel : {false, true}) {
        Fixture f;
        const auto id = f.submit();
        f.model.script = [](const ModelInput& i) {
            if (i.event["kind"] == "intake") return work();
            if (i.event["kind"] == "question") return reply();
            return ask();
        };
        f.reach(id, "running");
        f.t3.question("run-1");
        f.t3.loseAcknowledgement = true;
        const auto wait = f.wait(id, "unknown_delivery");
        f.t3.question("run-1");
        f.restart();

        if (cancel) {
            Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
            for (int i = 0; i < 12 && f.t3.interruptedRuns.empty(); ++i) f.runtime->tick();
            check(f.task(id)["state"] == "cancelling" && f.t3.interruptedRuns.size() == 1 &&
                      f.t3.interruptedRuns[0].threadId == "thread-1" && f.t3.interruptedRuns[0].runId == "run-1",
                  "unknown answer delivery blocked interruption of the known Run");
        }
        f.t3.finish("run-1", cancel ? "interrupted" : "completed");
        if (!cancel) {
            for (int i = 0; i < 12 && f.task(id)["wait"]["id"] == wait; ++i) f.runtime->tick();
            const auto review = f.wait(id, "user_question");
            f.rejects(id, wait, "reconcile");
            Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, review), "cancel");
        }
        f.reach(id, "cancelled");
        check(f.logs(id, "report") == 1 && f.t3.replies == 1 && f.t3.launches == 1, "settled Run duplicated its answer or report");
    }
}

void settledReportCanRecoverAfterRestart() {
    for (const bool available : {false, true}) {
        Fixture f;
        const std::string secret = "private-report-diagnostic";
        Policy policy{*f.store};
        policy.submitSecret(localOwner, "alpha", policy.requestSecret("alpha", {"diagnostic"}), secret);
        const auto id = f.submit();
        f.model.script = [&](const ModelInput& i) {
            if (i.event.at("kind") == "intake") return work();
            check(i.event.at("kind") == "result" && i.event.at("resultRunId") == "run-1-result" && i.event.at("status") == "completed",
                  "report recovery lost the terminal result identity");
            check(hasTool(i, "complete") == available, "completion did not require the settled Run's report");
            if (available) {
                check(i.event.at("report") == "Recovered report", "recovered report did not reach the Bot");
                return complete();
            }
            check(i.event.at("evidenceStatus") == "report_unavailable" && i.event.at("detail") == "[redacted]" &&
                      i.event.at("report") == "",
                  "the missing report or redacted diagnostic was misrepresented");
            return ask();
        };
        f.reach(id, "running");
        f.t3.finish("run-1");
        auto& observation = f.t3.runs.at("run-1");
        observation.evidence = RunEvidence::Availability::ReportUnavailable;
        observation.report.clear();
        observation.itemId.clear();
        observation.detail = secret;
        f.runtime->tick();
        check(f.task(id)["activeRun"].is_null() && f.task(id)["lastRun"]["runId"] == "run-1" &&
                  Tasks(*f.store).sessionTasks("alpha", 0, 1).at(0).runs.at(0).status == "completed" &&
                  Tasks(*f.store).history(id).dump().find(secret) == std::string::npos,
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
        check(f.model.calls == 1 && attempts == 0, "recovery spent model allowance or retried before the scheduled time");
        f.now += 5;
        f.runtime->tick();
        if (!available) {
            f.now += 10;
            f.runtime->tick();
            f.wait(id, "user_question");
        } else
            f.reach(id, "completed");
        check(attempts == (available ? 1 : 2) && f.t3.polls == polls && f.logs(id, "report") == 1 &&
                  f.logs(id, "completed") == int(available),
              "evidence retry repeated Run observation or published an unsupported completion");
    }
}

void lateReportCanRestoreCompletion() {
    Fixture f;
    const auto id = f.submit();
    int reportReads = 0;
    bool reportAvailable = false;
    f.t3.recoverEvidence = [&](const RunRef& source, std::string_view status) {
        check(source.threadId == "thread-1" && source.runId == "run-1-result" && status == "completed",
              "report recovery used a different Run");
        ++reportReads;
        if (reportAvailable) return RunEvidence{RunEvidence::Availability::Available, "Late report", "late-item", "", false};
        return RunEvidence{RunEvidence::Availability::ReportUnavailable, "", "", "Unavailable", false};
    };
    Id resumedEvent = 0;
    f.model.script = [&](const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "intake") return work();
        check(event.at("resultRunId") == "run-1-result", "lost the settled result Run");
        if (event.value("report", "") == "Late report") {
            check(hasTool(input, "complete") && !hasTool(input, "session_read") && !event.contains("evidenceStatus") &&
                      event.at("itemId") == "late-item" && event.at("id") == resumedEvent && event.at("userAnswer") == "Try again",
                  "late recovery changed the result event or failed to restore completion");
            return complete();
        }
        check(!hasTool(input, "complete") && hasTool(input, "session_read") && event.at("sessionReadViews") == Json::array({"report"}),
              "missing report was not recoverable in default mode");
        if (!input.evidence.empty()) {
            check(input.evidence.at(0).at("reportAvailable") == false && input.task.at("remainingEvidenceReads") == 2,
                  "failed report read did not use the event allowance");
            return ask();
        }
        auto run = "run-1-result";
        if (event.contains("userAnswer")) {
            resumedEvent = event.at("id");
            if (event.at("decisionFeedback").empty()) run = "run-1";
        }
        return Decision{"session_read", {{"view", "report"}, {"threadId", "thread-1"}, {"runId", run}}, "Retry the terminal report"};
    };
    f.reach(id, "running");
    f.t3.finish("run-1");
    f.t3.runs.at("run-1").evidence = RunEvidence::Availability::ReportUnavailable;
    f.runtime->tick();
    f.now += 5;
    f.runtime->tick();
    f.now += 10;
    f.runtime->tick();
    check(reportReads == 2 && f.logs(id, "report") == 1, "scheduled retries did not exhaust before recovery");
    const auto wait = f.wait(id, "user_question");
    check(reportReads == 3, "initial on-demand retry was not attempted exactly once");
    const auto polls = f.t3.polls;
    reportAvailable = true;
    f.restart();
    f.runtime->tick();
    check(reportReads == 3 && f.task(id).at("wait").at("id") == wait, "waiting repeated terminal report reads");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, wait), "answer", {{"text", "Try again"}});
    f.runtime->tick();
    check(reportReads == 3 && f.logs(id, "tool_error") == 1, "the assigned Run was accepted as the result source");
    setSessionReadJournalFailure(f.store->path(), true);
    bool storageFailure = false;
    try {
        f.runtime->tick();
    } catch (const std::runtime_error& error) {
        storageFailure = std::string_view(error.what()) == "injected journal failure";
    }
    check(storageFailure && Tasks(*f.store).history(id).dump().find("Late report") == std::string::npos && f.logs(id, "session_read") == 1,
          "a failed evidence transaction left a partially recovered report");
    setSessionReadJournalFailure(f.store->path(), false);
    f.runtime->tick();
    f.restart();
    f.reach(id, "completed");
    const auto history = Tasks(*f.store).history(id);
    const auto report = std::ranges::find_if(history, [](const Json& entry) { return entry.at("kind") == "report"; });
    const auto completed = std::ranges::find_if(history, [](const Json& entry) { return entry.at("kind") == "completed"; });
    check(report != history.end() && report->at("data").at("report") == "Late report" && completed != history.end() &&
              completed->at("data").at("source").at("itemId") == "late-item" && reportReads == 5 && f.t3.reads == 0 &&
              f.t3.polls == polls && f.t3.launches == 1 && f.t3.follows == 0 && f.logs(id, "report") == 1,
          "late recovery did not persist the report and its source on the same task");
}

void lateReportCanRestorePeerResult() {
    Fixture f;
    f.allowBeta();
    const auto sender = f.submit();
    int reportReads = 0;
    f.t3.recoverEvidence = [&](const RunRef& source, std::string_view) {
        check(source.threadId == "thread-1" && source.runId == "run-1-result", "peer recovery lost the result source");
        if (++reportReads <= 2) return RunEvidence{RunEvidence::Availability::ReportUnavailable};
        return RunEvidence{RunEvidence::Availability::Available, "Peer report", "peer-item", "", false};
    };
    f.model.script = [&](const ModelInput& input) {
        const auto& event = input.event;
        if (input.task.at("id") == sender) {
            if (event.at("kind") == "intake") return botRequest("beta", "work", "Verify the release");
            check(event.at("kind") == "bot_result" && event.at("references").at(0).at("itemId") == "peer-item",
                  "the peer result referenced stale evidence");
            return complete();
        }
        if (event.at("kind") == "bot_request") return work();
        const auto& kinds = input.task.at("peer").at("replyKinds");
        const bool hasResult = std::ranges::find(kinds, "result") != kinds.end();
        if (event.at("report") == "Peer report") {
            check(hasResult, "recovered evidence did not permit a peer result");
            return botReply("result", "Verified the release");
        }
        check(!hasResult && hasTool(input, "session_read"), "missing report did not gate the peer result");
        return Decision{
            "session_read", {{"view", "report"}, {"threadId", "thread-1"}, {"runId", "run-1-result"}}, "Retry the peer's terminal report"};
    };
    f.reach(sender, "peer_waiting");
    const Id recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    f.t3.finish("run-1");
    f.t3.runs.at("run-1").evidence = RunEvidence::Availability::ReportUnavailable;
    f.runtime->tick();
    f.now += 5;
    f.runtime->tick();
    f.now += 10;
    f.runtime->tick();
    f.reach(sender, "completed");
    check(f.task(recipient).at("state") == "completed" && reportReads == 3 && f.t3.launches == 1 && f.t3.follows == 0,
          "a late report did not close the original peer exchange");
}

void missingReportDoesNotBlockCancellation() {
    for (const bool settledBeforeCancel : {false, true}) {
        Fixture f;
        const auto id = f.submit();
        const auto next = f.submit("Next task");
        f.model.script = [=](const ModelInput& i) { return i.task.at("id") == id ? work() : ask(); };
        f.reach(id, "running");
        f.t3.finish("run-1", "interrupted");
        auto& observation = f.t3.runs.at("run-1");
        observation.evidence = RunEvidence::Availability::ReportUnavailable;
        observation.report.clear();
        if (settledBeforeCancel) f.runtime->tick();
        f.restart();
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, 0), "cancel");
        f.reach(id, "cancelled");
        f.wait(next, "user_question");
        check(f.t3.interrupts == 0 && f.t3.launches == 1 && f.logs(id, "report") == 1 && f.task(id)["activeRun"].is_null(),
              "missing report held cancellation or the next task's project slot");
    }
}

void evidenceResume() {
    Fixture f;
    f.read = "on_demand";
    f.restart();
    const auto id = f.submit();
    f.model.script = [](const ModelInput& i) {
        if (i.event["kind"] == "intake") return work();
        if (i.evidence.size() == 2) return complete();
        check(std::find(i.tools.begin(), i.tools.end(), "session_read") != i.tools.end(), "read tool not presented");
        auto req = Json{{"threadId", i.event["run"]["threadId"]}, {"runId", i.event["run"]["runId"]}, {"view", "activity"}};
        if (!i.evidence.empty()) req["cursor"] = i.evidence.back()["nextCursor"];
        return Decision{"session_read", req, {}};
    };
    f.reach(id, "running");
    f.t3.finish("run-1");
    f.reach(id, "deciding");
    f.runtime->tick();
    check(f.t3.reads == 1 && f.task(id)["additionalUsed"] == 0 && f.logs(id, "decision") == 1 && f.task(id)["state"] == "deciding",
          "read committed a terminal action");
    f.restart();
    f.reach(id, "completed");
    check(f.t3.reads == 2 && f.t3.launches == 1 && f.t3.follows == 0 && f.logs(id, "session_read") == 2,
          "history read did not resume its budget");
}

void redactedHistoryPagination() {
    Fixture f;
    f.read = "on_demand";
    f.restart();
    const std::string secret = "fixture-history-secret";
    Policy policy{*f.store};
    policy.submitSecret({"fixture", true}, "alpha", policy.requestSecret("alpha", {"history-token"}), secret);
    f.t3.historyPages = {
        {"Token: " + secret, {}, {}, "history-item", "7", 64, true},
        {"Verified " + secret, {}, {}, "history-item", {}, 0, false},
    };
    f.model.script = [&](const ModelInput& input) {
        if (input.event.at("kind") == "intake") return work();
        const auto& run = input.event.at("run");
        Json ref = {{"threadId", run.at("threadId")}, {"runId", run.at("runId")}};
        if (input.evidence.empty()) return Decision{"session_read", ref, "Read the first page"};
        const auto& first = input.evidence.front();
        check(first.at("text") == "Token: [redacted]" && first.at("more") == true && first.at("itemTruncated") == true &&
                  first.at("nextCursor") == "7" && first.at("nextOffset") == 64,
              "redaction lost the adapter's continuation");
        if (input.evidence.size() == 1) {
            ref["itemId"] = first.at("itemId");
            ref["offset"] = first.at("nextOffset");
            return Decision{"session_read", ref, "Read the final page"};
        }
        const auto& last = input.evidence.back();
        check(last.at("text") == "Verified [redacted]" && last.at("more") == false && last.at("itemTruncated") == false &&
                  last.at("nextCursor") == "" && last.at("nextOffset") == 0,
              "redaction made a complete excerpt appear truncated");
        return complete();
    };
    const auto id = f.submit();
    f.reach(id, "running");
    f.t3.finish("run-1");
    f.reach(id, "completed");
    check(f.t3.readRequests.size() == 2 && f.t3.readRequests.back().offset == 64, "history did not resume at the adapter's offset");
}

void taskPagesShowProjectOwners() {
    Fixture f;
    f.model.script = [](const ModelInput&) { return work(); };
    const auto desktop = f.submit("Verify main", "beta");
    f.reach(desktop, "running");
    const auto cloud = Tasks(*f.store).submit({"beta", "Check cloud", "cloud", "p"});
    const auto desktopNext = f.submit("Desktop follow-up");
    const auto independent = f.submit("Separate release", "alpha", "another-project");
    const auto cancelled = f.submit("Withdrawn release");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(cancelled).task, 0), "cancel");
    const auto cloudNext = Tasks(*f.store).submit({"alpha", "Cloud follow-up", "cloud", "p"});
    f.restart();

    const auto newest = taskJson(Tasks(*f.store).tasks("alpha", 0, 2));
    const Json cloudOwner = {{"id", cloud}, {"botId", "beta"}, {"request", "Check cloud"}, {"state", "queued"}};
    check(newest.size() == 2 && newest[0].at("id") == cloudNext && newest[0].at("blockedBy") == cloudOwner &&
              newest[1].at("id") == cancelled && newest[1].at("state") == "cancelled" && newest[1].at("blockedBy").is_null(),
          "the newest page lost the off-page cloud owner or blocked cancelled work");
    const auto older = taskJson(Tasks(*f.store).tasks("alpha", cancelled, 2));
    const Json desktopOwner = {{"id", desktop}, {"botId", "beta"}, {"request", "Verify main"}, {"state", "running"}};
    check(older.size() == 2 && older[0].at("id") == independent && older[0].at("blockedBy").is_null() && older[1].at("id") == desktopNext &&
              older[1].at("blockedBy") == desktopOwner && Tasks(*f.store).tasks("alpha", desktopNext, 2).empty(),
          "the older page skipped a task, repeated the cursor or mixed up project owners");
    const auto active = Tasks(*f.store).activeTasks("alpha");
    check(active.size() == 3 && active.front().task.id == cloudNext && active.front().blockedBy && active.front().blockedBy->id == cloud &&
              active.front().blockedBy->botId == "beta" && active.front().blockedBy->request == "Check cloud" &&
              active.front().blockedBy->state == TaskState::queued && active.back().task.id == desktopNext && active.back().blockedBy &&
              active.back().blockedBy->id == desktop && active.back().blockedBy->botId == "beta" &&
              active.back().blockedBy->request == "Verify main" && active.back().blockedBy->state == TaskState::running,
          "the active task list lost the owners of work in separate environments");

    const auto history = taskJson(Tasks(*f.store).tasks({}, cloud, 1));
    check(history.size() == 1 && history[0].at("id") == desktop && history[0].at("blockedBy").is_null() &&
              history[0].at("runs").size() == 1 && history[0].at("runs")[0].at("link") == "https://t3.test/thread" &&
              history[0].at("revisions").size() == 1 && history[0].at("revisions")[0].at("doneWhen") == Evidence &&
              history[0].at("runs")[0].at("revisionId") == history[0].at("revisions")[0].at("id"),
          "the unfiltered task page lost the running owner's Session link or revision after restart");
}

void peerWorkAndRestart() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Decide whether the PR can ship");
    const auto unrelated = f.submit("Ordinary request queued before the handoff");
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto taskId = input.task["id"].get<Id>();
        if (taskId == unrelated) return ask();
        if (taskId == root) {
            if (event["kind"] == "intake") {
                check(hasTool(input, "send_bot_message"), "configured peer was unavailable");
                return botRequest("beta", "work", "Verify the PR before release");
            }
            if (event["kind"] == "bot_question") {
                check(event["fromBotId"] == "beta" && event["text"] == "Which branch?", "peer question arrived without its sender or text");
                if (!event.contains("userAnswer")) return ask();
                check(event["userAnswer"] == "main", "the human answer did not return to the original Bot question");
                return botReply("answer", "Use main");
            }
            check(event["kind"] == "bot_result" && event["text"] == "Verified the PR on main" && input.task["doneWhen"] == Evidence,
                  "peer result did not resume the original work contract");
            if (input.evidence.empty()) {
                check(hasTool(input, "read_bot_evidence"), "peer result evidence could not be inspected");
                return {"read_bot_evidence", {{"messageId", event["messageId"]}}, "Verify the specialist's evidence"};
            }
            check(input.evidence.back()["text"] == "commit wanted, updates enabled", "owned peer Session evidence was not returned");
            return complete();
        }
        check(input.task["botId"] == "beta" && input.notes.soul == "Second SOUL" && input.notes.memory == "BETA_ONLY",
              "work did not run as the recipient Bot");
        if (event["kind"] == "bot_request") {
            check(event["fromBotId"] == "alpha" && event["text"] == "Verify the PR before release" &&
                      input.task["request"] == "Verify the PR before release" &&
                      input.history.dump().find("ALPHA_ONLY") == std::string::npos,
                  "peer intake lost its sender or inherited the sender's memory");
            return botReply("question", "Which branch?");
        }
        if (event["kind"] == "bot_answer") {
            check(event["text"] == "Use main", "clarification did not resume the recipient's task");
            return work();
        }
        check(event["kind"] == "result", "recipient did not receive its T3 result");
        return botReply("result", "Verified the PR on main");
    };

    f.reach(root, "peer_waiting");
    check(f.task(root)["wait"].is_null() && f.task(root)["doneWhen"] == Evidence,
          "waiting for a peer became a human wait or lost the completion contract");
    f.restart();
    const auto recipientTasks = taskJson(Tasks(*f.store).tasks("beta"));
    check(recipientTasks.size() == 1, "handoff was not persisted exactly once");
    const auto recipient = recipientTasks.front()["id"].get<Id>();
    check(f.task(recipient)["blockedBy"].is_null(), "recipient was shown as blocked despite having the execution slot");
    const auto humanWait = f.wait(root, "user_question");
    check(f.task(recipient)["state"] == "peer_waiting" && f.task(unrelated)["state"] == "queued" && f.t3.launches == 0,
          "clarification started unrelated work or launched a Session prematurely");
    f.restart();
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(root).task, humanWait), "answer", {{"text", "main"}});
    f.rejects(root, humanWait, "answer", {{"text", "dev"}});
    f.reach(recipient, "running");
    check(f.t3.launches == 1 && f.t3.seen.back().projectId == "p" && f.t3.seen.back().config.at("model") == "beta-model" &&
              f.task(unrelated)["state"] == "queued" && f.task(root)["state"] == "peer_waiting",
          "recipient did not inherit the project execution slot using its own configuration");

    f.restart();
    f.t3.finish("run-1");
    f.reach(recipient, "completed");
    f.restart();
    for (int step = 0; step < 8 && f.t3.reads == 0; ++step) f.runtime->tick();
    check(f.t3.reads == 1 && f.logs(root, "completed") == 0, "reading evidence did not leave a result to evaluate");
    const auto& read = f.t3.readRequests.back();
    check(read.environmentId == "e" && read.threadId == "thread-1" && read.runId == "run-1-result" &&
              read.config == f.task(root).at("config"),
          "peer evidence did not use the reading Bot's settings");
    f.restart();
    f.reach(root, "completed");
    f.wait(unrelated, "user_question");
    check(Tasks(*f.store).tasks("beta").size() == 1 && f.t3.launches == 1 && f.t3.reads == 1 && f.task(root)["additionalUsed"] == 0 &&
              f.logs(root, "user_answer") == 1 && f.logs(root, "completed") == 1,
          "restart duplicated the recipient, Session, evidence read, user answer or completion");
    const auto result = f.botMessages("alpha").back();
    const auto completion = lastTaskEvent(*f.store, root, "completed").at("data");
    check(!completion.contains("source") && completion.at("dependency") == Json{{"exchangeId", result.at("exchangeId")},
                                                                                {"messageId", result.at("messageId")},
                                                                                {"taskId", recipient},
                                                                                {"botId", "beta"},
                                                                                {"requestKind", "work"},
                                                                                {"status", "completed"}},
          "the completion did not retain the peer result as its dependency");
    check(completion.at("evidenceRefs") == Json::array({{{"environmentId", "e"},
                                                         {"threadId", "thread-1"},
                                                         {"runId", "run-1-result"},
                                                         {"itemId", "final-answer"},
                                                         {"kind", "peer_report"},
                                                         {"messageId", result.at("messageId")}},
                                                        {{"environmentId", "e"},
                                                         {"threadId", "thread-1"},
                                                         {"runId", "run-1-result"},
                                                         {"itemId", "item-1"},
                                                         {"kind", "session_read"},
                                                         {"messageId", result.at("messageId")}}}),
          "the completion lost the scoped references for the peer report and history item");
    const struct Message {
        const char* type;
        const char* from;
        const char* to;
        const char* text;
    } expected[] = {
        {"request", "alpha", "beta", "Verify the PR before release"},
        {"question", "beta", "alpha", "Which branch?"},
        {"answer", "alpha", "beta", "Use main"},
        {"result", "beta", "alpha", "Verified the PR on main"},
    };
    for (const auto bot : {"alpha", "beta"}) {
        const auto messages = f.botMessages(bot);
        check(messages.size() == std::size(expected), "conversation lost or duplicated a message after restart");
        const auto exchange = messages.front().at("exchangeId").get<std::string>();
        check(!exchange.empty(), "conversation lost its exchange identity");
        std::set<std::string> ids;
        for (std::size_t i = 0; i < messages.size(); ++i) {
            const auto& message = messages[i];
            const auto& want = expected[i];
            const auto id = message.at("messageId").get<std::string>();
            check(message.at("messageType") == want.type && message.at("fromBotId") == want.from && message.at("toBotId") == want.to &&
                      message.at("text") == want.text,
                  "conversation showed the wrong message or participants");
            check(!id.empty() && ids.insert(id).second && message.at("exchangeId") == exchange,
                  "conversation lost the identity of a reply or exchange");
            if (i > 0) check(message.at("replyTo") == messages[i - 1].at("messageId"), "conversation broke the reply chain");
        }
        check(messages.back().at("status") == "completed", "conversation lost the successful outcome");
    }
}

void peerWorkSharesBudgetWithDirectT3Work() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Verify and report the PR");
    f.model.usage = {{"total_tokens", 37}};
    int peerReplies = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] != root) {
            ++peerReplies;
            return botReply("result", "Verified the requested pass");
        }
        if (input.event["kind"] == "bot_result" && peerReplies == 4) return work("delegate", false);
        Json args = {{"botId", "beta"}, {"kind", "work"}, {"text", "Verify the PR"}};
        if (input.event["kind"] == "intake") args["doneWhen"] = Evidence;
        return {"send_bot_message", std::move(args), "Use the configured verifier"};
    };

    for (int step = 0; step < 64 && f.task(root)["state"] != "waiting"; ++step) f.runtime->tick();
    const auto budget = f.wait(root, "budget");
    const auto beforeApproval = f.task(root);
    check(beforeApproval["wait"]["attempted"] == "delegate" && beforeApproval["remaining"] == 0 && beforeApproval["additionalUsed"] == 3 &&
              f.t3.launches == 0 && Tasks(*f.store).tasks("beta").size() == 4,
          "direct T3 work bypassed the allowance exhausted by peer work");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(root).task, budget), "approve");
    f.restart();
    f.reach(root, "running");
    check(f.t3.launches == 1 && f.task(root)["remaining"] == 2 && f.task(root)["additionalUsed"] == 4,
          "the direct T3 action did not spend one round from the renewed allowance");
    f.t3.finish("run-1");
    for (int step = 0; step < 64 && f.task(root)["state"] != "waiting"; ++step) f.runtime->tick();
    const auto secondBudget = f.wait(root, "budget");
    const auto paused = f.task(root);
    check(secondBudget != budget && paused["remaining"] == 0 && paused["additionalUsed"] == 6 && Tasks(*f.store).tasks("beta").size() == 6,
          "T3 and peer work did not share the renewed allowance");
    const auto& wait = paused.at("wait");
    check(wait.at("attempted") == "send_bot_message" && wait.at("decisionReason") == "Use the configured verifier" &&
              wait.at("usage") == Json{{"total_tokens", 37}},
          "the peer budget wait lost the proposed action or its usage");
}

void peerCompletionRespectsSessionReadPolicy() {
    Fixture f;
    f.allowBeta();
    const auto sender = f.submit("Decide whether the PR can ship");
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto kind = input.event.at("kind");
        if (input.task.at("id") == sender) {
            if (kind == "intake") return botRequest("beta", "work", "Verify the PR");
            return complete();
        }
        if (kind == "bot_request") return work();
        return botReply("result", "Verified the PR");
    };
    f.reach(sender, "peer_waiting");
    const Id recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    Policy{*f.store}.changeNeverRule({"fixture", true}, Action::session_read, Resource::sessionRead("e", "thread-1").target, true);
    f.t3.finish("run-1");
    f.reach(recipient, "completed");
    f.restart();
    f.reach(sender, "completed");
    const auto completion = lastTaskEvent(*f.store, sender, "completed").at("data");
    check(completion.at("dependency").at("taskId") == recipient && completion.at("evidenceRefs").empty() && !completion.contains("source"),
          "completion exposed peer Run provenance despite a Never rule");
}

void checkPeerClarificationAfterRun(std::string_view readMode) {
    Fixture f;
    f.read = readMode;
    f.restart();
    f.allowBeta();
    const auto root = f.submit("Verify the release");
    bool requestedClarificationRead = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        if (input.task["id"] == root) {
            if (event["kind"] == "intake") return botRequest("beta", "work", "Verify the release");
            if (event["kind"] == "bot_question") {
                check(event["text"] == "Which branch should I retest?", "wrong question reached Release");
                if (!event.contains("userAnswer")) return ask();
                check(event["userAnswer"] == "main", "wrong human answer reached Release");
                return botReply("answer", "Retest main");
            }
            check(event["kind"] == "bot_result" && event["text"] == "Retested main" && event["references"][0]["runId"] == "run-2-result",
                  "Release lost the final evidence");
            if (input.evidence.empty()) return {"read_bot_evidence", {{"messageId", event["messageId"]}}, "Inspect the retest report"};
            check(input.evidence.back()["runId"] == "run-2-result", "Release read a different Run's evidence");
            return complete();
        }
        if (event["kind"] == "bot_request") return work();
        if (event["kind"] == "bot_answer") {
            const auto& report = event.at("sessionResult");
            check(event["text"] == "Retest main" && report["run"]["runId"] == "run-1" && report["resultRunId"] == "run-1-result",
                  "Verifier lost the clarification or its session context");
            check(hasTool(input, "session_read") == (readMode == "on_demand"), "history read ignored the selected mode");
            if (readMode == "on_demand" && input.evidence.empty()) {
                requestedClarificationRead = true;
                return {"session_read",
                        {{"threadId", report["run"]["threadId"]}, {"runId", report["resultRunId"]}},
                        "Inspect the earlier verification"};
            }
            if (readMode == "on_demand") check(requestedClarificationRead, "on-demand reading exposed evidence without a request");
            if (readMode != "off")
                check(!input.evidence.empty() && input.evidence.back()["runId"] == (readMode == "prefill" ? "run-1" : "run-1-result"),
                      "Verifier lost the read excerpt");
            check(hasTool(input, "follow_up"), "Verifier lost the settled Session");
            return {"follow_up", {{"instruction", "Retest main"}}, "Continue the existing verification Session"};
        }
        check(event["kind"] == "result", "unexpected event in Verifier");
        if (event["run"]["runId"] == "run-1") return botReply("question", "Which branch should I retest?");
        return botReply("result", "Retested main");
    };

    f.reach(root, "peer_waiting");
    const auto recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    const auto initialRun = f.task(recipient)["activeRun"];
    f.t3.finish("run-1");
    const auto waitId = f.wait(root, "user_question");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(root).task, waitId), "answer", {{"text", "main"}});
    const auto answerWasDelivered = [&] {
        const auto messages = f.botMessages("beta");
        return !messages.empty() && messages.back().at("messageType") == "answer" && messages.back().at("text") == "Retest main";
    };
    for (int step = 0; step < 12 && !answerWasDelivered(); ++step) f.runtime->tick();
    check(answerWasDelivered(), "recipient conversation did not show the clarification before restart");
    f.restart();
    const auto readsBeforeAnswer = f.t3.reads;
    f.reach(recipient, "running");
    if (readMode == "off")
        check(f.t3.reads == readsBeforeAnswer, "the Bot answer read T3 despite session history being off");
    else
        check(f.t3.reads == readsBeforeAnswer + 1 && f.t3.readRequests.back().runId == (readMode == "prefill" ? "run-1" : "run-1-result"),
              "the Bot answer did not reopen the earlier verification record");

    const auto continuation = f.t3.seen.back();
    check(f.t3.launches == 1 && f.t3.follows == 1 && continuation.target.threadId == initialRun["threadId"].get<std::string>() &&
              continuation.target.runId == initialRun["runId"].get<std::string>() &&
              continuation.instruction.find("Retest main") != std::string::npos && f.task(recipient)["additionalUsed"] == 1,
          "the retest did not continue the same settled T3 Session");
    f.t3.finish("run-2");
    f.reach(root, "completed");
    check(f.logs(recipient, "report") == 2 && f.logs(root, "completed") == 1 && f.botMessages("alpha").size() == 4,
          "the settled verification result was not delivered exactly once");
}

void peerClarificationAfterRun() {
    for (const auto mode : {"on_demand", "prefill", "off"}) checkPeerClarificationAfterRun(mode);
}

void revisionWaitsForPeerClarification(bool direct = false) {
    Fixture f;
    f.allowBeta();
    const Intake routing{"alpha", "Release check", "e", "p", {{"conversationEnvironments", {{"cloud", "cloud"}}}}};
    const auto root = Tasks(*f.store).submit(routing);
    bool answeredPeer = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") {
            if (input.event.at("text") == "Use main")
                return converse(input, "answer_task_question", root,
                                {{"waitId", input.event.at("replyTo").at("waitId")}, {"text", input.event.at("text")}});
            return converse(input, "change_task", root, {{"environment", "cloud"}});
        }
        if (input.task.at("botId") == "beta") {
            if (kind == "bot_request") return botReply("question", "Which branch?");
            check(kind == "bot_answer" && input.event.at("text") == "Use main", "The recipient did not receive its clarification");
            return botReply("result", "Verified main");
        }
        if (kind == "bot_question") {
            if (!input.event.contains("userAnswer")) return ask();
            check(f.task(root).at("environmentId") == "e" && f.t3.launches == 0, "Replanning ran before the Bot exchange finished");
            check(input.event.at("userAnswer") == "Use main", "The peer question lost the human answer");
            answeredPeer = true;
            return botReply("answer", input.event.at("userAnswer").get_ref<const std::string&>());
        }
        check(kind == "intake", "Unexpected event for the sender");
        return answeredPeer ? work() : botRequest("beta", "consult", "Check the release");
    };

    const auto oldWait = f.wait(root, "user_question");
    const auto old = TaskTarget::from(Tasks(*f.store).task(root).task, oldWait);
    Conversations(*f.store).receive(localOwner, routing, "move-release-check", "Run Release check on cloud");
    f.runtime->runConversations();
    check(f.task(root).value("replanning", false) && f.task(root).at("pendingDestination").at("environmentId") == "cloud",
          "The requested destination change did not reach the task");
    try {
        Tasks(*f.store).operate(localOwner, old, "answer", {{"text", "Use an old branch"}});
        throw std::runtime_error("The old peer question accepted an answer");
    } catch (const RequestChanged&) {
    }
    const auto pending = Inbox(*f.store).inbox(localOwner, "alpha");
    check(pending.size() == 1 && pending.at(0).at("itemKind") == "user_question" && pending.at(0).at("waitId") != old.waitId,
          "Replanning stranded the peer question");
    const auto& current = pending.at(0);
    if (direct) {
        Inbox(*f.store).decideInbox(localOwner,
                                    {"alpha", InboxKind::user_question, current.at("itemId"), current.at("waitId"), root,
                                     current.at("targetRevision"), current.at("targetGeneration")},
                                    "answer", {{"text", "Use main"}});
    } else {
        Conversations(*f.store).receive(localOwner, routing, "branch-answer", "Use main", f.task(root).at("wait").at("messageId"));
        f.runtime->runConversations();
    }
    check(f.logs(root, "user_answer") == 1, "The conversation rejected the answer the peer is waiting for");
    f.reach(root, "running");
    const auto messages = f.botMessages("alpha");
    check(answeredPeer && messages.size() == 4 && messages.back().at("messageType") == "result" && f.t3.launches == 1 &&
              f.t3.seen.front().environmentId == "cloud",
          "The task did not finish the Bot exchange before launching once in the new environment");
}

void peerCompletionAfterEnvironmentMove() {
    for (const auto [kind, environment] : {std::pair{"work", "cloud"}, {"consult", "cloud"}, {"work", "e"}}) {
        Fixture f;
        f.allowBeta();
        const Intake routing{"alpha", "Release check", "e", "p", {{"conversationEnvironments", {{"cloud", "cloud"}}}}};
        const auto sender = Tasks(*f.store).submit(routing);
        f.model.script = [&](const ModelInput& input) -> Decision {
            const auto& event = input.event;
            if (event.at("kind") == "conversation_turn") return converse(input, "change_task", sender, {{"environment", "cloud"}});
            if (input.task.at("botId") == "beta")
                return {"reply_to_bot", {{"kind", "result"}, {"text", event.at("text")}}, "Return the requested result"};
            if (event.at("kind") == "intake") {
                if (!input.task.contains("userInstructions")) return botRequest("beta", "work", "Original result");
                check(f.task(sender).at("environmentId") == "cloud" && !hasTool(input, "complete"),
                      "the pre-move result unlocked completion after the move");
                return {"send_bot_message", {{"botId", "beta"}, {"kind", kind}, {"text", "Cloud result"}}, "Verify the moved task"};
            }
            if (std::string_view(environment) != "cloud") {
                check(!hasTool(input, "complete"), "a peer on the old route unlocked completion");
                return ask();
            }
            check(event.at("kind") == "bot_result" && event.at("text") == "Cloud result" && hasTool(input, "complete"),
                  "the current peer result did not unlock completion");
            const auto& move = input.task.at("userInstructions").at(0);
            return {"complete",
                    {{"report", "Cloud result"},
                     {"acknowledgedInstructions", Json::array({{{"messageId", move.at("messageId")}, {"step", move.at("step")}}})}},
                    "Verified after the move"};
        };
        f.reach(sender, "peer_waiting");
        f.peers.at("alpha").front().intake.environmentId = environment;
        Conversations(*f.store).receive(localOwner, routing, "move-release-check", "Move Release check to cloud");
        f.runtime->runConversations();
        if (std::string_view(environment) != "cloud") {
            f.wait(sender, "user_question");
            continue;
        }
        f.reach(sender, "completed");
        const auto task = f.task(sender);
        check(task.at("revisions").size() == 1 && !task.contains("newSessionRequired") && !task.contains("pendingInstructions") &&
                  f.t3.launches == 0 && f.logs(sender, "completed") == 1,
              "peer completion left the moved task unfinished");
    }
}

void recipientRevisionContinuesTheOpenWorkExchange() {
    Fixture f;
    f.allowBeta();
    const auto sender = f.submit("Release verification");
    Id recipient = 0;
    bool attemptedFailure = false;
    constexpr auto revisedCriteria = "Confirm the rollback plan with the revised evidence";
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto kind = event.at("kind").get<std::string>();
        if (kind == "conversation_turn" && event["text"] == "How is verification going?")
            return {"reply", {{"text", "Checking."}}, "Report progress"};
        if (kind == "conversation_turn") return converse(input, "change_task", recipient, {{"doneWhen", revisedCriteria}});
        if (input.task["id"] == sender) {
            if (kind == "intake") return botRequest("beta", "work", "Verify release");
            check(kind == "bot_result" && event["text"] == "Verified under revised criteria" && event["references"].size() == 1 &&
                      event["references"][0]["runId"] == "run-2-result",
                  "the sender resumed from old or unrelated peer evidence");
            return complete();
        }
        if (kind == "bot_request") return work();
        if (input.task.contains("userInstructions") && kind == "result") {
            if (!std::exchange(attemptedFailure, true)) {
                Conversations(*f.store).receive(localOwner, {"beta", "", "e", "p", Json::object()}, "status", "How is verification going?");
                return botReply("failed", "Unable to verify the original criteria");
            }
            return work("follow_up", false);
        }
        check(kind == "result" && input.task["doneWhen"] == revisedCriteria && event["run"]["runId"] == "run-2",
              "the old Run result was exposed as the receiving Bot's revised evidence");
        return botReply("result", "Verified under revised criteria");
    };

    f.reach(sender, "peer_waiting");
    recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    const std::string revisionText = "Revise Verify release completion criteria to " + std::string(revisedCriteria);
    const Intake betaRouting{"beta", revisionText, "e", "p", {{"runTimeoutSeconds", 10}, {"model", "beta-model"}}};
    Conversations(*f.store).receive(localOwner, betaRouting, "recipient-revision", revisionText);
    f.runtime->runConversations();
    check(f.task(recipient).value("replanning", false) && f.task(recipient)["doneWhen"] == revisedCriteria &&
              f.task(sender)["state"] == "peer_waiting" && f.botMessages("alpha").size() == 1,
          "the recipient revision did not remain in the existing work exchange");

    f.restart();
    for (int step = 0; step < 4; ++step) f.runtime->tick();
    check(f.t3.seen.size() == 1 && f.task(recipient)["activeRun"]["runId"] == "run-1" && f.task(sender)["state"] == "peer_waiting" &&
              f.botMessages("alpha").size() == 1,
          "restart dispatched revised work or returned old evidence before the prior Run settled");
    f.t3.finish("run-1", "completed", false);
    for (int step = 0; step < 4; ++step) f.runtime->tick();
    check(f.t3.seen.size() == 1 && f.task(recipient)["activeRun"]["runId"] == "run-1" && f.task(sender)["state"] == "peer_waiting" &&
              f.botMessages("alpha").size() == 1,
          "revised work started before the old Run was settled");

    f.t3.finish("run-1");
    for (int step = 0; step < 12 && !attemptedFailure; ++step) f.runtime->tick();
    check(attemptedFailure && !f.task(recipient).contains("pendingDecision") && f.logs(recipient, "tool_error") == 1 &&
              !f.logs(recipient, "peer_completed"),
          "failed reply bypassed the outstanding revision before deferral");
    for (int step = 0; step < 24 && f.t3.follows == 0; ++step) f.runtime->tick();
    check(f.t3.follows == 1 && f.t3.seen.size() == 2 && f.t3.seen.back().tool == CommandTool::follow_up &&
              f.t3.seen.back().target.runId == "run-1" && f.t3.seen.back().instruction.find(revisedCriteria) != std::string::npos &&
              f.task(recipient)["activeRun"]["runId"] == "run-2" && f.botMessages("alpha").size() == 1,
          "settling the old Run did not continue the same exchange under revised criteria");

    f.t3.finish("run-2");
    f.reach(recipient, "completed");
    f.reach(sender, "completed");
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 2 && messages[1]["messageType"] == "result" && messages[1]["text"] == "Verified under revised criteria" &&
              messages[1]["evidence"][0]["runId"] == "run-2-result" && messages[0]["exchangeId"] == messages[1]["exchangeId"] &&
              f.botMessages("beta").size() == 2 && f.logs(recipient, "report") == 2 && f.logs(recipient, "peer_completed") == 1 &&
              f.logs(sender, "completed") == 1 && f.t3.launches == 1 && f.t3.follows == 1,
          "the revised result did not close the exchange once and resume its sender");
}

void checkRecipientRevisionDuringPeerQuestion(bool moves) {
    Fixture f;
    f.peers["alpha"] = {{"Verifier", {"beta", "", "e", "beta-project", {{"runTimeoutSeconds", 10}, {"model", "beta-model"}}}}};
    const auto sender = f.submit("Verify the release");
    Id recipient = 0;
    constexpr auto revisedCriteria = "Confirm the rollback plan with the revised evidence";
    bool attemptedInvalidDecision = false;
    bool sawAnswer = false;
    bool offeredFollowUp = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto kind = event.at("kind").get<std::string>();
        if (kind == "conversation_turn") {
            Json fields = {{"doneWhen", revisedCriteria}};
            if (moves) fields["environment"] = "cloud";
            return converse(input, "change_task", recipient, fields);
        }
        if (input.task["id"] == sender) {
            if (kind == "intake") return botRequest("beta", "work", "Verify the release");
            if (kind == "bot_question") {
                if (!event.contains("userAnswer")) return ask();
                check(event["userAnswer"] == "Use main", "the sender lost the human's answer to its question");
                return botReply("answer", "Use main");
            }
            check(kind == "bot_result" && event["text"] == "Retested main under revised criteria" && event["references"].size() == 1 &&
                      event["references"][0]["runId"] == "run-2-result",
                  "the sender resumed from the wrong peer evidence");
            return complete();
        }
        if (kind == "bot_request") return work();
        if (kind == "result") {
            if (event["run"]["runId"] == "run-1") return botReply("question", "Which branch?");
            check(event["run"]["runId"] == "run-2" && input.task["doneWhen"] == revisedCriteria,
                  "the revised result did not use the new completion criteria");
            return botReply("result", "Retested main under revised criteria");
        }
        check(kind == "bot_answer" && event["text"] == "Use main" && event["sessionResult"]["run"]["runId"] == "run-1" &&
                  input.task["doneWhen"] == revisedCriteria &&
                  input.task.at("userInstructions").front().at("text").get<std::string>().find(revisedCriteria) != std::string::npos,
              "the sender answer or revised instructions did not reach the recipient model");
        offeredFollowUp = hasTool(input, "follow_up");
        if (!attemptedInvalidDecision) {
            attemptedInvalidDecision = true;
            return moves ? work("follow_up", false) : botReply("result", "Result from the old criteria");
        }
        sawAnswer = true;
        if (moves) return {"delegate", {{"instruction", "Repeat verification"}}, "Start a fresh Session in cloud"};
        return {"follow_up",
                {{"instruction", "Retest main after the sender answered: Use main"}},
                "Continue under the revised completion criteria"};
    };

    f.reach(sender, "peer_waiting");
    recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    f.t3.finish("run-1");
    f.t3.runs.at("run-1").report = "Previous verification found one failing check";
    const auto waitId = f.wait(sender, "user_question");
    check(f.task(recipient)["state"] == "peer_waiting" && f.botMessages("beta").size() == 2 &&
              f.botMessages("beta").back()["messageType"] == "question",
          "the recipient did not leave its settled Run in a peer question");

    const std::string revisionText =
        "Revise Verify the release completion criteria to " + std::string(revisedCriteria) + (moves ? " and move it to cloud" : "");
    const Intake betaRouting{"beta",
                             revisionText,
                             "e",
                             "beta-project",
                             {{"runTimeoutSeconds", 10}, {"model", "beta-model"}, {"conversationEnvironments", {{"cloud", "cloud"}}}}};
    Conversations(*f.store).receive(localOwner, betaRouting, "recipient-question-revision", revisionText);
    f.runtime->runConversations();
    for (int step = 0; step < 3; ++step) f.runtime->tick();
    check(f.task(recipient).value("replanning", false) && f.task(recipient)["state"] == "peer_waiting" &&
              f.task(recipient)["doneWhen"] == revisedCriteria && f.task(sender)["wait"]["reason"] == "user_question",
          "the recipient replanned during the pending peer question");

    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(sender).task, waitId), "answer", {{"text", "Use main"}});
    f.reach(recipient, "running");
    const auto command = f.t3.seen.back();
    if (moves) {
        check(f.t3.launches == 2 && f.t3.follows == 0 && command.tool == CommandTool::delegate && command.environmentId == "cloud" &&
                  command.target.threadId.empty() && command.target.runId.empty() &&
                  command.instruction.find("Verify the release") != std::string::npos &&
                  command.instruction.find("Previous verification found one failing check") != std::string::npos,
              "the fresh Session received the old target or lost the task context");
    } else {
        check(f.t3.launches == 1 && f.t3.follows == 1 && command.tool == CommandTool::follow_up && command.target.runId == "run-1",
              "the same-environment revision lost its existing Session");
    }
    check(attemptedInvalidDecision && sawAnswer && offeredFollowUp == !moves && f.t3.seen.size() == 2 &&
              command.instruction.find("Use main") != std::string::npos && command.instruction.find(revisedCriteria) != std::string::npos &&
              f.botMessages("alpha").size() == 3 && f.botMessages("beta").back()["messageType"] == "answer" &&
              f.task(sender)["wait"].is_null() && f.task(recipient)["wait"].is_null(),
          "the answer did not reach revised work or an old-evidence result closed the exchange");

    f.t3.finish("run-2");
    f.reach(recipient, "completed");
    f.reach(sender, "completed");
    const auto alphaMessages = f.botMessages("alpha");
    const auto betaMessages = f.botMessages("beta");
    check(alphaMessages.size() == 4 && betaMessages.size() == 4 && alphaMessages.back()["messageType"] == "result" &&
              alphaMessages.back()["text"] == "Retested main under revised criteria" &&
              alphaMessages.back()["evidence"][0]["environmentId"] == (moves ? "cloud" : "e") &&
              alphaMessages.back()["evidence"][0]["runId"] == "run-2-result" && f.logs(recipient, "peer_completed") == 1 &&
              f.logs(sender, "completed") == 1 && f.logs(recipient, "tool_error") == 1 && f.logs(sender, "model_failure") == 0 &&
              f.task(sender)["wait"].is_null() && f.task(recipient)["wait"].is_null(),
          "the revised result did not close the same exchange once without model-failure waits");
}

void recipientRevisionDuringPeerQuestionPreservesTheAnswer() {
    checkRecipientRevisionDuringPeerQuestion(false);
}
void recipientMoveDuringPeerQuestionStartsAFreshSession() {
    checkRecipientRevisionDuringPeerQuestion(true);
}

void checkWorkRecipientMove(bool sharesSenderQueue) {
    Fixture f;
    const std::string project = sharesSenderQueue ? "p" : "beta-project";
    f.t3.projectsByEnvironment["e"] = {{project, "Destination project", {}}};
    executeSql(f.directory / "state.db", sharesSenderQueue ? "INSERT INTO bot_t3_projects VALUES('beta','e','p')"
                                                           : "INSERT INTO bot_t3_projects VALUES('beta','e','beta-project')");
    const Intake betaRouting{
        "beta", "Verify the release", "cloud", "cloud-beta-project", {{"conversationEnvironments", {{"desktop", "e"}}}}};
    f.peers["alpha"] = {{"Verifier", betaRouting}};
    const auto sender = f.submit("Release check");
    Id recipient = 0, unrelated = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") return converse(input, "change_task", recipient, {{"environment", "desktop"}});
        if (input.task["id"] == sender) {
            if (kind == "intake") return botRequest("beta", "work", "Verify the release");
            check(kind == "bot_result" && input.event["references"][0]["environmentId"] == "e",
                  "the sender did not receive evidence from the destination");
            return complete();
        }
        if (input.task["id"] == unrelated) return kind == "intake" ? work() : complete();
        if (kind == "bot_request") return work();
        if (kind == "result" && input.task.contains("userInstructions")) return work("delegate", false);
        check(kind == "result" && input.event["environmentId"] == "e", "the recipient judged the earlier Run");
        return botReply("result", "Verification complete");
    };

    f.reach(sender, "peer_waiting");
    recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    unrelated = f.submit("Unrelated verification", "alpha", project);
    if (!sharesSenderQueue) f.reach(unrelated, "running");
    const auto launchesBeforeMove = f.t3.launches;
    Conversations(*f.store).receive(localOwner, betaRouting, "recipient-move", "Move Verify the release to desktop");
    f.runtime->runConversations();
    check(f.task(recipient).value("replanning", false) && f.task(recipient)["pendingDestination"]["environmentId"] == "e",
          "the destination change did not reach the task");
    f.t3.finish("run-1");

    if (!sharesSenderQueue) {
        for (int step = 0; step < 5; ++step) f.runtime->tick();
        check(f.task(recipient)["state"] == "queued" && f.task(recipient)["environmentId"] == "e" &&
                  f.task(recipient)["blockedBy"]["id"] == unrelated && f.t3.launches == launchesBeforeMove &&
                  f.task(unrelated)["state"] == "running",
              "the move bypassed an unrelated destination owner");
        f.t3.finish("run-2");
        f.reach(unrelated, "completed");
    }
    for (int step = 0; step < 12 && f.t3.launches == launchesBeforeMove; ++step) f.runtime->tick();
    f.reach(recipient, "running");
    check(f.t3.launches == launchesBeforeMove + 1 && f.t3.follows == 0 && f.t3.seen.back().environmentId == "e" &&
              f.t3.seen.back().projectId == project && f.task(sender)["state"] == "peer_waiting" &&
              (!sharesSenderQueue || f.task(unrelated)["blockedBy"]["id"] == recipient),
          "the moved recipient did not inherit the exchange's queue position");
    f.t3.finish(f.task(recipient)["activeRun"]["runId"].get<std::string>());
    f.reach(recipient, "completed");
    f.reach(sender, "completed");
    check(f.logs(recipient, "peer_completed") == 1 && f.logs(sender, "completed") == 1,
          "the environment move did not finish the existing exchange once");
    if (sharesSenderQueue) f.reach(unrelated, "running");
}

void workRecipientMoveSharesSenderQueue() {
    checkWorkRecipientMove(true);
}
void workRecipientMoveWaitsForUnrelatedOwner() {
    checkWorkRecipientMove(false);
}

void consultationRepliesAfterUserInstructions() {
    for (const bool revise : {false, true}) {
        Fixture f;
        f.allowBeta();
        const auto sender = f.submit("Release consultation");
        Id recipient = 0;
        const std::string instruction = revise ? "Revise Check the release to inspect rollback policy"
                                               : "Also inspect rollback policy for Check the release\n" + std::string(7000, 'x') +
                                                     std::string(7000, '\n') + "exclude deprecated branches";
        f.model.script = [&](const ModelInput& input) -> Decision {
            const auto& event = input.event;
            const auto& kind = event.at("kind");
            if (kind == "conversation_turn") {
                if (revise) return converse(input, "change_task", recipient, {{"intent", "inspect rollback policy"}});
                return converse(input, "send_to_task", recipient, Json::object());
            }
            if (input.task["id"] == sender) {
                if (kind == "intake") return botRequest("beta", "consult", "Check the release");
                if (kind == "bot_question") {
                    if (!event.contains("userAnswer")) return ask();
                    return botReply("answer", "Use main");
                }
                check(kind == "bot_result" && event["text"] == "Rollback policy confirmed",
                      "the sender did not receive the consultation result");
                return complete();
            }
            if (kind == "bot_request") return botReply("question", "Which branch?");
            check(kind == "bot_answer" && event["text"] == "Use main" && !hasTool(input, "delegate") &&
                      input.task.at("userInstructions")
                              .front()
                              .at("text")
                              .get<std::string>()
                              .find(revise ? instruction : "exclude deprecated branches") != std::string::npos,
                  "the consultation lost the peer answer or the user instruction");
            return botReply("result", "Rollback policy confirmed");
        };

        f.reach(sender, "peer_waiting");
        recipient = Tasks(*f.store).tasks("beta").front().task.id;
        const auto wait = f.wait(sender, "user_question");
        Conversations(*f.store).receive(localOwner, {"beta", "", "e", "p", Json::object()}, "consultation-instruction", instruction);
        f.runtime->runConversations();
        check(f.logs(recipient, "instruction_accepted") == 1 && f.task(recipient).value("replanning", false) == revise,
              "the instruction did not reach the open consultation");
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(sender).task, wait), "answer", {{"text", "Use main"}});
        f.reach(recipient, "completed");
        f.reach(sender, "completed");
        check(f.t3.seen.empty() && f.logs(recipient, "model_failure") == 0 && f.botMessages("alpha").size() == 4,
              "the consultation did not close normally after the user instruction");
    }
}

void peerConsultationAndPermission() {
    Fixture f;
    f.allowBeta();
    const auto denied = f.submit("Consult a Bot outside the configured peers");
    int deniedAttempts = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        check(hasTool(input, "send_bot_message"), "permit validation was not tested with an available send tool");
        ++deniedAttempts;
        return botRequest("gamma", "consult", "What is the release policy?");
    };
    f.wait(denied, "model_failure");
    check(deniedAttempts == 6 && Tasks(*f.store).tasks().size() == 1 && f.botMessages("alpha").empty(),
          "an unlisted recipient received a message or the available send tool was not exercised");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(denied).task, 0), "cancel");
    f.reach(denied, "cancelled");

    f.peers.at("alpha").front().intake.projectId = "second";
    const auto busy = f.submit("Run a verification in the Verifier's project", "beta", "second");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == busy) return work();
        if (input.event["kind"] == "intake") return botRequest("beta", "consult", "What is the release policy?");
        if (input.event["kind"] == "bot_request") {
            check(!hasTool(input, "delegate") && !hasTool(input, "send_bot_message"),
                  "consultation inherited T3 work permissions or reverse peer permission");
            return botReply("result", "Require a verified PR");
        }
        check(input.event["kind"] == "bot_result" && input.event["text"] == "Require a verified PR",
              "consultation did not deliver the response in context");
        return complete();
    };
    f.reach(busy, "running");
    const auto consultation = f.submit("Ask the Verifier about our release policy");
    f.reach(consultation, "peer_waiting");
    const auto consulting = taskJson(Tasks(*f.store).tasks("beta", 0, 1));
    check(consulting.size() == 1 && consulting[0].at("request") == "What is the release policy?" &&
              consulting[0].at("state") == "deciding" && consulting[0].at("blockedBy").is_null(),
          "the consultation was shown as blocked by the Verifier's running task");
    f.reach(consultation, "completed");
    const auto messages = f.botMessages("alpha");
    check(f.t3.seen.size() == 1 && f.t3.interrupts == 0 && f.task(busy)["state"] == "running" &&
              f.task(consultation)["additionalUsed"] == 0 && Tasks(*f.store).tasks("beta").size() == 2,
          "consultation used T3, consumed work allowance or affected the Verifier's active Run");
    check(messages.size() == 2 && messages.front().at("text") == "What is the release policy?" &&
              messages.back().at("fromBotId") == "beta" && messages.back().at("text") == "Require a verified PR",
          "consultation did not record the corresponding request and response");
    const auto completion = lastTaskEvent(*f.store, consultation, "completed").at("data");
    check(completion.at("dependency").at("messageId") == messages.back().at("messageId") &&
              completion.at("dependency").at("requestKind") == "consult" && completion.at("evidenceRefs").empty() &&
              !completion.contains("source"),
          "the consultation lost its peer dependency or attributed the reply to a Session");
}

void consultationDoesNotRequireAT3Destination() {
    Fixture f;
    f.allowBeta();
    auto& peer = f.peers["alpha"].front().intake;
    peer.environmentId.clear();
    peer.projectId.clear();
    f.model.script = [](const ModelInput& input) {
        if (input.event["kind"] == "intake") return botRequest("beta", "consult", "What is the release policy?");
        if (input.event["kind"] == "bot_request") {
            check(!hasTool(input, "delegate"), "the consultation offered a T3 launch");
            return botReply("result", "Require a verified PR");
        }
        return complete();
    };
    const auto sender = f.submit();
    f.reach(sender, "completed");
    const auto tasks = taskJson(Tasks(*f.store).tasks("beta"));
    check(tasks.size() == 1 && tasks[0]["environmentId"] == "" && tasks[0]["projectId"] == "" && tasks[0]["state"] == "completed" &&
              f.t3.seen.empty(),
          "a consultation depended on T3 admission");
}

void automaticPeerWorkResolvesTheProject() {
    Fixture f(true);
    f.allowBeta();
    auto& peer = f.peers["alpha"].front().intake;
    peer.environmentId.clear();
    peer.projectId.clear();
    T3Connections connections(*f.store);
    connections.connect("e", localOwner.id, 4173);
    connections.connect("cloud", localOwner.id, 4173);
    f.t3.projectsByEnvironment["e"] = {{"verification", "Verification", {}}};
    f.model.script = [](const ModelInput& input) {
        if (input.event["kind"] == "intake") return botRequest("beta", "work", "Verify the release");
        return work();
    };
    const auto sender = f.submit();
    f.reach(sender, "peer_waiting");
    const auto recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    const auto task = f.task(recipient);
    check(task.at("environmentId") == "e" && task.at("projectId") == "verification", "peer work did not retain the selected destination");
    check(f.t3.seen.size() == 1 && f.t3.seen.back().environmentId == "e" && f.t3.seen.back().projectId == "verification",
          "an unpinned peer did not use the active project in the sender's eligible environment");
}

void requestedCriteriaApplyToFirstPeerWork() {
    Fixture f;
    f.allowBeta();
    const auto receipt = Conversations(*f.store).receive(localOwner, {"alpha", "", "e", "p"}, "work-with-criteria", "Check the release");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "conversation_turn") return converse(input, "start_task", 0, {{"doneWhen", "Confirm the signature"}});
        check(!input.needsDoneWhen && input.task.at("requestedDoneWhen") == "Confirm the signature",
              "peer work required the model to repeat the user's criterion");
        return {"send_bot_message", {{"kind", "work"}, {"text", "Verify the release"}, {"botId", "beta"}}, "Ask the verifier"};
    };
    f.runtime->runConversations();
    const Id sender = Tasks(*f.store).tasks("alpha").at(0).task.id;
    f.reach(sender, "peer_waiting");
    const auto task = f.task(sender);
    check(task.at("doneWhen") == "Confirm the signature" && task.at("remaining") == 3 &&
              task.at("revisions").at(0).at("sourceMessageId") == receipt.at("messageId"),
          "first peer work did not adopt the saved criterion and its source");
}

void modelBudgetFollowsBotConsultation() {
    Fixture f;
    f.allowBeta();
    constexpr auto request = "Check the release policy";
    const auto receipt =
        Conversations(*f.store).receive(localOwner, {"alpha", request, "e", "p", {{"modelCallLimit", 3}}}, "policy", request);
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event["kind"] == "conversation_turn") return converse(input, "start_task", 0, Json::object());
        if (input.event["kind"] == "intake") return botRequest("beta", "consult", "What is the release policy?");
        if (input.event["kind"] == "bot_request") {
            check(input.config["model"] == "beta-model", "recipient did not get its own model configuration");
            return botReply("result", "Require a verified PR");
        }
        check(input.event["kind"] == "bot_result", "sender did not resume with the consultation reply");
        return complete();
    };
    f.runtime->runConversations();
    const Id sender = Tasks(*f.store).tasks("alpha").front().task.id;
    check(f.task(sender)["sourceMessageId"] == receipt["messageId"], "routed task lost its originating message");
    f.reach(sender, "peer_waiting");
    const Id recipient = Tasks(*f.store).tasks("beta").front().task.id;
    const auto recipientWait = f.wait(recipient, "model_budget");
    f.restart();
    for (int i = 0; i < 4; ++i) f.runtime->tick();
    check(f.model.calls == 3 && f.task(recipient)["wait"]["id"] == recipientWait && f.task(recipient)["wait"]["allowance"] == 3,
          "the recipient reached the provider without approval, or restart changed its wait");
    Store writer(f.directory / "state.db");
    const Person verifier{"verifier", false};
    Policy{writer}.setMember(localOwner, "beta", verifier.id, true);
    executeSql(f.directory / "state.db", R"SQL(
INSERT INTO journal(time,kind,data,conversation_bot_id)
VALUES(0,'conversation_budget_wait','{}','alpha');
UPDATE conversation_turns SET status='paused',pause_id=last_insert_rowid()
WHERE message_id=(SELECT id FROM messages WHERE bot_id='alpha' ORDER BY id LIMIT 1);
)SQL");
    check(Conversations(writer).pausedConversations("alpha").size() == 1, "the shared conversation fixture is not paused");
    Tasks(writer).operate(verifier, TaskTarget::from(Tasks(writer).task(recipient).task, recipientWait), "approve");
    check(Conversations(writer).pausedConversations("alpha").size() == 1,
          "a member of the receiving Bot resumed a conversation on the originating Bot");
    f.rejects(recipient, recipientWait, "approve");
    f.reach(sender, "completed");
    check(f.model.calls == 5 && f.task(recipient)["state"] == "completed" && f.t3.launches == 0 && f.task(sender)["remaining"] == 3,
          "the recipient and sender did not finish under one renewal of their shared model allowance");
}

void peerResultWaitsForRecipientTurn() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Check the release");
    int recipientDecisions = 0;
    bool noted = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        const auto& kind = input.event.at("kind");
        if (kind == "conversation_turn") return {"reply", {{"text", "Noted."}}, "Acknowledge the note"};
        if (input.task.at("botId") == "beta") {
            ++recipientDecisions;
            if (!std::exchange(noted, true))
                Conversations(*f.store).receive(localOwner, {"beta", "", "e", "p", Json::object()}, "beta-note",
                                                "How is the release check going?");
            return botReply("result", "Verified");
        }
        if (kind == "bot_result") return complete();
        return botRequest("beta", "consult", "Check the release");
    };
    f.reach(root, "peer_waiting");
    for (int i = 0; i < 4 && !noted; ++i) f.runtime->tick();
    const Id recipient = Tasks(*f.store).tasks("beta").front().task.id;
    check(noted && f.task(recipient)["state"] != "completed" && f.task(root)["state"] == "peer_waiting",
          "the recipient closed the exchange while its own conversation turn was unfinished");
    f.reach(root, "completed");
    check(recipientDecisions == 1 && f.task(recipient)["state"] == "completed", "the held peer result asked the model again or was lost");
}

void cancelledPeerRefundsUnusedModelApproval() {
    Fixture f;
    f.allowBeta();
    constexpr auto request = "Check the release policy";
    Conversations(*f.store).receive(localOwner, {"alpha", request, "e", "p", {{"modelCallLimit", 3}}}, "policy", request);
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event["kind"] == "conversation_turn") return converse(input, "start_task", 0, Json::object());
        if (input.event["kind"] == "intake") return botRequest("beta", "consult", "What is the release policy?");
        if (input.event["kind"] == "bot_request") throw std::runtime_error("Cancelled recipient reached the provider");
        check(input.event["kind"] == "bot_result", "sender did not resume on peer cancellation");
        return ask();
    };
    f.runtime->runConversations();
    const Id sender = Tasks(*f.store).tasks("alpha").front().task.id;
    f.reach(sender, "peer_waiting");
    const Id recipient = Tasks(*f.store).tasks("beta").front().task.id;
    const auto recipientWait = f.wait(recipient, "model_budget");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(recipient).task, recipientWait), "approve");
    check(f.model.calls == 3, "shared approval was not reserved");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(recipient).task, 0), "cancel");
    f.reach(recipient, "cancelled");
    f.reach(sender, "waiting");
    const auto wait = f.task(sender).at("wait").at("reason");
    check(wait == "user_question" && f.model.calls == 4,
          "cancelled recipient lost the unused approved attempt from the sender's shared budget");
}

void revisedTaskRefundsUnusedModelApproval() {
    Fixture f;
    const auto taskId = Tasks(*f.store).submit({"alpha", "Original criteria", "e", "p", {{"modelCallLimit", 1}}});
    bool first = true;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event.at("kind") == "conversation_turn")
            return converse(input, "change_task", taskId, {{"intent", "Use revised criteria"}});
        if (std::exchange(first, false)) throw ModelError("Fixture failure", true);
        return work();
    };
    f.reach(taskId, "deciding");
    f.runtime->tick();
    f.now += 5;
    const auto approval = f.wait(taskId, "model_budget");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(taskId).task, approval), "approve");
    check(f.model.calls == 1, "provider already reached the new approval");
    Conversations(*f.store).receive(localOwner, {"alpha", "", "e", "p", Json::object()}, "revise", "Revise the original request.");
    f.runtime->runConversations();
    check(f.task(taskId).value("replanning", false), "conversation did not revise the task");
    f.reach(taskId, "running");
    check(f.model.calls == 4 && f.t3.launches == 1, "replanning the task consumed the approval without a call");
}

void modelBudgetCountsFailedAttempts() {
    Fixture f;
    const auto id = Tasks(*f.store).submit({"alpha", "Retry a decision", "e", "p", {{"modelCallLimit", 2}}});
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] != id || f.model.calls >= 4) return ask();
        if (f.model.calls == 1) throw ModelError("Transient failure with usage", true, {{"input_tokens", 42}});
        throw ModelError("Transient failure without usage", true);
    };
    f.reach(id, "deciding");
    f.runtime->tick();
    f.now += 5;
    f.runtime->tick();
    f.now += 5;
    const auto wait = f.wait(id, "model_budget");
    f.restart();
    f.runtime->tick();
    check(f.model.calls == 2 && f.task(id)["wait"]["id"] == wait, "the third failed retry reached the model");

    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(f.store->path().c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    check(opened == SQLITE_OK, "could not read the attempt ledger");
    std::vector<Json> usage;
    const auto result = sqlite3_exec(
        db.get(), "SELECT coalesce(usage,'null') FROM model_attempts ORDER BY id",
        [](void* context, int, char** values, char**) {
            static_cast<std::vector<Json>*>(context)->push_back(Json::parse(values[0], nullptr, false));
            return 0;
        },
        &usage, nullptr);
    check(result == SQLITE_OK && usage == std::vector<Json>{{{"input_tokens", 42}}, nullptr},
          "usage was lost or missing usage was counted as zero");

    const auto independent = Tasks(*f.store).submit({"alpha", "Independent request", "e", "other", {{"modelCallLimit", 2}}});
    f.wait(independent, "user_question");
    check(f.model.calls == 3 && f.task(id)["wait"]["id"] == wait, "an independent request needed the paused budget");
    const auto approval = TaskTarget::from(Tasks(*f.store).task(id).task, wait);
    check(f.task(id)["wait"]["allowance"] == 2, "the pause did not show the original allowance");
    Tasks(*f.store).operate(localOwner, approval, "approve");
    f.restart();
    f.rejects(id, wait, "approve");
    for (const auto* answer : {"Use main", "Yes, use main"}) {
        const auto question = f.wait(id, "user_question");
        Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, question), "answer", {{"text", answer}});
    }
    const auto next = f.wait(id, "model_budget");
    check(next != wait && f.model.calls == 5 && f.task(id)["remaining"] == 3,
          "one approval did not restore exactly the original allowance after restart");
    f.rejects(id, wait, "approve");
}

void taskInferenceSurvivesWriterContention(bool modelFails, bool restart = false) {
    Fixture f;
    const auto taskId = Tasks(*f.store).submit({"alpha", "Check the fixture", "e", "p", {{"modelCallLimit", 1}}});
    sqlite3* connection = nullptr;
    const auto opened = sqlite3_open((f.directory / "state.db").c_str(), &connection);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(connection, sqlite3_close);
    check(opened == SQLITE_OK, "Could not open the fixture writer");
    f.model.usage = {{"input_tokens", 7}, {"input_tokens_details", {{"cached_tokens", 3}}}};
    f.model.script = [&](const ModelInput&) {
        check(sqlite3_exec(writer.get(), "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK,
              "Could not acquire the fixture write lock");
        if (modelFails) throw ModelError("Temporary fixture failure", true, f.model.usage);
        return work();
    };
    f.reach(taskId, "deciding");
    bool contended = false;
    try {
        f.runtime->tick();
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()) == "database is locked", "Unexpected Task checkpoint error");
        contended = true;
    }
    check(sqlite3_exec(writer.get(), "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK && contended,
          "The model checkpoint did not contend with the fixture writer");

    if (restart) f.restart();
    if (modelFails) {
        f.runtime->tick();
        const auto task = f.task(taskId);
        check(f.model.calls == 1 && f.t3.launches == 0 && f.logs(taskId, "model_failure") == 1 && task.at("state") == "deciding" &&
                  task.at("feedback") == "Temporary fixture failure",
              "The Task lost the model failure or skipped its retry backoff");
    } else {
        f.reach(taskId, "running");
        check(f.model.calls == 1 && f.t3.launches == 1 && f.logs(taskId, "decision") == 1,
              "The Task repeated inference or lost the decision after the writer released its lock");
    }
    sqlite3_stmt* statement = nullptr;
    check(sqlite3_prepare_v2(writer.get(),
                             "SELECT json_extract(usage,'$.input_tokens'),"
                             "json_extract(usage,'$.input_tokens_details.cached_tokens') FROM model_attempts WHERE task_id=?",
                             -1, &statement, nullptr) == SQLITE_OK,
          "Could not read the fixture model usage");
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
    check(sqlite3_bind_int64(row.get(), 1, taskId) == SQLITE_OK && sqlite3_step(row.get()) == SQLITE_ROW &&
              sqlite3_column_int(row.get(), 0) == 7 && sqlite3_column_int(row.get(), 1) == 3 && sqlite3_step(row.get()) == SQLITE_DONE,
          "The Task did not save the original model attempt's usage");
}

void modelBudgetAllowsT3ObservationAndRecovery() {
    Fixture f;
    f.model.script = [](const ModelInput& input) { return input.event["kind"] == "intake" ? work() : reply(); };
    const auto id = Tasks(*f.store).submit({"alpha", "Use one model call", "e", "p", {{"modelCallLimit", 1}}});
    f.reach(id, "sending");
    f.t3.loseAcknowledgement = true;
    const auto deliveryWait = f.wait(id, "unknown_delivery");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, deliveryWait), "reconcile");
    f.reach(id, "running");
    check(f.model.calls == 1 && f.t3.reconciles == 1 && f.t3.launches == 1, "delivery recovery used a model call or resent the Run");
    f.t3.question("run-1");
    const auto modelWait = f.wait(id, "model_budget");
    const auto polls = f.t3.polls;
    f.restart();
    f.runtime->tick();
    check(f.t3.polls > polls && f.model.calls == 1 && f.task(id)["wait"]["id"] == modelWait,
          "model approval blocked T3 observation or reset the pending decision");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(id).task, modelWait), "approve");
    f.reach(id, "running");
    check(f.model.calls == 2 && f.t3.replies == 1 && f.task(id)["remaining"] == 3,
          "the approved call did not answer the original question");
}

void unsuccessfulConsultationsDoNotComplete() {
    for (const bool cancel : {false, true}) {
        Fixture f;
        f.allowBeta();
        const auto id = f.submit("Consult before deciding whether to release");
        f.model.script = [&](const ModelInput& input) -> Decision {
            if (input.event["kind"] == "intake") return botRequest("beta", "consult", "Can we release?");
            if (input.event["kind"] == "bot_request") return botReply("failed", "Required evidence unavailable");
            check(input.event["kind"] == "bot_result" && input.event["status"] == (cancel ? "cancelled" : "failed") &&
                      !hasTool(input, "complete") && input.task["doneWhen"] == "",
                  "unsuccessful initial consultation unlocked completion");
            return ask();
        };
        f.reach(id, "peer_waiting");
        if (cancel)
            Tasks(*f.store).operate(
                localOwner, TaskTarget::from(Tasks(*f.store).task(Tasks(*f.store).tasks("beta").front().task.id).task, 0), "cancel");
        f.wait(id, "user_question");
        check(f.logs(id, "completed") == 0 && f.t3.launches == 0, "unsuccessful consultation completed or launched work");
    }
}

void concurrentPeerHandoff() {
    Fixture f;
    f.allowBeta();
    const auto source = f.submit("Confirm the release");
    const auto next = f.submit("Next in the project queue");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == next) return ask();
        if (input.task["id"] == source) {
            if (input.event["kind"] == "intake") return botRequest("beta", "work", "Verify the release");
            check(input.event["kind"] == "bot_result" && input.event["status"] == "completed",
                  "concurrent handoff lost the recipient result");
            return complete();
        }
        if (input.event["kind"] == "bot_request") return work();
        check(input.event["kind"] == "result", "recipient missed its Session result");
        return botReply("result", "Verified the release");
    };
    const auto until = [&](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            f.runtime->tickConcurrent();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(ready(), "concurrent handoff did not advance before its deadline");
    };
    until([&] {
        const auto recipients = taskJson(Tasks(*f.store).tasks("beta"));
        return recipients.size() == 1 && recipients.front()["state"] == "running";
    });
    const auto recipient = Tasks(*f.store).tasks("beta").front().task.id;
    check(f.task(source)["state"] == "peer_waiting" && f.task(next)["blockedBy"]["id"] == recipient,
          "the queued Task did not report the active peer as the owner");
    const auto page = taskJson(Tasks(*f.store).tasks("alpha", 0, 1));
    const Json owner = {{"id", recipient}, {"botId", "beta"}, {"request", "Verify the release"}, {"state", "running"}};
    check(page.size() == 1 && page[0].at("id") == next && page[0].at("blockedBy") == owner,
          "the queued task page lost the work recipient that inherited the sender's slot");
    f.runtime.reset();
    f.t3.finish("run-1");
    f.restart();
    until([&] { return f.task(next)["state"] == "waiting"; });
    f.runtime.reset();
    check(f.task(source)["state"] == "completed" && f.task(recipient)["state"] == "completed" &&
              f.task(next)["wait"]["reason"] == "user_question" && f.t3.launches == 1 && f.t3.follows == 0 &&
              f.botMessages("alpha").size() == 2,
          "the concurrent handoff skipped or duplicated the source, recipient, or next Task");
}

void consultationRestoresOwnEvidence() {
    for (const auto mode : {"on_demand", "prefill", "off", "prefill-unavailable"}) {
        Fixture f;
        const bool unavailable = std::string_view(mode) == "prefill-unavailable";
        f.read = unavailable ? "prefill" : mode;
        if (unavailable)
            f.t3.beforeRead = [&] {
                if (f.t3.reads == 2) throw std::runtime_error("Fixture T3 history unavailable after the consultation");
            };
        f.allowBeta();
        f.restart();
        const auto source = f.submit("Verify with a Session and consult");
        f.model.script = [&](const ModelInput& input) -> Decision {
            if (input.event["kind"] == "intake") return work();
            if (input.event["kind"] == "result") return botRequest("beta", "consult", "Confirm policy");
            if (input.event["kind"] == "bot_request") return botReply("result", "Policy confirmed");
            check(input.event["kind"] == "bot_result" && input.event["sessionResult"]["resultRunId"] == "run-1-result",
                  "the consultation result omitted the source Bot's settled report");
            check(hasTool(input, "session_read") == (f.read == "on_demand") && !hasTool(input, "read_bot_evidence"),
                  "the source Bot's history ignored its selected read mode");
            if (f.read == "on_demand" && input.evidence.empty())
                return {"session_read", {{"threadId", "thread-1"}, {"runId", "run-1-result"}}, "Read original evidence"};
            check(input.evidence.size() == (f.read == "off" || unavailable ? 0 : 1),
                  "own Session evidence did not match its selected mode");
            return complete();
        };
        f.reach(source, "running");
        f.t3.finish("run-1");
        f.reach(source, "completed");
        const auto expected = f.read == "off" ? 0 : f.read == "prefill" ? 2 : 1;
        check(f.t3.reads == expected && f.t3.launches == 1, "the consultation changed read or Run counts");
    }
}

void freshResultSurvivesUnavailableHistory() {
    Fixture f;
    f.read = "prefill";
    f.restart();
    f.t3.beforeRead = [] { throw std::runtime_error("Fixture T3 history unavailable"); };
    const auto task = f.submit("Verify fresh evidence");
    f.model.script = [&](const ModelInput& input) {
        if (input.event["kind"] == "intake") return work();
        check(input.event.at("historyUnavailable") == true && input.evidence.empty(), "missing history was not visible");
        return complete();
    };
    f.reach(task, "running");
    f.t3.finish("run-1");
    f.reach(task, "completed");
    check(f.model.calls == 2 && f.logs(task, "completed") == 1, "settled report was lost after prefill failed");
}

void prefillPersistenceFailureDoesNotProceed() {
    Fixture f;
    f.read = "prefill";
    f.allowBeta();
    f.restart();
    const auto source = f.submit("Verify with a Session and consult");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == source) {
            if (input.event["kind"] == "intake") return work();
            if (input.event["kind"] == "result") return botRequest("beta", "consult", "Confirm policy");
            check(input.event["kind"] == "bot_result", "unexpected source event before persistence failure");
            return complete();
        }
        check(input.event["kind"] == "bot_request", "unexpected recipient event before persistence failure");
        return botReply("result", "Policy confirmed");
    };
    f.reach(source, "running");
    f.t3.finish("run-1");
    for (int step = 0; step < 12; ++step) {
        f.runtime->tick();
        if (f.t3.reads == 1 && f.task(source)["state"] == "peer_waiting") break;
    }
    check(f.t3.reads == 1 && f.task(source)["state"] == "peer_waiting", "test did not finish the initial prefill before consulting");
    for (int step = 0; step < 12; ++step) {
        f.runtime->tick();
        const auto messages = f.botMessages("alpha");
        if (messages.size() == 2 && messages.back()["messageType"] == "result" && f.task(source)["state"] == "deciding") break;
    }
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 2 && messages.back()["messageType"] == "result" && f.t3.reads == 1 && f.logs(source, "report") == 1,
          "test did not reach a settled report with an incoming peer result");
    setSessionReadJournalFailure(f.directory / "state.db", true);
    bool storageFailure = false;
    try {
        f.runtime->tick();
    } catch (const std::runtime_error& error) {
        storageFailure = std::string_view(error.what()) == "injected journal failure";
    }
    check(storageFailure && f.t3.reads == 2 && f.model.calls == 3 && f.logs(source, "session_read") == 1 &&
              f.logs(source, "model_failure") == 0 && f.logs(source, "completed") == 0 && f.task(source)["state"] == "deciding",
          "the model proceeded with a prefill excerpt that SQLite did not durably save");
    setSessionReadJournalFailure(f.directory / "state.db", false);
    f.runtime->tick();
    check(f.t3.reads == 3 && f.model.calls == 3 && f.logs(source, "session_read") == 2,
          "prefill did not reread and save the excerpt before retrying the model");
    f.reach(source, "completed");
    check(f.t3.reads == 3 && f.model.calls == 4, "the saved prefill excerpt did not resume the peer result");
}

void peerCancelWithLateSessionResult() {
    Fixture f;
    f.allowBeta();
    const auto root = f.submit("Ask the specialist to verify the PR");
    const auto unrelated = f.submit("Next ordinary request");
    int lateDecisions = 0;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == unrelated) return ask();
        if (input.event["kind"] == "bot_request") return work();
        if (input.event["kind"] == "intake") return botRequest("beta", "work", "Verify the PR");
        ++lateDecisions;
        return botReply("result", "Late verification");
    };
    f.reach(root, "peer_waiting");
    const auto recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(root).task, 0), "cancel");
    for (int step = 0; step < 8 && f.t3.interrupts == 0; ++step) f.runtime->tick();
    check(f.t3.interrupts == 1 && f.t3.interruptedRuns.back().threadId == "thread-1" && f.t3.interruptedRuns.back().runId == "run-1" &&
              f.task(unrelated)["state"] == "queued",
          "cancel interrupted the wrong Run or released its slot before settlement");
    f.restart();
    f.t3.finish("run-1");
    f.reach(recipient, "cancelled");
    f.reach(root, "cancelled");
    f.wait(unrelated, "user_question");
    const auto messages = f.botMessages("alpha");
    check(lateDecisions == 0 && f.t3.launches == 1 && f.logs(root, "completed") == 0 && messages.size() == 2 &&
              messages.back().at("status") == "cancelled",
          "a late worker result reopened or replaced the cancelled exchange");
}

void peerProjectCycle() {
    Fixture f;
    f.allowBeta();
    f.peers.at("alpha").front().intake.projectId = "second";
    f.peers["beta"] = {{"Release", {"alpha", "", "e", "p"}}};
    const auto alpha = f.submit("Verify in the second project");
    const auto beta = f.submit("Verify in the first project", "beta", "second");
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.event["kind"] == "bot_request") return work();
        if (input.event["kind"] == "result") {
            check(input.event["status"] == "failed", "recipient did not receive the failed Session outcome");
            return botReply("failed", "Could not verify the second project");
        }
        if (input.event["kind"] == "bot_result") {
            check(input.event["status"] == "failed" && input.event["text"] == "Could not verify the second project",
                  "sender lost the recipient's explicit failure");
            return ask();
        }
        if (input.task["id"] == alpha) return botRequest("beta", "work", "Verify in the second project");
        return botRequest("alpha", "work", "Verify in the first project");
    };
    f.wait(beta, "model_failure");
    const auto messagesBeforeCancel = f.botMessages("beta");
    check(f.task(beta)["doneWhen"] == "" && Tasks(*f.store).tasks("alpha").size() == 1 && messagesBeforeCancel.size() == 1 &&
              messagesBeforeCancel.front().at("fromBotId") == "alpha",
          "cross-project cycle was not rejected before creating work or saving a completion contract");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(beta).task, 0), "cancel");
    const auto recipient = Tasks(*f.store).tasks("beta").front().task.id;
    f.reach(recipient, "running");
    check(f.t3.launches == 1 && f.t3.seen.back().projectId == "second",
          "the original work request did not progress on its recipient project after the cycle was prevented");
    f.t3.finish("run-1", "failed");
    f.wait(alpha, "user_question");
    const auto messages = f.botMessages("alpha");
    check(messages.size() == 2 && messages.back()["status"] == "failed" && f.logs(alpha, "completed") == 0,
          "the original exchange hid the failed outcome or completed the sender's work");
}

struct PeerMoveCycleFixture : Fixture {
    const Intake verifier{
        "beta", "", "cloud", "p", {{"conversationEnvironments", {{"desktop", "e"}, {"remote", "remote"}, {"backup", "backup"}}}}};
    const Intake pinnedRemote{"beta", "", "remote", "remote-project", verifier.config};
    Intake specialist{"alpha", "", "e", "p", verifier.config};
    Id desktopSender = 0, cloudRecipient = 0, remoteSender = 0;

    PeerMoveCycleFixture() {
        executeSql(directory / "state.db",
                   "INSERT INTO bot_t3_projects VALUES "
                   "('beta','remote','remote-project'),('beta','e','p'),('alpha','e','p'),('beta','backup','backup-project')");
        peers["alpha"] = {{"Verifier", verifier}};
        model.script = [&](const ModelInput& input) -> Decision {
            const auto& kind = input.event.at("kind");
            if (kind == "intake") {
                if (input.task.at("id") == desktopSender) return botRequest("beta", "work", "Verify the release");
                check(input.task.at("id") == remoteSender, "Unexpected task intake");
                return botRequest("alpha", "work", "Check the deployment");
            }
            if (kind == "bot_request") return work();
            if (kind == "bot_result") return input.event.at("status") == "completed" ? complete() : ask();
            if (kind == "result" && input.task.contains("userInstructions")) return work("delegate", false);
            check(kind == "result", "Unexpected recipient event");
            return botReply("result", "Verification complete");
        };
        desktopSender = submit("Release check");
        reach(desktopSender, "peer_waiting");
        cloudRecipient = Tasks(*store).tasks("beta").front().task.id;
        reach(cloudRecipient, "running");
    }

    void startRemoteSender(std::string destination = "e") {
        specialist.environmentId = std::move(destination);
        specialist.projectId = specialist.environmentId == "backup" ? "backup-project" : "p";
        peers["beta"] = {{"Specialist", specialist}};
        remoteSender = Tasks(*store).submit({"beta", "Deployment check", "remote", "remote-project"});
    }

    Json move(Id recipient, const Intake& routing, std::string_view environment) {
        constexpr auto revision = "Check the deployment logs";
        const auto decideWork = model.script;
        Json result;
        model.script = [&](const ModelInput& input) {
            if (input.event.at("kind") != "conversation_turn") return decideWork(input);
            if (!input.event.at("turn").empty()) result = input.event.at("turn").back().at("result");
            return converse(input, "change_task", recipient, {{"environment", environment}, {"doneWhen", revision}});
        };
        const std::string instruction = "Move the verification to " + std::string(environment) + " and " + revision;
        Conversations(*store).receive(localOwner, routing, instruction, instruction);
        runtime->runConversations();
        model.script = decideWork;
        return result;
    }
};

void cyclicPeerMoveIsRejected() {
    PeerMoveCycleFixture f;
    f.startRemoteSender();
    f.reach(f.remoteSender, "peer_waiting");
    const auto queued = Tasks(*f.store).tasks("alpha").front().task.id;
    const auto originalRun = f.task(f.cloudRecipient).at("activeRun");
    const auto rejected = f.move(f.cloudRecipient, f.pinnedRemote, "remote");
    check(rejected.at("ok") == false && rejected.at("error").get<std::string>().find("project queues") != std::string::npos,
          "the conversation admitted a move that cycles back to the sender's queue");
    f.restart();
    check(f.task(f.cloudRecipient).at("activeRun") == originalRun && f.task(f.cloudRecipient).at("doneWhen") == Evidence &&
              f.logs(f.cloudRecipient, "instruction_accepted") == 0 && f.task(queued).at("blockedBy").at("id") == f.desktopSender,
          "the rejected move changed the run, criteria, or exchange after restart");

    check(f.move(f.cloudRecipient, f.verifier, "desktop").at("effect") == "replanning",
          "the recipient could not move into the sender's own queue");
    f.restart();
    f.t3.finish(originalRun.at("runId"));
    for (int tick = 0; tick < 12 && f.t3.launches == 1; ++tick) f.runtime->tick();
    check(f.t3.launches == 2 && f.t3.interrupts == 0 && f.t3.seen.back().environmentId == "e" &&
              f.task(queued).at("blockedBy").at("id") == f.cloudRecipient,
          "the valid move did not inherit its sender's slot and launch once");
    f.t3.finish("run-2");
    f.reach(f.desktopSender, "completed");
    f.reach(queued, "running");
    f.t3.finish("run-3");
    f.reach(f.remoteSender, "completed");
    check(f.logs(f.cloudRecipient, "peer_completed") == 1 && f.logs(queued, "peer_completed") == 1,
          "the valid move did not let both original exchanges close once");
}

void pendingPeerMovePreventsNewExchange() {
    PeerMoveCycleFixture f;
    check(f.move(f.cloudRecipient, f.verifier, "remote").at("effect") == "replanning",
          "the first move could not reserve a free destination");
    f.restart();
    f.startRemoteSender();
    f.wait(f.remoteSender, "model_failure");
    check(f.task(f.remoteSender).at("wait").at("text").get<std::string>().find("project queues") != std::string::npos &&
              Tasks(*f.store).tasks("alpha").size() == 1 && f.botMessages("alpha").size() == 1,
          "a new exchange was opened against the already accepted destination");
}

void replacingAStalePeerProjectReservationRechecksCycles() {
    PeerMoveCycleFixture f;
    executeSql(f.directory / "state.db",
               "UPDATE bot_t3_projects SET project_id='retired-project' WHERE bot_id='beta' AND environment_id='remote'");
    f.t3.projectsByEnvironment["remote"] = {{"remote-project", "Remote project", {}}};
    f.startRemoteSender();
    f.reach(f.remoteSender, "peer_waiting");
    check(f.move(f.cloudRecipient, f.verifier, "remote").at("effect") == "replanning",
          "the provisional destination did not proceed to project discovery");
    f.t3.finish(f.task(f.cloudRecipient).at("activeRun").at("runId"));
    f.wait(f.cloudRecipient, "settings");
    const auto unresolved = f.task(f.cloudRecipient);
    check(unresolved.at("environmentId") == "remote" && unresolved.at("projectId") == "" && !unresolved.contains("pendingDestination") &&
              unresolved.at("wait").at("text").get<std::string>().find("project queues") != std::string::npos && f.t3.launches == 1,
          "late project discovery admitted a peer route with a waiting cycle");
    check(f.move(f.cloudRecipient, f.verifier, "desktop").at("effect") == "replanning",
          "the task could not be moved away from an unavailable project");
    f.reach(f.cloudRecipient, "running");
    check(f.t3.launches == 2 && f.t3.seen.back().environmentId == "e" && f.t3.seen.back().projectId == "p",
          "the moved task did not recover into a viable project");
}

void staleCyclicPeerProjectDoesNotRejectMove() {
    PeerMoveCycleFixture f;
    f.startRemoteSender();
    f.reach(f.remoteSender, "peer_waiting");
    f.t3.projectsByEnvironment["remote"] = {{"active-project", "Active project", {}}};
    check(f.move(f.cloudRecipient, f.verifier, "remote").at("effect") == "replanning",
          "a stale remembered project prevented an automatic move");
    f.restart();
    f.t3.finish(f.task(f.cloudRecipient).at("activeRun").at("runId"));
    for (int tick = 0; tick < 12 && f.t3.launches == 1; ++tick) f.runtime->tick();
    check(f.t3.launches == 2 && f.t3.seen.back().environmentId == "remote" && f.t3.seen.back().projectId == "active-project",
          "the moved task did not launch in the active project without the cycle");
}

void pendingPeerMovePreventsAnotherMove() {
    PeerMoveCycleFixture f;
    f.startRemoteSender("backup");
    f.reach(f.remoteSender, "peer_waiting");
    const auto backupRecipient = Tasks(*f.store).tasks("alpha").front().task.id;
    check(f.move(backupRecipient, f.specialist, "desktop").at("effect") == "replanning",
          "the first move could not reserve the desktop destination");
    check(f.move(f.remoteSender, f.verifier, "backup").at("effect") == "replanning",
          "the sender could not request a move for after its exchange");
    f.restart();
    check(f.move(f.cloudRecipient, f.pinnedRemote, "remote").at("ok") == false && f.logs(f.cloudRecipient, "instruction_accepted") == 0 &&
              f.logs(backupRecipient, "instruction_accepted") == 1,
          "the second move introduced a cycle against an already accepted move");
}

void cancelledPeerMoveFreesItsPendingDestination() {
    PeerMoveCycleFixture f;
    f.startRemoteSender("backup");
    f.reach(f.remoteSender, "peer_waiting");
    const auto backupRecipient = Tasks(*f.store).tasks("alpha").front().task.id;
    check(f.move(f.cloudRecipient, f.verifier, "remote").at("effect") == "replanning",
          "the first move could not reserve the remote destination");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(f.cloudRecipient).task, 0), "cancel");
    check(f.task(f.cloudRecipient).at("state") == "cancelling" &&
              f.move(backupRecipient, f.specialist, "desktop").at("effect") == "replanning",
          "a cancelling recipient retained its abandoned destination");
}

void peerCancelWhileQueued() {
    Fixture f;
    f.allowBeta();
    f.peers.at("alpha").front().intake.projectId = "second";
    const auto running = f.submit("Existing verification", "beta", "second");
    const auto source = f.submit("Request separate verification in the occupied project");
    bool receivedCancellation = false;
    f.model.script = [&](const ModelInput& input) -> Decision {
        if (input.task["id"] == running) return work();
        check(input.task["id"] == source, "queued work started before its project was available");
        if (input.event["kind"] == "intake") return botRequest("beta", "work", "Verify the second project");
        receivedCancellation = input.event["kind"] == "bot_result" && input.event["status"] == "cancelled";
        return ask();
    };
    f.reach(running, "running");
    f.reach(source, "peer_waiting");
    const auto queued = Tasks(*f.store).tasks("beta").front().task.id;
    check(f.task(queued)["state"] == "queued" && f.task(queued)["blockedBy"]["id"] == running,
          "test did not queue the delegated task behind an active Run");
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(queued).task, 0), "cancel");
    f.reach(queued, "cancelled");
    f.wait(source, "user_question");
    check(receivedCancellation && f.task(running)["state"] == "running" && f.t3.launches == 1 && f.t3.interrupts == 0,
          "queued cancellation stranded the sender or affected the unrelated active Run");
}

void peerCancelDuringPreflight() {
    Fixture f;
    f.allowBeta();
    f.peers.at("alpha").front().intake.projectId = "second";
    const auto source = f.submit("Delegate verification to the second project");
    f.model.script = [](const ModelInput& input) {
        if (input.event["kind"] == "bot_request") return work();
        return botRequest("beta", "work", "Verify the second project");
    };
    f.reach(source, "peer_waiting");
    const auto recipient = Tasks(*f.store).tasks("beta").front().task.id;
    bool cancelledDuringPreflight = false;
    f.t3.onPreflight = [&] {
        Store cli(f.directory / "state.db");
        Tasks(cli).operate(localOwner, TaskTarget::from(Tasks(cli).task(source).task, 0), "cancel");
        cancelledDuringPreflight = true;
    };
    for (int step = 0; step < 8 && !cancelledDuringPreflight; ++step) f.runtime->tick();
    f.t3.onPreflight = {};
    check(cancelledDuringPreflight && f.t3.seen.empty(), "a peer launch started after its sender was cancelled");
    f.reach(source, "cancelled");
    f.reach(recipient, "cancelled");
}

void inferenceInvalidatedByCancel() {
    Fixture f;
    const auto id = f.submit();
    std::mutex mutex;
    std::condition_variable ready;
    bool inModel = false, cancelled = false;
    f.model.script = [&](const ModelInput&) {
        std::unique_lock lock(mutex);
        inModel = true;
        ready.notify_one();
        ready.wait(lock, [&] { return cancelled; });
        return work();
    };
    f.runtime->tick();
    std::thread deciding([&] { f.runtime->tick(); });
    {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return inModel; });
        Store cli(f.directory / "state.db");
        Tasks(cli).operate(localOwner, TaskTarget::from(Tasks(cli).task(id).task, 0), "cancel");
        cancelled = true;
        ready.notify_one();
    }
    deciding.join();
    f.reach(id, "cancelled");
    check(f.t3.launches == 0 && f.logs(id, "decision") == 0, "cancelled inference committed");
}
void prefillInFull() {
    Fixture f;
    f.read = "prefill";
    f.restart();
    Policy policy{*f.store};
    for (const auto& [name, token] : std::map<std::string, std::string>{{"first", "result"}, {"second", "complete"}}) {
        auto request = policy.requestSecret("alpha", {name});
        policy.submitSecret({"fixture", true}, "alpha", request, token);
    }
    const std::string hidden = "fixture-hidden-question";
    policy.submitSecret({"fixture", true}, "alpha", policy.requestSecret("alpha", {"question-token"}), hidden);
    const auto id = f.submit();
    f.model.script = [&](const ModelInput& in) {
        if (in.event.at("kind") == "intake") return work();
        if (in.event.at("kind") == "question") {
            check(in.event.dump().find(hidden) == std::string::npos, "Session question reached the model with a secret");
            return reply();
        }
        check(in.event.at("kind") == "result" && in.event.at("report") == "checked" && in.evidence.size() == 1,
              "prefill damaged the settled report or control fields");
        return complete();
    };
    f.reach(id, "running");
    f.t3.question("run-1");
    f.t3.runs.at("run-1").question.fields.at(0).prompt = hidden;
    f.runtime->tick();
    check(f.task(id).at("question").at("fields").at(0).at("question") == "[redacted]" &&
              Tasks(*f.store).history(id).dump().find(hidden) == std::string::npos,
          "raw question persisted in Task or journal");
    f.reach(id, "running");
    f.t3.finish("run-1");
    f.reach(id, "completed");
    check(f.t3.reads == 1, "owner could not read its own Session without an explicit share");
}

}

int main() {
    for (const auto& [name, run] : std::vector<std::pair<const char*, void (*)()>>{{"exclusive Runtime database lock",
                                                                                    runtimeDatabaseLockIsExclusive},
                                                                                   {"invalid decisions", invalidDecisions},
                                                                                   {"correct invalid task arguments",
                                                                                    correctInvalidTaskArguments},
                                                                                   {"failed preflight", failedPreflightIsUnsent},
                                                                                   {"replay saved question decision",
                                                                                    [] { replayQuestionDecision(); }},
                                                                                   {"clarification and failure", ambiguityAndFailure},
                                                                                   {"replay question decision after auth recovery",
                                                                                    [] { replayQuestionDecision("auth"); }},
                                                                                   {"replay question decision after settings reconciliation",
                                                                                    [] { replayQuestionDecision("settings"); }},
                                                                                   {"deadline, approval and cancel",
                                                                                    deadlineApprovalAndCancel},
                                                                                   {"additional work budget", budgetAndStaleWaits},
                                                                                   {"branch questions", sessionQuestions},
                                                                                   {"stale waits", waitsExpireWithReality},
                                                                                   {"recover live question across auth and settings waits",
                                                                                    connectionRecoveryRestoresSessionQuestion},
                                                                                   {"discard expired question after connection recovery",
                                                                                    connectionRecoveryDiscardsExpiredSessionQuestion},
                                                                                   {"recover the question's model approval",
                                                                                    connectionRecoveryRestoresQuestionModelApproval},
                                                                                   {"reconsider new directions after question recovery",
                                                                                    connectionRecoveryReconsidersNewDirections},
                                                                                   {"delivery recovery", recoveredDelivery},
                                                                                   {"launch receipt recovery",
                                                                                    launchReceiptSurvivesRecovery},
                                                                                   {"retry unsent Session answer",
                                                                                    unsentAnswerRemainsRetryable},
                                                                                   {"cancel during delivery", cancelDuringDelivery},
                                                                                   {"selected work reconciles and cancels only its own queued Run",
                                                                                    selectedWorkSurvivesLostReceiptAndStopsOnlyAssignedRun},
                                                                                   {"selected work rechecks its T3 connection after approval",
                                                                                    selectedWorkRechecksConnectionAfterApproval},
                                                                                   {"disconnect blocks new work and retries",
                                                                                    disconnectBlocksSendingAndRetry},
                                                                                   {"pause during preflight prevents a send",
                                                                                    pauseDuringPreflightPreventsSend},
                                                                                   {"resume skips reminders while waiting for dispatch",
                                                                                    resumeSkipsOccurrencesWhileWaitingForDispatch},
                                                                                   {"disconnect preserves owned Run recovery",
                                                                                    disconnectKeepsOwnedRunRecovery},
                                                                                   {"disconnect fences sends across Stores",
                                                                                    disconnectWaitsForInFlightSend},
                                                                                   {"Run settlement with unknown answer delivery",
                                                                                    settleWhileAnswerDeliveryIsUnknown},
                                                                                   {"settled report recovery across restart",
                                                                                    settledReportCanRecoverAfterRestart},
                                                                                   {"late report recovery after retries exhaust",
                                                                                    lateReportCanRestoreCompletion},
                                                                                   {"late report recovery for a peer result",
                                                                                    lateReportCanRestorePeerResult},
                                                                                   {"cancellation with unavailable report",
                                                                                    missingReportDoesNotBlockCancellation},
                                                                                   {"history reading restart", evidenceResume},
                                                                                   {"prefill in Full mode", prefillInFull},
                                                                                   {"redacted history pagination",
                                                                                    redactedHistoryPagination},
                                                                                   {"project owners on task pages",
                                                                                    taskPagesShowProjectOwners},
                                                                                   {"Bot handoff and restart", peerWorkAndRestart},
                                                                                   {"peer work budget shared with direct T3",
                                                                                    peerWorkSharesBudgetWithDirectT3Work},
                                                                                   {"Peer completion evidence follows the Session read policy",
                                                                                    peerCompletionRespectsSessionReadPolicy},
                                                                                   {"Bot clarification after a settled Run",
                                                                                    peerClarificationAfterRun},
                                                                                   {"Task revision during Bot clarification",
                                                                                    [] { revisionWaitsForPeerClarification(); }},
                                                                                   {"Direct answer during Bot clarification revision",
                                                                                    [] { revisionWaitsForPeerClarification(true); }},
                                                                                   {"peer completion after environment move",
                                                                                    peerCompletionAfterEnvironmentMove},
                                                                                   {"recipient revision continues its work exchange",
                                                                                    recipientRevisionContinuesTheOpenWorkExchange},
                                                                                   {"recipient revision during peer question preserves answer",
                                                                                    recipientRevisionDuringPeerQuestionPreservesTheAnswer},
                                                                                   {"recipient move during peer question starts a fresh Session",
                                                                                    recipientMoveDuringPeerQuestionStartsAFreshSession},
                                                                                   {"work recipient move shares its sender's queue",
                                                                                    workRecipientMoveSharesSenderQueue},
                                                                                   {"work recipient move waits for an unrelated owner",
                                                                                    workRecipientMoveWaitsForUnrelatedOwner},
                                                                                   {"consultation replies after user instructions",
                                                                                    consultationRepliesAfterUserInstructions},
                                                                                   {"Bot consultation and permission",
                                                                                    peerConsultationAndPermission},
                                                                                   {"Consultation without a T3 destination",
                                                                                    consultationDoesNotRequireAT3Destination},
                                                                                   {"Automatically routed peer work",
                                                                                    automaticPeerWorkResolvesTheProject},
                                                                                   {"Requested criteria for first peer work",
                                                                                    requestedCriteriaApplyToFirstPeerWork},
                                                                                   {"Model budget across a conversation turn and Bot consultation",
                                                                                    modelBudgetFollowsBotConsultation},
                                                                                   {"Peer result waits for the recipient's conversation turn",
                                                                                    peerResultWaitsForRecipientTurn},
                                                                                   {"Model budget counts failed attempts",
                                                                                    modelBudgetCountsFailedAttempts},
                                                                                   {"Model budget permits T3 observation and reconciliation",
                                                                                    modelBudgetAllowsT3ObservationAndRecovery},
                                                                                   {"Unsuccessful consultations",
                                                                                    unsuccessfulConsultationsDoNotComplete},
                                                                                   {"Concurrent Bot handoff", concurrentPeerHandoff},
                                                                                   {"Consultation own-Session evidence",
                                                                                    consultationRestoresOwnEvidence},
                                                                                   {"Fresh result prefill failure",
                                                                                    freshResultSurvivesUnavailableHistory},
                                                                                   {"Prefill persistence failure",
                                                                                    prefillPersistenceFailureDoesNotProceed},
                                                                                   {"Bot handoff cancellation",
                                                                                    peerCancelWithLateSessionResult},
                                                                                   {"Bot project cycle", peerProjectCycle},
                                                                                   {"Cyclic recipient move is rejected",
                                                                                    cyclicPeerMoveIsRejected},
                                                                                   {"Accepted peer move constrains new exchanges",
                                                                                    pendingPeerMovePreventsNewExchange},
                                                                                   {"Replacing a stale peer reservation rechecks cycles",
                                                                                    replacingAStalePeerProjectReservationRechecksCycles},
                                                                                   {"A stale cyclic peer project does not reject a move",
                                                                                    staleCyclicPeerProjectDoesNotRejectMove},
                                                                                   {"Accepted peer move constrains other moves",
                                                                                    pendingPeerMovePreventsAnotherMove},
                                                                                   {"Cancelled peer move frees its pending destination",
                                                                                    cancelledPeerMoveFreesItsPendingDestination},
                                                                                   {"Queued Bot handoff cancellation",
                                                                                    peerCancelWhileQueued},
                                                                                   {"Bot handoff cancellation during T3 preflight",
                                                                                    peerCancelDuringPreflight},
                                                                                   {"Pending conversation approval leaves an unrelated task free to complete",
                                                                                    [] {
                                                                                        Fixture f;
                                                                                        f.model.script = [](const ModelInput& input) {
                                                                                            if (input.event.at("kind") == "intake")
                                                                                                return work();
                                                                                            if (input.event.at("kind") ==
                                                                                                "conversation_turn")
                                                                                                return Decision{
                                                                                                    "request_bot_change",
                                                                                                    {{"operation", "create_bot"},
                                                                                                     {"targetBotId", "new-bot"},
                                                                                                     {"name", "New Bot"}},
                                                                                                    "Create"};
                                                                                            return complete();
                                                                                        };
                                                                                        const auto task = f.submit();
                                                                                        f.reach(task, "running");
                                                                                        Policy{*f.store}.setMode(localOwner, "supervised");
                                                                                        Conversations(*f.store).receive(
                                                                                            localOwner, {"alpha", "", "e", "separate"},
                                                                                            "unrelated", "Create another Bot");
                                                                                        f.runtime->runConversations();
                                                                                        check(Inbox(*f.store).inbox(localOwner).size() == 1,
                                                                                              "The unrelated turn did not await approval");
                                                                                        f.t3.finish("run-1");
                                                                                        f.reach(task, "completed");
                                                                                        check(
                                                                                            Inbox(*f.store).inbox(localOwner).size() == 1,
                                                                                            "Task completion settled an unrelated approval");
                                                                                    }},
                                                                                   {"Never blocks a follow-up launch in the Session environment",
                                                                                    [] {
                                                                                        Fixture f;
                                                                                        f.model.script = [](const ModelInput& input) {
                                                                                            if (input.event.at("kind") == "intake")
                                                                                                return work();
                                                                                            if (input.event.at("kind") == "result")
                                                                                                return work("follow_up", false);
                                                                                            return ask();
                                                                                        };
                                                                                        const auto task = f.submit();
                                                                                        f.reach(task, "running");
                                                                                        Policy{*f.store}.changeNeverRule(
                                                                                            localOwner, Action::environment_launch, "e",
                                                                                            true);
                                                                                        f.t3.finish("run-1", "failed");
                                                                                        f.wait(task, "user_question");
                                                                                        check(
                                                                                            f.t3.launches == 1 && f.t3.follows == 0,
                                                                                            "An environment launch ban allowed a follow-up Run");
                                                                                    }},
                                                                                   {"Never blocks ordinary launch with a T3 connection",
                                                                                    [] {
                                                                                        Fixture f;
                                                                                        Policy{*f.store}.changeNeverRule(
                                                                                            localOwner, Action::environment_launch, "e",
                                                                                            true);
                                                                                        T3Connections(*f.store).connect("e", localOwner.id,
                                                                                                                        4173);
                                                                                        f.model.script = [](const ModelInput&) {
                                                                                            return work();
                                                                                        };
                                                                                        f.wait(f.submit(), "settings");
                                                                                        check(f.t3.launches == 0,
                                                                                              "the connector allowed a blocked Bot launch");
                                                                                    }},
                                                                                   {"stale in-flight model decision",
                                                                                    inferenceInvalidatedByCancel}}) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
            return 1;
        }
    }
}
