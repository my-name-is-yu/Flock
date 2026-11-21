#include "flock/core.hpp"

#include <sqlite3.h>

#include <algorithm>
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

void checkSingleDelivery(Store& store, Id message) {
    const auto tasks = store.tasks("alpha");
    check(tasks.size() == 1 && tasks.at(0).at("request") == "Write a release note" &&
          tasks.at(0).at("state") == "queued","Delivery did not queue exactly the requested work");
    const auto timeline = store.conversation("alpha",0,100);
    const auto messages = std::ranges::count_if(timeline,[](const Json& row) {
        return row.at("kind") == "conversation_message";
    });
    const auto userMessage = std::ranges::find_if(timeline,[&](const Json& row) {
        return row.at("kind") == "conversation_message" && row.at("id") == message;
    });
    check(messages == 1 && userMessage != timeline.end() &&
          userMessage->at("data").at("text") == "Write a release note",
          "Delivery lost or duplicated the user's message");
    const auto replies = std::ranges::count_if(timeline,[](const Json& row) {
        return row.at("kind") == "conversation_reply";
    });
    check(replies == 1 && reply(store,message).at("route") == "create_task",
          "Delivery lost or duplicated the task acknowledgement");
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
    check(a && a == b,"Concurrent delivery returned different receipts");
    checkSingleDelivery(left,a);
    bool rejected = false;
    try { receive(right,"retry","Write a different release note"); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"Changed message payload did not conflict");
    Store restarted(fixture.database());
    check(receive(restarted,"retry","Write a release note") == a,"Receipt was lost on restart");
    checkSingleDelivery(restarted,a);
}

void clarificationAndStatus() {
    Fixture fixture;
    Store store(fixture.database());
    const auto a = task(store,"リリース作業");
    const auto b = task(store,"計測作業");
    const auto other = task(store,"別 Bot の作業","beta");
    const auto id = receive(store,"stop","それ止めて");
    const auto clarifier = reply(store,id);
    const auto hasCandidate = [&](Id id, std::string_view name) {
        return std::ranges::any_of(clarifier.at("candidates"),[&](const Json& candidate) {
            return candidate.at("taskId") == id && candidate.at("request") == name;
        });
    };
    check(clarifier.at("route") == "clarify" && clarifier.at("candidates").size() == 2 &&
          hasCandidate(a,"リリース作業") && hasCandidate(b,"計測作業"),
          "Ambiguous stop did not name exactly this Bot's candidates");
    check(store.task(a)["state"] == "queued" && store.task(b)["state"] == "queued","An ambiguous stop changed a task");
    const auto status = reply(store,receive(store,"status","進捗は？"));
    const auto& progress = status.at("text").get_ref<const std::string&>();
    check(status.at("route") == "status" && progress.find("リリース作業") != std::string::npos &&
          progress.find("計測作業") != std::string::npos && progress.find("順番待ち") != std::string::npos &&
          progress.find("別 Bot の作業") == std::string::npos,
          "Status did not report this Bot's queued work while clarification was open");
    check(store.tasks("alpha").size() == 2 && store.task(a).at("state") == "queued" &&
          store.task(b).at("state") == "queued","A status request changed the work");
    receive(store,"select","計測作業の方です");
    check(store.task(a)["state"] == "queued" && store.task(b)["state"] == "cancelled" &&
          store.task(other)["state"] == "queued" && store.clarifications("alpha").empty(),"Selection did not affect exactly the chosen task");
    const auto resume = reply(store,receive(store,"resume","計測作業を再開して"));
    const auto tasks = store.tasks("alpha");
    const auto resumed = std::ranges::find_if(tasks,[&](const Json& task) {
        return task.at("id") != a && task.at("id") != b;
    });
    check(resume.at("route") == "create_task" && tasks.size() == 3 && resumed != tasks.end() &&
          resumed->at("sourceTaskId") == b && resumed->at("state") == "queued" &&
          store.task(b).at("state") == "cancelled","Explicit resume did not queue new work linked to the cancelled task");
}

Json pendingText(Store& store, Id id) {
    auto text = Json::array();
    for (const auto& instruction : store.task(id).value("pendingInstructions",Json::array()))
        text.push_back(instruction.at("text"));
    return text;
}

void compoundAndUnmatchedCancellation() {
    Fixture fixture;
    Store store(fixture.database());
    const auto a = task(store,"A");
    const auto b = task(store,"B");
    const auto compound = reply(store,receive(store,"compound","A は main で。別件、C を実装して"));
    const auto tasks = store.tasks("alpha");
    const auto created = std::ranges::find_if(tasks,[](const Json& task) {
        return task.at("request") == "C を実装して";
    });
    check(compound.at("route") == "compound" && tasks.size() == 3 && created != tasks.end(),
          "The independent segment did not create exactly the requested task");
    const auto c = created->at("id").get<Id>();
    const auto checkWork = [&](const char* message) {
        check(store.tasks("alpha").size() == 3 && store.task(a).at("state") == "queued" &&
              store.task(b).at("state") == "queued" && store.task(c).at("state") == "queued" &&
              pendingText(store,a) == Json::array({"A は main で"}) &&
              pendingText(store,b).empty() && pendingText(store,c).empty(),message);
    };
    checkWork("The compound message did not save only the intended instruction");

    const auto unclear = reply(store,receive(store,"unclear","main で。別件、D を実装して"));
    check(unclear.at("route") == "clarify","An unclear compound request did not ask which work to change");
    checkWork("A compound request applied a segment while its other segment was unclear");

    const auto separate = task(store,"Release","beta");
    const auto response = store.receive(Fixture::bot("beta"),"unknown","存在しない作業を止めて");
    check(reply(store,response.at("messageId"),"beta").at("route") == "clarify" &&
          store.task(separate).at("state") == "queued","An unmatched name cancelled unrelated work");

    const auto extra = task(store,"Inventory","beta");
    const auto stop = store.receive(Fixture::bot("beta"),"stop","それ止めて");
    const auto betaQuestion = reply(store,stop.at("messageId"),"beta");
    check(betaQuestion.at("route") == "clarify" && betaQuestion.at("candidates").size() == 2,
          "The other Bot did not issue an actionable clarification");
    const auto crossBot = receive(store,"cross","A は main で。別件、E を実装して",betaQuestion.at("messageId"));
    reply(store,crossBot);
    checkWork("A reply to another Bot's clarification applied work");
    check(store.task(separate).at("state") == "queued" && store.task(extra).at("state") == "queued" &&
          store.clarifications("beta").size() == 2,"A foreign reply changed the other Bot's work or clarification");
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
    const auto completed = store.task(1);
    const auto& revisions = completed.at("revisions");
    check(completed.at("state") == "completed" && revisions.size() == 1 &&
          completed.at("currentRevision") == revisions.at(0).at("id") &&
          revisions.at(0).at("intent") == "Previous work" && revisions.at(0).at("doneWhen") == "Check all cases",
          "Legacy work lost its completion criteria or original request");
    const auto queued = store.task(2);
    check(queued.at("revisions").empty() && queued.at("state") == "queued",
          "Migration changed queued legacy work before its criteria were set");
    const auto latest = store.latestTaskResult(1);
    check(latest.at("id") == 9 && latest.at("data").at("text") == "Proposed two approaches",
          "The original completion disappeared from the migrated task");
    const auto detail = reply(store,receive(store,"result","2案目を詳しく"));
    const auto& explanation = detail.at("text").get_ref<const std::string&>();
    check(detail.at("route") == "respond" && detail.at("source").at("taskId") == 1 &&
          detail.at("source").at("messageId") == 8 && explanation.find("Chosen approach") != std::string::npos &&
          explanation.find("First approach") == std::string::npos && store.tasks("alpha").size() == 2,
          "The answer did not explain only the selected legacy option with its original source");
    Store refreshed(fixture.database());
    const auto reopened = refreshed.task(1);
    check(reopened.at("currentRevision") == completed.at("currentRevision") &&
          reopened.at("revisions").size() == 1 && refreshed.tasks("alpha").size() == 2,
          "Opening a migrated DB changed its current revision or duplicated legacy work");
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
