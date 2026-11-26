#include "flock/tasks.hpp"
#include "flock/secret_ref.hpp"
#include "flock/automations.hpp"
#include "flock/bot_memory.hpp"
#include "flock/board_inbox.hpp"
#include "flock/workspace_tools.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/session_ref.hpp"
#include "flock/conversations.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <sqlite3.h>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace flockv0;
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

Decision call(std::string tool, Json args = Json::object()) {
    return {std::move(tool), std::move(args), "Check the fixture evidence"};
}

const auto search = call("flock_session_search");
const auto readiness = call("check_environment_readiness");
const auto invoke = call("mcp_call_tool", {{"server", "fixture"}, {"name", "lookup"}, {"arguments", Json::object()}});
const auto readResource = call("mcp_read_resource", {{"server", "fixture"}, {"uri", "fixture://data"}});

struct FixedModel : Model {
    ModelTurn first;
    std::vector<ModelTurn> followUps;
    std::vector<Json> turns;
    ModelTurn decide(const ModelInput& input) override {
        check(input.event.at("kind") == "conversation_turn", "A batched tool started a task");
        turns.push_back(input.event.at("turn"));
        if (turns.back().empty()) return first;
        if (turns.size() > 1 && turns.size() - 2 < followUps.size()) return followUps.at(turns.size() - 2);
        return {{call("reply", {{"text", "Reviewed the results."}})}};
    }
};

struct FakeT3 : T3 {
    std::atomic_int searches = 0, readinessChecks = 0, eventReads = 0;
    bool knownSession = false;
    std::function<void()> onSearch, onReadiness, onEvents;
    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    std::optional<Delivery> preflight(const Command&) override {
        ++readinessChecks;
        if (onReadiness) onReadiness();
        return std::nullopt;
    }
    std::pair<std::vector<std::string>, std::optional<Id>> projectSessions(std::string_view, std::string_view, Id,
                                                                           SessionReadDeadline) override {
        ++searches;
        if (onSearch) onSearch();
        if (knownSession) return {{"fixture-thread"}, std::nullopt};
        return {};
    }
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) override {
        return SessionMetadata{"Fixture Session",      "Fixture Project",      "project", "completed",
                               "2026-11-21T00:00:00Z", "2026-11-21T00:00:00Z", {},        false};
    }
    std::optional<SessionCounts> sessionCounts(const SessionTarget&) override {
        ++eventReads;
        if (onEvents) onEvents();
        return SessionCounts{7, 2};
    }
    Delivery send(const Command&) override { throw std::runtime_error("A batched tool dispatched work"); }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};

struct FakeMcp : McpTools {
    std::atomic_int calls = 0, reads = 0;
    std::function<void()> onHasServer, onReadOnlyHint, onRead;
    bool hasServer(std::string_view server) const override {
        if (onHasServer) onHasServer();
        return server == "fixture";
    }
    bool readOnlyHint(std::string_view, std::string_view) override {
        if (onReadOnlyHint) onReadOnlyHint();
        return false;
    }
    Json discover(std::string_view, std::string_view, McpCollection, std::string_view) override { return {}; }
    McpToolResult invoke(std::string_view, std::string_view, const Json&) override {
        ++calls;
        return {{{"observed", "invoked"}}, false};
    }
    Json readResource(std::string_view, std::string_view) override {
        ++reads;
        if (onRead) onRead();
        return {{"invocations", calls.load()}};
    }
};

struct Fixture {
    fs::path home;
    FixedModel model;
    FakeT3 t3;
    std::shared_ptr<BotTools> tools = std::make_shared<BotTools>();
    std::shared_ptr<FakeMcp> mcp = std::make_shared<FakeMcp>();
    std::unique_ptr<Store> store;
    std::unique_ptr<Runtime> runtime;
    Id message = 0;
    int sequence = 0;
    std::function<void(std::string_view)> contention;

    Fixture() {
        char pattern[] = "/tmp/flock-conversation-batch-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        check(directory != nullptr, "Could not create batch fixture");
        home = fs::canonical(directory);
        tools->mcp = mcp;
        restart();
    }
    ~Fixture() {
        runtime.reset();
        store.reset();
        fs::remove_all(home);
    }
    void restart() {
        runtime.reset();
        store.reset();
        store = std::make_unique<Store>(home / "state.db");
        RuntimeOptions options;
        options.botTools = tools;
        options.storageContention = contention;
        runtime = std::make_unique<Runtime>(*store, model, t3, [](auto) { return std::string{}; }, options);
    }
    void receive(std::vector<Decision> calls, int limit = 20) {
        model.first = {std::move(calls)};
        model.followUps.clear();
        model.turns.clear();
        message = Conversations(*store)
                      .receive(localOwner, {"alpha", "", "env", "project", {{"modelCallLimit", limit}}}, std::to_string(++sequence),
                               "Check the fixture")
                      .at("messageId");
    }
    auto connect() const {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open((home / "state.db").c_str(), &raw);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
        check(opened == SQLITE_OK, "Could not open batch fixture database");
        return connection;
    }
    static void write(sqlite3* db, const std::string& statement) {
        check(sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "Fixture write failed");
    }
    std::string query(const std::string& statement) const {
        std::string result;
        check(sqlite3_exec(
                  connect().get(), statement.c_str(),
                  [](void* value, int size, char** values, char**) {
                      if (size && values[0]) *static_cast<std::string*>(value) = values[0];
                      return 0;
                  },
                  &result, nullptr) == SQLITE_OK,
              "Fixture query failed");
        return result;
    }
    std::string whereStep(int step = 1) const {
        return " WHERE message_id=" + std::to_string(message) + " AND step=" + std::to_string(step);
    }
    std::string status() const { return query("SELECT status FROM conversation_turns WHERE message_id=" + std::to_string(message)); }
    void checkFinished(int attempts) const {
        const auto count =
            query("SELECT count(*) FROM model_attempts WHERE purpose='conversation_turn' AND message_id=" + std::to_string(message));
        if (count != std::to_string(attempts))
            throw std::runtime_error("Expected " + std::to_string(attempts) + " model attempts, found " + count + " for " +
                                     model.first.calls.front().tool + " (" + std::to_string(model.first.calls.size()) + " slots)");
        check(query("SELECT count(*) FROM conversation_steps s JOIN model_attempts a ON a.id=s.attempt_id"
                    " AND a.message_id=s.message_id AND a.purpose='conversation_turn' WHERE s.message_id=" +
                    std::to_string(message)) == std::to_string(attempts),
              "Conversation steps lost model attempt attribution");
        check(status() == "done" && query("SELECT json_extract(j.data,'$.text') FROM conversation_turns t"
                                          " JOIN journal j ON j.id=t.reply_id WHERE t.message_id=" +
                                          std::to_string(message)) == "Reviewed the results.",
              "The batch never advanced to the model's follow-up reply");
    }
    Json results(std::string_view column = "result", int step = 1) const {
        return Json::parse(query("SELECT " + std::string(column) + " FROM conversation_steps" + whereStep(step)));
    }
    void decideApproval(bool allow) {
        const auto items = Inbox(*store).inbox(localOwner);
        check(items.size() == 1 && items[0].at("itemKind") == "policy_approval", "Expected one approval card");
        Inbox(*store).decideInbox(localOwner, {"alpha", InboxKind::policy_approval, items[0].at("itemId"), items[0].at("waitId"), 0, 0, 0},
                                  allow ? "allow_once" : "deny");
    }
    void requireApproval(const Resource& resource) {
        Policy policy{*store};
        policy.setMode(localOwner, "supervised");
        policy.changeRule(localOwner, RuleKind::ask, resource.action, resource.target, true);
    }
};

void mixedReads() {
    std::mutex mutex;
    std::condition_variable ready;
    unsigned arrived = 0;
    Fixture f;
    const auto rendezvous = [&] {
        std::unique_lock lock(mutex);
        ++arrived;
        ready.notify_all();
        if (!ready.wait_for(lock, 3s, [&] { return arrived == 2; })) throw Conflict("Reads did not overlap");
    };
    f.t3.onSearch = f.t3.onReadiness = rendezvous;
    f.t3.knownSession = true;
    f.receive({search, readiness}, 2);
    f.runtime->runConversations();
    f.checkFinished(2);
    check(f.t3.searches == 1 && f.t3.readinessChecks == 1 && f.model.turns.size() == 2, "Expected two reads and exactly one reply attempt");
    const auto& turn = f.model.turns[1];
    const auto results = f.results();
    check(turn.size() == 2 && turn[0].at("tool") == search.tool && turn[1].at("tool") == readiness.tool,
          "The model did not receive results in call order");
    check(turn[0].at("step") == 1 && turn[1].at("step") == 1 && turn[0].at("callIndex") == 1 && turn[1].at("callIndex") == 2,
          "The model lost the original call slots");
    check(results[0].at("result").at("ok") == true && results[0].at("sessions").size() == 1 &&
              turn[0].at("result").at("sessions").at(0).at("title") == "Fixture Session",
          "The Session search result did not reach the model");
    check(results[1] == Json({{"ok", true}, {"ready", true}}) && turn[1].at("result") == results[1],
          "The readiness result did not reach the model");
}

void approval(bool allow) {
    Fixture f;
    Policy{*f.store}.setMode(localOwner, "supervised");
    f.receive({search, invoke, readResource});
    f.runtime->runConversations();
    const auto slots = f.results("effect");
    check(f.status() == "policy_waiting" && slots[0].at("result").at("ok") == true && slots[1].is_null() && slots[2].is_null() &&
              f.t3.searches == 1 && f.mcp->calls == 0 && f.mcp->reads == 0 && f.model.turns.size() == 1,
          "Approval failed to hold a dependent call or preserve an independent result");
    check(f.query("SELECT operation FROM policy_approvals") == "turn/" + std::to_string(f.message) + "/1/1",
          "The policy approval lost its call index");
    f.decideApproval(allow);
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.t3.searches == 1 && f.mcp->calls == (allow ? 1 : 0) && f.mcp->reads == 1 && results[0] == slots[0] &&
              results[1].at("ok") == allow && results[2].at("data").at("invocations") == (allow ? 1 : 0),
          "Approval settlement repeated a sibling or let a dependent call overtake it");
}

void checkStoreResponsiveDuringProviderCall(Fixture& f, std::function<void()>& providerCall) {
    std::promise<void> entered, release;
    auto released = release.get_future();
    providerCall = [&] {
        entered.set_value();
        released.wait();
    };
    auto batch = std::async(std::launch::async, [&] { f.runtime->runConversations(); });
    const bool reachedProvider = entered.get_future().wait_for(3s) == std::future_status::ready;
    auto inbox = std::async(std::launch::async, [&] { return Inbox(*f.store).inbox(localOwner); });
    const bool servedInbox = inbox.wait_for(3s) == std::future_status::ready;
    release.set_value();
    inbox.get();
    batch.get();
    providerCall = {};
    check(reachedProvider, "The batch did not reach the provider call");
    check(servedInbox, "A provider call held the store mutex");
    f.checkFinished(2);
}

void storeRemainsResponsiveDuringMcpCapabilityChecks() {
    for (const auto callback : {&FakeMcp::onHasServer, &FakeMcp::onReadOnlyHint}) {
        Fixture f;
        f.receive({invoke, search});
        checkStoreResponsiveDuringProviderCall(f, f.mcp.get()->*callback);
        check(f.mcp->calls == 1, "The MCP call did not complete after checking capabilities");
    }
}

void followUpNeeded(const Decision& action, std::string_view guidance) {
    Fixture f;
    f.receive({search, action}, 2);
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.query("SELECT count(*) FROM tasks") == "0" && f.t3.searches == 1 && results[0].at("result").at("ok") == true &&
              results[1] == Json({{"ok", false}, {"notRun", true}, {"error", guidance}}) &&
              f.model.turns.at(1)[1].at("result") == results[1],
          "The standalone action ran in the batch or its result was lost");
}

void gatewayOnlyCancelDoesNotCancelBatchSibling() {
    Fixture f;
    const auto task = Tasks(*f.store).submit({"alpha", "Keep this task running", "env", "project"});
    const auto generation = Tasks(*f.store).task(task).task.generation;
    f.receive({call("cancel_task", {{"taskId", task}, {"generation", generation}, {"gatewayEnvironmentId", "env"}}), search});
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    const auto first = results[0].value("result", results[0]);
    const auto second = results[1].value("result", results[1]);
    check((first.value("ok", true) == false || first.contains("error")) && second.value("ok", false) == true &&
              Tasks(*f.store).task(task).task.state == TaskState::queued && f.t3.searches == 1,
          ("a gateway-only batch cancel changed the task or suppressed its sibling: " + results.dump()).c_str());
}

void conflictingWorkspaceCalls() {
    Fixture f;
    f.receive({call("workspace_write_file", {{"path", "ordered.txt"}, {"text", "Fresh contents"}}),
               call("workspace_read_file", {{"path", "ordered.txt"}})},
              4);
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(results[0].at("ok") == true && results[1].at("ok") == true && results[1].at("text") == "Fresh contents",
          "The dependent workspace read overtook the write");
}

void rejectedCall() {
    Fixture f;
    f.receive({search, readiness}, 2);
    f.model.first.rejections.emplace(1, "Invalid environment arguments");
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.t3.searches == 1 && f.t3.readinessChecks == 0 && results[0].at("result").at("ok") == true &&
              results[1].at("error") == "Invalid environment arguments",
          "The adapter rejection did not stay in its slot");
}

void restartWithPartialResults() {
    Fixture f;
    f.receive({search, readiness, invoke, readResource}, 5);
    const auto id = std::to_string(f.message);
    const auto budget = "'message/" + id + "'";
    auto decisions = Json::array();
    for (const auto& action : f.model.first.calls)
        decisions.push_back({{"tool", action.tool}, {"args", action.args}, {"reason", action.reason}});
    const Json completed{{"result", {{"ok", true}, {"sessions", Json::array()}, {"marker", "completed before restart"}}},
                         {"sessions", Json::array()}};
    const Json inFlight{{"inFlight", true}};
    const auto slots = Json::array({completed, inFlight, inFlight, inFlight});
    Fixture::write(f.connect().get(), "INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(" + budget +
                                          ",5);"
                                          "UPDATE model_budgets SET attempts=3 WHERE id=" +
                                          budget +
                                          ";"
                                          "INSERT INTO model_attempts(budget_id,purpose,time,message_id) VALUES(" +
                                          budget + ",'conversation_turn',1," + id + "),(" + budget + ",'bot_tool',1," + id + "),(" +
                                          budget + ",'bot_tool',1," + id +
                                          ");"
                                          "INSERT INTO conversation_steps(message_id,step,attempt_id,decision,effect) VALUES(" +
                                          id + ",1,(SELECT id FROM model_attempts WHERE purpose='conversation_turn' AND message_id=" + id +
                                          "),'" + decisions.dump() + "','" + slots.dump() + "');");
    f.restart();
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.model.turns.size() == 1 && f.t3.searches == 0 && f.t3.readinessChecks == 1 && f.mcp->calls == 0 && f.mcp->reads == 1 &&
              results[0] == completed && results[1].at("ready") == true && results[2].at("delivery") == "unknown" &&
              results[3].at("ok") == true &&
              f.query("SELECT count(*) FROM model_attempts WHERE purpose='bot_tool' AND message_id=" + id) == "3",
          "Restart repeated completed work, lost an interrupted read, or resent an uncertain invocation");
}

void contendedSlotPersistence() {
    std::promise<void> entered, release;
    const auto resume = release.get_future().share();
    Fixture f;
    auto locker = f.connect();
    bool reported = false;
    f.contention = [&](std::string_view error) {
        if (error.starts_with("conversation worker")) {
            Fixture::write(locker.get(), "ROLLBACK");
            reported = true;
        }
    };
    f.restart();
    f.t3.onReadiness = [&] {
        if (f.t3.readinessChecks != 1) return;
        entered.set_value();
        resume.wait();
    };
    f.receive({search, readiness}, 2);
    f.runtime->tickConcurrent();
    bool searchSaved = false;
    if (entered.get_future().wait_for(5s) == std::future_status::ready) {
        for (const auto deadline = std::chrono::steady_clock::now() + 5s; !searchSaved && std::chrono::steady_clock::now() < deadline;
             std::this_thread::sleep_for(5ms))
            searchSaved = f.query("SELECT json_extract(effect,'$[0].result.ok') FROM conversation_steps" + f.whereStep()) == "1";
    }
    if (searchSaved) Fixture::write(locker.get(), "BEGIN IMMEDIATE");
    release.set_value();
    check(searchSaved, "The first slot did not persist while its sibling was in flight");
    for (const auto deadline = std::chrono::steady_clock::now() + 25s; f.status() != "done" && std::chrono::steady_clock::now() < deadline;
         std::this_thread::sleep_for(5ms))
        f.runtime->tickConcurrent();
    f.checkFinished(2);
    check(reported && f.t3.searches == 1 && f.t3.readinessChecks == 1 && f.results()[1].at("ready") == true,
          "SQLite writer contention duplicated a completed slot or lost its result");
}

Decision sessionEvents(Fixture& f) {
    f.t3.knownSession = true;
    f.receive({search});
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto& session = f.model.turns.back().at(0).at("result").at("sessions").at(0);
    return call("flock_session_events", {{"sessionRef", session.at("sessionRef")}, {"operation", "counts"}});
}

void compareWithSingle(Fixture& f, const Decision& read) {
    f.receive({read});
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto single = f.results().at(0);
    const auto projected = f.model.turns.back().at(0).at("result");
    check(projected.at("ok") == true, "The single read fixture failed");

    f.receive({search, read});
    f.runtime->runConversations();
    f.checkFinished(2);
    check(f.results().at(0).at("result").at("ok") == true, "Session search did not run alongside the read");
    if (f.results().at(1) != single || f.model.turns.back().at(1).at("result") != projected)
        throw std::runtime_error(read.tool + " differed from its single-call result");
}

constexpr auto sharedDoc = "fixture/playbook";

void conversationReadParity() {
    Fixture f;
    const Intake intake{"alpha", "Fixture task", "env", "project"};
    const auto task = Tasks(*f.store).submit(intake, &localOwner);
    Tasks(*f.store).operate(localOwner, TaskTarget::from(Tasks(*f.store).task(task).task, 0), "cancel");
    const auto result = std::stoll(f.query("SELECT id FROM journal WHERE kind='cancelled' AND task_id=" + std::to_string(task)));
    f.receive(
        {call("start_workflow", {{"request", "Collect the release notes"}, {"stages", {{{"stageId", "collect"}, {"role", "collect"}}}}})});
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto workflow = f.results().at(0).at("workflowId");
    Automations(*f.store).createAutomation(
        localOwner, intake, {{"kind", "schedule"}, {"cron", "0 9 * * 1"}, {"timezone", "Etc/UTC"}, {"request", "Weekly progress"}},
        unixTime());
    Policy policy{*f.store};
    const auto secret = policy.requestSecret("alpha", {"fixture-token"});
    policy.submitSecret(localOwner, "alpha", secret, "fixture-protected-value");
    Memory(*f.store).memory(localOwner, "alpha", "apply_memory_edit", {{"baseVersion", 0}, {"content", "Private notes"}});
    Memory(*f.store).memory(localOwner, "alpha", "apply_memory_edit",
                            {{"docId", sharedDoc}, {"baseVersion", 0}, {"content", "Shared playbook"}});
    for (const auto& read : {call("find_tasks", {{"taskId", task}}), call("read_result", {{"messageId", result}}),
                             call("workflow_status", {{"workflowId", workflow}}), call("list_schedules"), call("list_secrets"),
                             call("list_connections"), call("read_memory"), call("memory_history"),
                             call("read_memory", {{"docId", sharedDoc}}), call("memory_history", {{"docId", sharedDoc}}), sessionEvents(f)})
        compareWithSingle(f, read);
    check(f.t3.eventReads == 2, "Session events did not reach T3 once in each mode");
}

void rereadEarlierResult() {
    Fixture f;
    const auto reread = [&](bool batch) {
        f.receive({call("list_connections")});
        std::vector<Decision> calls{call("read_tool_result", {{"messageId", f.message}, {"step", 1}, {"callIndex", 1}})};
        if (batch) calls.insert(calls.begin(), search);
        f.model.followUps.push_back({std::move(calls)});
        f.runtime->runConversations();
        f.checkFinished(3);
        const auto result = f.model.turns.back().back().at("result");
        check(f.results("result", 2).back().at("ok") == true && result.at("tool") == "list_connections" &&
                  result.at("result") == f.results().at(0),
              "The earlier step's result was not projected");
        return result;
    };
    const auto single = reread(false);
    check(reread(true) == single && f.t3.searches == 1, "Batched result reread differed from the single-call path");
}

void gatedRead(std::string_view tool, bool batch, bool allow, bool restart) {
    Fixture f;
    const bool events = tool == "flock_session_events";
    const auto read = events ? sessionEvents(f) : call(std::string(tool), {{"docId", sharedDoc}});
    if (!events)
        Memory(*f.store).memory(localOwner, "alpha", "apply_memory_edit",
                                {{"docId", sharedDoc}, {"baseVersion", 0}, {"content", "Shared playbook"}});
    f.requireApproval(events ? Resource::sessionRead("env", "fixture-thread") : Resource{Action::shared_memory_read, sharedDoc});
    f.receive(batch ? std::vector{search, read} : std::vector{read});
    f.runtime->runConversations();
    const auto pending = f.results("effect");
    const auto searches = f.t3.searches.load();
    const std::size_t slot = batch ? 1 : 0;
    const auto operation = "turn/" + std::to_string(f.message) + (batch ? "/1/1" : "/1");
    check(f.status() == "policy_waiting" && pending.size() == slot + 1 && pending.at(slot).is_null() && f.t3.eventReads == 0 &&
              f.model.turns.size() == 1 && f.query("SELECT operation FROM policy_approvals") == operation,
          "Gated read lost its approval or slot");
    if (batch) check(pending.at(0).at("result").at("ok") == true, "Gated read blocked its sibling");
    f.decideApproval(allow);
    if (restart) {
        if (!batch) {
            // Before singleton reads used ToolBatch, approvals had no effect checkpoint.
            Fixture::write(f.connect().get(), "UPDATE conversation_steps SET effect=NULL" + f.whereStep());
        } else if (events)
            Fixture::write(f.connect().get(),
                           R"SQL(UPDATE conversation_steps SET effect=json_set(effect,'$[1]',json('{"inFlight":true}')))SQL" +
                               f.whereStep());
        f.restart();
    }
    bool approvalHeldDuringRead = false;
    if (events)
        f.t3.onEvents = [&] {
            approvalHeldDuringRead =
                f.query("SELECT status FROM policy_approvals") == "approved" && f.results("effect").at(slot).value("inFlight", false);
        };
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.t3.searches == searches && f.t3.eventReads == (events && allow ? 1 : 0) && approvalHeldDuringRead == (events && allow) &&
              (!batch || results.at(0) == pending.at(0)) && f.model.turns.back().at(slot).at("result").at("ok") == allow &&
              f.query("SELECT status FROM policy_approvals") == (allow ? "used" : "denied"),
          "Read approval settlement changed a sibling, skipped the approved read, or read after denial");
}

void singleEventReadRecoversFromFailedWrites() {
    Fixture f;
    const auto read = sessionEvents(f);
    f.requireApproval(Resource::sessionRead("env", "fixture-thread"));
    f.receive({read});
    f.runtime->runConversations();
    f.decideApproval(true);
    auto connection = f.connect();
    const auto expectFailedWrite = [&] {
        try {
            f.runtime->runConversations();
        } catch (const std::runtime_error& error) {
            if (std::string_view(error.what()).find("fixture result save failed") != std::string_view::npos) return;
            throw;
        }
        throw std::runtime_error("The approved read did not reach the failing result save");
    };
    Fixture::write(connection.get(), R"SQL(
CREATE TRIGGER fail_slot_save BEFORE UPDATE OF effect ON conversation_steps
WHEN json_extract(NEW.effect,'$[0].result.ok') = 1
BEGIN SELECT RAISE(ABORT,'fixture result save failed'); END;
)SQL");
    expectFailedWrite();
    check(f.t3.eventReads == 1 && f.results("effect").at(0).value("inFlight", false) &&
              f.query("SELECT status FROM policy_approvals") == "approved",
          "The failed slot write consumed the approval before the result was saved");
    Fixture::write(connection.get(), R"SQL(
DROP TRIGGER fail_slot_save;
CREATE TRIGGER fail_result_save BEFORE UPDATE OF result ON conversation_steps
BEGIN SELECT RAISE(ABORT,'fixture result save failed'); END;
)SQL");
    f.restart();
    expectFailedWrite();
    const auto checkpoint = f.results("effect");
    check(f.t3.eventReads == 2 && checkpoint.at(0).at("result").at("ok") == true &&
              f.query("SELECT status FROM policy_approvals") == "used",
          "The approved event read did not commit its result and approval together");
    Fixture::write(connection.get(), "DROP TRIGGER fail_result_save");
    f.restart();
    f.runtime->runConversations();
    f.checkFinished(2);
    check(f.t3.eventReads == 2 && f.results() == checkpoint, "Restart repeated a read with a saved result");
}

void storeRemainsResponsiveDuringSessionEventRead() {
    Fixture f;
    f.receive({sessionEvents(f), call("list_connections")});
    checkStoreResponsiveDuringProviderCall(f, f.t3.onEvents);
    const auto results = f.results();
    check(f.t3.eventReads == 1 && results.at(0).at("result").at("ok") == true && results.at(1).at("ok") == true,
          "The batch lost a Session event or Store read result");
}

struct Measurement {
    int conversationAttempts, botToolAttempts, budget, pauses;
    std::chrono::milliseconds elapsed;
};

Measurement measure(const std::vector<Decision>& reads, bool batch, int limit, Fixture& f) {
    const auto delay = [] { std::this_thread::sleep_for(200ms); };
    f.t3.onSearch = f.t3.onReadiness = f.t3.onEvents = f.mcp->onRead = delay;
    const auto start = std::chrono::steady_clock::now();
    f.receive(batch ? reads : std::vector<Decision>{reads.front()}, limit);
    if (!batch)
        for (auto read = reads.begin() + 1; read != reads.end(); ++read) f.model.followUps.push_back({{*read}});
    f.runtime->runConversations();
    if (f.status() == "paused") {
        const Id pause = Conversations(*f.store).pausedConversations("alpha").at(0).at("messageId");
        Inbox(*f.store).decideInbox(localOwner, {"alpha", InboxKind::conversation_model_budget, f.message, pause, 0, 0, pause}, "approve");
        f.runtime->runConversations();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    f.checkFinished(batch ? 2 : static_cast<int>(reads.size()) + 1);
    const auto id = std::to_string(f.message);
    const auto purposes = Json::parse(
        f.query("SELECT json_group_object(purpose,n) FROM (SELECT purpose,count(*) n FROM model_attempts WHERE message_id=" + id +
                " GROUP BY purpose)"));
    check(purposes.size() == 1 + static_cast<std::size_t>(purposes.contains("bot_tool")),
          "Measurement included unexpected attempt purposes");
    const auto& evidence = f.model.turns.back();
    check(evidence.size() == reads.size(), "The model did not see every measured read");
    for (std::size_t i = 0; i < reads.size(); ++i)
        check(evidence[i].at("tool") == reads[i].tool && evidence[i].at("result").at("ok") == true,
              "A measured read failed or reached the model out of order");
    return {purposes.at("conversation_turn"), purposes.value("bot_tool", 0),
            std::stoi(f.query("SELECT attempts FROM model_budgets WHERE id='message/" + id + "'")),
            std::stoi(f.query("SELECT count(*) FROM journal WHERE kind='conversation_reply'"
                              " AND json_extract(data,'$.route')='model_budget' AND json_extract(data,'$.replyToMessageId')=" +
                              id)),
            elapsed};
}

void measureBatches() {
    std::cout << "| Scenario | Mode | Conversation attempts | Bot tool attempts | Budget units | Budget pauses | Wall ms |\n"
                 "| --- | --- | ---: | ---: | ---: | ---: | ---: |\n";
    const auto compare = [](std::string_view scenario, const auto& makeReads, int limit, int botAttempts, int baselinePauses) {
        const auto run = [&](bool batch) {
            Fixture f;
            const auto m = measure(makeReads(f), batch, limit, f);
            std::cout << "| " << scenario << " | " << (batch ? "batch" : "baseline") << " | " << m.conversationAttempts << " | "
                      << m.botToolAttempts << " | " << m.budget << " | " << m.pauses << " | " << m.elapsed.count() << " |\n";
            check(m.botToolAttempts == botAttempts && m.budget == m.conversationAttempts + m.botToolAttempts,
                  "Measurement budget disagreed with attempt accounting");
            return m;
        };
        const auto baseline = run(false);
        const auto batch = run(true);
        if (baseline.pauses != baselinePauses || batch.pauses != 0)
            throw std::runtime_error("Unexpected budget pauses for " + std::string(scenario));
    };
    const auto searchAndReadiness = [](Fixture&) { return std::vector{search, readiness}; };
    compare("Session search + readiness", searchAndReadiness, 20, 0, 0);
    compare(
        "Four conversation reads", [](Fixture& f) { return std::vector{search, readiness, sessionEvents(f), call("find_tasks")}; }, 20, 0,
        0);
    compare("Budget limit 2", searchAndReadiness, 2, 0, 1);
    compare(
        "Bot tool reads",
        [](Fixture&) { return std::vector{readResource, call("mcp_read_resource", {{"server", "fixture"}, {"uri", "fixture://other"}})}; },
        20, 2, 0);
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "gateway-only-cancel") {
            gatewayOnlyCancelDoesNotCancelBatchSibling();
            std::cout << "Gateway-only batch cancel acceptance passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--measure") {
            measureBatches();
            return 0;
        }
        mixedReads();
        approval(true);
        approval(false);
        storeRemainsResponsiveDuringMcpCapabilityChecks();
        followUpNeeded(call("start_task", {{"request", "Start a task"}}), "Run this action on its own after reviewing these results.");
        followUpNeeded(call("reply", {{"text", "Premature reply"}}), "Reply after reviewing these results.");
        gatewayOnlyCancelDoesNotCancelBatchSibling();
        conflictingWorkspaceCalls();
        rejectedCall();
        restartWithPartialResults();
        contendedSlotPersistence();
        conversationReadParity();
        rereadEarlierResult();
        for (const auto tool : {"flock_session_events", "read_memory", "memory_history"}) {
            for (bool batch : {false, true}) {
                gatedRead(tool, batch, false, false);
                gatedRead(tool, batch, true, false);
                gatedRead(tool, batch, true, true);
            }
        }
        singleEventReadRecoversFromFailedWrites();
        storeRemainsResponsiveDuringSessionEventRead();
        std::cout << "Conversation batch acceptance passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
