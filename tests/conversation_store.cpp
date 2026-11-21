#include "flock/core.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace flockv0;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path dir;

    Fixture() {
        char pattern[] = "/tmp/flock-conversation-XXXXXX";
        if (const auto* path = mkdtemp(pattern)) dir = path;
        else throw std::runtime_error("Cannot create fixture");
    }
    ~Fixture() { std::filesystem::remove_all(dir); }

    std::filesystem::path database() const { return dir / "state.db"; }
    static Intake bot(std::string id = "alpha") { return {std::move(id),"","env","project"}; }
};

Id task(Store& store, std::string name, std::string bot = "alpha") {
    auto intake = Fixture::bot(std::move(bot));
    intake.request = std::move(name);
    return store.submit(intake);
}

Json reply(Store& store, Id message, std::string_view bot = "alpha") {
    const auto timeline = store.conversation(bot,0,100);
    for (const auto& row : timeline)
        if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message)
            return row["data"];
    throw std::runtime_error("Missing conversation reply");
}

Id receive(Store& store, std::string_view id, std::string_view text, Id anchor = 0) {
    return store.receive(Fixture::bot(),id,text,anchor).at("messageId");
}

void retryAcrossConnections() {
    Fixture fixture;
    Store left(fixture.database()), right(fixture.database());
    Id a = 0, b = 0;
    std::exception_ptr first, second;
    std::thread one([&] {
        try { a = receive(left,"retry","Write a release note"); }
        catch (...) { first = std::current_exception(); }
    });
    std::thread two([&] {
        try { b = receive(right,"retry","Write a release note"); }
        catch (...) { second = std::current_exception(); }
    });
    one.join();
    two.join();
    if (first) std::rethrow_exception(first);
    if (second) std::rethrow_exception(second);
    check(a && a == b && left.tasks("alpha").size() == 1,"Concurrent delivery duplicated work");
    check(reply(left,a)["route"] == "create_task","Expected explicit new task");
    bool rejected = false;
    try { receive(right,"retry","Write a different release note"); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"Changed message payload did not conflict");
    Store restarted(fixture.database());
    check(receive(restarted,"retry","Write a release note") == a && restarted.tasks("alpha").size() == 1,
          "Receipt or effect was lost on restart");
}

void clarificationAndStatus() {
    Fixture fixture;
    Store store(fixture.database());
    const auto a = task(store,"リリース作業");
    const auto b = task(store,"計測作業");
    const auto other = task(store,"別 Bot の作業","beta");
    const auto id = receive(store,"stop","それ止めて");
    const auto clarifier = reply(store,id);
    check(clarifier["route"] == "clarify" && clarifier["candidates"].size() == 2,
          "Ambiguous stop did not name exactly this Bot's candidates");
    check(store.task(a)["state"] == "queued" && store.task(b)["state"] == "queued","An ambiguous stop changed a task");
    const auto status = reply(store,receive(store,"status","進捗は？"));
    check(status["route"] == "status" && status["text"].get<std::string>().find("リリース作業") != std::string::npos,
          "An unresolved clarification blocked the status message");
    receive(store,"select","計測作業の方です");
    check(store.task(a)["state"] == "queued" && store.task(b)["state"] == "cancelled" &&
          store.task(other)["state"] == "queued" && store.clarifications("alpha").empty(),"Selection did not affect exactly the chosen task");
    const auto resume = reply(store,receive(store,"resume","計測作業を再開して"));
    check(resume["route"] == "create_task" && store.tasks("alpha").size() == 3 &&
          store.tasks("alpha").front()["sourceTaskId"] == b && store.task(b)["state"] == "cancelled",
          "Explicit resume did not create a linked task");
}

void compoundAndUnmatchedCancellation() {
    Fixture fixture;
    Store store(fixture.database());
    const auto a = task(store,"A");
    const auto b = task(store,"B");
    const auto compound = reply(store,receive(store,"compound","A は main で。別件、C を実装して"));
    check(compound["route"] == "compound" && store.tasks("alpha").size() == 3 &&
          store.task(a)["pendingInstructions"].size() == 1 && !store.task(b).contains("pendingInstructions"),
          "Compound message did not apply both segments to the intended work");

    const auto unclear = reply(store,receive(store,"unclear","main で。別件、D を実装して"));
    check(unclear["route"] == "clarify" && store.tasks("alpha").size() == 3,
          "A compound request applied an independent segment while its other segment was unclear");

    const auto separate = task(store,"Release","beta");
    const auto response = store.receive(Fixture::bot("beta"),"unknown","存在しない作業を止めて");
    check(reply(store,response["messageId"],"beta")["route"] != "cancel_task" &&
          store.task(separate)["state"] == "queued","An unmatched name cancelled unrelated work");

    const auto crossBot = reply(store,receive(store,"cross","main。別件、E を実装して",response["messageId"]));
    check(crossBot["route"] != "compound" && store.tasks("alpha").size() == 3,
          "A stale or foreign reply anchor allowed a partial compound effect");
}

void legacyHistory() {
    Fixture fixture;
    sqlite3* db = nullptr;
    check(sqlite3_open(fixture.database().c_str(),&db) == SQLITE_OK,"Cannot create legacy DB");
    const auto rc = sqlite3_exec(db,R"SQL(
CREATE TABLE tasks(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL,
  project_id TEXT NOT NULL, schedule_id TEXT, scheduled_at INTEGER,
  state TEXT NOT NULL, generation INTEGER NOT NULL, data TEXT NOT NULL,
  UNIQUE(bot_id,schedule_id,scheduled_at));
CREATE TABLE journal(id INTEGER PRIMARY KEY,task_id INTEGER NOT NULL REFERENCES tasks(id),
  time INTEGER NOT NULL,kind TEXT NOT NULL,data TEXT NOT NULL);
CREATE INDEX journal_task ON journal(task_id,id);
INSERT INTO tasks VALUES
  (1,'alpha','env','p',NULL,0,'completed',2,
     '{"botId":"alpha","environmentId":"env","projectId":"p","request":"Previous work","doneWhen":"Check all cases","wait":null}'),
  (2,'alpha','env','p',NULL,0,'queued',0,
     '{"botId":"alpha","environmentId":"env","projectId":"p","request":"Pending legacy work","doneWhen":"","wait":null}');
INSERT INTO journal VALUES(8,1,99,'report',json_object('report','1. First approach'||char(10)||'2. Chosen approach'));
INSERT INTO journal VALUES(9,1,100,'completed',json_object('text','Proposed two approaches'));
INSERT INTO journal VALUES(10,2,101,'request','{"text":"Pending legacy work","source":"manual"}');
)SQL",nullptr,nullptr,nullptr);
    sqlite3_close(db);
    check(rc == SQLITE_OK,"Legacy schema fixture failed");
    Store store(fixture.database());
    check(store.task(1)["revisions"].size() == 1 && store.task(1)["revisions"][0]["doneWhen"] == "Check all cases" &&
          store.task(2)["revisions"].empty() && store.task(2)["state"] == "queued","Legacy revision migration is incorrect");
    const auto detail = reply(store,receive(store,"result","2案目を詳しく"));
    check(detail["route"] == "respond" && detail["source"]["messageId"] == 8 &&
          detail["text"].get<std::string>().find("Chosen approach") != std::string::npos &&
          store.tasks("alpha").size() == 2,"Legacy report lost its identity or created work");
    Store refreshed(fixture.database());
    check(refreshed.task(1)["revisions"].size() == 1 && refreshed.tasks("alpha").size() == 2,
          "Opening a migrated DB repeated legacy work or revision backfill");
}
}

int main() {
    try {
        retryAcrossConnections();
        clarificationAndStatus();
        compoundAndUnmatchedCancellation();
        legacyHistory();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
