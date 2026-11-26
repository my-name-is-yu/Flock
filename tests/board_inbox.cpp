#include "flock/conversation_intake.hpp"
#include "flock/task_control.hpp"
#include "flock/task_history.hpp"
#include "flock/task_intake.hpp"
#include "flock/task_list.hpp"
#include "flock/tasks.hpp"
#include "flock/secret_ref.hpp"
#include "flock/decision_errors.hpp"
#include "flock/conversations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/bot_access.hpp"
#include "flock/secret_requests.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <barrier>
#include <cstdlib>
#include <future>
#include <iostream>
#include <sqlite3.h>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <unistd.h>

using namespace flockv0;

namespace {
const Person owner{"owner", true}, alice{"alice"}, bob{"bob"}, reader{"reader"};
constexpr auto originalCriteria = "Verify the published report";

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <class Error, class Work> void rejected(Work work, std::string_view message) {
    try {
        work();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        char name[] = "/tmp/flock-board-inbox-XXXXXX";
        const auto directory = mkdtemp(name);
        check(directory != nullptr, "could not create fixture directory");
        path = directory;
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

struct Database {
    sqlite3* db = nullptr;
    explicit Database(const std::filesystem::path& path) {
        check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "could not open fixture database");
    }
    ~Database() { sqlite3_close(db); }
    Id exec(const std::string& sql) {
        Id value = 0;
        const auto read = [](void* context, int count, char** fields, char**) {
            if (count && fields[0]) *static_cast<Id*>(context) = std::strtoll(fields[0], nullptr, 10);
            return 0;
        };
        if (sqlite3_exec(db, sql.c_str(), read, &value, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        return value;
    }
};

struct Script : Model {
    Id managed = 0;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "conversation_turn") {
            const auto prompt = event.at("text").get<std::string>();
            if (event.at("turn").empty()) {
                for (const auto& task : input.task.at("tasks")) {
                    if (task.at("taskId") != managed) continue;
                    Json args{{"taskId", managed}, {"generation", task.at("generation")}};
                    if (prompt == "Report the blocker") {
                        args["blockerReason"] = "Waiting for a signed report";
                        return {"record_task_blocker", args, "Report the current blocker"};
                    }
                    if (prompt == "Clear the blocker" || prompt == "The question is blocking") {
                        args["blockerReason"] = prompt == "Clear the blocker" ? "" : "Waiting for the branch";
                        if (prompt == "The question is blocking") args["waitId"] = task.at("wait").at("waitId");
                        return {"record_task_blocker", args, "Update the current blocker"};
                    }
                    if (prompt == "Use Final") {
                        args.update({{"waitId", task.at("wait").at("waitId")}, {"revisionId", task.at("revisionId")}, {"text", "Final"}});
                        return {"answer_task_question", args, "Send the person's answer"};
                    }
                    if (prompt == "Withdraw the question") {
                        args["waitId"] = task.at("wait").at("waitId");
                        return {"withdraw_task_question", args, "The answer is no longer needed"};
                    }
                    args.update(Json{{"intent", "Revision B"},
                                     {"revisionId", task.at("revisionId")},
                                     {"instruction", "Use the second source"},
                                     {"doneWhen", "Verify source B"}});
                    return {"change_task", args, "Change the work"};
                }
            }
            return {"reply", {{"text", "Recorded."}}, "Report the outcome"};
        }
        if (event.at("kind") == "question" && !event.contains("withdrawnUserQuestion"))
            return {"ask_user", {{"question", "Which report?"}, {"choices", {"Draft", "Final"}}}, "A choice is needed"};
        return {"delegate", {{"instruction", "Draft the report"}, {"doneWhen", originalCriteria}}, "Set the completion criteria"};
    }
};

struct Sessions : T3 {
    std::string questionProject;
    int sends = 0, reconciles = 0, interrupts = 0;
    std::string authGeneration(const std::string&, const Json&) override { return "test-session"; }
    Delivery send(const Command& cmd) override {
        ++sends;
        if (cmd.projectId == "unknown") return {};
        return {Delivery::Kind::Accepted, {"thread-" + cmd.projectId, "run-" + cmd.projectId, {}}, {}};
    }
    Delivery reconcile(const Command&) override {
        ++reconciles;
        return {Delivery::Kind::Accepted, {"thread-unknown", "run-unknown", {}}, {}};
    }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        Observation result;
        if (run.threadId == "thread-" + questionProject) {
            result.kind = Observation::Kind::Question;
            result.question = {"fixture-question", {{.id = "report"}}};
        }
        return result;
    }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override {
        ++interrupts;
        return InterruptOutcome::Requested;
    }
};

Intake intake(std::string bot, std::string text, std::string project, Json config = Json::object()) {
    return {std::move(bot), std::move(text), "sandbox", std::move(project), std::move(config)};
}

Json row(const Json& rows, std::string_view key, const Json& value) {
    for (const auto& item : rows)
        if (item.at(key) == value) return item;
    throw std::runtime_error("Missing row for " + std::string(key) + " " + value.dump());
}

InboxTarget target(const Json& item) {
    return {item.at("botId"),           parseInboxKind(item.at("itemKind").get<std::string>()),
            item.at("itemId"),          item.at("waitId"),
            item.at("taskId"),          item.at("targetRevision"),
            item.at("targetGeneration")};
}

InboxTarget pending(Store& store, InboxKind kind, std::string_view bot = {}) {
    return target(row(Inbox(store).inbox(owner, bot), "itemKind", inboxKindName(kind)));
}

void exercise(const std::filesystem::path& path) {
    using enum InboxKind;
    Store store(path);
    Store concurrent(path);
    Database db(path);
    BotAccess botAccess{store};
    SecretRequests secretRequests{store};
    botAccess.setMember(owner, "report", alice.id, true);
    botAccess.setMember(owner, "review", alice.id, true);
    botAccess.setMember(owner, "review", bob.id, true);
    botAccess.setMember(owner, "report", reader.id, true);

    const auto question = TaskIntake(store).submit(intake("report", "Report the work\nInclude the source", "question"));
    const auto unknown = TaskIntake(store).submit(intake("review", "Recover the delivery", "unknown"));
    const auto changed = TaskIntake(store).submit(intake("report", "Revision A", "changed"));
    const auto terminal = TaskIntake(store).submit(intake("review", "Cancelled request", "closed"));
    TaskControl(store).operate(localOwner, TaskTarget::from(Tasks(store).task(terminal).task, 0), "cancel");

    Script script;
    Sessions sessions;
    sessions.questionProject = "question";
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; });
    for (int step = 0; step < 12; ++step) runtime.tick();
    check(taskJson(Tasks(store).task(unknown)).at("wait").at("reason") == "unknown_delivery", "fixture did not pause on delivery");

    const auto report = taskJson(Tasks(store).task(question));
    const auto secret = secretRequests.requestSecret("review", {"report-credential"});
    for (const auto* kind : {"extra_run", "deadline", "model_budget", "conversation_model_budget", "stuck", "conversation_stuck",
                             "pr_review", "pr_reconciliation"})
        rejected<std::invalid_argument>([kind] { parseInboxKind(kind); }, "a removed inbox kind was accepted");
    const auto before = db.exec("PRAGMA data_version");
    const auto inbox = Inbox(store).inbox(alice);
    check(Inbox(store).inbox(alice) == inbox && db.exec("PRAGMA data_version") == before, "refreshing the inbox mutated SQLite");
    const auto questionRow = row(inbox, "itemKind", "user_question");
    check(questionRow.at("botId") == "report" && questionRow.at("botName") == "report" && questionRow.at("taskId") == question &&
              questionRow.at("requestTitle") == "Report the work" && questionRow.at("request") == "Report the work\nInclude the source" &&
              questionRow.at("prompt") == "Which report?",
          "inbox dropped the question's request or prompt");
    check(Inbox(store).inbox(alice, "private").empty() && Inbox(store).inbox(reader, "review").empty(),
          "inbox membership restrictions were ignored");
    check(row(inbox, "itemKind", "user_question").at("choices") == Json::array({"Draft", "Final"}) &&
              row(inbox, "itemKind", "protected_input").at("itemId") == secret &&
              row(inbox, "itemKind", "unknown_delivery").at("taskId") == unknown && inbox.size() == 3,
          "inbox omitted a pending decision or its prompt");
    const auto readerQuestion = row(Inbox(store).inbox(reader), "itemKind", "user_question");
    const auto waitAnchor = report.at("wait").at("messageId");
    check(readerQuestion.at("canDecide").get<bool>() && readerQuestion.at("conversationEventId") == waitAnchor &&
              readerQuestion.at("waitId") != waitAnchor &&
              row(Conversations(store).conversation("report"), "id", waitAnchor).at("kind") == "waiting",
          "member did not get answer controls");
    db.exec(R"SQL(
WITH RECURSIVE newer(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM newer WHERE n<129)
INSERT INTO journal(task_id,time,kind,data)
SELECT )SQL" +
            std::to_string(question) + ",1,'fixture_activity','{}' FROM newer");
    check(taskJson(Tasks(store).task(question)).at("wait").at("messageId") == waitAnchor,
          "task detail lost the wait anchor after 129 newer events");
    check(row(Inbox(store).inbox(reader), "itemKind", "user_question") == readerQuestion,
          "Inbox disagreed with task detail about the wait anchor");
    const auto active = TaskList(store).activeTasks("report");
    const auto activeQuestion = std::ranges::find_if(active, [&](const TaskList::ActiveTask& task) { return task.task.id == question; });
    check(taskJson(Tasks(store).task(question)).at("wait") == report.at("wait") &&
              row(taskJson(TaskList(store).tasks("report")), "id", question).at("wait") == report.at("wait") &&
              activeQuestion != active.end() && activeQuestion->wait && activeQuestion->wait->id == report.at("wait").at("id").get<Id>() &&
              activeQuestion->wait->messageId == waitAnchor.get<Id>() && activeQuestion->wait->reason() == WaitReason::user_question,
          "task views disagreed about the open wait");
    db.exec("DELETE FROM journal WHERE kind='fixture_activity' AND task_id=" + std::to_string(question));
    check(!row(inbox, "itemKind", "protected_input").contains("conversationEventId"),
          "inbox invented an anchor for an item without a visible source");
    const auto questionBefore = pending(store, user_question);
    rejected<DecisionDenied>([&] { Inbox(store).decideInbox(bob, questionBefore, "answer", {{"text", "Final"}}); },
                             "nonmember answered a question");
    const auto taskQuestion = TaskTarget::from(Tasks(store).task(question).task, questionBefore.waitId);
    rejected<DecisionDenied>([&] { TaskControl(store).operate(bob, taskQuestion, "answer", {{"text", "Final"}}); },
                             "nonmember answered through the direct controls");
    auto forged = questionBefore;
    ++forged.targetGeneration;
    rejected<RequestChanged>([&] { Inbox(store).decideInbox(alice, forged, "answer", {{"text", "Final"}}); },
                             "someone could submit a different task generation");

    Inbox(store).decideInbox(owner, pending(store, protected_input), "supply", {{"value", "test-vault-value"}});
    check(Inbox(store).decisionHistory(owner).dump().find("test-vault-value") == std::string::npos, "secret was journaled");
    const auto old = TaskTarget::from(Tasks(store).task(changed).task, 0);
    script.managed = changed;
    ConversationIntake(store).receive(localOwner, intake("report", "unused", "turn"), "change", "Switch the scope");
    runtime.runConversations();
    const auto revisionB = taskJson(Tasks(store).task(changed));
    check(revisionB.at("currentRevision") != old.revision && revisionB.at("doneWhen") == "Verify source B",
          "conversation did not change the task revision");
    const auto sent = sessions.sends;
    const auto interrupted = sessions.interrupts;
    rejected<RequestChanged>([&] { TaskControl(store).operate(alice, old, "cancel"); }, "old stop applied to revision B");
    check(taskJson(Tasks(store).task(changed)) == revisionB && sessions.sends == sent && sessions.interrupts == interrupted &&
              Inbox(store).decisionHistory(owner).size() == 1,
          "rejected stop changed task, delivery, or history");

    script.managed = question;
    ConversationIntake(store).receive(localOwner, intake("report", "unused", "turn"), "block", "Report the blocker");
    runtime.runConversations();
    const auto blockerReason = [&](Id task) { return taskJson(Tasks(store).task(task)).value("blockerReason", std::string{}); };
    check(blockerReason(question) == "Waiting for a signed report" &&
              row(TaskHistory(store).history(question), "kind", "blocker_recorded").at("data").at("blockerReason") ==
                  "Waiting for a signed report",
          "blocker tool did not update the task and history");
    ConversationIntake(store).receive(alice, intake("report", "unused", "turn"), "clear", "Clear the blocker");
    runtime.runConversations();
    check(blockerReason(question).empty() && row(TaskHistory(store).history(question), "kind", "blocker_cleared").is_object(),
          "explicitly clearing an independent blocker left the task blocked");
    ConversationIntake(store).receive(alice, intake("report", "unused", "turn"), "wait-blocker", "The question is blocking");
    runtime.runConversations();
    check(blockerReason(question) == "Waiting for the branch", "the recorded blocker lost its progress condition");
    const auto withdrawing = pending(store, user_question);
    ConversationIntake(store).receive(localOwner, intake("report", "unused", "turn"), "withdraw", "Withdraw the question");
    runtime.runConversations();
    for (const auto& remaining : Inbox(store).inbox(alice, "report"))
        check(remaining.at("itemKind") != "user_question", "withdrawn question remained in the Inbox");
    const auto withdrawn = taskJson(Tasks(store).task(question));
    const auto& sessionQuestionTarget = withdrawn.at("question").at("target");
    check(withdrawn.at("wait").is_null() && !withdrawn.contains("blockerReason") &&
              row(TaskHistory(store).history(question), "kind", "question_withdrawn").at("data").at("waitId") == withdrawing.waitId,
          "withdraw action did not close and journal the question");
    check(sessionQuestionTarget.at("waitId").is_null() && sessionQuestionTarget.at("eventId") == withdrawn.at("eventId") &&
              withdrawn.at("eventId") != report.at("eventId"),
          "withdraw left the active Session question on the old event");
    rejected<DecisionUnavailable>([&] { Inbox(store).decideInbox(alice, withdrawing, "answer", {{"text", "Final"}}); },
                                  "withdrawn question accepted an answer");

    const auto delivery = pending(store, unknown_delivery);
    db.exec("CREATE TRIGGER reject_audit BEFORE INSERT ON inbox_decisions BEGIN SELECT RAISE(ABORT,'test rollback'); END");
    const auto previous = taskJson(Tasks(store).task(unknown));
    rejected<std::runtime_error>([&] { Inbox(store).decideInbox(alice, delivery, "reconcile", {{"threadId", "candidate"}}); },
                                 "failed history insertion did not abort a decision");
    check(taskJson(Tasks(store).task(unknown)) == previous && pending(store, unknown_delivery) == delivery && sessions.reconciles == 0,
          "failed history insertion committed reconciliation intent or wait update");
    db.exec("DROP TRIGGER reject_audit");
    std::barrier ready(2);
    const auto act = [&](Store& connection, const Person& actor) {
        ready.arrive_and_wait();
        try {
            Inbox(connection).decideInbox(actor, delivery, "reconcile", {{"threadId", "candidate"}});
            return actor.id;
        } catch (const DecisionUnavailable&) {
            return std::string{};
        }
    };
    auto first = std::async(std::launch::async, [&] { return act(store, alice); });
    auto second = std::async(std::launch::async, [&] { return act(concurrent, bob); });
    const auto a = first.get();
    const auto b = second.get();
    check(a.empty() != b.empty(), "simultaneous delivery reconciliations took effect more or less than once");
    const auto decided = row(TaskHistory(store).history(unknown), "kind", "inbox_decision").at("data");
    check(decided.at("decidedBy") == (a.empty() ? b : a) && decided.at("decidedAt").get<Id>() > 0 &&
              decided.at("decision") == "reconcile" && decided.at("targetRevision") == delivery.targetRevision,
          "task history lost who decided, when, or what they reviewed");

    const auto reconciling = Tasks(store).task(unknown);
    check(reconciling.pendingAction && reconciling.pendingAction->command.candidate &&
              reconciling.pendingAction->command.candidate->threadId == "candidate" && reconciling.task.state == TaskState::sending &&
              sessions.reconciles == 0,
          "reconciliation did not commit the canonical delivery intent");
    secretRequests.requestSecret("review", {"after-restart"});
}

void protectedInputSource(const std::filesystem::path& path) {
    Store store(path);
    SecretRequests secretRequests{store};
    const SecretRef ref{"report-credential"};
    const auto routing = intake("report", "Prepare the report", "question");
    const auto request = secretRequests.requestSecret("report", ref);
    const auto source = ConversationIntake(store).receive(owner, routing, "secret", "First request").at("messageId").get<Id>();
    const auto repeat = ConversationIntake(store).receive(owner, routing, "repeat", "Repeated request").at("messageId").get<Id>();
    check(secretRequests.requestSecret("report", ref, source) == request && secretRequests.requestSecret("report", ref, repeat) == request,
          "repeated protected input changed its request ID");
    const auto inbox = Inbox(store).inbox(owner, "report");
    check(inbox.size() == 1 && inbox.at(0).at("itemId") == request && inbox.at(0).at("request") == "First request" &&
              inbox.at(0).at("conversationEventId") == source,
          "protected input lost its first conversation source");
}

void chatDecision(const std::filesystem::path& path) {
    Store store(path);
    BotAccess botAccess{store};
    botAccess.setMember(owner, "report", alice.id, true);
    const auto routing = intake("report", "Prepare the report", "question");
    const auto taskId = TaskIntake(store).submit(routing);
    Script script;
    Sessions sessions;
    sessions.questionProject = "question";
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; });
    for (int step = 0; step < 6; ++step) runtime.tick();
    script.managed = taskId;
    ConversationIntake(store).receive(alice, routing, "report-blocker", "The question is blocking");
    runtime.runConversations();
    check(taskJson(Tasks(store).task(taskId)).value("blockerReason", std::string{}) == "Waiting for the branch",
          "question-linked blocker was not recorded");
    const auto task = taskJson(Tasks(store).task(taskId));
    const auto& wait = task.at("wait");
    const auto item = pending(store, InboxKind::user_question, "report");
    ConversationIntake(store).receive(alice, routing, "revoked", "Use Final", wait.at("messageId"));
    botAccess.setMember(owner, "report", alice.id, false);
    runtime.runConversations();
    check(taskJson(Tasks(store).task(taskId)) == task && Inbox(store).decisionHistory(owner).empty(),
          "a queued chat answer survived access revocation");
    botAccess.setMember(owner, "report", alice.id, true);
    ConversationIntake(store).receive(alice, routing, "answer", "Use Final", wait.at("messageId"));
    runtime.runConversations();
    const auto history = Inbox(store).decisionHistory(owner);
    check(history.size() == 1 && history.at(0).at("decidedBy") == alice.id && history.at(0).at("decision") == "answer" &&
              history.at(0).at("decidedByYou") == false && history.at(0).at("requestTitle") == "Prepare the report" &&
              history.at(0).at("targetRevision") == item.targetRevision && Inbox(store).inbox(owner, "report").empty() &&
              !taskJson(Tasks(store).task(taskId)).contains("blockerReason"),
          "the chat answer did not use the canonical decision audit");
    rejected<DecisionUnavailable>([&] { Inbox(store).decideInbox(alice, item, "answer", {{"text", "Draft"}}); },
                                  "the Inbox applied an answer after chat decided the wait");
    ConversationIntake(store).receive(owner, routing, "change-report", "Switch the scope");
    runtime.runConversations();
    check(Tasks(store).task(taskId).task.intent == "Revision B" &&
              Inbox(store).decisionHistory(owner).at(0).at("requestTitle") == "Prepare the report",
          "replanning changed the historical decision title");
}

}

int main() {
    try {
        Directory directory;
        const auto path = directory.path / "state.db";
        exercise(path);
        protectedInputSource(directory.path / "protected-input.db");
        chatDecision(directory.path / "chat.db");
        Store restarted(path);
        const auto history = Inbox(restarted).decisionHistory(alice);
        const auto delivery = row(history, "itemKind", "unknown_delivery");
        check(history.size() == 2 && delivery.at("decidedAt").get<Id>() > 0 &&
                  (delivery.at("decidedBy") == "alice" || delivery.at("decidedBy") == "bob") &&
                  row(Inbox(restarted).inbox(alice), "itemKind", "protected_input").at("prompt") == "Supply a value for after-restart",
              "restart lost pending requests or recorded decisions");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
