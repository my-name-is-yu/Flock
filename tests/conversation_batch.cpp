#include "flock/board_inbox.hpp"
#include "flock/workspace_tools.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <sqlite3.h>
#include <thread>
#include <unistd.h>

using namespace flockv0;
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

Decision call(std::string tool, Json args = Json::object()) {
    return {std::move(tool),std::move(args),"Check the fixture evidence"};
}

const auto search = call("flock_session_search");
const auto readiness = call("check_environment_readiness");
const auto invoke = call("mcp_call_tool",{{"server","fixture"},{"name","lookup"},{"arguments",Json::object()}});
const auto readResource = call("mcp_read_resource",{{"server","fixture"},{"uri","fixture://data"}});

struct FixedModel : Model {
    ModelTurn first;
    std::vector<Json> turns;
    ModelTurn decide(const ModelInput& input) override {
        check(input.event.at("kind") == "conversation_turn","A batched tool started a task");
        turns.push_back(input.event.at("turn"));
        if (turns.back().empty()) return first;
        return {{call("reply",{{"text","Reviewed the results."}})}};
    }
};

struct FakeT3 : T3 {
    std::atomic_int searches = 0, readinessChecks = 0;
    bool knownSession = false;
    std::function<void()> onSearch, onReadiness;
    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    std::optional<Delivery> preflight(const Command&) override {
        ++readinessChecks;
        if (onReadiness) onReadiness();
        return std::nullopt;
    }
    std::pair<std::vector<std::string>,std::optional<Id>> projectSessions(
        std::string_view, std::string_view, Id, SessionReadDeadline) override {
        ++searches;
        if (onSearch) onSearch();
        if (knownSession) return {{"fixture-thread"},std::nullopt};
        return {};
    }
    std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) override {
        return SessionMetadata{"Fixture Session","Fixture Project","project","completed",
                               "2026-11-21T00:00:00Z","2026-11-21T00:00:00Z",{},false};
    }
    Delivery send(const Command&) override { throw std::runtime_error("A batched tool dispatched work"); }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { return InterruptOutcome::Requested; }
};

struct FakeMcp : McpTools {
    std::atomic_int calls = 0, reads = 0;
    std::function<void()> onHasServer, onReadOnlyHint;
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
        return {{{"observed","invoked"}},false};
    }
    Json readResource(std::string_view, std::string_view) override {
        ++reads;
        return {{"invocations",calls.load()}};
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
    std::function<void(std::string_view)> contention;

    Fixture() {
        char pattern[] = "/tmp/flock-conversation-batch-XXXXXX";
        const auto* directory = mkdtemp(pattern);
        check(directory != nullptr,"Could not create batch fixture");
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
        store = std::make_unique<Store>(home/"state.db");
        RuntimeOptions options;
        options.botTools = tools;
        options.storageContention = contention;
        runtime = std::make_unique<Runtime>(*store,model,t3,[](auto) { return std::string{}; },options);
    }
    void receive(std::vector<Decision> calls, int limit = 20) {
        model.first.calls = std::move(calls);
        message = store->receive(localOwner,{"alpha","","env","project",{{"modelCallLimit",limit}}},
                                 "fixture","Check the fixture").at("messageId");
    }
    auto connect() const {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open((home/"state.db").c_str(),&raw);
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(raw,sqlite3_close);
        check(opened == SQLITE_OK,"Could not open batch fixture database");
        return connection;
    }
    static void write(sqlite3* db, const std::string& statement) {
        check(sqlite3_exec(db,statement.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,"Fixture write failed");
    }
    std::string query(const std::string& statement) const {
        std::string result;
        check(sqlite3_exec(connect().get(),statement.c_str(),[](void* value,int size,char** values,char**) {
            if (size && values[0]) *static_cast<std::string*>(value) = values[0];
            return 0;
        },&result,nullptr) == SQLITE_OK,"Fixture query failed");
        return result;
    }
    std::string whereStep() const { return " WHERE message_id=" + std::to_string(message) + " AND step=1"; }
    std::string status() const {
        return query("SELECT status FROM conversation_turns WHERE message_id=" + std::to_string(message));
    }
    void checkFinished(int attempts) const {
        const auto count = query("SELECT count(*) FROM model_attempts WHERE purpose='conversation_turn' AND message_id=" +
                                 std::to_string(message));
        if (count != std::to_string(attempts))
            throw std::runtime_error("Expected " + std::to_string(attempts) + " model attempts, found " + count +
                                     " for " + model.first.calls.front().tool + " (" +
                                     std::to_string(model.first.calls.size()) + " slots)");
        check(query("SELECT count(*) FROM conversation_steps s JOIN model_attempts a ON a.id=s.attempt_id"
                    " AND a.message_id=s.message_id AND a.purpose='conversation_turn' WHERE s.message_id=" +
                    std::to_string(message)) == std::to_string(attempts),"Conversation steps lost model attempt attribution");
        check(status() == "done" && query("SELECT json_extract(j.data,'$.text') FROM conversation_turns t"
                                         " JOIN journal j ON j.id=t.reply_id WHERE t.message_id=" +
                                         std::to_string(message)) == "Reviewed the results.",
              "The batch never advanced to the model's follow-up reply");
    }
    Json results(std::string_view column = "result") const {
        return Json::parse(query("SELECT " + std::string(column) + " FROM conversation_steps" + whereStep()));
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
        if (!ready.wait_for(lock,3s,[&] { return arrived == 2; })) throw Conflict("Reads did not overlap");
    };
    f.t3.onSearch = f.t3.onReadiness = rendezvous;
    f.t3.knownSession = true;
    f.receive({search,readiness},2);
    f.runtime->runConversations();
    f.checkFinished(2);
    check(f.t3.searches == 1 && f.t3.readinessChecks == 1 && f.model.turns.size() == 2,
          "Expected two reads and exactly one reply attempt");
    const auto& turn = f.model.turns[1];
    const auto results = f.results();
    check(turn.size() == 2 && turn[0].at("tool") == search.tool && turn[1].at("tool") == readiness.tool,
          "The model did not receive results in call order");
    check(turn[0].at("step") == 1 && turn[1].at("step") == 1 &&
          turn[0].at("callIndex") == 1 && turn[1].at("callIndex") == 2,"The model lost the original call slots");
    check(results[0].at("result").at("ok") == true && results[0].at("sessions").size() == 1 &&
          turn[0].at("result").at("sessions").at(0).at("title") == "Fixture Session",
          "The Session search result did not reach the model");
    check(results[1] == Json({{"ok",true},{"ready",true}}) && turn[1].at("result") == results[1],
          "The readiness result did not reach the model");
}

void approval(bool allow) {
    Fixture f;
    f.store->policy().setMode(localOwner,"supervised");
    f.receive({search,invoke,readResource});
    f.runtime->runConversations();
    const auto slots = f.results("effect");
    check(f.status() == "policy_waiting" && slots[0].at("result").at("ok") == true &&
          slots[1].is_null() && slots[2].is_null() &&
          f.t3.searches == 1 && f.mcp->calls == 0 && f.mcp->reads == 0 && f.model.turns.size() == 1,
          "Approval failed to hold a dependent call or preserve an independent result");
    check(f.query("SELECT operation FROM policy_approvals") == "turn/" + std::to_string(f.message) + "/1/1",
          "The policy approval lost its call index");
    const auto items = f.store->inbox(localOwner);
    check(items.size() == 1 && items[0].at("itemKind") == "policy_approval","Expected one approval card");
    f.store->decideInbox(localOwner,{"alpha",InboxKind::policy_approval,items[0].at("itemId"),
                         items[0].at("waitId"),0,0,0},allow ? "allow_once" : "deny");
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.t3.searches == 1 && f.mcp->calls == (allow ? 1 : 0) && f.mcp->reads == 1 &&
          results[0] == slots[0] && results[1].at("ok") == allow &&
          results[2].at("data").at("invocations") == (allow ? 1 : 0),
          "Approval settlement repeated a sibling or let a dependent call overtake it");
}

void storeRemainsResponsiveDuringMcpCapabilityChecks() {
    for (const auto callback : {&FakeMcp::onHasServer,&FakeMcp::onReadOnlyHint}) {
        std::promise<void> entered, release;
        auto released = release.get_future();
        Fixture f;
        f.mcp.get()->*callback = [&] {
            entered.set_value();
            released.wait();
        };
        f.receive({invoke,search});
        auto batch = std::async(std::launch::async,[&] { f.runtime->runConversations(); });
        const bool reachedCheck = entered.get_future().wait_for(3s) == std::future_status::ready;
        auto inbox = std::async(std::launch::async,[&] { return f.store->inbox(localOwner); });
        const bool servedInbox = inbox.wait_for(3s) == std::future_status::ready;
        release.set_value();
        inbox.get();
        batch.get();
        check(reachedCheck,"The batch did not reach the MCP capability check");
        check(servedInbox,"An MCP capability check held the store mutex");
        f.checkFinished(2);
        check(f.mcp->calls == 1,"The MCP call did not complete after checking capabilities");
    }
}

void followUpNeeded(const Decision& action, std::string_view guidance) {
    Fixture f;
    f.receive({search,action},2);
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.query("SELECT count(*) FROM tasks") == "0" && f.t3.searches == 1 &&
          results[0].at("result").at("ok") == true && results[1] == Json({{"ok",false},{"notRun",true},{"error",guidance}}) &&
          f.model.turns.at(1)[1].at("result") == results[1],
          "The standalone action ran in the batch or its result was lost");
}

void workspaceCommandRunsAlone() {
    Fixture f;
    const auto command = call("workspace_run_command",{{"argv",{"pwd"}}});
    const auto findFiles = call("workspace_search",{{"query","marker"}});
    f.receive({command,findFiles},4);
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(results[0].value("notRun",false) && results[0].at("ok") == false &&
          f.query("SELECT count(*) FROM workspace_commands") == "0",
          "A background workspace command started in a batch");
    check(results[1].at("ok") == true,"A standalone command suppressed a sibling read");
}

void conflictingWorkspaceCalls() {
    Fixture f;
    f.receive({call("workspace_write_file",{{"path","ordered.txt"},{"text","Fresh contents"}}),
               call("workspace_read_file",{{"path","ordered.txt"}})},4);
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(results[0].at("ok") == true && results[1].at("ok") == true &&
          results[1].at("text") == "Fresh contents","The dependent workspace read overtook the write");
}

void rejectedCall() {
    Fixture f;
    f.receive({search,readiness},2);
    f.model.first.rejections.emplace(1,"Invalid environment arguments");
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.t3.searches == 1 && f.t3.readinessChecks == 0 && results[0].at("result").at("ok") == true &&
          results[1].at("error") == "Invalid environment arguments","The adapter rejection did not stay in its slot");
}

void restartWithPartialResults() {
    Fixture f;
    f.receive({search,readiness,invoke,readResource},5);
    const auto id = std::to_string(f.message);
    const auto budget = "'message/" + id + "'";
    auto decisions = Json::array();
    for (const auto& action : f.model.first.calls)
        decisions.push_back({{"tool",action.tool},{"args",action.args},{"reason",action.reason}});
    const Json completed{{"result",{{"ok",true},{"sessions",Json::array()},{"marker","completed before restart"}}},
                         {"sessions",Json::array()}};
    const Json inFlight{{"inFlight",true}};
    const auto slots = Json::array({completed,inFlight,inFlight,inFlight});
    Fixture::write(f.connect().get(),
        "INSERT OR IGNORE INTO model_budgets(id,call_limit) VALUES(" + budget + ",5);"
        "UPDATE model_budgets SET attempts=3 WHERE id=" + budget + ";"
        "INSERT INTO model_attempts(budget_id,purpose,time,message_id) VALUES("
        + budget + ",'conversation_turn',1," + id + "),(" + budget + ",'bot_tool',1," + id + "),("
        + budget + ",'bot_tool',1," + id + ");"
        "INSERT INTO conversation_steps(message_id,step,attempt_id,decision,effect) VALUES(" + id +
        ",1,(SELECT id FROM model_attempts WHERE purpose='conversation_turn' AND message_id=" + id +
        "),'" + decisions.dump() + "','" + slots.dump() + "');");
    f.restart();
    f.runtime->runConversations();
    f.checkFinished(2);
    const auto results = f.results();
    check(f.model.turns.size() == 1 && f.t3.searches == 0 && f.t3.readinessChecks == 1 && f.mcp->calls == 0 &&
          f.mcp->reads == 1 && results[0] == completed && results[1].at("ready") == true &&
          results[2].at("delivery") == "unknown" && results[3].at("ok") == true &&
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
            Fixture::write(locker.get(),"ROLLBACK");
            reported = true;
        }
    };
    f.restart();
    f.t3.onReadiness = [&] {
        if (f.t3.readinessChecks != 1) return;
        entered.set_value();
        resume.wait();
    };
    f.receive({search,readiness},2);
    f.runtime->tickConcurrent();
    bool searchSaved = false;
    if (entered.get_future().wait_for(5s) == std::future_status::ready) {
        for (const auto deadline = std::chrono::steady_clock::now()+5s;
             !searchSaved && std::chrono::steady_clock::now() < deadline; std::this_thread::sleep_for(5ms))
            searchSaved = f.query("SELECT json_extract(effect,'$[0].result.ok') FROM conversation_steps" + f.whereStep()) == "1";
    }
    if (searchSaved) Fixture::write(locker.get(),"BEGIN IMMEDIATE");
    release.set_value();
    check(searchSaved,"The first slot did not persist while its sibling was in flight");
    for (const auto deadline = std::chrono::steady_clock::now()+25s;
         f.status() != "done" && std::chrono::steady_clock::now() < deadline; std::this_thread::sleep_for(5ms))
        f.runtime->tickConcurrent();
    f.checkFinished(2);
    check(reported && f.t3.searches == 1 && f.t3.readinessChecks == 1 && f.results()[1].at("ready") == true,
          "SQLite writer contention duplicated a completed slot or lost its result");
}
}

int main() {
    try {
        mixedReads();
        approval(true);
        approval(false);
        storeRemainsResponsiveDuringMcpCapabilityChecks();
        followUpNeeded(call("start_task",{{"request","Start a task"}}),"Run this action on its own after reviewing these results.");
        followUpNeeded(call("reply",{{"text","Premature reply"}}),"Reply after reviewing these results.");
        workspaceCommandRunsAlone();
        conflictingWorkspaceCalls();
        rejectedCall();
        restartWithPartialResults();
        contendedSlotPersistence();
        std::cout << "Conversation batch acceptance passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
