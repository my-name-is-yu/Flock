#include "flock/tasks.hpp"
#include "flock/automations.hpp"
#include "flock/conversations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/channels.hpp"
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
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace flockv0;
using flockv0::test::lastTaskEvent;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path dir;

    Fixture() {
        char pattern[] = "/tmp/flock-conversation-XXXXXX";
        if (const auto* path = mkdtemp(pattern))
            dir = path;
        else
            throw std::runtime_error("Cannot create fixture");
    }
    ~Fixture() { std::filesystem::remove_all(dir); }

    std::filesystem::path database() const { return dir / "state.db"; }
    static Intake bot(std::string id = "alpha", std::string project = "project") {
        return {std::move(id), "", "env", std::move(project), Json::object()};
    }
    auto connect() const {
        sqlite3* raw = nullptr;
        check(sqlite3_open(database().c_str(), &raw) == SQLITE_OK, "Cannot open the fixture database");
        return std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(raw, sqlite3_close);
    }
    static void sql(sqlite3* db, const std::string& statement) {
        check(sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "The fixture SQL failed");
    }
    void sql(const std::string& statement) const { sql(connect().get(), statement); }
    long long count(const std::string& statement) const {
        sqlite3* raw = nullptr;
        check(sqlite3_open(database().c_str(), &raw) == SQLITE_OK, "Cannot open the fixture database");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        long long result = -1;
        check(sqlite3_exec(
                  db.get(), statement.c_str(),
                  [](void* out, int, char** values, char**) {
                      *static_cast<long long*>(out) = values[0] ? std::atoll(values[0]) : 0;
                      return 0;
                  },
                  &result, nullptr) == SQLITE_OK,
              "The fixture query failed");
        return result;
    }
};

Id task(Store& store, std::string name, std::string bot = "alpha", std::string project = "project") {
    auto intake = Fixture::bot(std::move(bot), std::move(project));
    intake.request = std::move(name);
    return Tasks(store).submit(intake);
}

Json reply(Store& store, Id message, std::string_view bot = "alpha") {
    for (const auto& row : Conversations(store).conversation(bot, 0, 200))
        if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message) return row["data"];
    return nullptr;
}

Id receive(Store& store, std::string_view id, std::string_view text, Id anchor = 0, Intake intake = Fixture::bot()) {
    return Conversations(store).receive(localOwner, intake, id, text, anchor).at("messageId");
}

Decision say(std::string text, Json suggestions = nullptr) {
    Json args = {{"text", std::move(text)}};
    if (!suggestions.is_null()) args["suggestions"] = std::move(suggestions);
    return {"reply", std::move(args), "Reply"};
}

struct ScriptedModel : Model {
    std::mutex mutex;
    std::function<Decision(const ModelInput&)> conversation;
    std::vector<ModelInput> turns;
    Json usage = Json::object();

    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}, usage}; }
    Decision choose(const ModelInput& input) {
        if (input.event.value("kind", "") == "conversation_turn") {
            {
                std::lock_guard lock(mutex);
                turns.push_back(input);
            }
            return conversation(input);
        }
        if (input.event.at("kind") == "intake")
            return {"delegate", {{"instruction", "Run the fixture work"}, {"doneWhen", "Provide a fixture report"}}, "Run the fixture"};
        check(input.event.at("kind") == "result", "Unexpected execution fixture event");
        return {"complete", {{"report", input.event.at("report")}}, "The fixture report exists"};
    }
};

struct SimulatedT3 : T3 {
    std::map<std::string, Observation> observations;
    unsigned launches = 0, sessionSends = 0;
    std::function<void()> beforePreflight;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    std::optional<Delivery> preflight(const Command&) override {
        if (beforePreflight) beforePreflight();
        return std::nullopt;
    }
    Delivery send(const Command& command) override {
        if (command.tool == CommandTool::message_session) {
            ++sessionSends;
            return {Delivery::Kind::Accepted, command.target, {}};
        }
        check(command.tool == CommandTool::delegate, "Unexpected work command in the Store fixture");
        const auto suffix = std::to_string(++launches);
        return {Delivery::Kind::Accepted, {"thread-" + suffix, "run-" + suffix, {}}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return observations[run.runId]; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};

struct RuntimeFixture {
    ScriptedModel model;
    SimulatedT3 t3;
    Runtime::SoulLoader notes = [](std::string_view) { return std::string{}; };
    Seconds now = 100;
    Runtime runtime;

    explicit RuntimeFixture(Store& store, std::function<void(std::string_view)> contention = {})
        : runtime(
              store, model, t3, [this](std::string_view bot) { return notes(bot); },
              RuntimeOptions{.clock = [this] { return now; }, .storageContention = std::move(contention)}) {}

    void reach(Store& store, Id id, std::string_view state) {
        for (int i = 0; i < 16 && taskStateName(Tasks(store).task(id).task.state) != state; ++i) runtime.tick();
        check(taskStateName(Tasks(store).task(id).task.state) == state, "The work did not reach the required fixture state");
    }

    void finish(Store& store, Id id, std::string result) {
        reach(store, id, "running");
        const auto run = Tasks(store).task(id).task.activeRun.value().ref.runId;
        t3.observations[run] = {.kind = Observation::Kind::Finished,
                                .status = "completed",
                                .report = std::move(result),
                                .resultRunId = run,
                                .itemId = "report-" + run};
        reach(store, id, "completed");
    }
};

Json turn(const ModelInput& input) {
    return input.event.at("turn");
}

void retryAcrossConnections() {
    Fixture fixture;
    Store left(fixture.database()), right(fixture.database());
    Id a = 0, b = 0;
    std::exception_ptr first, second;
    std::thread one([&] {
        try {
            a = receive(left, "retry", "Write a release note");
        } catch (...) {
            first = std::current_exception();
        }
    });
    std::thread two([&] {
        try {
            b = receive(right, "retry", "Write a release note");
        } catch (...) {
            second = std::current_exception();
        }
    });
    one.join();
    two.join();
    if (first) std::rethrow_exception(first);
    if (second) std::rethrow_exception(second);
    check(a && a == b, "Concurrent delivery returned different receipts");
    check(Tasks(left).tasks("alpha").empty(), "Receipt intake executed work before its turn");
    Store restarted(fixture.database());
    RuntimeFixture execution(restarted);
    unsigned calls = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        check(input.event.at("text") == "Write a release note", "Restart lost the pending message");
        ++calls;
        if (turn(input).empty()) return Decision{"start_task", {{"request", "Write a release note"}}, "Start it"};
        return say("I queued your request.");
    };
    execution.runtime.runConversations();
    check(Tasks(restarted).tasks("alpha").size() == 1 && reply(restarted, a)["actions"].size() == 1,
          "Delivery did not queue exactly the requested work");
    bool rejected = false;
    try {
        receive(right, "retry", "Write a different release note");
    } catch (const Conflict&) {
        rejected = true;
    }
    check(rejected, "Changed message text did not conflict");
    rejected = false;
    try {
        receive(right, "retry", "Write a release note", reply(restarted, a).at("messageId"));
    } catch (const Conflict&) {
        rejected = true;
    }
    check(rejected, "Changed message reply target did not conflict");
    check(receive(restarted, "retry", "Write a release note") == a, "Receipt was lost on restart");
    execution.runtime.runConversations();
    check(calls == 2 && Tasks(restarted).tasks("alpha").size() == 1, "The retry ran an applied message again");
}

void dueAutomationWakesAfterIdle(bool externalWrite, bool duringTick) {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    ScriptedModel model;
    SimulatedT3 t3;
    std::atomic<Seconds> now{100};
    auto reminder = Fixture::bot("beta");
    reminder.request = "Fire a scheduled reminder";
    const auto createReminder = [&](Seconds from) {
        Automations(writer).createAutomation(
            localOwner, reminder, {{"kind", "reminder"}, {"timezone", "Etc/UTC"}, {"fireAt", from + 1}, {"request", reminder.request}},
            from);
    };
    const auto makeDue = [&] {
        if (externalWrite)
            createReminder(now.load() - 1);
        else
            ++now;
    };
    const auto tickThread = std::this_thread::get_id();
    enum class DueReminder { Unarmed, AfterApprovalClockRead, OnNextClockRead };
    auto dueReminder = DueReminder::Unarmed;
    Runtime runtime(
        store, model, t3, [](std::string_view) { return std::string{}; }, RuntimeOptions{.clock = [&] {
            if (std::this_thread::get_id() == tickThread) {
                switch (dueReminder) {
                case DueReminder::Unarmed: break;
                case DueReminder::AfterApprovalClockRead: dueReminder = DueReminder::OnNextClockRead; break;
                case DueReminder::OnNextClockRead:
                    dueReminder = DueReminder::Unarmed;
                    makeDue();
                    break;
                }
            }
            return now.load();
        }});
    if (!externalWrite) createReminder(now.load());
    runtime.tickConcurrent();
    runtime.tickConcurrent();
    check(Tasks(store).tasks("beta").empty(), "A reminder ran before it was due");

    if (duringTick)
        dueReminder = DueReminder::AfterApprovalClockRead;
    else
        makeDue();
    runtime.tickConcurrent();
    check(Tasks(store).tasks("beta").size() == 1, "An idle check missed a newly due reminder");
}

void failedAutomationDoesNotInvalidateOtherMaintenance() {
    Fixture fixture;
    Store store(fixture.database());
    ScriptedModel model;
    SimulatedT3 t3;
    auto maintenanceWriteLock = fixture.connect();
    const auto releaseWriteLock = [&] { Fixture::sql(maintenanceWriteLock.get(), "ROLLBACK"); };
    int errors = 0;
    bool unexpectedMaintenanceWrite = false;
    Runtime runtime(
        store, model, t3, [](std::string_view) { return std::string{}; },
        RuntimeOptions{.clock = [] { return Seconds{-1}; },
                       .automationError = [&](std::string_view) { ++errors; },
                       .storageContention =
                           [&](std::string_view) {
                               if (!unexpectedMaintenanceWrite) releaseWriteLock();
                               unexpectedMaintenanceWrite = true;
                           }});
    runtime.tickConcurrent();

    Fixture::sql(maintenanceWriteLock.get(), "BEGIN IMMEDIATE");
    runtime.tickConcurrent();
    if (!unexpectedMaintenanceWrite) releaseWriteLock();
    check(errors == 2 && !unexpectedMaintenanceWrite, "Retrying an automation error repeated completed maintenance");
}

void savedDecisionIsAppliedWithoutAnotherModelCall() {
    Fixture fixture;
    Store store(fixture.database());
    const auto message = receive(store, "saved-decision", "Write a release note");
    const auto id = std::to_string(message);
    fixture.sql("INSERT INTO conversation_steps(message_id,step,decision) VALUES(" + id +
                ",1,'[{\"tool\":\"start_task\",\"args\":{\"request\":\"Write a release note\"}}]')");
    RuntimeFixture execution(store);
    unsigned calls = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        ++calls;
        check(turn(input).size() == 1 && turn(input)[0]["result"]["ok"] == true, "The saved step was not applied first");
        return say("Queued.");
    };
    execution.runtime.runConversations();
    check(calls == 1 && Tasks(store).tasks("alpha").size() == 1 && reply(store, message)["text"] == "Queued.",
          "A saved decision was decided again or applied twice");
}

void missingToolCannotBeAddedAfterInferenceStarts() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    store.setRootPeople({localOwner.id});
    const auto id = task(store, "Prepare a local Session");
    execution.reach(store, id, "running");
    const auto run = Tasks(store).task(id).task.activeRun.value().ref;
    const Decision request{"request_local_machine_connection",
                           {{"threadId", run.threadId},
                            {"runId", run.runId},
                            {"machineId", "fixture-machine"},
                            {"rules", Json::array({{{"operation", "read_file"}, {"path", fixture.database().string()}}})}},
                           "Request local access"};

    bool rejected = false;
    execution.model.conversation = [&](const ModelInput& input) {
        if (turn(input).empty()) {
            check(std::ranges::find(input.tools, request.tool) == input.tools.end(), "The tool was offered before connecting");
            store.connectLocalMachine("fixture-machine", hostLocalMachine(localOwner.id));
            return request;
        }
        rejected = turn(input).at(0).at("result").at("ok") == false;
        return say("Checked the request.");
    };
    receive(store, "unoffered", "Check whether local access can be requested.");
    execution.runtime.runConversations();
    check(rejected && store.localConnections(localOwner).empty(), "The unoffered call created a connection request");

    execution.model.conversation = [&](const ModelInput& input) {
        if (!turn(input).empty()) return say("Requested local access.");
        check(std::ranges::find(input.tools, request.tool) != input.tools.end(), "The connected machine did not enable the tool");
        return request;
    };
    receive(store, "offered", "Request local access now.");
    execution.runtime.runConversations();
    check(store.localConnections(localOwner).size() == 1, "The offered call did not create a connection request");
}

void protectedArgumentKeyCannotReachTaskIntake() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const std::string marker = "protected-top-level-key-fixture";
    auto policy = store.policy();
    policy.submitSecret(localOwner, "alpha", policy.requestSecret("alpha", {"fixture"}), marker);
    Json observed;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        observed = turn(input);
        if (!observed.empty()) return say("The blocked content was " + marker);
        return {"start_task", {{"request", "Prepare release notes"}, {marker, "Extra"}}, "Candidate"};
    };
    const auto message = receive(store, "protected-key", "Prepare release notes.");
    execution.runtime.runConversations();
    check(Tasks(store).tasks("alpha").empty() && observed.at(0).at("result").at("ok") == false &&
              observed.dump().find(marker) == std::string::npos &&
              reply(store, message).at("text") == "The blocked content was [redacted]" &&
              fixture.count("SELECT count(*) FROM conversation_steps WHERE instr(decision,'" + marker + "') > 0") == 0,
          "A protected top-level key reached task intake, persistence, or conversation history");
}

void rejectedSessionReadStillAllowsAnOrdinaryReply() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    auto policy = store.policy();
    policy.submitSecret(localOwner, "alpha", policy.requestSecret("alpha", {"fixture"}), "text");
    bool rejected = false;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        const auto steps = turn(input);
        if (steps.empty()) return {"flock_session_search", {{"query", "text"}}, "Find matching Sessions"};
        rejected = steps.at(0).at("result").at("ok") == false;
        return say("The search was blocked.");
    };
    const auto message = receive(store, "protected-session-read", "Find my previous Sessions.");
    execution.runtime.runConversations();
    check(rejected && reply(store, message).at("text") == "The search was blocked.",
          "a rejected Session read or a protocol key matching a secret prevented a reply");
}

void savedCallsAreCheckedBeforeExecution() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto save = [&](Id message, const Decision& call) {
        const auto id = std::to_string(message);
        const auto decision = Json::array({{{"tool", call.tool}, {"args", call.args}, {"reason", call.reason}}});
        fixture.sql("INSERT INTO conversation_steps(message_id,step,decision) VALUES(" + id + ",1,'" + decision.dump() + "')");
    };
    save(receive(store, "saved-unavailable", "Check my repository."),
         {"git_repo_info", {{"host", "github.com"}, {"repo", "example/fixture"}}, "Saved with a Git connection"});
    const std::string protectedPath = "protected-replay-fixture";
    const auto protectedMessage = receive(store, "saved-protected", "Write my workspace file.");
    save(protectedMessage,
         {"workspace_write_file", {{"path", protectedPath}, {"text", "Fixture data"}}, "Saved before the path was protected"});
    auto access = store.policy();
    access.submitSecret(localOwner, "alpha", access.requestSecret("alpha", {"replay-fixture"}), protectedPath);
    const auto taskId = task(store, "Prepare repository evidence");
    const auto inFlight = receive(store, "saved-git-effect", "Comment on the issue.");
    const Json comment{{"taskId", taskId},  {"host", "github.com"}, {"repo", "example/fixture"},
                       {"target", "issue"}, {"number", 1},          {"body", "Fixture comment"}};
    save(inFlight, {"git_comment", comment, "Saved with a Git connection"});
    const auto invocation = Json::array({"turn/" + std::to_string(inFlight) + "/1", "git_comment", comment});
    fixture.sql("INSERT INTO git_mutations(task_id,invocation) VALUES(" + std::to_string(taskId) + ",'" + invocation.dump() + "')");
    unsigned replies = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        const auto steps = turn(input);
        check(steps.size() == 1 && steps.at(0).at("args").empty() && steps.at(0).at("result").at("ok") == false,
              "The model received an executable or unsanitized saved call");
        if (input.event.at("messageId") == protectedMessage)
            check(steps.at(0).at("result").at("error") ==
                      "Tool arguments contain protected material. Use a secret reference where supported.",
                  "The saved call bypassed current secret redaction");
        if (input.event.at("messageId") == inFlight)
            check(steps.at(0).at("result").at("delivery") == "unknown", "The started Git operation lost its uncertain outcome");
        ++replies;
        return say("The saved call could not run.");
    };
    execution.runtime.runConversations();
    check(replies == 3 && !std::filesystem::exists(fixture.dir / "workspaces" / "alpha" / protectedPath),
          "An unavailable or protected saved call executed");
}

void sessionNoteIsNotResentAfterARestart(bool webEvidence, bool started) {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto message = receive(store, "note", "Use the staging database");
    const auto mid = std::to_string(message);
    const std::string step = webEvidence ? "2" : "1";
    const Json effect = {{"kind", "message_session"}};
    if (webEvidence)
        fixture.sql("INSERT INTO conversation_steps(message_id,step,decision,result) VALUES(" + mid +
                    ",1,"
                    "'[{\"tool\":\"web_fetch\",\"args\":{\"url\":\"https://example.com/source\"},\"reason\":\"Read a source\"}]',"
                    "'[{\"ok\":true,\"untrustedData\":true}]')");
    fixture.sql("INSERT INTO conversation_steps(message_id,step,decision,effect) VALUES(" + mid + "," + step + ",'" +
                Json::array({{{"tool", "message_session"},
                              {"args", {{"environmentId", "env"}, {"threadId", "thread"}, {"text", "x"}}},
                              {"reason", "Saved"}}})
                    .dump() +
                "'," + (started ? "'" + effect.dump() + "'" : "NULL") + ")");
    Json seen;
    execution.model.conversation = [&](const ModelInput& input) {
        seen = turn(input);
        return say("I may have sent that already.");
    };
    execution.runtime.runConversations();
    check(seen.size() == (webEvidence ? 2 : 1) && execution.t3.sessionSends == 0,
          "an uncertain or unauthorized Session send was dispatched");
    const auto result = seen.back().at("result");
    check(result.at("ok") == false &&
              (started ? result.value("delivery", "") == "unknown"
                       : !result.contains("delivery") &&
                             result.at("error").get<std::string>().find("unavailable through your T3 connection") != std::string::npos),
          "a saved note lost the distinction between an uncertain and a refused delivery");
}

void transientFailureResumesAfterTheLastFinishedStep() {
    Fixture fixture;
    Id message = 0, later = 0;
    std::map<std::size_t, int> calls;
    {
        Store store(fixture.database());
        RuntimeFixture execution(store);
        execution.model.conversation = [&](const ModelInput& input) -> Decision {
            ++calls[turn(input).size()];
            if (input.event.at("text") == "Hello beta") return say("Hello.");
            if (turn(input).empty()) return {"start_task", {{"request", "Write a release note"}}, "Start it"};
            throw ModelError("The connection dropped", true);
        };
        message = receive(store, "transient", "Write a release note");
        later = receive(store, "later", "How should we announce it?");
        const auto other = receive(store, "beta", "Hello beta", 0, Fixture::bot("beta"));
        for (int i = 0; i < 24; ++i) execution.runtime.runConversations();
        check(Tasks(store).tasks("alpha").size() == 1 && reply(store, message).is_null() && reply(store, later).is_null() &&
                  reply(store, other, "beta")["text"] == "Hello." && calls[0] == 2 && calls[1] == 1 &&
                  Conversations(store).pausedConversations("alpha").empty(),
              "A fixed-clock outage spent the budget or displaced a Bot's oldest turn");
    }
    Store restarted(fixture.database());
    RuntimeFixture execution(restarted);
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        ++calls[turn(input).size()];
        if (input.event.at("text") == "How should we announce it?") {
            check(reply(restarted, message)["text"] == "Queued.", "The later turn overtook the oldest turn");
            return say("In the changelog.");
        }
        check(turn(input).size() == 1, "The restarted turn did not continue after its finished step");
        if (calls[1] == 2) throw ModelError("Still offline", true);
        return say("Queued.");
    };
    execution.runtime.runConversations();
    check(calls[1] == 1, "Restart removed the first backoff");
    ++execution.now;
    execution.runtime.runConversations();
    check(calls[1] == 2, "The first retry was not eligible after one second");
    ++execution.now;
    execution.runtime.runConversations();
    check(calls[1] == 2, "Restart reset the consecutive failure count");
    ++execution.now;
    execution.runtime.runConversations();
    check(calls[0] == 3 && calls[1] == 3 && Tasks(restarted).tasks("alpha").size() == 1 && reply(restarted, message)["text"] == "Queued." &&
              reply(restarted, later)["text"] == "In the changelog.",
          "Restart repeated a finished step or lost a turn");
    check(fixture.count("SELECT count(*) FROM model_attempts WHERE message_id=" + std::to_string(message)) == 4,
          "The failed call was not counted against the message budget");
    check(fixture.count("SELECT count(*) FROM conversation_retries") == 0, "A completed turn kept stale retry metadata");
    check(fixture.count("SELECT attempts FROM model_budgets WHERE id='message/" + std::to_string(message) + "'") == 4 &&
              fixture.count("SELECT count(*) FROM messages WHERE id=" + std::to_string(message) +
                            " AND json_type(routing,'$.modelBudgetId') IS NULL") == 1,
          "Restart lost the deterministic budget or stored a redundant routing ID");
}

void concurrentWorkerRecoversFromWriterContention(bool repairResponse) {
    using namespace std::chrono_literals;
    Fixture fixture;
    Store store(fixture.database());
    auto locker = fixture.connect();
    bool reported = false;
    std::atomic_int blockedCalls = 0;
    std::promise<void> entered, allow;
    const auto resume = allow.get_future().share();
    RuntimeFixture execution(store, [&](std::string_view error) {
        if (error.starts_with("conversation worker")) {
            Fixture::sql(locker.get(), "ROLLBACK");
            reported = true;
        }
    });
    execution.model.usage = {{"input_tokens", 7}, {"input_tokens_details", {{"cached_tokens", 3}}}};
    execution.model.conversation = [&](const ModelInput& input) {
        if (input.event.at("text") == "Wait for SQLite") {
            const auto previous = turn(input);
            if (previous.empty()) {
                if (++blockedCalls == 1) {
                    entered.set_value();
                    resume.wait();
                }
                if (repairResponse) {
                    if (input.feedback.empty()) {
                        throw ModelError("Unexpected batch", false, execution.model.usage, ModelErrorCode::invalid_tool_count);
                    }
                    check(input.feedback.find("invalid_tool_count") != std::string::npos, "The contended repair lost its reason");
                    return say("Done.");
                }
                return Decision{"complete", {{"report", "This tool was not offered"}}, "Check the checkpoint"};
            }
            check(previous.front().at("result").at("error") == "Tool unavailable for this turn",
                  "The recovered model outcome lost its rejection");
        }
        return say("Done.");
    };
    auto intake = Fixture::bot();
    intake.config["modelCallLimit"] = 2;
    const auto blocked = receive(store, "blocked", "Wait for SQLite", 0, intake);
    Id other = 0;
    execution.runtime.tickConcurrent();
    const auto ready = entered.get_future().wait_for(5s) == std::future_status::ready;
    if (ready) Fixture::sql(locker.get(), "BEGIN IMMEDIATE");
    allow.set_value();
    check(ready, "The conversation worker did not start");
    const auto deadline = std::chrono::steady_clock::now() + 25s;
    while (std::chrono::steady_clock::now() < deadline) {
        execution.runtime.tickConcurrent();
        if (reported && !other) other = receive(store, "other", "Different Bot", 0, Fixture::bot("beta"));
        if (other && !reply(store, blocked).is_null() && !reply(store, other, "beta").is_null()) break;
        std::this_thread::sleep_for(5ms);
    }
    check(reported && blockedCalls == (repairResponse ? 2 : 1) && reply(store, blocked)["text"] == "Done." &&
              reply(store, other, "beta")["text"] == "Done." &&
              fixture.count("SELECT json_extract(usage,'$.input_tokens') FROM model_attempts WHERE message_id=" + std::to_string(blocked) +
                            " ORDER BY id LIMIT 1") == 7 &&
              fixture.count("SELECT json_extract(usage,'$.input_tokens_details.cached_tokens') FROM model_attempts WHERE message_id=" +
                            std::to_string(blocked) + " ORDER BY id LIMIT 1") == 3 &&
              fixture.count("SELECT count(*) FROM model_attempts WHERE message_id=" + std::to_string(blocked)) == 2,
          "A contended worker stopped the Runtime or left a Bot without a reply");
    if (repairResponse)
        check(fixture.count("SELECT count(*) FROM conversation_model_failures WHERE message_id=" + std::to_string(blocked) +
                            " AND reason='invalid_tool_count' AND repair_attempt_id IS NOT NULL") == 1,
              "SQLite contention lost or duplicated the repair");
    else
        check(fixture.count("SELECT count(*) FROM conversation_steps WHERE message_id=" + std::to_string(blocked) +
                            " AND step=1 AND json_extract(decision,'$[0].rejection')='Tool unavailable for this turn'") == 1,
              "SQLite contention lost the saved rejection");
}

void repairSurvivesRestart(bool alreadyClaimed) {
    Fixture fixture;
    Id message;
    {
        Store store(fixture.database());
        RuntimeFixture execution(store);
        auto intake = Fixture::bot();
        intake.config["modelCallLimit"] = 3;
        message = receive(store, "repair", "Write a release note", 0, intake);
        execution.model.conversation = [&](const ModelInput& input) -> Decision {
            if (turn(input).empty()) return {"start_task", {{"request", "Write a release note"}}, "Start the requested task"};
            execution.runtime.requestStop();
            throw ModelError("Unexpected batch", false, {{"output_tokens", 42}}, ModelErrorCode::invalid_tool_count);
        };
        execution.runtime.runConversations();
        check(execution.model.turns.size() == 2 && reply(store, message).is_null() && Tasks(store).tasks("alpha").size() == 1 &&
                  execution.t3.launches == 0,
              "The rejected response dispatched an action or finished the turn");
    }
    check(fixture.count("SELECT count(*) FROM conversation_model_failures f JOIN model_attempts a ON a.id=f.attempt_id"
                        " WHERE f.step=2 AND f.reason='invalid_tool_count' AND f.repair_attempt_id IS NOT NULL"
                        " AND json_extract(a.usage,'$.output_tokens')=42") == 1,
          "The repair reservation and rejection were not stored with the failed attempt's usage");
    if (alreadyClaimed) {
        // Simulate a worker that committed its reservation claim and exited before saving the model outcome.
        fixture.sql("BEGIN;"
                    "UPDATE conversation_steps SET attempt_id=(SELECT repair_attempt_id FROM conversation_model_failures)"
                    " WHERE step=2;"
                    "UPDATE conversation_turns SET reserved_attempt=NULL;"
                    "COMMIT;");
    }

    Store reopened(fixture.database());
    RuntimeFixture execution(reopened);
    execution.model.conversation = [&](const ModelInput& input) {
        check(turn(input).size() == 1 && turn(input).at(0).at("tool") == "start_task" &&
                  input.feedback.find("invalid_tool_count") != std::string::npos,
              "The repair lost the completed step or the reason for rejection");
        return say("The task is queued.");
    };
    execution.runtime.runConversations();
    check(execution.model.turns.size() == (alreadyClaimed ? 0 : 1) && Tasks(reopened).tasks("alpha").size() == 1 &&
              reply(reopened, message).at("route") == (alreadyClaimed ? "error" : "reply") &&
              fixture.count("SELECT count(*) FROM model_attempts") == 3,
          "Restart duplicated an operation or charged another repair attempt");
    if (alreadyClaimed)
        check(fixture.count("SELECT count(*) FROM conversation_model_failures WHERE reason='repair_interrupted'") == 1,
              "The interrupted repair lost its diagnostic");
}

void invalidResponseRepairRespectsBudget(bool hasBudget) {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    auto intake = Fixture::bot();
    const Id limit = hasBudget ? 3 : 1;
    intake.config["modelCallLimit"] = limit;
    const auto message = receive(store, "invalid", "Inspect the T3 environment", 0, intake);
    execution.model.conversation = [](const ModelInput&) -> Decision {
        throw ModelError("Unexpected batch", ModelErrorCode::invalid_tool_count);
    };
    execution.runtime.runConversations();
    const auto attempts = hasBudget ? 2 : 1;
    check(execution.model.turns.size() == attempts && reply(store, message).at("route") == "error" &&
              fixture.count("SELECT count(*) FROM model_attempts") == attempts &&
              fixture.count("SELECT count(*) FROM conversation_model_failures") == attempts &&
              fixture.count("SELECT count(*) FROM conversation_steps WHERE decision IS NOT NULL OR effect IS NOT NULL") == 0 &&
              execution.t3.launches == 0,
          "A rejected repair retried again, increased the budget, or ran a tool");
}

void shutdownLeavesAnUnsentRunReadyForRestart() {
    using namespace std::chrono_literals;
    Fixture fixture;
    Store store(fixture.database());
    Id id;
    {
        std::promise<void> entered, allow;
        const auto resume = allow.get_future().share();
        RuntimeFixture execution(store);
        id = task(store, "Wait for T3");
        execution.reach(store, id, "sending");
        execution.t3.beforePreflight = [&] {
            entered.set_value();
            resume.wait();
        };
        execution.runtime.tickConcurrent();
        const auto ready = entered.get_future().wait_for(5s) == std::future_status::ready;
        execution.runtime.requestStop();
        allow.set_value();
        check(ready, "The task worker did not reach preflight");
    }
    const auto pending = Tasks(store).task(id).task;
    check(pending.state == TaskState::sending && !pending.activeRun, "A blocked worker launched a Run after shutdown");
    RuntimeFixture execution(store);
    execution.reach(store, id, "running");
    check(execution.t3.launches == 1, "The unsent action could not launch once after restart");
}

void concurrentWorkerSurfacesStructuralStorageFailure() {
    using namespace std::chrono_literals;
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    fixture.sql("CREATE TRIGGER reject_checkpoint BEFORE UPDATE OF decision ON conversation_steps "
                "BEGIN SELECT RAISE(ABORT,'structural fixture failure'); END");
    execution.model.conversation = [](const ModelInput&) { return say("Done."); };
    const auto message = receive(store, "broken-checkpoint", "Hello");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            execution.runtime.tickConcurrent();
        } catch (const std::runtime_error& error) {
            check(std::string_view(error.what()) == "structural fixture failure" && reply(store, message).is_null(),
                  "The Runtime surfaced the wrong failure or published an uncommitted reply");
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("A structural worker storage failure was swallowed");
}

void modelFailureReportsWhatWasAlreadyDone() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        if (turn(input).empty()) return {"start_task", {{"request", "Write a release note"}}, "Start it"};
        throw ModelError("The response was malformed", false);
    };
    const auto message = receive(store, "failure", "Write a release note and a changelog");
    execution.runtime.runConversations();
    const auto response = reply(store, message);
    check(response["route"] == "error" && response["status"] == "working" &&
              response["text"].get<std::string>().find("Already done for this message") != std::string::npos &&
              response["text"].get<std::string>().find("Write a release note") != std::string::npos &&
              Tasks(store).tasks("alpha").size() == 1,
          "The error reply hid the work the turn had already started");
}

void notesFailureEndsTheTurnWithoutWork() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    execution.notes = [](std::string_view) -> std::string { throw std::runtime_error("SOUL.md is unreadable"); };
    execution.model.conversation = [](const ModelInput&) -> Decision { throw std::runtime_error("The model was called without notes"); };
    const auto message = receive(store, "notes", "Write a release note");
    execution.runtime.runConversations();
    const auto response = reply(store, message);
    check(response["route"] == "error" && response["text"].get<std::string>().find("SOUL.md is unreadable") != std::string::npos &&
              Tasks(store).tasks("alpha").empty(),
          "A notes failure started work or hid the cause");
}

void toolErrorsReturnToTheModel() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto mine = task(writer, "Prepare the release");
    const auto foreign = task(writer, "Beta work", "beta");
    const auto before = taskJson(Tasks(store).task(mine));
    const std::vector<Decision> steps{
        {"start_task", {{"request", "Run it"}, {"environment", "mars"}}, "Unknown environment"},
        {"change_task",
         {{"taskId", mine}, {"generation", before.at("generation")}, {"revisionId", 0}, {"instruction", "Change nothing"}},
         "Missing fields"},
        {"send_to_task", {{"taskId", foreign}, {"generation", 0}, {"instruction", "Hi"}}, "Another Bot's task"},
        {"send_to_task", {{"taskId", 999999}, {"generation", 0}, {"instruction", "Hi"}}, "Missing task"},
        {"send_to_task", {{"taskId", mine}, {"generation", before.at("generation").get<Id>() + 5}, {"instruction", "Hi"}}, "Stale"},
        {"launch_rocket", Json::object(), "Unknown tool"},
        say("Pick one", Json::array({"a", "b", "c", "d", "e"})),
        say("Which release should I prepare?", Json::array({"1.4", "1.5"})),
    };
    execution.model.conversation = [&](const ModelInput& input) { return steps.at(turn(input).size()); };
    const auto message = receive(store, "errors", "Prepare it");
    execution.runtime.runConversations();
    const auto results = execution.model.turns.back().event.at("turn");
    check(results.size() == 7 && std::ranges::all_of(results, [](const Json& item) { return item["result"]["ok"] == false; }),
          "An invalid call was applied or not reported");
    check(results[2]["result"]["error"] == "No task with taskId " + std::to_string(foreign) &&
              results[3]["result"]["error"] == "No task with taskId 999999",
          "Another Bot's task was distinguishable from a missing one");
    check(results[4]["result"]["error"] == "stale" && results[4]["result"]["current"]["generation"] == before.at("generation"),
          "A stale call did not return the current version");
    check(taskJson(Tasks(store).task(mine)) == before && taskJson(Tasks(store).task(foreign))["pendingInstructions"].is_null() &&
              Tasks(store).tasks("alpha").size() == 1,
          "A failed call changed a task");
    const auto response = reply(store, message);
    check(response["route"] == "reply" && response["suggestions"] == Json::array({"1.4", "1.5"}) && response["status"] == "done",
          "The reply or its suggestions were not saved");
}

void boundedContextAndOlderTasks() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    constexpr auto oldName = "古い計測計画";
    const auto old = task(writer, oldName);
    execution.finish(store, old, "Compare paired measurements using the same workload on both builds.");
    const auto original = lastTaskEvent(store, old, "completed");
    for (int i = 0; i < 30; ++i) execution.finish(store, task(writer, "New completed plan " + std::to_string(i)), "Latest plan");
    const auto foreign = task(writer, "Beta hidden project fixture", "beta");
    execution.finish(store, foreign, "Beta-only result fixture");
    for (int i = 0; i < 20; ++i) task(writer, "Outstanding plan " + std::to_string(i));
    const auto before = taskJson(Tasks(store).tasks("alpha", 0, 200));
    const auto launches = execution.t3.launches;

    Json found, read;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        const auto encoded = input.task.dump() + input.event.dump() + input.history.dump();
        check(encoded.find("Beta hidden project fixture") == std::string::npos &&
                  encoded.find("Beta-only result fixture") == std::string::npos,
              "Another Bot leaked into the turn");
        check(input.task.at("tasks").size() <= 20 && input.task.at("moreTasks") == true && encoded.size() < 128 * 1024,
              "The turn context grew with the Bot's full history");
        const auto& steps = turn(input);
        if (steps.empty()) return {"find_tasks", {{"query", "計測計画"}}, "Find the older plan"};
        if (steps.size() == 1) {
            found = steps[0]["result"];
            return {"read_result", {{"messageId", found["tasks"][0]["latestResult"]["messageId"]}}, "Read it"};
        }
        read = steps[1]["result"];
        return say("同じ負荷で比較する計画です。");
    };
    const auto message = receive(writer, "named", "「古い計測計画」を要約して");
    execution.runtime.runConversations();
    check(found["tasks"].size() == 1 && found["tasks"][0]["taskId"] == old &&
              found["tasks"][0]["generation"] == Tasks(store).task(old).task.generation && found["tasks"][0].contains("revisionId") &&
              read["text"] == "Compare paired measurements using the same workload on both builds." && read["end"] == true &&
              read["currentRevision"] == true && original.at("id") == read["messageId"],
          "An older task outside the list could not be found and read");
    check(reply(store, message)["actions"].empty() && taskJson(Tasks(store).tasks("alpha", 0, 200)) == before &&
              execution.t3.launches == launches,
          "Reading an older result changed work");
}

void longResultsAreReadInChunks() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto id = task(store, "Long report");
    const std::string report = std::string(5990, 'a') + "あいうえお" + std::string(1500, 'b');
    execution.finish(store, id, report);
    const auto result = lastTaskEvent(store, id, "completed").at("id");
    std::vector<Json> chunks;
    execution.model.conversation = [&](const ModelInput& input) -> Decision {
        const auto& steps = turn(input);
        if (!steps.empty()) chunks.push_back(steps.back()["result"]);
        if (steps.empty()) return {"read_result", {{"messageId", result}}, "Read"};
        if (steps.size() == 1) return {"read_result", {{"messageId", result}, {"offset", steps[0]["result"]["nextOffset"]}}, "More"};
        return say("Read it.");
    };
    receive(store, "chunks", "Summarize the long report");
    execution.runtime.runConversations();
    check(chunks.size() == 2 && chunks[0]["end"] == false && chunks[1]["end"] == true &&
              chunks[0]["text"].get<std::string>() + chunks[1]["text"].get<std::string>() == report,
          "A long result was not readable in UTF-8 safe chunks");
}

void budgetPauseLetsLaterMessagesRunAndApprovalResumes() {
    Fixture fixture;
    Store store(fixture.database());
    auto limited = Fixture::bot();
    limited.config["modelCallLimit"] = 14;
    int calls = 0;
    const auto script = [&](const ModelInput& input) -> Decision {
        if (input.event["text"] == "Thanks") return say("You're welcome.");
        if (++calls <= 27) return {"find_tasks", Json::object(), "Look"};
        return say("Nothing yet.");
    };
    const auto paused = receive(store, "limited", "What is running?", 0, limited);
    const auto later = receive(store, "later", "Thanks");
    {
        RuntimeFixture execution(store);
        execution.model.conversation = script;
        execution.runtime.runConversations();
    }
    const auto pauses = Conversations(store).pausedConversations("alpha");
    check(calls == 14 && pauses.size() == 1 && pauses[0]["allowance"] == 14 && pauses[0]["route"] == "model_budget" &&
              reply(store, paused)["route"] == "model_budget" && reply(store, later)["text"] == "You're welcome.",
          "A paused turn blocked a later message or was not reported");
    const auto waitMessage = pauses[0]["messageId"].get<Id>();
    const InboxTarget target{"alpha",    InboxKind::conversation_model_budget, pauses.at(0).at("replyToMessageId"), waitMessage, 0, 0,
                             waitMessage};
    Store reopened(fixture.database());
    const auto history = Conversations(reopened).conversation("alpha");
    const auto input = std::ranges::find_if(history, [&](const Json& row) { return row.at("id") == paused; });
    check(input != history.end() && input->at("data").at("status") == "needs_you", "Reopening the journal lost a persisted pause");
    const auto beforeApproval = Conversations(reopened).conversation("alpha", 0, 2);
    const auto cursor = Conversations(reopened).conversationChangesSince(std::nullopt, "alpha").through;
    std::barrier ready(2);
    const auto approve = [&](Store& connection) {
        ready.arrive_and_wait();
        try {
            Inbox(connection).decideInbox(localOwner, target, "approve");
            return true;
        } catch (const DecisionUnavailable&) {
            return false;
        }
    };
    auto first = std::async(std::launch::async, [&] { return approve(store); });
    const auto second = approve(reopened);
    check(second != first.get(), "The same pause was accepted twice or both approvals failed");
    const auto changes = Conversations(reopened).conversationChangesSince(cursor, "alpha");
    const auto afterApproval = Conversations(reopened).conversation("alpha", 0, 2);
    check(changes.through > cursor && changes.messages.at("alpha") == std::vector<Id>{paused, waitMessage} && afterApproval.size() == 2 &&
              afterApproval.at(0).at("id") == beforeApproval.at(0).at("id") &&
              afterApproval.at(1).at("id") == beforeApproval.at(1).at("id"),
          "Approval did not invalidate both entries or consumed a visible page slot");
    check(reply(reopened, paused).at("status") == "done" &&
              fixture.count("SELECT count(*) FROM journal WHERE conversation_status='done' AND id=" + std::to_string(waitMessage)) == 1,
          "Approval did not update the saved pause and its status column together");
    bool rejected = false;
    try {
        Inbox(store).decideInbox(localOwner, target, "approve");
    } catch (const Conflict&) {
        rejected = true;
    }
    check(rejected && Conversations(store).pausedConversations("alpha").empty(), "The same pause was approved twice");
    Store afterRestart(fixture.database());
    RuntimeFixture resumed(afterRestart);
    resumed.model.conversation = script;
    resumed.runtime.runConversations();
    Json answered;
    for (const auto& row : Conversations(store).conversation("alpha", 0, 200))
        if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == paused && row["data"]["route"] == "reply")
            answered = row["data"];
    check(answered["text"] == "Nothing yet." && calls == 28 &&
              fixture.count("SELECT count(*) FROM model_attempts WHERE message_id=" + std::to_string(paused)) == 28 &&
              fixture.count("SELECT call_limit FROM model_budgets WHERE id='message/" + std::to_string(paused) + "'") == 28,
          "The full renewed allowance did not survive restart or was granted twice");
}

void oneApprovalResumesSharedBudgetWaits(bool approveTask) {
    Fixture fixture;
    Store store(fixture.database());
    auto limited = Fixture::bot();
    limited.config["modelCallLimit"] = 3;
    auto unrelated = Fixture::bot("beta");
    unrelated.config["modelCallLimit"] = 1;
    const auto message = receive(store, "shared", "Write a release note", 0, limited);
    receive(store, "unrelated", "Check another request", 0, unrelated);
    const auto script = [&](const ModelInput& input) -> Decision {
        const auto steps = turn(input).size();
        if (input.event.at("text") == "Check another request" || (steps > 0 && steps < 3))
            return {"find_tasks", Json::object(), "Look up the work"};
        if (steps == 0) return {"start_task", {{"request", "Write a release note"}}, "Start it"};
        return say("I started the release note.");
    };
    Id taskId = 0;
    {
        RuntimeFixture execution(store);
        execution.model.conversation = script;
        execution.runtime.runConversations();
        taskId = Tasks(store).tasks("alpha").at(0).task.id;
        execution.reach(store, taskId, "waiting");
    }
    const auto task = Tasks(store).task(taskId);
    const auto taskTarget = TaskTarget::from(task.task, task.wait.value().id);
    const auto pause = Conversations(store).pausedConversations("alpha").at(0).at("messageId").get<Id>();
    const InboxTarget turnTarget{"alpha", InboxKind::conversation_model_budget, message, pause, 0, 0, pause};
    check(task.wait->reason() == WaitReason::model_budget && Conversations(store).pausedConversations("beta").size() == 1,
          "The fixture did not exhaust the shared and independent allowances");
    const auto beforeApproval = Conversations(store).conversationChangesSince(std::nullopt, "alpha").through;
    if (approveTask)
        Tasks(store).operate(localOwner, taskTarget, "approve");
    else
        Inbox(store).decideInbox(localOwner, turnTarget, "approve");
    const auto changes = Conversations(store).conversationChangesSince(beforeApproval, "alpha");
    check(Conversations(store).pausedConversations("alpha").empty() && taskJson(Tasks(store).task(taskId)).at("wait").is_null() &&
              Conversations(store).pausedConversations("beta").size() == 1 &&
              changes.messages.at("alpha") == std::vector<Id>{message, pause},
          "Renewal did not clear both shared pauses, invalidate the conversation, or preserve the independent pause");
    bool rejected = false;
    try {
        if (approveTask)
            Inbox(store).decideInbox(localOwner, turnTarget, "approve");
        else
            Tasks(store).operate(localOwner, taskTarget, "approve");
    } catch (const Conflict&) {
        rejected = true;
    }
    check(rejected, "A sibling pause could grant another block from a stale approval");

    Store reopened(fixture.database());
    RuntimeFixture resumed(reopened);
    resumed.model.conversation = script;
    resumed.runtime.runConversations();
    resumed.finish(reopened, taskId, "The release note is ready.");
    auto events = Conversations(reopened).conversation("alpha", 0, 200);
    check(std::ranges::any_of(events,
                              [&](const Json& row) {
                                  return row.at("kind") == "conversation_reply" && row.at("data").at("replyToMessageId") == message &&
                                         row.at("data").at("text") == "I started the release note.";
                              }) &&
              Inbox(reopened).inbox(localOwner, "alpha").empty() &&
              fixture.count("SELECT call_limit FROM model_budgets WHERE id='message/" + std::to_string(message) + "'") == 6,
          "Shared work did not finish after restart with exactly one new block");
}

void notesFailureReleasesUnusedApproval() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    auto limited = Fixture::bot();
    limited.config["modelCallLimit"] = 1;
    execution.model.conversation = [](const ModelInput&) -> Decision {
        return {"start_task", {{"request", "Write a release note"}}, "Start it"};
    };
    const auto message = receive(store, "approval-notes", "Write a release note", 0, limited);
    execution.runtime.runConversations();
    const auto tasks = taskJson(Tasks(store).tasks("alpha"));
    const auto pauses = Conversations(store).pausedConversations("alpha");
    check(tasks.size() == 1 && pauses.size() == 1, "The turn did not start a task and pause for approval");
    const Id pause = pauses.at(0).at("messageId");
    Inbox(store).decideInbox(localOwner, {"alpha", InboxKind::conversation_model_budget, message, pause, 0, 0, pause}, "approve");

    execution.notes = [](std::string_view) -> std::string { throw std::runtime_error("SOUL.md is unreadable"); };
    execution.runtime.runConversations();
    const auto failure = Conversations(store).conversation("alpha", 0, 1).at(0).at("data");
    check(execution.model.turns.size() == 1 && failure.at("route") == "error" && failure.at("replyToMessageId") == message,
          "The resumed turn did not fail before model dispatch");
    check(fixture.count("SELECT count(*) FROM model_attempts WHERE message_id=" + std::to_string(message)) == 1,
          "The failed turn consumed an unused approved call");

    execution.notes = [](std::string_view) { return std::string{}; };
    execution.reach(store, tasks.at(0).at("id"), "running");
    check(execution.t3.launches == 1, "The shared approval did not allow the task to launch once");
}

void replyTargetsReachTheTurn() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto id = task(writer, "Measure startup");
    execution.finish(store, id, "Startup took 100ms");
    const auto result = lastTaskEvent(store, id, "completed").at("id").get<Id>();
    const auto foreignMessage = receive(writer, "foreign", "Hello beta", 0, Fixture::bot("beta"));
    Json seen = Json::array();
    execution.model.conversation = [&](const ModelInput& input) {
        if (input.event.at("text") != "Hello beta") seen.push_back(input.event.at("replyTo"));
        return say("Noted.");
    };
    const auto first = receive(writer, "about-result", "Why so slow?", result);
    execution.runtime.runConversations();
    receive(writer, "about-reply", "What do you mean?", reply(store, first).at("messageId"));
    receive(writer, "about-foreign", "And this?", foreignMessage);
    execution.runtime.runConversations();
    check(seen.size() == 3 && seen[0]["kind"] == "result" && seen[0]["taskId"] == id && seen[0]["text"] == "Startup took 100ms",
          "A reply to a result did not reach the turn");
    check(seen[1]["kind"] == "bot_message" && seen[1]["text"] == "Noted." && seen[1]["inReplyTo"]["text"] == "Why so slow?",
          "A reply to a Bot message lost the message it answered");
    check(seen[2]["kind"] == "unavailable", "A reply to another Bot's message was exposed");
}

void conversationPagesCombineTaskAndMessageHistory() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    execution.model.conversation = [](const ModelInput&) { return say("Noted."); };

    const auto older = Tasks(store).history(task(store, "Older task")).at(0).at("id").get<Id>();
    const auto message = receive(store, "message", "Hello alpha");
    task(store, "Another Bot's task", "beta");
    const auto newer = Tasks(store).history(task(store, "Newer task")).at(0).at("id").get<Id>();
    const auto cursor = Conversations(store).conversationChangesSince(std::nullopt, "alpha").through;
    execution.runtime.runConversations();

    const auto latest = Conversations(store).conversation("alpha", 0, 2);
    check(latest.size() == 2 && latest.at(0).at("id") == newer && latest.at(1).at("kind") == "conversation_reply" &&
              latest.at(1).at("data").at("replyToMessageId") == message,
          "The newest page did not merge task and reply history in order");
    const auto earlier = Conversations(store).conversation("alpha", newer, 2);
    check(earlier.size() == 2 && earlier.at(0).at("id") == older && earlier.at(1).at("id") == message &&
              earlier.at(1).at("data").at("text") == "Hello alpha" && Conversations(store).conversation("alpha", older, 2).empty(),
          "An older page skipped, repeated, or lost details of a visible entry");

    using Events = Conversations::ConversationEvents;
    const auto incoming = Conversations(store).conversation("alpha", 0, 2, Events::incoming);
    check(incoming == Json::array({latest.at(1)}) &&
              Conversations(store).conversation("alpha", latest.at(1).at("id"), 2, Events::incoming).empty(),
          "The incoming-only page included outgoing history or repeated its cursor");
    const auto changes = Conversations(store).conversationChangesSince(cursor, "alpha");
    check(changes.through == latest.at(1).at("id") && changes.messages.at("alpha") == std::vector<Id>{message},
          "A new reply did not invalidate the earlier message");
}
}

int main() {
    try {
        retryAcrossConnections();
        for (bool duringTick : {false, true}) {
            dueAutomationWakesAfterIdle(false, duringTick);
            dueAutomationWakesAfterIdle(true, duringTick);
        }
        failedAutomationDoesNotInvalidateOtherMaintenance();
        savedDecisionIsAppliedWithoutAnotherModelCall();
        missingToolCannotBeAddedAfterInferenceStarts();
        protectedArgumentKeyCannotReachTaskIntake();
        rejectedSessionReadStillAllowsAnOrdinaryReply();
        savedCallsAreCheckedBeforeExecution();
        transientFailureResumesAfterTheLastFinishedStep();
        sessionNoteIsNotResentAfterARestart(false, true);
        sessionNoteIsNotResentAfterARestart(true, true);
        sessionNoteIsNotResentAfterARestart(false, false);
        concurrentWorkerRecoversFromWriterContention(false);
        concurrentWorkerRecoversFromWriterContention(true);
        repairSurvivesRestart(false);
        repairSurvivesRestart(true);
        invalidResponseRepairRespectsBudget(false);
        invalidResponseRepairRespectsBudget(true);
        concurrentWorkerSurfacesStructuralStorageFailure();
        shutdownLeavesAnUnsentRunReadyForRestart();
        modelFailureReportsWhatWasAlreadyDone();
        notesFailureEndsTheTurnWithoutWork();
        toolErrorsReturnToTheModel();
        boundedContextAndOlderTasks();
        longResultsAreReadInChunks();
        budgetPauseLetsLaterMessagesRunAndApprovalResumes();
        oneApprovalResumesSharedBudgetWaits(false);
        oneApprovalResumesSharedBudgetWaits(true);
        notesFailureReleasesUnusedApproval();
        replyTargetsReachTheTurn();
        conversationPagesCombineTaskAndMessageHistory();
        std::cout << "Conversation Store acceptance passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
