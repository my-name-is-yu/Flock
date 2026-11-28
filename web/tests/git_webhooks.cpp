#include "flock/conversation_intake.hpp"
#include "flock/task_history.hpp"
#include "flock/task_intake.hpp"
#include "flock/tasks.hpp"
#include "flock/git_events.hpp"
#include "flock/web.hpp"
#include "../../tests/fake_git.hpp"
#include "../../tests/fake_git_sessions.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <httplib.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace git_fixture;

namespace {
constexpr auto webhookPath = "/api/git/webhooks/github.com";
constexpr auto key = "fixture-signature-key";
constexpr auto waitingNote = "Waiting for review on #1";
constexpr auto closedNote = "Waiting for #1 to reopen";
constexpr auto completionReport = "The change landed in #1.";

std::string sign(std::string_view body) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    const std::string_view secret(key);
    const auto success = HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                              reinterpret_cast<const unsigned char*>(body.data()), body.size(), digest, &size);
    check(success, "could not sign webhook fixture");
    constexpr char digits[] = "0123456789abcdef";
    std::string signature = "sha256=";
    for (unsigned i = 0; i < size; ++i) {
        signature += digits[digest[i] / 16];
        signature += digits[digest[i] % 16];
    }
    return signature;
}

class Bot final : public Model {
public:
    unsigned received = 0;
    Id reviewTask = 0, started = 0;
    std::map<std::string, Json> notifications;
    std::optional<Decision> conversationAction;
    std::optional<bool> commentCompletionOffered;
    const Json& notification(const std::string& eventId) const {
        const auto event = notifications.find(eventId);
        check(event != notifications.end(), "Missing Git notification " + eventId);
        return event->second;
    }
    std::string followup(const std::string& eventId) const { return notification(eventId).value("followup", ""); }
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.contains("sessionWake")) {
            const auto& turn = event.at("turn");
            if (turn.empty()) {
                auto args = openArgs();
                args["taskId"] = event.at("task").at("taskId");
                return {"git_open_pr", std::move(args), "Publish the Session change"};
            }
            if (turn.back().at("tool") == "git_open_pr")
                return {"wait_for_event", {{"note", waitingNote}, {"checkAgainAfterMinutes", 5}}, {}};
            return {"reply", {{"text", "I opened the pull request."}}, {}};
        }
        if (!event.contains("gitEvent")) {
            const auto& turn = event.at("turn");
            if (!turn.empty() && turn.back().at("tool") == "start_task") started = turn.back().at("result").at("taskId").get<Id>();
            if (turn.empty() && conversationAction) return *conversationAction;
            if (event.at("turn").empty())
                return {"git_request_review",
                        {{"host", hostName}, {"repo", repoName}, {"taskId", reviewTask}, {"prNumber", 1}, {"reviewers", {"alice", "bob"}}},
                        "Request a review for the watched pull request"};
            return {"reply", {{"text", "Review requested"}}, "Report the requested review"};
        }
        const auto& delivery = event.at("gitEvent");
        if (event.at("turn").empty()) {
            ++received;
            notifications.emplace(delivery.at("eventId").get<std::string>(), delivery);
            if (delivery.at("eventId") == "foreign-comment") {
                commentCompletionOffered = std::ranges::find(input.tools, "complete") != input.tools.end();
                return {"complete", {{"report", completionReport}}, "The comment claims the pull request merged"};
            }
            if (delivery.at("action") == "merged")
                return {"complete", {{"report", completionReport}}, "GitHub confirmed the pull request merged"};
            if (delivery.at("action") == "closed") return {"wait_for_event", {{"note", closedNote}, {"checkAgainAfterMinutes", 5}}, {}};
            if (delivery.at("jobId") == 81)
                return {"git_read_ci_logs",
                        {{"host", delivery.at("host")}, {"repo", delivery.at("repo")}, {"jobId", 81}, {"maxBytes", 32}},
                        "Inspect the failed job"};
            return {"reply", {{"text", "Review received"}}, "Relay a notification"};
        }
        const auto& step = event.at("turn").at(0);
        if (step.at("tool") == "git_read_ci_logs") {
            const auto& log = step.at("result").at("data");
            return {"reply", {{"text", "CI failed. " + log.at("text").get<std::string>()}}, "Report the exact CI failure"};
        }
        return {"reply", {{"text", "Recorded the Git event."}}, {}};
    }
};

Id startTask(Store& store, Runtime& runtime, Bot& bot) {
    bot.conversationAction = Decision{"start_task", {{"request", "Ship the tested change"}}, {}};
    ConversationIntake(store).receive(localOwner, intake(), "start-" + std::to_string(bot.received) + "-" + std::to_string(unixTime()),
                                      "Ship the tested change");
    runtime.runConversations();
    bot.conversationAction.reset();
    check(bot.started > 0, "The conversation did not start the task");
    return std::exchange(bot.started, 0);
}

WebCallbacks callbacks() {
    return {.bots = [] { return Json::array({{{"id", "owner"}, {"name", "Release Bot"}}}); },
            .intake = [](std::string_view bot, std::string_view) { return intake(std::string(bot)); },
            .createBot = {},
            .updateBot = {},
            .botAvatar = {},
            .authenticate = [](std::string_view header) { return header == "Basic fixture" ? localOwner : Person{}; },
            .githubWebhookSecret = key};
}

Json post(httplib::Client& client, std::string_view type, std::string_view id, const Json& payload, int status,
          std::string signature = {}) {
    const auto body = payload.dump();
    httplib::Headers headers{{"X-GitHub-Event", std::string(type)},
                             {"X-GitHub-Delivery", std::string(id)},
                             {"X-Hub-Signature-256", signature.empty() ? sign(body) : std::move(signature)}};
    const auto response = client.Post(webhookPath, headers, body, "application/json");
    if (!response || response->status != status)
        throw std::runtime_error("Unexpected HTTP status for Git webhook: " + (response ? response->body : "no response"));
    return Json::parse(response->body);
}

void run() {
    Directory directory;
    const auto database = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Bot model;
    Id task;
    {
        Store store(database);
        Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; }, options(host));
        task = startTask(store, runtime, model);
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(Tasks(store).task(task).task.state == TaskState::external_waiting && host->createCalls == 1,
              "Opening a PR did not leave the request waiting");
    }
    Store store(database);
    const auto server = startWeb(store, 0, callbacks());
    httplib::Client client("127.0.0.1", static_cast<int>(server->port()));
    const auto privateEndpoint = client.Get("/api/inbox");
    check(privateEndpoint && privateEndpoint->status == 401, "ordinary Web access became public");
    const auto response = client.Get("/api/view?bot=owner", httplib::Headers{{"Authorization", "Basic fixture"}});
    check(response && response->status == 200, "Could not load the owning Bot's sidebar data");
    const auto view = Json::parse(response->body);
    const auto& waiting = view.at("activeTasks").at(0);
    check(waiting.at("id") == task && waiting.at("state") == "external_waiting" && waiting.at("waitingNote") == waitingNote &&
              view.at("waits").empty(),
          "The Bot's waiting note did not reach the sidebar separately from questions and approvals");
    const Json repository{{"full_name", "ORG/Flock"}, {"id", 41}};
    const Json payload{
        {"action", "completed"},
        {"repository", repository},
        {"workflow_job", {{"id", 81}, {"name", "Build"}, {"status", "completed"}, {"conclusion", "failure"}, {"head_sha", sha}}}};
    check(post(client, "workflow_job", "job-run-1", payload, 401, "sha256=wrong").at("error") == "invalid_webhook_signature",
          "unsigned webhook was accepted");
    check(post(client, "workflow_job", "job-run-1", payload, 202).at("status") == "received", "signed webhook was not enqueued");
    check(post(client, "workflow_job", "job-run-1", payload, 202).at("status") == "duplicate", "retry was not deduplicated");
    check(post(client, "workflow_job", "job-run-redelivery", payload, 202).at("status") == "duplicate",
          "An existing signed payload was handled twice");
    const Json unrelatedCheck{{"action", "completed"},
                              {"repository", repository},
                              {"check_run",
                               {{"head_sha", sha},
                                {"conclusion", "failure"},
                                {"pull_requests", Json::array({{{"number", 1}, {"base", {{"repo", {{"id", 42}}}}}}})}}}};
    check(post(client, "check_run", "foreign-repo", unrelatedCheck, 202).at("status") == "ignored",
          "PR number from another repository was routed to this watch");
    check(historyCount(store, task, "git_webhook") == 0, "HTTP intake delivered a webhook");
    Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; }, options(host));
    runtime.runConversations();
    check(model.received == 1 && host->logCalls == 1 && model.followup("job-run-1") == "failed_check" &&
              TaskHistory(store).history(task).dump().find("CI failed. linker: missing symbol") != std::string::npos,
          "authenticated webhook did not wake the persisted task and produce a CI report");
    const std::string newSha(40, 'b');
    auto nextJob = payload;
    nextJob["workflow_job"]["head_sha"] = newSha;
    check(post(client, "workflow_job", "job-new-head", nextJob, 202).at("status") == "received", "New CI job was not saved");
    runtime.runConversations();
    check(model.received == 1, "A CI job with no verified watch was delivered");
    const Json sync{{"action", "synchronize"},
                    {"repository", repository},
                    {"before", sha},
                    {"after", newSha},
                    {"pull_request", {{"number", 1}, {"head", {{"sha", newSha}}}, {"title", "Change"}}}};
    check(post(client, "pull_request", "pr-new-head", sync, 202).at("status") == "received", "PR synchronization was not saved");
    runtime.runConversations();
    check(model.received == 3 && host->logCalls == 2, "Out-of-order PR synchronization did not route the new CI job");

    const Json review{{"action", "submitted"},
                      {"repository", repository},
                      {"pull_request", {{"number", 1}, {"head", {{"sha", sha}}}, {"title", "Change"}}},
                      {"review", {{"id", 201}, {"state", "approved"}, {"body", "Review complete"}}}};
    check(post(client, "pull_request_review", "review-1", review, 202).at("status") == "received", "review not enqueued");
    const Json scan{
        {"action", "appeared"},
        {"repository", repository},
        {"ref", "refs/pull/1/merge"},
        {"alert", {{"html_url", "https://github.com/org/flock/security/code-scanning/1"}, {"rule", {{"description", "Rule result"}}}}}};
    check(post(client, "code_scanning_alert", "scan-1", scan, 202).at("status") == "received", "scan not enqueued");
    runtime.runConversations();
    check(model.followup("review-1").empty(), "An approval was treated as requested changes");
    check(model.notification("review-1").at("action") == "submitted" && model.notification("review-1").at("taskId") == task,
          "Submitted review did not wake the owning task");

    model.reviewTask = task;
    const Json reviewRequested{{"action", "review_requested"},
                               {"repository", repository},
                               {"pull_request", {{"number", 1}, {"head", {{"sha", newSha}}}, {"title", "Change"}}},
                               {"requested_reviewer", {{"login", "Alice"}}}};
    host->beforeCall = [&](const GitContext&) {
        check(post(client, "pull_request", "tool-review-request", reviewRequested, 202).at("status") == "received",
              "review request webhook not enqueued during the host call");
    };
    ConversationIntake(store).receive(localOwner, intake(), "request-review", "Request a review");
    runtime.runConversations();
    host->beforeCall = {};
    runtime.runConversations();
    check(model.notification("tool-review-request").at("action") == "review_requested", "Review request did not wake the owning task");
    check(model.notification("scan-1").at("taskId") == task, "Code scanning did not wake the owning task");
    const httplib::Headers auth{{"Authorization", "Basic fixture"}};
    const auto inbox = client.Get("/api/inbox?bot=owner", auth);
    const auto history = client.Get("/api/inbox/history?bot=owner", auth);
    check(inbox && inbox->status == 200 && Json::parse(inbox->body).at("items").empty() && history && history->status == 200 &&
              Json::parse(history->body).at("decisions").empty(),
          "PR creation, review request, submitted review or code scanning appeared in Needs you or History");

    auto changes = review;
    changes["review"] = {{"id", 202}, {"state", "changes_requested"}, {"body", "Please check the empty input."}};
    check(post(client, "pull_request_review", "external-changes", changes, 202).at("status") == "received",
          "Changes-requested review was not enqueued");
    auto reviewFeedback = review;
    reviewFeedback["review"] = {{"id", 204}, {"state", "commented"}, {"body", "Please check null values."}};
    check(post(client, "pull_request_review", "review-feedback", reviewFeedback, 202).at("status") == "received",
          "Commented review was not enqueued");
    const Json inlineComment{{"action", "created"},
                             {"repository", repository},
                             {"pull_request", review.at("pull_request")},
                             {"comment",
                              {{"id", 301},
                               {"body", "Could you validate this field?"},
                               {"path", "src/example.cpp"},
                               {"side", "RIGHT"},
                               {"line", nullptr},
                               {"original_line", 72}}}};
    check(post(client, "pull_request_review_comment", "inline-comment", inlineComment, 202).at("status") == "received",
          "Inline review comment was not enqueued");
    auto editedInline = inlineComment;
    editedInline["action"] = "edited";
    check(post(client, "pull_request_review_comment", "inline-edit", editedInline, 202).at("status") == "received",
          "Inline review edit was not enqueued");
    const Json checkRun{{"action", "completed"},
                        {"repository", repository},
                        {"check_run",
                         {{"id", 401},
                          {"head_sha", newSha},
                          {"name", "Test"},
                          {"conclusion", "failure"},
                          {"pull_requests", Json::array({{{"number", 1}, {"base", {{"repo", {{"id", 41}}}}}}})}}}};
    check(post(client, "check_run", "failed-check", checkRun, 202).at("status") == "received", "Failed check was not enqueued");
    auto passedCheck = checkRun;
    passedCheck["check_run"]["name"] = "Diagnose an earlier failure";
    passedCheck["check_run"]["conclusion"] = "success";
    check(post(client, "check_run", "passed-check", passedCheck, 202).at("status") == "received", "Successful check was not enqueued");
    runtime.runConversations();
    check(model.followup("external-changes") == "changes_requested" && model.followup("review-feedback") == "review_comment" &&
              model.followup("inline-comment") == "review_comment" && model.followup("inline-edit").empty() &&
              model.followup("failed-check") == "failed_check" && model.followup("passed-check").empty(),
          "Signed notifications did not distinguish actionable events from report-only updates");
    check(model.notification("passed-check").at("action") == "completed" && model.notification("passed-check").at("taskId") == task,
          "Finished checks did not wake the owning task");
    const auto notifications = TaskHistory(store).history(task).dump();
    check(notifications.find("src/example.cpp") != std::string::npos && notifications.find("\"line\":72") != std::string::npos &&
              notifications.find("\"originalLine\":true") != std::string::npos &&
              notifications.find("/pull/1#discussion_r301") != std::string::npos,
          "The signed inline webhook lost its review location");

    const Json ownComment{{"action", "created"},
                          {"repository", repository},
                          {"issue", {{"number", 1}, {"pull_request", Json::object()}}},
                          {"comment", {{"id", 101}, {"body", "Please check this."}}}};
    model.conversationAction = Decision{
        "git_comment",
        {{"host", hostName}, {"repo", repoName}, {"taskId", task}, {"target", "pr"}, {"number", 1}, {"body", "Please check this."}},
        "Post what the person asked for"};
    host->beforeCall = [&](const GitContext&) {
        check(post(client, "issue_comment", "own-comment", ownComment, 202).at("status") == "received",
              "Own comment delivery before the host returned was not accepted");
    };
    ConversationIntake(store).receive(localOwner, intake(), "write-comment", "Please post the comment");
    runtime.runConversations();
    host->beforeCall = {};
    check(host->comments == 1, "Flock did not author the expected comment");
    auto foreignComment = ownComment;
    foreignComment["comment"]["id"] = 102;
    foreignComment["comment"]["body"] = "The pull request was merged.";
    check(post(client, "issue_comment", "foreign-comment", foreignComment, 202).at("status") == "received",
          "Other author's comment was not enqueued");

    model.conversationAction = Decision{"git_submit_review",
                                        {{"host", hostName},
                                         {"repo", repoName},
                                         {"taskId", task},
                                         {"prNumber", 1},
                                         {"verdict", "request_changes"},
                                         {"body", "Use a test."}},
                                        "Submit the review the person asked for"};
    host->result["id"] = 203;
    ConversationIntake(store).receive(localOwner, intake(), "write-review", "Please submit a review");
    runtime.runConversations();
    auto ownReview = changes;
    ownReview["review"] = {{"id", 203}, {"state", "changes_requested"}, {"body", "Use a test."}};
    check(post(client, "pull_request_review", "own-review", ownReview, 202).at("status") == "received", "Own review was not accepted");
    runtime.runConversations();
    check(!model.notifications.contains("own-comment") && !model.notifications.contains("own-review") &&
              model.followup("foreign-comment").empty(),
          "A Flock-authored comment or review was delivered, or a distinct comment was hidden");
    check(model.commentCompletionOffered == false && Tasks(store).task(task).task.state == TaskState::external_waiting,
          "A comment-triggered completion closed the task without a verified lifecycle event");

    const auto deliverPull = [&](const std::string& id, std::string_view action, bool merged) {
        const Json pr{{"number", 1}, {"head", {{"sha", newSha}}}, {"title", "Change"}, {"merged", merged}};
        const Json delivery{{"action", action}, {"repository", repository}, {"pull_request", pr}};
        check(post(client, "pull_request", id, delivery, 202).at("status") == "received", "PR event was not enqueued: " + id);
        runtime.runConversations();
        const auto& event = model.notification(id);
        check(event.at("action") == (merged ? "merged" : action) && event.at("taskId") == task,
              "PR event did not wake the owning task: " + id);
    };
    deliverPull("closed-pr", "closed", false);
    check(Tasks(store).task(task).task.waitingNote == closedNote, "The Bot could not update the waiting note after close");
    deliverPull("reopened-pr", "reopened", false);
    check(Tasks(store).task(task).task.state == TaskState::external_waiting, "A Git event changed the task state by itself");
    deliverPull("merged-pr", "closed", true);
    check(Tasks(store).task(task).task.state == TaskState::completed, "The Bot could not complete after a merge");
    const auto completed = TaskHistory(store).history(task, 0, 200).dump();
    check(completed.find(completionReport) != completed.npos, "Completion did not save the Bot's report");
    auto postMergeCheck = passedCheck;
    postMergeCheck["check_run"]["id"] = 402;
    check(post(client, "check_run", "check-after-merge", postMergeCheck, 202).at("status") == "received", "Post-merge check not saved");
    runtime.runConversations();
    check(!model.notifications.contains("check-after-merge"), "A completed task still received PR events");
}

void mergeDuringPullRequestCreation() {
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Bot model;
    host->inspect = [&](const GitContext&, const GitOpenPr&) {
        check(GitEvents(store).receiveGitWebhook({hostName, "early-merge", repoName, "pull_request", "merged", {1}, sha, "Change", 0}),
              "Could not save a merge while the PR was being created");
    };
    Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = startTask(store, runtime, model);
    for (int i = 0; i < 12 && Tasks(store).task(task).task.state != TaskState::completed; ++i) runtime.tick();
    check(model.notification("early-merge").at("taskId") == task && Tasks(store).task(task).task.state == TaskState::completed,
          "The merge arrived before the Bot could wait and was lost");
}

void completeAfterPostMergeCI() {
    class SmokeBot final : public Model {
    public:
        Bot fallback;
        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            if (!event.contains("gitEvent")) return fallback.decide(input);
            const auto& delivery = event.at("gitEvent");
            const auto& turn = event.at("turn");
            if (delivery.at("action") == "merged" && turn.empty())
                return {{Decision{"wait_for_event", {{"note", "Merged. Waiting for smoke CI."}, {"checkAgainAfterMinutes", 15}}, {}}}};
            if (delivery.at("action") != "completed") return fallback.decide(input);
            if (turn.empty())
                return {{Decision{"git_read_ci_logs",
                                  {{"host", delivery.at("host")}, {"repo", delivery.at("repo")}, {"jobId", 81}, {"maxBytes", 32}},
                                  "Verify the deployed version"}}};
            const auto& log = turn.at(0).at("result").at("data");
            check(log.at("text") == "smoke: served v2" && event.at("watchedPullRequests").at(0).at("priorLifecycleAction") == "merged",
                  "The CI turn did not receive the smoke log and the prior merge");
            check(std::ranges::find(input.tools, "complete") != input.tools.end(), "The post-merge CI turn could not offer completion");
            return {{Decision{"complete", {{"report", "The PR merged and smoke verified version 2."}}, "Verified both criteria"}}};
        }
    } model;
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    host->log = {"smoke: served v2", false};
    Sessions sessions;
    Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = startTask(store, runtime, model.fallback);
    for (int i = 0; i < 8; ++i) runtime.tick();
    const auto server = startWeb(store, 0, callbacks());
    httplib::Client client("127.0.0.1", static_cast<int>(server->port()));
    const Json repo{{"full_name", repoName}, {"id", 41}};
    const Json merged{{"action", "closed"},
                      {"repository", repo},
                      {"pull_request", {{"number", 1}, {"head", {{"sha", sha}}}, {"title", "Change"}, {"merged", true}}}};
    check(post(client, "pull_request", "merge-before-ci", merged, 202).at("status") == "received", "The merge was not accepted");
    runtime.runConversations();
    check(Tasks(store).task(task).task.waitingNote == "Merged. Waiting for smoke CI.", "The Bot did not wait for CI after merge");
    const Json passed{
        {"action", "completed"},
        {"repository", repo},
        {"workflow_job", {{"id", 81}, {"head_sha", sha}, {"name", "Smoke"}, {"status", "completed"}, {"conclusion", "success"}}}};
    check(post(client, "workflow_job", "post-merge-ci", passed, 202).at("status") == "received",
          "The post-merge CI event was not accepted");
    runtime.runConversations();
    check(host->logCalls == 1 && Tasks(store).task(task).task.state == TaskState::completed,
          "The signed post-merge CI result did not complete the waiting task");
}

void scheduledPullRequestCheck(bool connected, bool webhookFirst, bool waitForCI = false) {
    class CheckBot final : public Model {
    public:
        bool waitForCI = false;
        Bot fallback;
        std::vector<Json> checks;
        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            const auto wake = event.find("sessionWake");
            if (wake == event.end() || wake->value("reason", "") != "check_due") return fallback.decide(input);
            const auto& turn = event.at("turn");
            if (turn.empty()) checks.push_back(event);
            if (!turn.empty() && (turn.back().at("tool") == "complete" || turn.back().at("tool") == "wait_for_event"))
                return {{Decision{"reply", {{"text", "Checked the pull request."}}, {}}}};
            const bool canRead = std::ranges::find(input.tools, "git_pr_status") != input.tools.end();
            if (canRead && turn.empty())
                return {{Decision{"git_pr_status", {{"host", hostName}, {"repo", repoName}, {"prNumber", 1}}, {}}}};
            const auto* pr = turn.empty() ? nullptr : &turn.front().at("result");
            if (waitForCI && pr && turn.size() == 1 && pr->value("ok", false))
                return {
                    {Decision{"git_commit_status", {{"host", hostName}, {"repo", repoName}, {"sha", pr->at("data").at("headSha")}}, {}}}};
            const auto& last = turn.empty() ? Json() : turn.back().at("result");
            const bool finished =
                !last.is_null() && last.value("ok", false) &&
                (waitForCI ? last.at("data").at("checks").at(0).at("conclusion") == "success" : last.at("data").value("merged", false));
            if (finished)
                return {{Decision{"complete",
                                  {{"report", waitForCI ? "Build passed." : completionReport}},
                                  waitForCI ? "The status read confirmed the Build check passed" : "The status read confirmed the merge"}}};
            return {{Decision{"wait_for_event", {{"note", "Waiting for GitHub status"}, {"checkAgainAfterMinutes", 10}}, {}}}};
        }
    } model;
    model.waitForCI = waitForCI;
    Directory directory;
    const auto path = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    const std::string currentHead(40, 'b');
    std::string conclusion = "pending";
    bool checksAvailable = true;
    std::vector<Decision> reads;
    host->responseForCall = [&](const GitRepository& repo, const Decision& request) -> Json {
        reads.push_back(request);
        check(repo.host == hostName && repo.repo == repoName, "The scheduled read targeted a different repository");
        if (request.tool == "git_pr_status") return host->result;
        check(request.tool == "git_commit_status" && request.args.at("sha") == currentHead,
              "Scheduled checks did not use the current PR head");
        if (!checksAvailable) throw GitRejected("Checks unavailable");
        return {{"checks", Json::array({{{"name", "Build"}, {"status", "completed"}, {"conclusion", conclusion}}})}};
    };
    Sessions sessions;
    Seconds now = 1'800'000'000;
    RuntimeOptions config{.clock = [&] { return now; }, .gitHost = host};
    const auto soul = [](std::string_view) { return std::string{}; };
    Id task;
    {
        Store store(path);
        Runtime runtime(store, model, sessions, soul, config);
        task = startTask(store, runtime, model.fallback);
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(Tasks(store).task(task).task.state == TaskState::external_waiting, "The task did not schedule its first check");
    }
    if (!connected) config.gitHost.reset();
    host->result = {{"state", waitForCI ? "open" : "closed"}, {"merged", !waitForCI}, {"headSha", currentHead}};
    Store store(path);
    Runtime runtime(store, model, sessions, soul, config);
    now += 299;
    runtime.tick();
    check(model.checks.empty() && reads.empty(), "A re-check ran before its deadline");
    if (webhookFirst) {
        check(GitEvents(store).receiveGitWebhook({hostName, "closed-early", repoName, "pull_request", "closed", {1}, sha, "Change", 0}),
              "The earlier signed event was not accepted");
        ++now;
        runtime.tick();
        check(Tasks(store).task(task).task.waitingNote == closedNote, "The earlier webhook did not wake the task");
        runtime.tick();
        check(model.checks.empty() && reads.empty(), "The superseded deadline still ran");
        now += 300;
        host->result["merged"] = false;
        checksAvailable = false;
    } else {
        conclusion = "success";
        ++now;
    }
    for (int i = 0; i < 5; ++i) runtime.tick();
    const auto state = Tasks(store).task(task).task;
    if (connected && !webhookFirst) {
        check(model.checks.size() == 1 && reads.size() == (waitForCI ? 2u : 1u) && state.state == TaskState::completed &&
                  reads.front().tool == "git_pr_status" && reads.front().args.at("prNumber") == 1 &&
                  (!waitForCI || (reads.back().tool == "git_commit_status" && reads.back().args.at("sha") == currentHead)),
              "The re-check did not read the PR and current-head CI status before completing");
        return;
    }
    check(model.checks.size() == 1 && reads.size() == (connected ? 1u : 0u) && state.state == TaskState::external_waiting &&
              state.waitingNote == "Waiting for GitHub status",
          "An unfinished or unreadable PR did not leave the task waiting for the next check");
    now += 599;
    runtime.tick();
    check(model.checks.size() == 1, "A new check ran before the new model-chosen deadline");
    ++now;
    for (int i = 0; i < 3; ++i) runtime.tick();
    check(model.checks.size() == 2, "Waiting again did not schedule the next one-shot check");
}

void multipleWatchedPullRequests() {
    constexpr auto relatedRepo = "org/related";
    const auto watch = [](const Json& event, std::string_view repo) {
        const auto& watches = event.at("watchedPullRequests");
        const auto matched = std::ranges::find_if(watches, [&](const Json& candidate) {
            return candidate.at("host") == hostName && candidate.at("repo") == repo && candidate.at("prNumber") == 1;
        });
        check(matched != watches.end(), "A watched pull request was missing from the notification");
        return *matched;
    };
    class LandingBot final : public Model {
    public:
        Bot fallback;
        std::map<std::string, Json> events;
        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            if (!event.contains("gitEvent")) return fallback.decide(input);
            if (event.at("turn").empty()) {
                const auto id = event.at("gitEvent").at("eventId").get<std::string>();
                events.emplace(id, event);
                if (id == "land-main" && std::ranges::any_of(event.value("watchedPullRequests", Json::array()), [](const Json& pr) {
                        return pr.at("repo") == "org/related" && pr.value("priorLifecycleAction", "") == "merged";
                    }))
                    return {{Decision{"complete", {{"report", "Both changes landed."}}, "Both pull requests merged"}}};
            }
            return {{Decision{"reply", {{"text", "Recorded the Git event."}}, {}}}};
        }
    } model;
    Directory directory;
    Store store(directory.path / "state.db");
    Sessions sessions;
    auto host = std::make_shared<FakeHost>();
    Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; }, options(host));
    const auto task = startTask(store, runtime, model.fallback);
    for (int i = 0; i < 8; ++i) runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::external_waiting, "The task did not enter external waiting");
    const auto doGit = [&](const std::string& id, Decision action) {
        model.fallback.conversationAction = std::move(action);
        ConversationIntake(store).receive(localOwner, intake(), id, "Work on another pull request");
        runtime.runConversations();
    };
    doGit("watch-related", {"git_request_review",
                            {{"host", hostName}, {"repo", relatedRepo}, {"taskId", task}, {"prNumber", 1}, {"reviewers", {"alice"}}},
                            "Request review for the related pull request"});
    const auto server = startWeb(store, 0, callbacks());
    httplib::Client client("127.0.0.1", static_cast<int>(server->port()));
    const auto queue = [&](std::string_view id, std::string_view repo, std::string_view action, bool merged = false) {
        const Json pr{{"number", 1}, {"head", {{"sha", sha}}}, {"title", "Change"}, {"merged", merged}};
        const Json delivery{{"action", action}, {"repository", {{"full_name", repo}, {"id", 41}}}, {"pull_request", pr}};
        check(post(client, "pull_request", id, delivery, 202).at("status") == "received", "PR event was not enqueued");
    };
    queue("close-main", repoName, "closed");
    runtime.runConversations();
    check(!watch(model.events.at("close-main"), relatedRepo).contains("priorLifecycleAction"),
          "A watch without earlier signed events appeared to have lifecycle evidence");
    queue("reopen-main", repoName, "reopened");
    const auto related = GitRepository{hostName, relatedRepo}.resource();
    const auto denyRelatedReads = [&](bool add) {
        const Json rule{{"kind", "never"}, {"action", "repository.read"}, {"target", related}, {"add", add}};
        const httplib::Headers headers{
            {"Authorization", "Basic fixture"}, {"Origin", "http://127.0.0.1:" + std::to_string(server->port())}, {"X-Flock-Action", "1"}};
        const auto response = client.Post("/api/settings/rule", headers, rule.dump(), "application/json");
        check(response && response->status == 200, "The Web policy route did not update repository access");
    };
    denyRelatedReads(true);
    queue("review-main", repoName, "ready_for_review");
    runtime.runConversations();
    check(model.events.at("review-main").at("watchedPullRequests").size() == 1,
          "A notification exposed a watch after repository read access was revoked");
    denyRelatedReads(false);
    queue("land-related", relatedRepo, "closed", true);
    queue("land-main", repoName, "closed", true);
    runtime.runConversations();
    check(watch(model.events.at("land-related"), repoName).at("priorLifecycleAction") == "reopened",
          "The first merge turn missed the reopen or saw the later merge");
    check(watch(model.events.at("land-main"), relatedRepo).at("priorLifecycleAction") == "merged" &&
              Tasks(store).task(task).task.state == TaskState::completed,
          "The second merge turn could not verify both merges to complete the task");
}

void mergeWhileScheduledCheckWaitsForProject() {
    Bot model;
    Directory directory;
    Store store(directory.path / "state.db");
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Seconds now = 1'800'000'000;
    Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; },
                    {.clock = [&] { return now; }, .gitHost = host});
    const auto task = startTask(store, runtime, model);
    for (int i = 0; i < 8; ++i) runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::external_waiting, "The PR task did not release its project");
    sessions.holdRuns = true;
    auto other = intake();
    other.request = "Another request";
    const auto blocker = TaskIntake(store).submit(other);
    for (int i = 0; i < 4; ++i) runtime.tick();
    check(Tasks(store).task(blocker).task.state == TaskState::running, "Another request did not hold the project");
    now += 5 * 60;
    runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::queued, "The check did not queue behind the active project request");
    check(GitEvents(store).receiveGitWebhook(
              {hostName, "merge-with-busy-project", repoName, "pull_request", "merged", {1}, sha, "Change", 0}),
          "The merge was not accepted");
    runtime.tick();
    check(Tasks(store).task(task).task.state == TaskState::completed && Tasks(store).task(blocker).task.state == TaskState::running &&
              host->hostCalls == 0,
          "A queued re-check blocked merge completion or disturbed the other request");
}
}

int main() {
    try {
        run();
        mergeDuringPullRequestCreation();
        completeAfterPostMergeCI();
        scheduledPullRequestCheck(true, false);
        scheduledPullRequestCheck(true, false, true);
        scheduledPullRequestCheck(false, false);
        scheduledPullRequestCheck(true, true);
        multipleWatchedPullRequests();
        mergeWhileScheduledCheckWaitsForProject();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
