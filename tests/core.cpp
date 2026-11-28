#include "flock/task_control.hpp"
#include "support.hpp"
#include "flock/task_history.hpp"
#include "flock/task_target.hpp"
#include "flock/person.hpp"
#include "flock/task_intake.hpp"
#include "flock/tasks.hpp"
#include "flock/command.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/intake.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sqlite3.h>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

using namespace flockv0;
namespace {
using flockv0::test::check;

void unsupportedSchemaLeavesDatabaseIntact(const std::filesystem::path& file, int application, int version, int currentVersion) {
    {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open(file.c_str(), &raw);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        check(opened == SQLITE_OK, "could not open the old database fixture");
        const auto sql = "PRAGMA application_id=" + std::to_string(application) + "; PRAGMA user_version=" + std::to_string(version) +
                         "; CREATE TABLE tasks(id INTEGER PRIMARY KEY); INSERT INTO tasks VALUES(1);";
        check(sqlite3_exec(db.get(), sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
              "could not create the unsupported database fixture");
    }
    const auto contents = [&] {
        std::ifstream data(file, std::ios::binary);
        check(data.good(), "could not read the old database fixture");
        return std::string(std::istreambuf_iterator<char>(data), std::istreambuf_iterator<char>());
    };
    const auto original = contents();
    bool refused = false;
    try {
        Store store(file);
    } catch (const Conflict& error) {
        const std::string_view message = error.what();
        check(message.find("(schema version " + std::to_string(version) + ")") != message.npos &&
                  message.find("This build requires a Flock database at schema version " + std::to_string(currentVersion) + ".") !=
                      message.npos &&
                  message.find("Use --home with a new state directory") != message.npos,
              "unexpected refusal for " + file.filename().string() + ": " + error.what());
        refused = true;
    }
    check(refused, "opening unsupported state succeeded: " + file.filename().string());
    check(contents() == original && !std::filesystem::exists(file.string() + "-wal"),
          "opening unsupported state changed the original database: " + file.filename().string());
}

void checkSchemaRefusals(const std::filesystem::path& freshDatabase) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(freshDatabase.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    check(opened == SQLITE_OK, "could not read the fresh database fixture");
    sqlite3_stmt* query = nullptr;
    const auto prepared = sqlite3_prepare_v2(db.get(), "PRAGMA user_version", -1, &query, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> versionRow(query, sqlite3_finalize);
    check(prepared == SQLITE_OK && sqlite3_step(query) == SQLITE_ROW, "could not read the fresh database version");
    const int currentVersion = sqlite3_column_int(query, 0);
    const auto dir = freshDatabase.parent_path();
    unsupportedSchemaLeavesDatabaseIntact(dir / "unmarked.db", 0, 0, currentVersion);
    unsupportedSchemaLeavesDatabaseIntact(dir / "unrelated.db", 123, currentVersion, currentVersion);
    for (const int version : {1, 2, 6, 7, 8, 9, 10, 11, currentVersion - 1, currentVersion + 1})
        unsupportedSchemaLeavesDatabaseIntact(dir / ("version-" + std::to_string(version) + ".db"), 0x464c4f43, version, currentVersion);
}

void tickUntil(Runtime& runtime, const auto& satisfied, std::string_view failure) {
    for (int i = 0; i < 12 && !satisfied(); ++i) runtime.tick();
    check(satisfied(), failure);
}

struct FixedModel : Model {
    ModelTurn decide(const ModelInput&) override { throw std::runtime_error("a task without a conversation called the model"); }
};
struct FixedT3 : T3 {
    int launches = 0, replies = 0, polls = 0;
    Observation current;
    std::string authGeneration(const std::string&, const Json&) override { return "fixed-auth"; }
    Delivery send(const Command& cmd) override {
        if (cmd.tool == CommandTool::delegate) {
            ++launches;
            check(!cmd.actionId.empty() && cmd.instruction.starts_with("Standing rules from release's role:\nCheck the branch\n\n") &&
                      cmd.instruction.find("\n\nTask intent:\nCheck main\n\n") != std::string::npos &&
                      cmd.instruction.find("[Flock action ID: " + cmd.actionId + "]") != std::string::npos,
                  "T3 did not receive the work instruction and its recovery identifier");
            return {Delivery::Kind::Accepted, {"thread-1", "run-1", "t3://thread-1"}, {}};
        }
        check(cmd.tool == CommandTool::answer_session_question && cmd.questionId == "q1" && cmd.target.threadId == "thread-1" &&
                  cmd.target.runId == "run-1" && cmd.answers == Json{{"branch", "main"}},
              "wrong reply destination or answer");
        ++replies;
        current = {};
        return {Delivery::Kind::Accepted, {}, {}};
    }
    Delivery reconcile(const Command&) override { throw std::runtime_error("unexpected reconciliation"); }
    Observation observe(const std::string&, const Json&, const RunRef& ref) override {
        ++polls;
        check(ref.threadId == "thread-1" && ref.runId == "run-1", "wrong run");
        return current;
    }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override {
        throw std::runtime_error("unexpected interrupt");
    }
};
}
int main() {
    const flockv0::test::TemporaryDirectory scratch;
    const auto path = scratch.path;
    try {
        const auto db = path / "state.db";
        FixedModel model;
        FixedT3 t3;
        auto notes = [](std::string_view) { return std::string("Check the branch"); };
        Id id;
        {
            Store store(db);
            checkSchemaRefusals(db);
            Runtime runtime(store, model, t3, notes);
            id = TaskIntake(store).submit({"release", "Check main", "env", "proj"});
            tickUntil(runtime, [&] { return Tasks(store).task(id).task.state == TaskState::running; }, "delegate did not start");
            const auto pollsBeforeIdle = t3.polls;
            for (int i = 0; i < 3; ++i) runtime.tick();
            check(t3.polls > pollsBeforeIdle, "idle monitoring stopped polling the Run");
            t3.current.kind = Observation::Kind::Question;
            t3.current.question = {"q1", {{.id = "branch", .prompt = "Which branch?"}}};
            tickUntil(runtime, [&] { return Tasks(store).task(id).wait.has_value(); }, "the Session question did not reach the person");
            const auto asked = Tasks(store).task(id);
            check(asked.wait->text == "Which branch?", "the person did not see the Session's own question");
            TaskControl(store).operate(localOwner, TaskTarget::from(asked.task, asked.wait->id), "answer", {{"text", "main"}});
            tickUntil(
                runtime, [&] { return t3.replies == 1 && Tasks(store).task(id).task.state == TaskState::running; },
                "did not answer and return to the same Run");
        }
        {
            Store store(db);
            Runtime runtime(store, model, t3, notes);
            t3.current.kind = Observation::Kind::Finished;
            t3.current.status = "completed";
            t3.current.report = "main confirmed";
            tickUntil(runtime, [&] { return Tasks(store).task(id).task.state == TaskState::completed; }, "restart did not complete");
            const auto task = taskJson(Tasks(store).task(id));
            check(task["runs"].size() == 1 && task["runs"][0]["threadId"] == "thread-1" && task["runs"][0]["runId"] == "run-1" &&
                      t3.launches == 1 && t3.replies == 1,
                  "restart duplicated the Run or consumed additional work");
            const auto history = TaskHistory(store).history(id);
            const auto isCompletion = [](const Json& entry) { return entry["kind"] == "completed"; };
            const auto completion = std::find_if(history.begin(), history.end(), isCompletion);
            check(std::count_if(history.begin(), history.end(), isCompletion) == 1 && (*completion)["data"]["text"] == "main confirmed" &&
                      (*completion)["data"]["doneWhen"] == "",
                  "completion history lost or duplicated the report");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
