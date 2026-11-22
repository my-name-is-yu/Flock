#include "flock/board_inbox.hpp"

#include <barrier>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

using namespace flockv0;

namespace {
const Person owner{"owner",true}, alice{"alice"}, bob{"bob"}, reader{"reader"};
constexpr auto originalCriteria = "Verify the published report";
constexpr auto prUrl = "https://github.com/example/report/pull/12";

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Error, class Work>
void rejected(Work work, std::string_view message) {
    try { work(); }
    catch (const Error&) { return; }
    throw std::runtime_error(std::string(message));
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        char name[] = "/tmp/flock-board-inbox-XXXXXX";
        const auto directory = mkdtemp(name);
        check(directory != nullptr,"could not create fixture directory");
        path = directory;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path,error); }
};

struct Database {
    sqlite3* db = nullptr;
    explicit Database(const std::filesystem::path& path) {
        check(sqlite3_open(path.c_str(),&db) == SQLITE_OK,"could not open fixture database");
    }
    ~Database() { sqlite3_close(db); }
    Id exec(const std::string& sql) {
        Id value = 0;
        const auto read = [](void* context,int count,char** fields,char**) {
            if (count && fields[0]) *static_cast<Id*>(context) = std::strtoll(fields[0],nullptr,10);
            return 0;
        };
        if (sqlite3_exec(db,sql.c_str(),read,&value,nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        return value;
    }
    void pause(Id task, std::string_view reason) {
        const auto id = std::to_string(task);
        exec("INSERT INTO waits(task_id,data) VALUES("+id+",json_object('reason','"+std::string(reason)+
             "','text','Fixture approval'))");
        const auto wait = std::to_string(sqlite3_last_insert_rowid(db));
        exec("UPDATE tasks SET state='waiting',generation=generation+1,data=json_set(data,'$.wait',"
             "json((SELECT json_set(data,'$.id',id) FROM waits WHERE id="+wait+"))) WHERE id="+id);
    }
};

struct Script : Model {
    Id managed = 0;
    bool failConversation = false;
    Decision decide(const ModelInput& input) override {
        const auto& event = input.event;
        if (event.at("kind") == "conversation_turn") {
            const auto prompt = event.at("text").get<std::string>();
            if (failConversation && prompt == "Pause this conversation")
                return {"find_tasks",Json::object(),"Use the final available call"};
            if (event.at("turn").empty() && prompt != "Pause this conversation") {
                for (const auto& task : input.task.at("tasks")) {
                    if (task.at("taskId") != managed) continue;
                    Json args{{"taskId",managed},{"generation",task.at("generation")}};
                    if (prompt == "Report the blocker") {
                        args["blockerReason"] = "Waiting for a signed report";
                        return {"record_task_blocker",args,"Report the current blocker"};
                    }
                    if (prompt == "Withdraw the question") {
                        args["waitId"] = task.at("wait").at("waitId");
                        return {"withdraw_task_question",args,"The answer is no longer needed"};
                    }
                    args.update(Json{{"intent","Revision B"},{"revisionId",task.at("revisionId")},
                                     {"instruction","Use the second source"},{"doneWhen","Verify source B"}});
                    return {"change_task",args,"Change the work"};
                }
            }
            return {"reply",{{"text","Recorded."}},"Report the outcome"};
        }
        if (event.at("kind") == "question" && !event.contains("withdrawnUserQuestion"))
            return {"ask_user",{{"question","Which report?"},{"choices",{"Draft","Final"}}},"A choice is needed"};
        return {"delegate",{{"instruction","Draft the report"},{"doneWhen",originalCriteria}},"Set the completion criteria"};
    }
};

struct Sessions : T3 {
    std::string questionProject;
    int sends = 0, reconciles = 0, interrupts = 0;
    std::string authGeneration(const std::string&, const Json&) override { return "test-session"; }
    Delivery send(const Command& cmd) override {
        ++sends;
        if (cmd.projectId == "unknown") return {};
        return {Delivery::Kind::Accepted,{"thread-"+cmd.projectId,"run-"+cmd.projectId,{}},{}};
    }
    Delivery reconcile(const Command&) override { ++reconciles; return {Delivery::Kind::Accepted,{"thread-unknown","run-unknown",{}},{}}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        Observation result;
        if (run.threadId == "thread-"+questionProject) {
            result.kind = Observation::Kind::Question;
            result.question = {"fixture-question",Json::array({{{"id","report"},{"required",true}}})};
        }
        return result;
    }
    void interrupt(const std::string&, const Json&, const RunRef&) override { ++interrupts; }
};

Intake intake(std::string bot, std::string text, std::string project, Json config = Json::object()) {
    return {std::move(bot),std::move(text),"sandbox",std::move(project),std::move(config),{},0};
}

Json row(const Json& rows, std::string_view key, const Json& value) {
    for (const auto& item : rows) if (item.at(key) == value) return item;
    throw std::runtime_error("Missing row for "+std::string(key)+" "+value.dump());
}

InboxTarget target(const Json& item) {
    return {item.at("botId"),parseInboxKind(item.at("itemKind").get<std::string>()),item.at("itemId"),
            item.at("waitId"),item.at("taskId"),item.at("targetRevision"),item.at("targetGeneration")};
}

InboxTarget pending(Store& store, InboxKind kind, std::string_view bot = {}) {
    return target(row(store.inbox(owner,bot),"itemKind",inboxKindName(kind)));
}

void exercise(const std::filesystem::path& path) {
    using enum InboxKind;
    Store store(path);
    Store concurrent(path);
    Database db(path);
    auto access = store.access();
    for (const auto* bot : {"report","review","private"})
        access.resourceGrant(owner,bot,{"environment","sandbox","launch"},GrantChange::grant);
    access.personGrant(owner,"report",alice.id,Role::decide,GrantChange::grant);
    access.personGrant(owner,"review",alice.id,Role::decide,GrantChange::grant);
    access.personGrant(owner,"review",bob.id,Role::decide,GrantChange::grant);
    access.personGrant(owner,"report",reader.id,Role::view,GrantChange::grant);

    const auto question = store.submit(intake("report","Report the work\nInclude the source","question"));
    const auto unknown = store.submit(intake("review","Recover the delivery","unknown"));
    const auto deadlineTask = store.submit(intake("review","Track the deadline","deadline"));
    const auto changed = store.submit(intake("report","Revision A","changed"));
    const auto budget = store.submit(intake("review","Approve an extra run","budget"));
    const auto model = store.submit(intake("review","Add a model call","model"));
    const auto hidden = store.submit(intake("private","Unshared request","private"));
    const auto terminal = store.submit(intake("review","Cancelled request","closed"));
    store.operate(terminal,0,"cancel");

    Seconds now = unixTime();
    Script script;
    Sessions sessions;
    sessions.questionProject = "question";
    RuntimeOptions options;
    options.clock = [&] { return now; };
    Runtime runtime(store,script,sessions,[](std::string_view) { return BotNotes{}; },options);
    for (int step = 0; step < 6; ++step) runtime.tick();
    check(store.task(unknown).at("wait").at("reason") == "unknown_delivery","fixture did not pause on delivery");

    db.pause(changed,"budget");
    db.pause(budget,"budget");
    db.pause(model,"model_budget");
    db.pause(hidden,"model_budget");
    const auto due = store.task(deadlineTask).at("deadline").get<Id>();
    now = due;
    runtime.tick();
    check(store.task(deadlineTask).at("wait").at("reason") == "deadline","fixture did not reach its deadline");

    const auto report = store.task(question);
    const auto review = store.requestPrReview(question,report.at("currentRevision"),report.at("generation"),
                                              prUrl,"Review the published report","Check the report and source");
    const auto grant = access.request("review",{"repository","report","read"});
    const auto secret = access.requestSecret("review",{"report-credential"});
    access.resourceGrant(owner,"review",{"bot_admin","review","rename_bot"},GrantChange::grant);
    const auto botRequest = access.requestBotChange("review","rename_bot","review","Reviewer");
    const auto conversation = store.receive(intake("review","unused","turn",{{"modelCallLimit",1}}),
                                            "pause","Pause this conversation");
    script.failConversation = true;
    runtime.runConversations();

    const auto before = db.exec("PRAGMA data_version");
    const auto board = store.board(alice);
    const auto inbox = store.inbox(alice);
    check(store.board(alice) == board && store.inbox(alice) == inbox && db.exec("PRAGMA data_version") == before,
          "refreshing board/inbox mutated SQLite");
    const auto questionRow = row(board,"taskId",question);
    check(questionRow.at("botId") == "report" && questionRow.at("botName") == "report" &&
          questionRow.at("requestTitle") == "Report the work" && questionRow.at("description") == "Report the work\nInclude the source" &&
          questionRow.at("doneWhen") == originalCriteria && questionRow.at("status") == "waiting" &&
          questionRow.at("wait").at("text") == "Which report?" && questionRow.at("prRefs").at(0).at("url") == prUrl,
          "board dropped assignment, criteria, wait, or PR reference");
    check(row(board,"taskId",deadlineTask).at("deadlineAt") == due &&
          store.board(alice,"review").size() == 5 && store.board(alice,"private").empty() &&
          store.board(alice,{},"terminal").at(0).at("taskId") == terminal &&
          store.board(alice,{},"active").size() == board.size()-1 && board.size() == 7,
          "board filters did not enforce Bot visibility or task state");
    check(store.board(alice,{},"waiting").size() == 6 &&
          store.inbox(alice,"private").empty() && store.inbox(reader,"review").empty(),
          "waiting filter or inbox grants were ignored");
    check(row(inbox,"itemKind","user_question").at("choices") == Json::array({"Draft","Final"}) &&
          row(inbox,"itemKind","conversation_model_budget").at("itemId") == conversation.at("messageId") &&
          row(inbox,"itemKind","conversation_model_budget").at("request") == "Pause this conversation" &&
          row(inbox,"itemKind","protected_input").at("itemId") == secret &&
          row(inbox,"itemKind","bot_change").at("itemId") == botRequest &&
          row(inbox,"itemKind","grant_request").at("itemId") == grant &&
          row(inbox,"itemKind","pr_review").at("itemId") == review &&
          row(inbox,"itemKind","model_budget").at("taskId") == model &&
          row(inbox,"itemKind","deadline").at("taskId") == deadlineTask &&
          row(inbox,"itemKind","unknown_delivery").at("taskId") == unknown &&
          inbox.size() == 11, "inbox omitted a pending decision or its prompt");
    check(!row(store.inbox(reader),"itemKind","user_question").at("canDecide").get<bool>(),
          "view-only account got approval controls");

    const auto questionBefore = pending(store,user_question);
    rejected<DecisionDenied>([&] { store.decideInbox(reader,questionBefore,"answer",{{"text","Final"}}); },
                             "view-only person answered a question");
    rejected<DecisionDenied>([&] { store.decideInbox(alice,pending(store,grant_request),"approve"); },
                             "decider approved a grant requiring admin");
    auto forged = questionBefore;
    ++forged.targetGeneration;
    rejected<RequestChanged>([&] { store.decideInbox(alice,forged,"answer",{{"text","Final"}}); },
                             "someone could submit a different task generation");

    const ApplyBotChange apply = [](const BotChange& change) {
        check(change.target == "review" && change.name == "Reviewer","wrong Bot change was applied");
    };
    store.decideInbox(alice,pending(store,pr_review),"approve");
    check(row(store.board(alice),"taskId",question).at("prRefs").at(0).at("status") == "approved",
          "PR approval failed to update the linked review");
    store.decideInbox(owner,pending(store,grant_request),"approve");
    check(access.permits("review",{"repository","report","read"},true),"inbox grant bypassed canonical approval");
    store.decideInbox(owner,pending(store,protected_input),"supply",{{"value","test-vault-value"}});
    check(store.decisionHistory(owner).dump().find("test-vault-value") == std::string::npos,"secret was journaled");
    store.decideInbox(owner,pending(store,bot_change),"approve",Json::object(),apply);
    check(access.administration(owner,"review").at("botChanges").at(0).at("status") == "applied",
          "inbox Bot change bypassed canonical application");

    const auto old = target(row(store.inbox(alice),"taskId",changed));
    script.managed = changed;
    store.receive(intake("report","unused","turn"),"change","Switch the scope");
    runtime.runConversations();
    const auto revisionB = store.task(changed);
    check(revisionB.at("currentRevision") != old.targetRevision && revisionB.at("doneWhen") == "Verify source B",
          "conversation did not change the task revision");
    const auto sent = sessions.sends;
    const auto interrupted = sessions.interrupts;
    rejected<RequestChanged>([&] { store.decideInbox(alice,old,"approve"); },"old approval applied to revision B");
    check(store.task(changed) == revisionB && sessions.sends == sent && sessions.interrupts == interrupted &&
          store.decisionHistory(owner).size() == 4, "rejected approval changed task, delivery, or history");

    const auto extra = target(row(store.inbox(alice),"taskId",budget));
    const auto allowance = store.task(budget).at("remaining").get<int>();
    std::barrier ready(2);
    const auto act = [&](Store& connection, const Person& actor) {
        ready.arrive_and_wait();
        try { connection.decideInbox(actor,extra,"approve"); return actor.id; }
        catch (const DecisionUnavailable&) { return std::string{}; }
    };
    auto first = std::async(std::launch::async,[&] { return act(store,alice); });
    auto second = std::async(std::launch::async,[&] { return act(concurrent,bob); });
    const auto a = first.get();
    const auto b = second.get();
    check(a.empty() != b.empty() && store.task(budget).at("remaining") == allowance+1,
          "simultaneous approval took effect more or less than once");
    const auto decided = row(store.history(budget),"kind","inbox_decision").at("data");
    check(decided.at("decidedBy") == (a.empty() ? b : a) && decided.at("decidedAt").get<Id>() > 0 &&
          decided.at("decision") == "approve" && decided.at("targetRevision") == extra.targetRevision,
          "task history lost who decided, when, or what they reviewed");

    script.managed = question;
    store.receive(intake("report","unused","turn"),"block","Report the blocker");
    runtime.runConversations();
    const auto blocked = store.board(reader,"report","blocked");
    check(row(blocked,"taskId",question).at("blockerReason") == "Waiting for a signed report" &&
          row(store.history(question),"kind","blocker_recorded").at("data").at("blockerReason") == "Waiting for a signed report",
          "blocker tool did not update the board and history");
    const auto withdrawing = pending(store,user_question);
    store.receive(intake("report","unused","turn"),"withdraw","Withdraw the question");
    runtime.runConversations();
    for (const auto& remaining : store.inbox(alice,"report"))
        check(remaining.at("itemKind") != "user_question","withdrawn question remained in the Inbox");
    check(store.task(question).at("wait").is_null() &&
          row(store.history(question),"kind","question_withdrawn").at("data").at("waitId") == withdrawing.waitId,
          "withdraw action did not close and journal the question");
    rejected<DecisionUnavailable>([&] { store.decideInbox(alice,withdrawing,"answer",{{"text","Final"}}); },
                                  "withdrawn question accepted an answer");

    const auto delivery = pending(store,unknown_delivery);
    db.exec("CREATE TRIGGER reject_audit BEFORE INSERT ON inbox_decisions BEGIN SELECT RAISE(ABORT,'test rollback'); END");
    const auto previous = store.task(unknown);
    rejected<std::runtime_error>([&] { store.decideInbox(alice,delivery,"reconcile",{{"threadId","candidate"}}); },
                                 "failed history insertion did not abort a decision");
    check(store.task(unknown) == previous && pending(store,unknown_delivery) == delivery && sessions.reconciles == 0,
          "failed history insertion committed reconciliation intent or wait update");
    db.exec("DROP TRIGGER reject_audit");
    store.decideInbox(alice,delivery,"reconcile",{{"threadId","candidate"}});
    check(store.task(unknown).at("pendingAction").at("candidate").at("threadId") == "candidate" &&
          store.task(unknown).at("state") == "sending" && sessions.reconciles == 0,
          "reconciliation did not commit the canonical delivery intent");
    store.decideInbox(alice,pending(store,deadline),"extend",{{"deadline",due+3600}});
    check(store.task(deadlineTask).at("deadline") == due+3600,"deadline extension did not resume the run");
    store.decideInbox(alice,pending(store,model_budget,"review"),"approve");
    check(store.task(model).at("state") == "deciding","model budget approval did not release the wait");
    const auto turn = pending(store,conversation_model_budget);
    store.decideInbox(alice,turn,"approve");
    rejected<DecisionUnavailable>([&] { store.decideInbox(alice,turn,"approve"); },"turn approved twice");
    check(store.pausedConversations("review").empty() && access.grants("review").size() >= 2,
          "conversation model approval did not release the pause");
    access.request("review",{"repository","after-restart","read"});
}
}

int main() {
    try {
        Directory directory;
        const auto path = directory.path/"state.db";
        exercise(path);
        Store restarted(path);
        const auto history = restarted.decisionHistory(alice);
        const auto approval = row(history,"itemKind","extra_run");
        check(history.size() == 9 && approval.at("decidedAt").get<Id>() > 0 &&
              (approval.at("decidedBy") == "alice" || approval.at("decidedBy") == "bob") &&
              row(restarted.inbox(alice),"itemKind","grant_request").at("prompt") == "read repository after-restart",
              "restart lost pending requests or recorded decisions");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
