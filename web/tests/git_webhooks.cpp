#include "flock/web.hpp"
#include "../../tests/fake_git.hpp"

#include <httplib.h>
#include <openssl/hmac.h>

#include <iostream>
#include <map>
#include <optional>

using namespace git_fixture;

namespace {
constexpr auto webhookPath = "/api/git/webhooks/github.com";
constexpr auto key = "fixture-signature-key";

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
    Id reviewTask = 0;
    std::map<std::string, std::string> followups;
    std::optional<Decision> conversationAction;
    const std::string& followup(const std::string& eventId) const {
        const auto event = followups.find(eventId);
        check(event != followups.end(), "Missing Git notification " + eventId);
        return event->second;
    }
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "intake")
            return {"delegate", {{"instruction", "Push the change"}, {"doneWhen", "Open a pull request"}}, "Implement the change"};
        if (event.at("kind") == "result") {
            if (!event.contains("gitResults")) return {"git_open_pr", openArgs(), "Publish the Session change"};
            return {"complete", {{"report", "Opened pull request"}}, "Report the published PR"};
        }
        if (!event.contains("gitEvent")) {
            if (event.at("turn").empty() && conversationAction) return *conversationAction;
            if (event.at("turn").empty())
                return {"git_request_review",
                        {{"host", hostName}, {"repo", repoName}, {"taskId", reviewTask}, {"prNumber", 1}, {"reviewers", {"alice", "bob"}}},
                        "Request a review for the watched pull request"};
            return {"reply", {{"text", "Review requested"}}, "Report the requested review"};
        }
        const auto& delivery = event.at("gitEvent");
        if (event.at("turn").empty()) {
            ++received;
            followups.emplace(delivery.at("eventId").get<std::string>(), delivery.value("followup", ""));
            if (delivery.at("jobId") == 81)
                return {"git_read_ci_logs",
                        {{"host", delivery.at("host")}, {"repo", delivery.at("repo")}, {"jobId", 81}, {"maxBytes", 32}},
                        "Inspect the failed job"};
            return {"reply", {{"text", "Review received"}}, "Relay a notification"};
        }
        const auto& log = event.at("turn").at(0).at("result").at("data");
        return {"reply", {{"text", "CI failed. " + log.at("text").get<std::string>()}}, "Report the exact CI failure"};
    }
};

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
        task = store.submit(intake());
        Runtime runtime(store, model, sessions, [](std::string_view) { return std::string{}; }, options(host));
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(store.task(task).at("state") == "completed" && host->createCalls == 1, "initial watch was not recorded");
    }
    Store store(database);
    const auto server = startWeb(store, 0, callbacks());
    httplib::Client client("127.0.0.1", static_cast<int>(server->port()));
    const auto privateEndpoint = client.Get("/api/inbox");
    check(privateEndpoint && privateEndpoint->status == 401, "ordinary Web access became public");
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
              store.history(task).dump().find("CI failed. linker: missing symbol") != std::string::npos,
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
    const auto inbox = client.Get("/api/inbox?bot=owner", httplib::Headers{{"Authorization", "Basic fixture"}});
    check(inbox && inbox->status == 200 && inbox->body.find("Pull request review submitted") != std::string::npos &&
              inbox->body.find("Code scanning result") != std::string::npos,
          "review or scan missing in the Web inbox");
    check(model.followup("review-1").empty(), "An approval was treated as requested changes");

    const auto inboxBeforeRequest = store.inbox(localOwner, "owner").size();
    model.reviewTask = task;
    const Json reviewRequested{{"action", "review_requested"},
                               {"repository", repository},
                               {"pull_request", {{"number", 1}, {"head", {{"sha", newSha}}}, {"title", "Change"}}},
                               {"requested_reviewer", {{"login", "Alice"}}}};
    host->beforeCall = [&](const GitContext&) {
        check(post(client, "pull_request", "tool-review-request", reviewRequested, 202).at("status") == "received",
              "review request webhook not enqueued during the host call");
    };
    store.receive(localOwner, intake(), "request-review", "Request a review");
    runtime.runConversations();
    host->beforeCall = {};
    runtime.runConversations();
    check(store.inbox(localOwner, "owner").size() == inboxBeforeRequest + 1 &&
              store.history(task).dump().find("tool-review-request") != std::string::npos,
          "A signed review webhook duplicated the tool's inbox item or was lost from the conversation");

    auto lateReview = reviewRequested;
    lateReview["requested_reviewer"]["login"] = "Bob";
    check(post(client, "pull_request", "delayed-review-request", lateReview, 202).at("status") == "received",
          "delayed review request webhook was not enqueued");
    runtime.runConversations();
    check(store.inbox(localOwner, "owner").size() == inboxBeforeRequest + 1 &&
              store.history(task).dump().find("delayed-review-request") != std::string::npos,
          "A late webhook for the same request created another card or lost the notification");

    auto externalReview = lateReview;
    externalReview["pull_request"]["title"] = "Review requested again";
    check(post(client, "pull_request", "external-review-request", externalReview, 202).at("status") == "received",
          "external review request webhook was not enqueued");
    runtime.runConversations();
    check(store.inbox(localOwner, "owner").size() == inboxBeforeRequest + 2, "A distinct review request was suppressed");

    const auto inboxBeforeMissingWebhook = store.inbox(localOwner, "owner").size();
    store.receive(localOwner, intake(), "review-without-webhook", "Request another review");
    runtime.runConversations();
    const auto afterRequest = store.inbox(localOwner, "owner");
    check(afterRequest.size() == inboxBeforeMissingWebhook + 1 && afterRequest.front().at("prompt") == "Review requested on the Git host.",
          "The tool should create a card even if no webhook arrives");
    store.decideInbox(localOwner, target(afterRequest.front()), "approve");

    auto laterReview = reviewRequested;
    laterReview["pull_request"]["title"] = "Another review requested";
    check(post(client, "pull_request", "review-after-prior-decision", laterReview, 202).at("status") == "received",
          "later review request webhook was not enqueued");
    runtime.runConversations();
    check(store.inbox(localOwner, "owner").size() == inboxBeforeMissingWebhook + 1 &&
              store.history(task).dump().find("review-after-prior-decision") != std::string::npos,
          "A handled review with a missing webhook hid a later request");

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
    const auto notifications = store.history(task).dump();
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
    store.receive(localOwner, intake(), "write-comment", "Please post the comment");
    runtime.runConversations();
    host->beforeCall = {};
    check(host->comments == 1, "Flock did not author the expected comment");
    auto foreignComment = ownComment;
    foreignComment["comment"]["id"] = 102;
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
    store.receive(localOwner, intake(), "write-review", "Please submit a review");
    runtime.runConversations();
    auto ownReview = changes;
    ownReview["review"] = {{"id", 203}, {"state", "changes_requested"}, {"body", "Use a test."}};
    check(post(client, "pull_request_review", "own-review", ownReview, 202).at("status") == "received", "Own review was not accepted");
    runtime.runConversations();
    check(!model.followups.contains("own-comment") && !model.followups.contains("own-review") && model.followup("foreign-comment").empty(),
          "A Flock-authored comment or review was delivered, or a distinct comment was hidden");
}
}

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
