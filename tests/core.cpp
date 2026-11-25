#include "flock/policy.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <unistd.h>

using namespace flockv0;
namespace {
void check(bool b, std::string_view message) { if (!b) throw std::runtime_error(std::string(message)); }

void unsupportedSchemaLeavesDatabaseIntact(const std::filesystem::path& file, const char* schema) {
    {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open(file.c_str(),&raw);
        const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
        check(opened == SQLITE_OK,"could not open the old database fixture");
        check(sqlite3_exec(db.get(),schema,nullptr,nullptr,nullptr) == SQLITE_OK,"could not create the unsupported database fixture");
    }
    const auto contents = [&] {
        std::ifstream data(file,std::ios::binary);
        check(data.good(),"could not read the old database fixture");
        return std::string(std::istreambuf_iterator<char>(data),std::istreambuf_iterator<char>());
    };
    const auto original = contents();
    bool refused = false;
    try {
        Store store(file);
    } catch (const Conflict& error) {
        refused = std::string_view(error.what()).find("--home") != std::string_view::npos;
    }
    check(refused && contents() == original && !std::filesystem::exists(file.string() + "-wal"),
          "opening unsupported state did not fail with guidance or changed the original database");
}

void pauseSchemaPreservesExistingSessionReplies(const std::filesystem::path& file) {
    {
        Store store(file);
        store.receiveWeb(localOwner,{"release","Check staging","env","proj"},
                         "before-pause-schema","Check staging");
    }
    {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open(file.c_str(),&raw);
        const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
        check(opened == SQLITE_OK,"could not open the existing Flock database fixture");
        constexpr auto schema = R"SQL(
DROP TABLE bot_pauses;
DROP TABLE session_message_sends;
DROP INDEX session_message_interrupts_due;
ALTER TABLE session_message_waits DROP COLUMN run_finished;
ALTER TABLE session_message_waits DROP COLUMN interrupt_at;
INSERT INTO session_message_waits(message_id,bot,environment,thread,run,title)
SELECT id,'release','env','thread','existing-run','Staging' FROM messages
WHERE client_message_id='before-pause-schema';
)SQL";
        check(sqlite3_exec(db.get(),schema,nullptr,nullptr,nullptr) == SQLITE_OK && sqlite3_changes(db.get()) == 1,
              "could not prepare the pre-Pause v9 database: " + std::string(sqlite3_errmsg(db.get())));
    }
    {
        Store store(file);
        store.setBotPaused(localOwner,"release",true);
    }
    {
        Store store(file);
        check(store.botPaused("release"),"Pause did not persist in the existing v9 database");
        bool stopPending = false;
        try { store.deleteBot(localOwner,"release"); }
        catch (const BotBusy&) { stopPending = true; }
        check(stopPending,"opening the existing v9 database discarded the pending Session reply");
    }
}

void tickUntil(Runtime& runtime, const auto& satisfied, std::string_view failure) {
    for (int i = 0; i < 12 && !satisfied(); ++i) runtime.tick();
    check(satisfied(), failure);
}

struct FixedModel : Model {
    int calls = 0;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        ++calls;
        if (input.event["kind"] == "intake") return {"delegate",{{"instruction","Check main"},{"doneWhen","Provide a report on main"}},"The request specifies main"};
        if (input.event["kind"] == "question")
            return {"answer_session_question",{{"requestId","q1"},{"answers",{{"branch","main"}}}},"The request specifies main"};
        check(input.task["doneWhen"] == "Provide a report on main","lost doneWhen");
        check(input.event["report"] == "main confirmed","lost report");
        return {"complete",{{"report","main confirmed"}},"The Run report satisfies the saved criterion"};
    }
};
struct FixedT3 : T3 {
    int launches = 0, replies = 0, polls = 0;
    Observation current;
    std::string authGeneration(const std::string&,const Json&) override { return "fixed-auth"; }
    Delivery send(const Command& cmd) override {
        if (cmd.tool == "delegate") {
            ++launches;
            check(!cmd.actionId.empty() && cmd.instruction.starts_with("Check main") &&
                  cmd.instruction.find("[Flock action ID: " + cmd.actionId + "]") != std::string::npos,
                  "T3 did not receive the work instruction and its recovery identifier");
            return {Delivery::Kind::Accepted,{"thread-1","run-1","t3://thread-1"},{}};
        }
        check(cmd.tool == "answer_session_question" && cmd.questionId == "q1" &&
              cmd.target.threadId == "thread-1" && cmd.target.runId == "run-1" &&
              cmd.answers == Json{{"branch","main"}},"wrong reply destination or answer");
        ++replies;
        current = {};
        return {Delivery::Kind::Accepted,{},{}};
    }
    Delivery reconcile(const Command&) override { throw std::runtime_error("unexpected reconciliation"); }
    Observation observe(const std::string&,const Json&,const RunRef& ref) override {
        ++polls;
        check(ref.threadId == "thread-1" && ref.runId == "run-1","wrong run");
        return current;
    }
    InterruptOutcome interrupt(const std::string&,const Json&,const RunRef&) override { throw std::runtime_error("unexpected interrupt"); }
};
}
int main() {
    char temp[] = "/tmp/flock-core-XXXXXX";
    const auto path = std::filesystem::path(mkdtemp(temp));
    try {
        unsupportedSchemaLeavesDatabaseIntact(path / "old-state.db",
            "CREATE TABLE tasks(id INTEGER PRIMARY KEY); INSERT INTO tasks VALUES (1);");
        unsupportedSchemaLeavesDatabaseIntact(path / "unrelated-state.db",
            "PRAGMA application_id=123; PRAGMA user_version=1; CREATE TABLE items(id INTEGER PRIMARY KEY);");
        unsupportedSchemaLeavesDatabaseIntact(path / "pre-policy-state.db",
            "PRAGMA application_id=0x464c4f43; PRAGMA user_version=1; CREATE TABLE tasks(id INTEGER PRIMARY KEY);");
        unsupportedSchemaLeavesDatabaseIntact(path / "full-policy-state.db",
            "PRAGMA application_id=0x464c4f43; PRAGMA user_version=2;"
            "CREATE TABLE policy_settings(id INTEGER PRIMARY KEY,mode TEXT NOT NULL);"
            "INSERT INTO policy_settings VALUES (1,'full');");
        unsupportedSchemaLeavesDatabaseIntact(path / "pre-project-selection.db",
            "PRAGMA application_id=0x464c4f43; PRAGMA user_version=6;"
            "CREATE TABLE tasks(id INTEGER PRIMARY KEY); INSERT INTO tasks VALUES (1);");
        unsupportedSchemaLeavesDatabaseIntact(path / "pre-gateway-identity.db",
            "PRAGMA application_id=0x464c4f43; PRAGMA user_version=7;"
            "CREATE TABLE session_message_waits(message_id INTEGER NOT NULL, bot TEXT NOT NULL,"
            "environment TEXT NOT NULL, thread TEXT NOT NULL, run TEXT NOT NULL, title TEXT NOT NULL,"
            "notified INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(message_id,environment,thread,run));"
            "INSERT INTO session_message_waits VALUES (1,'release','env','thread','run','Session',0);");
        unsupportedSchemaLeavesDatabaseIntact(path / "version-8.db",
            "PRAGMA application_id=0x464c4f43; PRAGMA user_version=8;"
            "CREATE TABLE tasks(id INTEGER PRIMARY KEY); INSERT INTO tasks VALUES (1);");
        pauseSchemaPreservesExistingSessionReplies(path / "pre-bot-pause.db");
        const auto db = path / "state.db";
        FixedModel model;
        FixedT3 t3;
        auto notes = [](std::string_view) { return std::string("Check the branch"); };
        Id id;
        {
            Store store(db);
            Runtime runtime(store,model,t3,notes);
            id = store.submit({"release","Check main","env","proj"});
            tickUntil(runtime, [&] { return store.task(id)["state"] == "running"; }, "delegate did not start");
            const auto callsBeforeIdle = model.calls;
            const auto pollsBeforeIdle = t3.polls;
            for (int i = 0; i < 3; ++i) runtime.tick();
            check(t3.polls > pollsBeforeIdle && model.calls == callsBeforeIdle,"idle monitoring did not stay model-free");
            t3.current.kind = Observation::Kind::Question;
            t3.current.question = {"q1",Json::array({{{"id","branch"},{"required",true}}})};
            tickUntil(runtime, [&] { return t3.replies == 1 && store.task(id)["state"] == "running"; },
                      "did not answer and return to the same Run");
        }
        {
            Store store(db);
            Runtime runtime(store,model,t3,notes);
            t3.current.kind = Observation::Kind::Finished;
            t3.current.status = "completed";
            t3.current.report = "main confirmed";
            tickUntil(runtime, [&] { return store.task(id)["state"] == "completed"; }, "restart did not complete");
            const auto task = store.task(id);
            check(task["runs"].size() == 1 && task["runs"][0]["threadId"] == "thread-1" &&
                  task["runs"][0]["runId"] == "run-1" && task["additionalUsed"] == 0 &&
                  t3.launches == 1 && t3.replies == 1,"restart duplicated the Run or consumed additional work");
            const auto history = store.history(id);
            const auto isCompletion = [](const Json& entry) { return entry["kind"] == "completed"; };
            const auto completion = std::find_if(history.begin(),history.end(),isCompletion);
            check(std::count_if(history.begin(),history.end(),isCompletion) == 1 &&
                  (*completion)["data"]["text"] == "main confirmed" &&
                  (*completion)["data"]["doneWhen"] == "Provide a report on main",
                  "completion history lost or duplicated the report and agreed criterion");
        }
        std::filesystem::remove_all(path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
