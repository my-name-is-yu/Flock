#include "fake_git.hpp"
#include "flock/tasks.hpp"
#include "flock/git_events.hpp"
#include "flock/conversations.hpp"
#include "flock/board_inbox.hpp"
#include "model.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <map>
#include <sqlite3.h>
#include <spawn.h>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <sys/wait.h>

extern char** environ;

using namespace git_fixture;

void testGitPrFollowup();

namespace {
constexpr auto fixtureReason = "Use the verified fixture evidence";
constexpr auto mutationChildFlag = "--task-mutation-child";
std::string executable;

ModelTurn decodeCall(const Json& request, std::string_view tool, const Json& args) {
    const Json call{{"type", "function_call"}, {"namespace", "flock"}, {"status", "completed"}, {"name", tool}, {"arguments", args.dump()}};
    return app::responseDecision(Json::array({call}), request);
}

class Script final : public Model {
public:
    std::map<std::string, Decision> calls;
    Json results = Json::object();
    unsigned notifications = 0;
    bool openAfterSession = false;
    bool checkOriginatingSession = false;
    std::function<void()> onNotification;
    std::function<Decision(const ModelInput&)> rawNotification;
    std::function<Decision(const ModelInput&)> onTaskResult;

    ModelTurn decide(const ModelInput& input) override {
        if (rawNotification && input.event.contains("gitEvent")) return {{rawNotification(input)}};
        auto request = app::responseRequest(input);
        auto next = choose(input);
        const auto operation = gitOperation(next);
        if (recordsTaskDecisionReason(next.tool) || (!operation.empty() && operation != "read")) next.args["reason"] = fixtureReason;
        return decodeCall(request, next.tool, next.args);
    }
private:
    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "intake")
            return {"delegate", {{"instruction", "Push change"}, {"doneWhen", "Open a PR for the pushed branch"}}, {}};
        if (event.at("kind") == "result") {
            if (onTaskResult) return onTaskResult(input);
            const auto steps = event.value("gitResults", Json::array());
            if (openAfterSession && steps.empty()) return {"git_open_pr", openArgs(), {}};
            if (!steps.empty()) {
                results["session-pr"] = steps.at(0).at("result");
                check(results["session-pr"].value("ok", false), "task PR open failed: " + results["session-pr"].dump());
            }
            return {"complete", {{"report", "Change prepared and checked"}}, {}};
        }
        const auto& steps = event.at("turn");
        if (event.contains("gitEvent")) {
            const auto& gitEvent = event.at("gitEvent");
            if (steps.empty()) {
                ++notifications;
                if (onNotification) onNotification();
                check(input.task.empty() && input.history.empty(), "Notification received unrelated task context");
                if (checkOriginatingSession)
                    check(gitEvent.at("run").at("threadId") == "thread-project", "Webhook lost its originating Session");
                std::vector<std::string> expected{"reply", "read_tool_result", "git_pr_status"};
                if (gitEvent.at("headSha") != "") expected.push_back("git_commit_status");
                if (gitEvent.at("jobId") != 0) expected.push_back("git_read_ci_logs");
                check(input.tools == expected, "Webhook supplied unexpected tools or changed their order");
                if (gitEvent.at("jobId") == 81)
                    return {
                        "git_read_ci_logs",
                        {{"host", gitEvent.at("host")}, {"repo", gitEvent.at("repo")}, {"jobId", gitEvent.at("jobId")}, {"maxBytes", 32}},
                        {}};
                return {"reply", {{"text", "Git notification received."}}, {}};
            }
            results["log"] = steps.at(0).at("result");
            const auto& log = results.at("log").at("data");
            check(log.at("truncated") == true && log.at("text").get<std::string>().size() == 32, "CI excerpt is not explicitly bounded");
            return {"reply", {{"text", "CI failed. " + log.at("text").get<std::string>()}}, {}};
        }
        const auto name = event.at("text").get<std::string>();
        if (steps.empty()) return calls.at(name);
        results[name] = steps.at(0).at("result");
        return {"reply", {{"text", "Git tool result recorded"}}, {}};
    }
};

Json invoke(Store& store, Runtime& runtime, Script& model, std::string id, std::string tool, Json args) {
    model.calls.insert_or_assign(id, Decision{std::move(tool), std::move(args), {}});
    Conversations(store).receive(localOwner, intake(), id, id);
    runtime.runConversations();
    if (!model.results.contains(id)) throw std::runtime_error("No result for " + id);
    return model.results.at(id);
}

bool recordedMutationReason(const Json& history, std::string_view tool) {
    return std::ranges::any_of(history, [&](const Json& event) {
        if (event.at("kind") != "git_mutation") return false;
        const auto& data = event.at("data");
        const auto& result = data.at("result");
        return data.at("tool") == tool && result.value("status", "") == "pending" && result.value("reason", "") == fixtureReason;
    });
}

Json persistence(const std::filesystem::path& db, std::string_view operationId) {
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2(db.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "could not read fixture database");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(raw,
                             "SELECT json_object('status',status,'refs',json_array(host,base_repo,base_ref,head_repo,head_ref),"
                             "'taskId',task_id,'botId',bot_id,'prNumber',pr_number) FROM git_pr_operations WHERE operation_id=?",
                             -1, &stmt, nullptr) == SQLITE_OK,
          "could not query PR operations");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
    sqlite3_bind_text(stmt, 1, operationId.data(), static_cast<int>(operationId.size()), SQLITE_TRANSIENT);
    check(sqlite3_step(stmt) == SQLITE_ROW, "PR operation was missing before the host call");
    return Json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
}

std::string watchedPrUrl(const std::filesystem::path& db) {
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2(db.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "could not read fixture database");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(raw, "SELECT url FROM git_pr_watches", -1, &stmt, nullptr) == SQLITE_OK, "could not query PR watches");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
    check(sqlite3_step(stmt) == SQLITE_ROW, "no pull request is watched");
    std::string url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    check(sqlite3_step(stmt) == SQLITE_DONE, "more than one pull request is watched");
    return url;
}

void freshPrReviewSchema() {
    Directory directory;
    const auto path = directory.path / "fresh.db";
    Store store(path);
    sqlite3* raw = nullptr;
    check(sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "could not open fresh database");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    sqlite3_stmt* stmt = nullptr;
    const auto prepared = sqlite3_prepare_v2(raw, R"SQL(
SELECT (SELECT count(*) FROM pragma_table_info('pr_review_items')
        WHERE name IN ('operation_id','git_key','candidates'))=3
   AND NOT EXISTS (SELECT 1 FROM pragma_table_info('pr_review_items') WHERE name='target_generation')
   AND EXISTS (SELECT 1 FROM pragma_index_list('pr_review_items')
               WHERE name='pending_pr_reconciliation' AND "unique"=1 AND partial=1)
   AND (SELECT group_concat(name) FROM pragma_index_info('pending_pr_reconciliation'))='operation_id'
)SQL",
                                             -1, &stmt, nullptr);
    check(prepared == SQLITE_OK, "could not query fresh PR review schema: " + std::string(sqlite3_errmsg(raw)));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
    check(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1,
          "Fresh DB has incorrect PR review columns or reconciliation index");
}

void runner() {
    Directory directory;
    const auto path = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Script script;
    script.openAfterSession = true;
    script.checkOriginatingSession = true;
    Id task, other;
    {
        Store store(path);
        task = Tasks(store).submit(intake());
        Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(Tasks(store).task(task).task.state == TaskState::completed && host->createCalls == 1,
              "Session-to-PR flow did not complete: " + Tasks(store).history(task).dump());
        other = Tasks(store).submit(intake("other", "other-project"));
        check(script.results.at("session-pr").at("prNumber") == 1, "task did not receive the PR number");
        check(historyCount(store, task, "git_mutation") == 2, "PR operation history must include intent and result");
        check(recordedMutationReason(Tasks(store).history(task), "git_open_pr"), "PR creation lost its rationale in task history");
        check(historyCount(store, task, "report") == 1, "Session result missing");
        check(Inbox(store).inbox(localOwner, "owner").at(0).at("prTitle") == "Pull request awaiting review", "PR awaiting review missing");
        check(GitEvents(store).receiveGitWebhook(
                  {hostName, "ci-before-restart", repoName, "ci", "completed", {1}, sha, "test job failed", 81}),
              "CI event was rejected before restart");
        check(historyCount(store, task, "git_webhook") == 0, "intake ran delivery synchronously");
    }
    {
        Store store(path);
        const auto otherHistory = Tasks(store).history(other);
        Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
        check(!GitEvents(store).receiveGitWebhook(
                  {hostName, "ci-before-restart", "org/other", "ci", "completed", {2}, sha, "changed duplicate", 81}),
              "same host and delivery ID inserted twice");
        check(GitEvents(store).receiveGitWebhook({hostName, "wrong-pr", repoName, "ci", "completed", {2}, sha, "wrong PR", 81}),
              "unmatched webhook was not recorded");
        check(GitEvents(store).receiveGitWebhook({hostName, "wrong-repo", "org/other", "ci", "completed", {1}, sha, "wrong repo", 81}),
              "unmatched repository event was not recorded");
        runtime.runConversations();
        check(script.notifications == 1 && host->logCalls == 1 && historyCount(store, task, "git_webhook") == 1,
              "CI event did not wake exactly the persistent PR owner");
        check(Tasks(store).history(other) == otherHistory &&
                  Conversations(store).conversation("other").dump().find("CI failed") == std::string::npos,
              "unrelated task was woken");
        check(Tasks(store).history(task).dump().find("CI failed. linker: missing symbol") != std::string::npos,
              "CI report is missing from the owning task");
        check(GitEvents(store).receiveGitWebhook({hostName, "review", repoName, "review", "submitted", {1}, sha, "Approved", 0}),
              "review rejected");
        check(GitEvents(store).receiveGitWebhook({hostName, "scan", repoName, "code_scan", "appeared", {}, sha, "Scan result", 0}),
              "scan rejected");
        runtime.runConversations();
        runtime.runConversations();
        const auto inbox = Inbox(store).inbox(localOwner, "owner");
        check(inbox.size() == 3 && inbox.dump().find("Pull request review submitted") != std::string::npos &&
                  inbox.dump().find("Code scanning result") != std::string::npos,
              "Git inbox decisions missing");
        const auto review = target(inbox.at(0));
        Inbox(store).decideInbox(localOwner, review, "request_changes");
        check(Inbox(store).inbox(localOwner, "owner").size() == 2 &&
                  Inbox(store).decisionHistory(localOwner, "owner").at(0).at("decision") == "request_changes",
              "Git inbox decision did not update and record");
        check(script.notifications == 3 && historyCount(store, task, "git_webhook") == 3, "webhooks were delivered more than once");

        const auto sibling = Tasks(store).submit(intake());
        auto args = openArgs("sibling-change");
        args["taskId"] = sibling;
        check(invoke(store, runtime, script, "watch-sibling", "git_open_pr", args).at("prNumber") == 2,
              "Could not create a second watch at the same commit");
        check(GitEvents(store).receiveGitWebhook({hostName, "shared-commit", repoName, "ci", "completed", {}, sha, "Shared SHA", 81}),
              "CI event with a shared SHA was not saved");
        runtime.runConversations();
        check(script.notifications == 3 && historyCount(store, sibling, "git_webhook") == 0,
              "A SHA-only CI event woke a task without an unambiguous watch");
    }
}

void invalidWebhookMetadata() {
    Directory directory;
    Store store(directory.path / "state.db");
    const GitWebhook valid{hostName, "metadata", repoName, "ci", "completed", {1}, sha, "Finished"};
    const auto rejects = [&](std::string_view name, auto change) {
        auto event = valid;
        change(event);
        bool invalid = false;
        try {
            GitEvents(store).receiveGitWebhook(event);
        } catch (const std::invalid_argument&) {
            invalid = true;
        }
        check(invalid, "Accepted webhook with " + std::string(name));
    };
    rejects("an invalid digest", [](auto& event) { event.payloadDigest = std::string(64, 'g'); });
    rejects("an unknown kind", [](auto& event) { event.kind = "other"; });
    rejects("a negative job ID", [](auto& event) { event.jobId = -1; });
    rejects("an invalid head SHA", [](auto& event) { event.headSha = std::string(40, 'g'); });
    rejects("an invalid PR number", [](auto& event) { event.prNumbers = {0}; });
    rejects("no PR number or head SHA", [](auto& event) {
        event.prNumbers.clear();
        event.headSha.clear();
    });
    check(GitEvents(store).receiveGitWebhook(valid), "Rejected valid webhook metadata");
}

void webhookHeadPeriods() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto first = Tasks(store).submit(intake());
    auto args = openArgs();
    args["taskId"] = first;
    check(invoke(store, runtime, script, "watch-first", "git_open_pr", args).at("prNumber") == 1, "could not create first watch");

    const std::string newSha(40, 'b');
    check(GitEvents(store).receiveGitWebhook({hostName, "original-job", repoName, "ci", "completed", {}, sha, "Original head", 0}),
          "could not enqueue original job");
    check(GitEvents(store).receiveGitWebhook({hostName, "early-job", repoName, "ci", "completed", {}, newSha, "New head", 0}),
          "could not enqueue early job");
    runtime.runConversations();
    check(historyCount(store, first, "git_webhook") == 1, "Early job was routed before synchronization");
    check(GitEvents(store).receiveGitWebhook({hostName, "queued-old-job", repoName, "ci", "completed", {}, sha, "Queued old job", 0}),
          "could not enqueue a job before synchronization");
    check(GitEvents(store).receiveGitWebhook({hostName, "first-sync", repoName, "pull_request", "synchronize", {1}, newSha, "New head", 0}),
          "could not enqueue synchronization");

    const auto second = Tasks(store).submit(intake());
    args = openArgs("other-change");
    args["taskId"] = second;
    check(invoke(store, runtime, script, "watch-second", "git_open_pr", args).at("prNumber") == 2, "could not create second watch");
    check(historyCount(store, first, "git_webhook") == 4, "The old or early job was lost across synchronization");
    check(GitEvents(store).receiveGitWebhook({hostName, "reused-head-job", repoName, "ci", "completed", {}, sha, "Reused head", 0}),
          "could not enqueue job on reused head");
    runtime.runConversations();
    check(historyCount(store, first, "git_webhook") == 4 && historyCount(store, second, "git_webhook") == 1,
          "A superseded head prevented the current watch from receiving a SHA-only job");
    check(GitEvents(store).receiveGitWebhook(
              {hostName, "return-to-head", repoName, "pull_request", "synchronize", {1}, sha, "Original head", 0}),
          "could not enqueue return to original head");
    runtime.runConversations();
    check(historyCount(store, first, "git_webhook") == 5 && historyCount(store, second, "git_webhook") == 1,
          "A SHA-only delivery was routed again when another watch moved to the same head");
}

void pausedGitNotifications() {
    Directory directory;
    const auto path = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    Script script;
    script.openAfterSession = true;
    Sessions sessions;
    Id task;
    {
        Store store(path);
        task = Tasks(store).submit(intake());
        Runtime runtime(store, script, sessions, {}, options(host));
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(Tasks(store).task(task).task.state == TaskState::completed && host->createCalls == 1,
              "Git notification fixture did not finish its watched request");
        store.setBotPaused(localOwner, "owner", true);
        check(GitEvents(store).receiveGitWebhook({hostName, "paused-delivery", repoName, "comment", "created", {1}, sha, "Paused", 0}),
              "Paused delivery was not acknowledged");
        runtime.runConversations();
        check(script.notifications == 0 && historyCount(store, task, "git_webhook") == 0, "A Git webhook woke a paused Bot");
        check(GitEvents(store).receiveGitWebhook({hostName, "paused-backlog", repoName, "ci", "completed", {}, sha, "Backlog", 0}),
              "Paused backlog delivery was not acknowledged");
    }
    {
        Store restarted(path);
        Runtime runtime(restarted, script, sessions, {}, options(host));
        restarted.setBotPaused(localOwner, "owner", false);
        runtime.runConversations();
        check(script.notifications == 0 && historyCount(restarted, task, "git_webhook") == 0,
              "Resume backfilled a Git webhook received during the pause");
        check(GitEvents(restarted).receiveGitWebhook({hostName, "after-resume", repoName, "ci", "completed", {}, sha, "Active", 0}),
              "New delivery was not acknowledged");
        runtime.runConversations();
        check(script.notifications == 1 && historyCount(restarted, task, "git_webhook") == 1,
              "A new SHA-only webhook missed the resumed Bot's watch");
    }
}

void reviewAfterTaskProgress() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = Tasks(store).submit(intake());
    for (int i = 0; i < 4 && Tasks(store).task(task).task.currentRevision.value_or(0) == 0; ++i) runtime.tick();
    check(Tasks(store).task(task).task.currentRevision.value_or(0) > 0, "task has no initial revision");

    auto args = openArgs();
    args["taskId"] = task;
    check(invoke(store, runtime, script, "open-review-pr", "git_open_pr", args).at("status") == "created",
          "fixture did not open a PR awaiting review");
    const auto gitReview = target(Inbox(store).inbox(localOwner, "owner").at(0));

    check(invoke(store, runtime, script, "send-instruction", "send_to_task",
                 {{"taskId", task}, {"generation", gitReview.targetGeneration}, {"instruction", "Include the test results"}})
                  .at("ok") == true,
          "fixture did not update the task within its revision");
    const auto inbox = Inbox(store).inbox(localOwner, "owner");
    check(inbox.size() == 1, "Git review was lost after a Task generation change");
    const auto latest = target(inbox.at(0));
    check(latest.itemId == gitReview.itemId && latest.targetRevision == gitReview.targetRevision &&
              latest.targetGeneration > gitReview.targetGeneration,
          "Git review did not follow the Task within its revision");
    try {
        Inbox(store).decideInbox(localOwner, gitReview, "approve");
        throw std::runtime_error("A PR review decision with an invalid generation was accepted");
    } catch (const RequestChanged&) {
    }
    Inbox(store).decideInbox(localOwner, latest, "approve");
    check(Inbox(store).inbox(localOwner, "owner").empty(), "Refreshed Git review decision did not close the item");
}

void webhookDeliveryResumesAfterAnIdlePoll() {
    Directory directory;
    const auto path = directory.path / "state.db";
    Store store(path), receiver(path);
    auto host = std::make_shared<FakeHost>();
    Script script;
    script.openAfterSession = true;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = Tasks(store).submit(intake());
    host->inspect = [&](const GitContext&, const GitOpenPr&) {
        check(GitEvents(store).receiveGitWebhook({hostName, "early-ci", repoName, "ci", "completed", {}, sha, "Early head", 0}),
              "Could not receive a webhook while the PR was opening");
        runtime.runConversations();
        check(historyCount(store, task, "git_webhook") == 0, "An unwatched event was delivered");
    };
    for (int i = 0; i < 8; ++i) runtime.tick();
    check(historyCount(store, task, "git_webhook") == 1, "A new watch did not receive the previously unmatched event");
    runtime.runConversations();

    constexpr unsigned queued = 101;
    for (unsigned i = 0; i < queued; ++i)
        check(GitEvents(receiver).receiveGitWebhook(
                  {hostName, "queued-" + std::to_string(i), repoName, "ci", "completed", {1}, sha, "Queued job", 0}),
              "Could not receive a webhook on another connection");
    for (int i = 0; i < 3; ++i) runtime.runConversations();
    check(historyCount(store, task, "git_webhook") == queued + 1, "Delivery lost or repeated events across connections or batches");
}

void reconciliationAfterTaskRevision() {
    Directory directory;
    const auto path = directory.path / "state.db";
    Store store(path);
    auto host = std::make_shared<FakeHost>();
    host->create = FakeHost::Create::lostResponse;
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = Tasks(store).submit(intake());
    for (int i = 0; i < 4 && Tasks(store).task(task).task.currentRevision.value_or(0) == 0; ++i) runtime.tick();
    const auto before = taskJson(Tasks(store).task(task));
    check(before.value("currentRevision", Id{0}) > 0, "task has no initial revision");

    auto args = openArgs();
    args["taskId"] = task;
    const auto lost = invoke(store, runtime, script, "lose-pr-response", "git_open_pr", args);
    check(lost.at("status") == "ambiguous", "fixture did not create an ambiguous operation");
    auto extra = host->prs.front();
    extra.pr.number = 2;
    extra.pr.url = "https://github.com/org/flock/pull/2";
    host->prs.push_back(std::move(extra));
    check(invoke(store, runtime, script, "find-two-prs", "git_open_pr", args).at("status") == "ambiguous",
          "fixture did not leave two verified candidates");
    const auto initial = Inbox(store).inbox(localOwner, "owner").at(0);
    check(initial.at("candidates").size() == 2, "fixture failed to put candidates in the inbox");
    const auto changed = invoke(store, runtime, script, "revise-task", "change_task",
                                {{"taskId", task},
                                 {"generation", before.at("generation")},
                                 {"revisionId", before.at("currentRevision")},
                                 {"instruction", "Update the success criteria"},
                                 {"doneWhen", "Verify the new completion criteria"}});
    check(changed.at("ok") == true && Tasks(store).task(task).task.replanning, "fixture did not revise the active task");
    check(invoke(store, runtime, script, "retry-after-replan", "git_open_pr", args).at("status") == "ambiguous",
          "fixture did not keep the PR creation unresolved");
    const auto latest = Inbox(store).inbox(localOwner, "owner").at(0);
    check(latest.at("itemId") == initial.at("itemId") && latest.at("targetRevision") != initial.at("targetRevision") &&
              latest.at("targetRevision") == Tasks(store).task(task).task.currentRevision.value() &&
              latest.at("targetGeneration") == Tasks(store).task(task).task.generation &&
              latest.at("candidates") == initial.at("candidates"),
          "Reconciliation lost its current target or verified candidates");
    try {
        Inbox(store).decideInbox(localOwner, target(initial), "reject_pr");
        throw std::runtime_error("A stale reconciliation decision was accepted after task revision");
    } catch (const RequestChanged&) {
    }
    Inbox(store).decideInbox(localOwner, target(latest), "link_pr", {{"prNumber", 2}});
    const auto saved = persistence(path, lost.at("operationId").get<std::string>());
    check(saved.at("status") == "created" && saved.at("prNumber") == 2 && host->createCalls == 1,
          "The current reconciliation target could not link the original operation");
}

void recovery(FakeHost::State state, bool losesRequest, bool multipleMatches = false, bool wrongCorrelation = false) {
    Directory directory;
    const auto path = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    host->create = losesRequest ? FakeHost::Create::lostRequest : FakeHost::Create::lostResponse;
    host->state = state;
    Sessions sessions;
    Script script;
    Id task;
    std::string operationId;
    InboxTarget initial{};
    {
        Store store(path);
        task = Tasks(store).submit(intake());
        Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
        host->inspect = [&](const GitContext& context, const GitOpenPr& request) {
            operationId = request.operationId;
            const auto pending = persistence(path, operationId);
            check(pending.at("status") == "pending" && pending.at("taskId") == context.taskId && pending.at("botId") == context.botId &&
                      pending.at("refs") == Json::array({hostName, repoName, "main", repoName, "change"}),
                  "create fired before durable operation intent");
        };
        auto args = openArgs();
        args["taskId"] = task;
        const auto lost = invoke(store, runtime, script, "lost", "git_open_pr", args);
        check(lost.at("status") == "ambiguous" && persistence(path, operationId).at("status") == "ambiguous",
              "lost response was not recorded unresolved");
        initial = target(Inbox(store).inbox(localOwner, "owner").at(0));
    }
    host->inspect = {};
    const auto repo = GitRepository::parse(hostName, repoName);
    host->prs.push_back({{{repo, repo, "main", "unrelated"}, 4, "https://github.com/org/flock/pull/4", operationId, sha, true},
                         "Change",
                         FakeHost::State::open});
    host->prs.push_back({{{repo, repo, "old", "change"}, 5, "https://github.com/org/flock/pull/5", operationId, sha, false},
                         "Change",
                         FakeHost::State::closed});
    for (const auto collisionState : {FakeHost::State::open, FakeHost::State::closed, FakeHost::State::merged}) {
        const auto number = static_cast<Id>(host->prs.size() + 10);
        host->prs.push_back(
            {{{repo, repo, "main", "change"}, number, "https://github.com/org/flock/pull/" + std::to_string(number), "", sha, false},
             "Change",
             collisionState});
    }
    if (wrongCorrelation) host->prs.front().pr.operationId = "someone-elses-operation";
    if (multipleMatches) {
        auto extra = host->prs.front();
        extra.pr.number = 6;
        extra.pr.url = "https://github.com/org/flock/pull/6";
        host->prs.push_back(std::move(extra));
    }
    {
        Store store(path);
        Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
        auto args = openArgs();
        args["taskId"] = task;
        const auto recovered = invoke(store, runtime, script, "retry", "git_open_pr", args);
        check(host->createCalls == 1 && host->lookupCalls == 1, "retry attempted a second create");
        if (losesRequest || multipleMatches || wrongCorrelation) {
            check(recovered.at("status") == "ambiguous" && !recovered.contains("prNumber"), "unverified PR was adopted");
            const auto items = Inbox(store).inbox(localOwner, "owner");
            check(items.size() == 1 && items.at(0).at("itemKind") == "pr_reconciliation", "reconciliation item missing or duplicated");
            const auto decision = target(items.at(0));
            const auto candidates = items.at(0).at("candidates");
            if (multipleMatches) {
                check(candidates.size() == 2, "Only exact correlations should be candidates");
                try {
                    Inbox(store).decideInbox(localOwner, initial, "reject_pr");
                    throw std::runtime_error("An outdated reconciliation decision was accepted");
                } catch (const DecisionUnavailable&) {
                }
                try {
                    Inbox(store).decideInbox(localOwner, decision, "link_pr", {{"prNumber", 4}});
                    throw std::runtime_error("An unverified candidate was accepted");
                } catch (const Conflict&) {
                }
                Inbox(store).decideInbox(localOwner, decision, "link_pr", {{"prNumber", 1}});
                check(persistence(path, operationId).at("prNumber") == 1, "Selecting a verified PR did not link it");
                check(watchedPrUrl(path) == "https://github.com/org/flock/pull/1", "Selected PR was not watched");
            } else {
                check(candidates.empty(), "Title collision was offered for manual linking");
                Inbox(store).decideInbox(localOwner, decision, "reject_pr");
                check(Inbox(store).inbox(localOwner, "owner").empty() && persistence(path, operationId).at("status") == "rejected",
                      "Rejecting reconciliation did not close the decision");
                check(invoke(store, runtime, script, "retry-rejected", "git_open_pr", args).at("status") == "rejected" &&
                          host->createCalls == 1,
                      "Rejected unknown operation was resent");
            }
        } else {
            check(recovered.at("status") == "created" && recovered.at("prNumber") == 1 &&
                      recovered.at("url") == "https://github.com/org/flock/pull/1" && recovered.at("operationId") == operationId &&
                      persistence(path, operationId).at("prNumber") == 1,
                  "the original PR was not recovered");
            check(Inbox(store).inbox(localOwner, "owner").dump().find("pr_reconciliation") == std::string::npos,
                  "resolved reconciliation still pending");
            check(watchedPrUrl(path) == recovered.at("url").get<std::string>(), "recovered pull request is not watched");
        }
    }
}

void accessAndActions() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    const auto task = Tasks(store).submit(intake());
    const auto other = Tasks(store).submit(intake("other"));
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const std::vector<std::pair<std::string, Json>> commands{
        {"git_repo_info", Json::object()},
        {"git_open_pr", openArgs()},
        {"git_update_pr", {{"prNumber", 1}, {"title", "Changed"}}},
        {"git_pr_status", {{"prNumber", 1}}},
        {"git_commit_status", {{"sha", sha}}},
        {"git_read_ci_logs", {{"jobId", 81}, {"maxBytes", 32}}},
        {"git_request_review", {{"prNumber", 1}, {"reviewers", {"alice"}}}},
        {"git_submit_review", {{"prNumber", 1}, {"verdict", "approve"}}},
        {"git_comment", {{"target", "pr"}, {"number", 1}, {"body", "Initial"}}},
        {"git_comment", {{"target", "pr"}, {"number", 1}, {"body", "Updated"}, {"commentId", 101}}},
        {"git_manage_issue", {{"action", "list"}}},
        {"git_manage_issue", {{"action", "read"}, {"issueNumber", 2}}},
        {"git_manage_issue", {{"action", "list_comments"}, {"issueNumber", 2}}},
        {"git_manage_issue", {{"action", "create"}, {"title", "Follow-up"}, {"labels", {"bug"}}, {"assignees", {"alice"}}}},
        {"git_manage_issue", {{"action", "update"}, {"issueNumber", 2}, {"state", "closed"}, {"labels", Json::array()}}},
    };
    for (auto action : {Action::repository_pr, Action::repository_issues})
        store.policy().changeNeverRule(localOwner, action, repositoryTarget, true);
    auto readOnly = openArgs();
    readOnly["taskId"] = task;
    check(invoke(store, runtime, script, "read-is-not-pr", "git_open_pr", readOnly).at("ok") == false && host->createCalls == 0,
          "repository read or write allowed PR creation");
    const auto deniedIssueComment =
        invoke(store, runtime, script, "read-is-not-issues", "git_comment",
               {{"host", hostName}, {"repo", repoName}, {"taskId", task}, {"target", "issue"}, {"number", 2}, {"body", "Comment"}});
    check(deniedIssueComment.at("ok") == false && host->hostCalls == 0, "repository read or write allowed an issue comment");
    const auto deniedIssueUpdate =
        invoke(store, runtime, script, "read-is-not-issue-update", "git_manage_issue",
               {{"host", hostName}, {"repo", repoName}, {"taskId", task}, {"action", "update"}, {"issueNumber", 2}, {"state", "closed"}});
    check(deniedIssueUpdate.at("error") == "Repository access denied" && host->hostCalls == 0,
          "repository read or write allowed an issue update");
    for (auto action : {Action::repository_pr, Action::repository_issues})
        store.policy().changeNeverRule(localOwner, action, repositoryTarget, false);
    for (auto action :
         {Action::repository_read, Action::repository_write, Action::repository_pr, Action::repository_issues, Action::repository_review})
        store.policy().changeNeverRule(localOwner, action, "github.com/*/private", true);
    auto argsFor = [task](Json args, std::string repo) {
        args.update({{"host", hostName}, {"repo", std::move(repo)}, {"taskId", task}});
        return args;
    };
    int counter = 0;
    for (const auto& [tool, args] : commands) {
        const auto denied = invoke(store, runtime, script, "denied-" + std::to_string(++counter), tool, argsFor(args, "org/private"));
        check(denied.at("ok") == false && denied.at("error") == "Repository access denied", "blocked repo reached Git host");
    }
    check(host->hostCalls == 0 && host->createCalls == 0 && host->logCalls == 0, "denied call reached adapter");
    host->upstreamPermitted = false;
    check(invoke(store, runtime, script, "upstream-denied", "git_repo_info", argsFor(Json::object(), repoName)).at("ok") == false &&
              host->hostCalls == 0,
          "upstream permission was ignored");
    host->upstreamPermitted = true;
    const auto find = invoke(store, runtime, script, "find", "git_find_repos", {{"host", hostName}, {"query", "FLOCK"}});
    check(find.at("repos") == Json::array({repoName}), "repository discovery exposed ungranted repositories");
    const auto repoTarget = GitRepository{hostName, repoName}.resource();
    store.policy().setMode(localOwner, "supervised");
    store.policy().changeRule(localOwner, RuleKind::ask, Action::repository_read, repoTarget, true);
    const auto askFirst = invoke(store, runtime, script, "ask-first-repos", "git_find_repos", {{"host", hostName}, {"query", "FLOCK"}});
    check(askFirst.at("repos").empty(), "Discovery exposed a repository requiring approval");
    store.policy().changeRule(localOwner, RuleKind::ask, Action::repository_read, repoTarget, false);
    store.policy().setMode(localOwner, "full");
    const auto later =
        invoke(store, runtime, script, "later-repos", "git_find_repos", {{"host", hostName}, {"query", "FLOCK"}, {"page", 11}});
    check(later.at("repos").empty() && later.at("nextPage") == 12, "An empty filtered page lost the GitHub continuation");
    const auto last = invoke(store, runtime, script, "last-repos", "git_find_repos",
                             {{"host", hostName}, {"query", "FLOCK"}, {"page", later.at("nextPage")}});
    check(last.at("repos") == Json::array({repoName}) && last.at("nextPage").is_null(),
          "Repository discovery could not reach a later page");
    const auto tasklessIssueWrite = invoke(store, runtime, script, "taskless-issue-write", "git_manage_issue",
                                           {{"host", hostName}, {"repo", repoName}, {"action", "create"}, {"title", "Change"}});
    check(tasklessIssueWrite.at("error") == "Select an owned task for the Git mutation" && host->hostCalls == 0,
          "issue mutation did not require an owned task");
    const auto tasklessIssueRead = invoke(store, runtime, script, "taskless-issue-read", "git_manage_issue",
                                          {{"host", hostName}, {"repo", repoName}, {"action", "list"}});
    check(tasklessIssueRead.at("ok") == true && host->hostCalls == 1, "issue listing unexpectedly required a task");
    auto foreign = argsFor(openArgs("foreign"), repoName);
    foreign["taskId"] = other;
    check(invoke(store, runtime, script, "foreign-task", "git_open_pr", foreign).at("ok") == false && host->createCalls == 0,
          "another Bot's task was mutated");
    auto cross = argsFor(openArgs("cross"), repoName);
    cross["headRepo"] = "fork/private";
    check(invoke(store, runtime, script, "cross-repo", "git_open_pr", cross).at("ok") == false && host->createCalls == 0,
          "blocked fork was used");
    const auto fork = GitRepository{hostName, "fork/public"}.resource();
    cross["headRepo"] = "fork/public";
    store.policy().changeRule(localOwner, RuleKind::ask, Action::repository_read, fork, true);
    store.policy().changeRule(localOwner, RuleKind::allow, Action::repository_pr, repoTarget, true);
    store.policy().setMode(localOwner, "supervised");
    check(invoke(store, runtime, script, "cross-repo-head-ask", "git_open_pr", cross).at("error") == "Head repository read access denied" &&
              host->createCalls == 0,
          "A base PR Allow bypassed the fork's Ask-first read rule");
    store.policy().setMode(localOwner, "full");
    for (const auto& [tool, args] : commands) {
        const auto name = "allowed-" + std::to_string(++counter);
        const auto result = invoke(store, runtime, script, name, tool, argsFor(args, repoName));
        check(result.at("ok") == true, "allowed action failed: " + tool + " " + result.dump());
    }
    for (auto [label, args] :
         std::vector<std::pair<std::string, Json>>{{"create", {{"action", "create"}, {"title", "Requires a task"}}},
                                                   {"update", {{"action", "update"}, {"issueNumber", 2}, {"state", "open"}}}}) {
        args.update({{"host", hostName}, {"repo", repoName}});
        const auto calls = host->hostCalls;
        check(invoke(store, runtime, script, "missing-task-" + label, "git_manage_issue", args).at("ok") == false &&
                  host->hostCalls == calls,
              "conversation issue mutation ran without a task");
    }
    check(invoke(store, runtime, script, "read-without-task", "git_manage_issue",
                 {{"host", hostName}, {"repo", repoName}, {"action", "read"}, {"issueNumber", 2}})
                  .at("ok") == true,
          "conversation issue read required a task");
    const auto duplicate = invoke(store, runtime, script, "retry-comment-create", "git_comment",
                                  argsFor({{"target", "pr"}, {"number", 1}, {"body", "Initial"}}, repoName));
    check(duplicate.at("data").at("id") == 101 && host->comments == 1, "repeated comment creation posted a duplicate");
    auto edit = argsFor({{"target", "pr"}, {"number", 1}, {"body", "Updated"}, {"commentId", 101}}, repoName);
    const auto retry = invoke(store, runtime, script, "retry-edit", "git_comment", edit);
    check(retry.at("data").at("id") == 101 && host->comments == 1 && host->edits == 2, "comment edit retry duplicated a comment");
    check(historyCount(store, task, "git_mutation") == 18 && historyCount(store, other, "git_mutation") == 0,
          "Git mutations were not recorded on the owning task");
    check(recordedMutationReason(Tasks(store).history(task, 0, 200), "git_update_pr"), "PR update lost its rationale in task history");

    const auto review = invoke(store, runtime, script, "review-existing-pr", "git_request_review",
                               argsFor({{"prNumber", 20}, {"reviewers", {"alice"}}}, repoName));
    check(review.at("ok") == true && review.at("watchAttached") == true, "Review request did not watch the existing PR");
    check(GitEvents(store).receiveGitWebhook({hostName, "existing-pr-review", repoName, "review", "submitted", {20}, sha, "Approved", 0}),
          "Could not enqueue a review for the existing PR");
    runtime.runConversations();
    check(script.notifications == 1 && historyCount(store, task, "git_webhook") == 1 && historyCount(store, other, "git_webhook") == 0,
          "Review result did not go to the requesting task");
}

void newAttempts() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto originalTask = Tasks(store).submit(intake());
    auto args = openArgs();
    args["taskId"] = originalTask;
    host->create = FakeHost::Create::rejected;
    const auto failed = invoke(store, runtime, script, "definite-rejection", "git_open_pr", args);
    check(failed.at("status") == "failed", "Definite rejection was treated as unknown");
    host->create = FakeHost::Create::success;
    const auto created = invoke(store, runtime, script, "fresh-attempt", "git_open_pr", args);
    check(created.at("prNumber") == 1 && created.at("operationId") != failed.at("operationId") && host->createCalls == 2,
          "A fresh request could not recover from a definitive rejection");

    auto comment = Json{{"taskId", originalTask}, {"host", hostName}, {"repo", repoName},
                        {"target", "pr"},         {"number", 1},      {"body", "Update requested"}};
    host->rejectComment = true;
    check(invoke(store, runtime, script, "comment-rejected", "git_comment", comment).at("status") == "failed",
          "Definite comment rejection was not recorded");
    host->rejectComment = false;
    check(invoke(store, runtime, script, "comment-retried", "git_comment", comment).at("ok") == true && host->comments == 1,
          "A fresh comment request could not retry a definitive rejection");

    host->prs.front().state = FakeHost::State::merged;
    args["taskId"] = Tasks(store).submit(intake());
    const auto next = invoke(store, runtime, script, "replacement-pr", "git_open_pr", args);
    check(next.at("prNumber") == 2 && host->createCalls == 3, "Branch pair was locked after the previous task's PR merged");
}

void taskMutationAttempts() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    const auto task = Tasks(store).submit(intake());
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const std::vector<std::string> states{"closed", "open", "closed"};
    script.onTaskResult = [&](const ModelInput& input) -> Decision {
        const auto previous = input.event.value("gitResults", Json::array());
        if (previous.empty()) return {"git_open_pr", openArgs(), {}};
        if (previous.back().at("tool") == "git_manage_issue") {
            check(previous.back().at("result").at("ok") == true, "task issue mutation did not use implicit task identity");
            return {"complete", {{"report", "PR is closed"}}, {}};
        }
        if (previous.size() > 1) {
            const auto& result = previous.back().at("result");
            check(result.at("ok") == true && result.at("data").at("state") == states.at(previous.size() - 2),
                  "task did not receive its own PR update result");
        }
        if (previous.size() <= states.size())
            return {
                "git_update_pr", {{"host", hostName}, {"repo", repoName}, {"prNumber", 1}, {"state", states.at(previous.size() - 1)}}, {}};
        return {"git_manage_issue", {{"host", hostName}, {"repo", repoName}, {"action", "create"}, {"title", "Follow-up"}}, {}};
    };
    for (int i = 0; i < 14 && Tasks(store).task(task).task.state != TaskState::completed; ++i) runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::completed && host->prStateUpdates == states &&
              host->prs.at(0).state == FakeHost::State::closed,
          "separate decisions on one event did not all update the PR");
}

int taskMutationChild(const std::filesystem::path& path, const std::filesystem::path& effects, const std::string& title) {
    auto host = std::make_shared<FakeHost>();
    host->prs.push_back({GitPullRequest{}, "Change", FakeHost::State::open});
    host->onPrStateUpdate = [&](std::string_view state) {
        std::ofstream(effects, std::ios::app) << state << '\n';
        _exit(73);
    };
    Script script;
    script.onTaskResult = [&](const ModelInput&) -> Decision {
        return {"git_update_pr", {{"host", hostName}, {"repo", repoName}, {"prNumber", 1}, {"state", "closed"}, {"title", title}}, {}};
    };
    Sessions sessions;
    Store store(path);
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    for (int i = 0; i < 10; ++i) runtime.tick();
    return 1;
}

void taskMutationRestart(bool protectTitleOnRestart) {
    Directory directory;
    const auto path = directory.path / "state.db";
    const auto effects = directory.path / "host-effects";
    const std::string title = "fixture-title-protected-after-restart";
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Id task;
    {
        Store store(path);
        task = Tasks(store).submit(intake());
        Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
        auto args = openArgs();
        args["taskId"] = task;
        check(invoke(store, runtime, script, "fixture-pr", "git_open_pr", args).at("ok") == true, "fixture PR did not open");
    }
    const auto writeEffect = [&](std::string_view state) { std::ofstream(effects, std::ios::app) << state << '\n'; };
    const std::string pathArg = path.string(), effectsArg = effects.string();
    char* const argv[]{executable.data(),
                       const_cast<char*>(mutationChildFlag),
                       const_cast<char*>(pathArg.c_str()),
                       const_cast<char*>(effectsArg.c_str()),
                       const_cast<char*>(title.c_str()),
                       nullptr};
    pid_t child = 0;
    check(posix_spawn(&child, executable.c_str(), nullptr, nullptr, argv, environ) == 0, "could not start the crash fixture");
    int status = 0;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73,
          "fixture did not crash after dispatching the saved decision");
    host->onPrStateUpdate = writeEffect;
    int calls = 0;
    script.onTaskResult = [&](const ModelInput& input) -> Decision {
        ++calls;
        if (protectTitleOnRestart) {
            const auto& feedback = input.event.at("decisionFeedback").back();
            check(feedback.at("result").at("delivery") == "unknown" && !feedback.at("args").contains("title"),
                  "Rejecting a saved mutation lost the unknown outcome or exposed its arguments");
        } else {
            const auto& result = input.event.at("gitResults").back().at("result");
            check(result.at("ok") == false && result.at("status") == "unknown", "interrupted mutation did not retain unknown outcome");
        }
        return {"complete", {{"report", "PR update outcome unknown"}}, {}};
    };
    Store store(path);
    if (protectTitleOnRestart) GitEvents(store).protectGitCredential(title);
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    for (int i = 0; i < 5 && Tasks(store).task(task).task.state != TaskState::completed; ++i) runtime.tick();
    std::ifstream log(effects);
    const std::string actions((std::istreambuf_iterator<char>(log)), {});
    check(Tasks(store).task(task).task.state == TaskState::completed && calls == 1 && actions == "closed\n",
          "restart repeated inference or dispatched the saved Git mutation twice");
}

void taskReadRecovery() {
    Directory directory;
    const auto path = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    const std::string secret = "fixture-private-git-log-value";
    for (Id job = 81; job <= 85; ++job) host->jobLogs.emplace(job, GitLog{"job " + std::to_string(job) + " " + secret.substr(0, 12), true});
    Script script;
    Sessions sessions;
    Id task;
    {
        Store store(path);
        GitEvents(store).protectGitCredential(secret);
        task = Tasks(store).submit(intake());
        script.onTaskResult = [&](const ModelInput&) -> Decision {
            return {
                "git_read_ci_logs", {{"host", "GITHUB.COM"}, {"repo", "ORG/FLOCK"}, {"jobId", 81 + host->logCalls}, {"maxBytes", 32}}, {}};
        };
        Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
        for (int i = 0; i < 14 && host->logCalls < 5; ++i) runtime.tick();
        check(host->logCalls == 5, "fixture did not read five jobs");
    }
    Store store(path);
    const auto blockRead = [&](bool block) {
        store.policy().changeNeverRule(localOwner, Action::repository_read, repositoryTarget, block);
    };
    int phase = 0;
    Id saved = 0;
    script.onTaskResult = [&](const ModelInput& input) -> Decision {
        const auto& event = input.event;
        const auto& recent = event.at("gitResults");
        if (phase == 0) {
            check(recent.size() == 4 && recent.front().at("target").at("jobId") == 82,
                  "first Git read did not leave the recent projection");
            check(event.at("gitReadRefs").size() == 5, "saved references were lost on restart");
            saved = event.at("gitReadRefs").front().get<Id>();
        } else if (phase == 1) {
            const auto& replay = recent.back();
            check(replay.at("readId") == saved && replay.at("sourceTool") == "git_read_ci_logs" &&
                      replay.at("target") == Json{{"host", hostName}, {"repo", repoName}, {"jobId", 81}, {"maxBytes", 32}} &&
                      replay.at("result").at("data") == Json{{"text", "job 81 [redacted]"}, {"truncated", true}},
                  "reopened database did not recover the original authorized, redacted read");
            blockRead(true);
        } else {
            check(std::ranges::all_of(recent,
                                      [](const Json& entry) { return !entry.contains("target") && entry.at("result").at("ok") == false; }),
                  "revoked read access leaked previous Git evidence");
            if (phase == 2) {
                blockRead(false);
                host->upstreamPermitted = false;
            } else
                return {"complete", {{"report", "Checked historical job evidence and revocation"}}, {}};
        }
        ++phase;
        return {"git_read_result", {{"host", hostName}, {"readId", saved}}, {}};
    };
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    for (int i = 0; i < 8 && Tasks(store).task(task).task.state != TaskState::completed; ++i) runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::completed && phase == 3 && host->logCalls == 5,
          "recovery did not use saved snapshots through both access checks");
}

void approvedTaskReadProjects() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    int phase = 0;
    script.onTaskResult = [&](const ModelInput& input) -> Decision {
        if (phase++ == 0) return {"git_repo_info", {{"host", hostName}, {"repo", repoName}}, {}};
        const auto& read = input.event.at("gitResults").back();
        check(read.at("sourceTool") == "git_repo_info" && read.at("result").at("ok") == true, "Ask-first hid its approved Git read");
        return {"complete", {{"report", "Reviewed repository information"}}, {}};
    };
    store.policy().setMode(localOwner, "supervised");
    store.policy().changeRule(localOwner, RuleKind::allow, Action::environment_launch, intake().environmentId, true);
    store.policy().changeRule(localOwner, RuleKind::ask, Action::repository_read, GitRepository{hostName, repoName}.resource(), true);
    Runtime runtime(store, script, sessions, [](auto) { return std::string{}; }, options(host));
    const auto task = Tasks(store).submit(intake(), &localOwner);
    for (int i = 0; i < 8 && Inbox(store).inbox(localOwner).empty(); ++i) runtime.tick();
    const auto items = Inbox(store).inbox(localOwner);
    check(items.size() == 1 && host->hostCalls == 0, "Git read reached the host before approval");
    const auto& card = items.at(0);
    Inbox(store).decideInbox(localOwner,
                             {card.at("botId"), InboxKind::policy_approval, card.at("itemId"), card.at("waitId"), card.at("taskId"),
                              card.at("targetRevision"), card.at("targetGeneration")},
                             "allow_once");
    for (int i = 0; i < 8 && Tasks(store).task(task).task.state != TaskState::completed; ++i) runtime.tick();
    check(phase == 2 && host->hostCalls == 1 && Tasks(store).task(task).task.state == TaskState::completed,
          "Approved Git read did not settle through the task");
}

void discoveryReplayRevocation() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    int phase = 0;
    script.onTaskResult = [&](const ModelInput& input) -> Decision {
        if (phase++ == 0) {
            host->storedDiscoveryPermitted = false;
            return {"git_find_repos", {{"host", hostName}, {"query", "flock"}}, {}};
        }
        const auto& recent = input.event.at("gitResults").back();
        check(recent.at("result").at("ok") == false && !recent.contains("target"),
              "Discovery replay exposed a repository after upstream revocation");
        return {"complete", {{"report", "Checked discovery authorization"}}, {}};
    };
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = Tasks(store).submit(intake());
    for (int i = 0; i < 10 && Tasks(store).task(task).task.state != TaskState::completed; ++i) runtime.tick();
    check(phase == 2 && Tasks(store).task(task).task.state == TaskState::completed, "Discovery replay was not exercised");
}

void boundedTaskReadAndForeignReference(bool comments) {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Json page = Json::array();
    for (int number = 1; number <= 30; ++number) {
        if (comments)
            page.push_back({{"id", number}, {"author", "reviewer-" + std::to_string(number)}, {"body", std::string(1500, '\1')}});
        else
            page.push_back({{"number", number}, {"title", std::string(800, '"')}, {"body", std::string(1500, '"')}});
    }
    host->result = {{"items", std::move(page)}, {"nextPage", 2}};
    Script script;
    Sessions sessions;
    const auto first = Tasks(store).submit(intake());
    Id saved = 0;
    script.onTaskResult = [&](const ModelInput& input) -> Decision {
        const bool owner = input.task.at("id") == first;
        const auto recent = input.event.value("gitResults", Json::array());
        if (recent.empty()) {
            if (owner) {
                Json args{{"host", hostName}, {"repo", repoName}, {"action", comments ? "list_comments" : "list"}};
                if (comments) args["issueNumber"] = 42;
                return {"git_manage_issue", std::move(args), {}};
            }
            check(saved > 0 && !input.event.contains("gitReadRefs"), "another task discovered the saved reference");
            return {"git_read_result", {{"host", hostName}, {"readId", saved}}, {}};
        }
        const auto& result = recent.back().at("result");
        if (owner) {
            saved = recent.back().at("readId");
            check(result.at("ok") == true && result.at("truncated") == true && result.dump().size() <= 24576 &&
                      result.at("data").at("nextPage") == 2,
                  "oversized Git read lost its bound or pagination");
            const auto& items = result.at("data").at("items");
            check(items.size() == 30, "bounded Git result omitted items from the page");
            for (int number = 1; number <= 30; ++number) {
                const auto& item = items.at(number - 1);
                check(item.at(comments ? "id" : "number") == number, "bounded Git result lost a page item's identifier");
                if (comments) check(item.at("author") == "reviewer-" + std::to_string(number), "bounded Git result lost a comment author");
            }
        } else
            check(result.at("ok") == false && !recent.back().contains("target"), "another task opened a saved read");
        return {"complete", {{"report", "Checked the Git read"}}, {}};
    };
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    for (int i = 0; i < 10 && Tasks(store).task(first).task.state != TaskState::completed; ++i) runtime.tick();
    check(Tasks(store).task(first).task.state == TaskState::completed, "bounded read did not reach the task");
    const auto other = Tasks(store).submit(intake("owner", "other-project"));
    for (int i = 0; i < 10 && Tasks(store).task(other).task.state != TaskState::completed; ++i) runtime.tick();
    check(Tasks(store).task(other).task.state == TaskState::completed && host->hostCalls == 1,
          "task-scoped Git lookup reached the wrong repository read");
}

void redaction() {
    Directory directory;
    const auto path = directory.path / "state.db";
    Store store(path);
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const std::string credential = "fixture-pr-credential-value";
    const std::string secret = "fixture-protected-other-bot-value";
    GitEvents(store).protectGitCredential(credential);
    auto policy = store.policy();
    const auto input = policy.requestSecret("other", {"example"});
    policy.submitSecret(localOwner, "other", input, secret);
    const auto task = Tasks(store).submit(intake());
    host->result["description"] = credential + " " + secret;
    host->result[credential] = secret;
    const auto repo = Json{{"host", hostName}, {"repo", repoName}};
    const auto details = invoke(store, runtime, script, "details", "git_repo_info", repo).dump();
    check(details.find(secret) == details.npos && details.find(credential) == details.npos && details.find("[redacted]") != details.npos,
          "Repository output exposed secrets");
    host->log = {"build error " + credential.substr(0, 12), true};
    auto args = repo;
    args.update({{"jobId", 81}, {"maxBytes", 32}});
    const auto log = invoke(store, runtime, script, "redacted-log", "git_read_ci_logs", args).at("data");
    check(log.at("text") == "build error [redacted]" && log.at("truncated") == true, "Log byte limit exposed a partial credential");

    auto open = openArgs();
    open["taskId"] = task;
    check(invoke(store, runtime, script, "open-pr", "git_open_pr", open).at("ok") == true, "Could not create watch");
    check(GitEvents(store).receiveGitWebhook(
              {hostName, "secret-event", repoName, "review", "submitted", {1}, sha, credential + " " + secret, 0}),
          "Webhook with redactable content was rejected");
    runtime.runConversations();
    for (const auto& content : {Tasks(store).history(task).dump(), Inbox(store).inbox(localOwner, "owner").dump()})
        check(content.find(secret) == content.npos && content.find(credential) == content.npos, "Webhook output exposed secrets");
    for (const auto& file : {path.string(), path.string() + "-wal"}) {
        std::ifstream database(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(database)), {});
        check(bytes.find(secret) == bytes.npos && bytes.find(credential) == bytes.npos, "Raw webhook content was persisted");
    }
}

void notificationCannotControlASession() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = Tasks(store).submit(intake());
    for (int i = 0; i < 8 && Tasks(store).task(task).task.state != TaskState::running; ++i) runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::running, "Session did not start");
    auto args = openArgs();
    args["taskId"] = task;
    check(invoke(store, runtime, script, "create-watch", "git_open_pr", args).at("ok") == true, "Could not create watch");

    Json result;
    script.rawNotification = [&](const ModelInput& input) {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) {
            check(std::ranges::find(input.tools, "message_session") == input.tools.end(), "Session write was offered in a notification");
            return Decision{"message_session",
                            {{"taskId", task}, {"generation", Tasks(store).task(task).task.generation}, {"text", "Act on the webhook"}},
                            {}};
        }
        result = steps.at(0).at("result");
        return Decision{"reply", {{"text", "Git notification received."}}, {}};
    };
    check(GitEvents(store).receiveGitWebhook({hostName, "session-effect", repoName, "comment", "created", {1}, sha, "A comment", 0}),
          "Could not enqueue comment");
    runtime.runConversations();
    check(result.value("ok", true) == false && sessions.sessionSends.empty(),
          "A generic model executed a Session write that the turn did not offer");
}

void notificationBudgetAndRevocation() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Script script;
    Sessions sessions;
    Runtime runtime(store, script, sessions, [](std::string_view) { return std::string{}; }, options(host));
    auto limited = intake();
    limited.config["modelCallLimit"] = 1;
    const auto task = Tasks(store).submit(limited);
    auto args = openArgs();
    args["taskId"] = task;
    check(invoke(store, runtime, script, "create-watch", "git_open_pr", args).at("ok") == true, "Could not create watch");
    check(GitEvents(store).receiveGitWebhook({hostName, "budget-event", repoName, "ci", "completed", {1}, sha, "A job failed", 81}),
          "Could not enqueue job");
    runtime.runConversations();
    const auto inbox = Inbox(store).inbox(localOwner, "owner");
    const auto approval =
        std::find_if(inbox.begin(), inbox.end(), [](const Json& row) { return row.at("itemKind") == "conversation_model_budget"; });
    check(approval != inbox.end(), "The notification did not pause for model budget");
    Inbox(store).decideInbox(localOwner, target(*approval), "approve");
    runtime.runConversations();
    const auto history = Tasks(store).history(task);
    const auto event = std::find_if(history.begin(), history.end(), [](const Json& row) { return row.at("kind") == "git_webhook"; });
    check(event != history.end() && event->at("data").at("jobId") == 81 && event->at("data").at("headSha") == sha &&
              event->at("data").at("prNumber") == 1,
          "Budget approval erased the Git delivery from task history");
    check(history.dump().find("CI failed. linker: missing symbol") != std::string::npos, "Approved model call did not resume");

    script.onNotification = [&] { store.policy().changeNeverRule(localOwner, Action::repository_read, repositoryTarget, true); };
    const auto replies = historyCount(store, task, "conversation_reply");
    check(GitEvents(store).receiveGitWebhook({hostName, "revoked-turn", repoName, "comment", "created", {1}, sha, "An update", 0}),
          "Could not enqueue new event");
    runtime.runConversations();
    check(script.notifications == 2 && historyCount(store, task, "conversation_reply") == replies,
          "A queued notification posted a reply after adding a Never rule");
    check(GitEvents(store).receiveGitWebhook({hostName, "revoked-event", repoName, "comment", "created", {1}, sha, "Another update", 0}),
          "Could not enqueue event after revocation");
    runtime.runConversations();
    check(historyCount(store, task, "git_webhook") == 2, "A blocked repository received another task event");
}

void schemas() {
    ModelInput input{.task = Json::object(),
                     .event = {{"kind", "conversation_turn"}},
                     .history = Json::array(),
                     .notes = {},
                     .tools = gitTools(),
                     .feedback = {},
                     .evidence = Json::array(),
                     .config = intake().config};
    const auto request = app::responseRequest(input);
    const auto functions = request.at("tools").at(0).at("tools");
    check(functions.size() == 11, "missing Git model schemas");
    for (const auto& function : functions) {
        const auto& schema = function.at("parameters");
        check(schema.at("additionalProperties") == false && schema.at("properties").contains("host"), "unscoped Git schema");
        if (function.at("name") == "git_read_ci_logs") {
            check(schema.at("properties").at("maxBytes").at("maximum") == 16384 &&
                      std::ranges::find(schema.at("required"), "jobId") != schema.at("required").end(),
                  "incorrect log schema");
            check(!schema.at("properties").contains("reason"), "Git log read still offers a rationale");
        }
        if (function.at("name") == "git_comment") {
            check(schema.at("properties").at("commentId").at("type") == "integer" &&
                      schema.at("properties").at("target").at("enum") == Json::array({"pr", "issue"}),
                  "incorrect comment schema");
            check(std::ranges::find(schema.at("required"), "reason") != schema.at("required").end(),
                  "Git comment schema does not require a rationale");
        }
        if (function.at("name") == "git_manage_issue") {
            for (const auto* action : {"list", "read", "list_comments", "create", "update"}) {
                const auto& conditions = schema.at("oneOf");
                const auto matching = [&](const Json& branch) {
                    const auto& actions = branch.at("properties").at("action").at("enum");
                    return std::ranges::find(actions, action) != actions.end();
                };
                check(std::ranges::count_if(conditions, matching) == 1, "issue schema action did not match exactly one branch");
                const auto branch = std::ranges::find_if(conditions, matching);
                const auto required = branch->value("required", Json::array());
                const bool mutates = std::string_view(action) == "create" || std::string_view(action) == "update";
                for (const auto* key : {"taskId", "reason"})
                    check((std::ranges::find(required, key) != required.end()) == mutates,
                          "issue schema requires the wrong fields for this action");
            }
        }
    }

    const Json list{{"host", hostName}, {"repo", repoName}, {"action", "list"}};
    const auto read = decodeCall(request, "git_manage_issue", list).calls.at(0);
    check(read.args == list && read.reason.empty(), "reasonless issue listing did not decode");

    const Json create{{"host", hostName}, {"repo", repoName}, {"action", "create"}, {"title", "Follow-up"}, {"taskId", 1}};
    bool rejected = false;
    try {
        decodeCall(request, "git_manage_issue", create);
    } catch (const ModelError& error) {
        rejected = error.code == ModelErrorCode::invalid_tool_arguments;
    }
    check(rejected, "issue creation decoded without a required rationale");
    auto withReason = create;
    withReason["reason"] = fixtureReason;
    const auto write = decodeCall(request, "git_manage_issue", withReason).calls.at(0);
    check(write.args == create && write.reason == fixtureReason, "issue creation lost its rationale when decoded");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string_view(argv[1]) == mutationChildFlag) return taskMutationChild(argv[2], argv[3], argv[4]);
        executable = argv[0];
        freshPrReviewSchema();
        schemas();
        runner();
        invalidWebhookMetadata();
        webhookHeadPeriods();
        pausedGitNotifications();
        reviewAfterTaskProgress();
        webhookDeliveryResumesAfterAnIdlePoll();
        reconciliationAfterTaskRevision();
        recovery(FakeHost::State::open, false);
        recovery(FakeHost::State::closed, false);
        recovery(FakeHost::State::merged, false);
        recovery(FakeHost::State::open, true);
        recovery(FakeHost::State::open, false, true);
        recovery(FakeHost::State::open, false, false, true);
        accessAndActions();
        newAttempts();
        taskMutationAttempts();
        taskMutationRestart(false);
        taskMutationRestart(true);
        taskReadRecovery();
        approvedTaskReadProjects();
        discoveryReplayRevocation();
        boundedTaskReadAndForeignReference(false);
        boundedTaskReadAndForeignReference(true);
        redaction();
        notificationCannotControlASession();
        notificationBudgetAndRevocation();
        testGitPrFollowup();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
