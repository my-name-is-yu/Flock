#include "fake_git.hpp"
#include "flock/tasks.hpp"
#include "flock/board_inbox.hpp"
#include "flock/git_events.hpp"
#include "flock/conversations.hpp"
#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <sqlite3.h>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace git_fixture;

namespace {
class HandoffModel final : public Model {
public:
    void queueTool(Store& store, std::string_view bot, std::string_view message, Decision tool, std::function<void()> beforeTool = {}) {
        {
            std::lock_guard lock(mutex);
            tools.emplace(message, Script{std::move(tool), std::move(beforeTool), nullptr});
        }
        Conversations(store).receive(localOwner, intake(std::string(bot)), message, message);
    }

    Json result(std::string_view message) {
        std::lock_guard lock(mutex);
        return tools.find(message)->second.result;
    }

    Json runTool(Store& store, Runtime& runtime, std::string_view bot, std::string_view message, Decision tool) {
        queueTool(store, bot, message, std::move(tool));
        runtime.runConversations();
        auto output = result(message);
        check(output.is_object(), "Expected the conversation to record a Git tool result");
        return output;
    }

    ModelTurn decide(const ModelInput& input) override {
        auto action = choose(input);
        action.reason = "Advance the scripted Git scenario";
        return {{std::move(action)}};
    }
private:
    struct Script {
        Decision tool;
        std::function<void()> beforeTool;
        Json result;
    };

    Decision choose(const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("kind") == "intake") return {"delegate", {{"instruction", "Ship the change"}, {"doneWhen", "Report the result"}}, {}};
        if (event.at("kind") == "result") {
            if (input.task.at("botId") == "owner" && event.value("gitResults", Json::array()).empty())
                return {"git_open_pr", openArgs(), {}};
            return {"complete", {{"report", "Change shipped"}}, {}};
        }
        if (!event.contains("gitEvent")) {
            const auto& steps = event.at("turn");
            std::unique_lock lock(mutex);
            auto& script = tools.at(event.at("text").get_ref<const std::string&>());
            if (steps.empty()) {
                const auto beforeTool = script.beforeTool;
                const auto tool = script.tool;
                lock.unlock();
                if (beforeTool) beforeTool();
                return tool;
            }
            script.result = steps.at(0).at("result");
        }
        return {"reply", {{"text", "Git activity recorded"}}, {}};
    }

    std::mutex mutex;
    std::map<std::string, Script, std::less<>> tools;
};

bool webhookWasUndelivered(const std::filesystem::path& path, std::string_view event) {
    sqlite3* db = nullptr;
    const auto opened = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(db, sqlite3_close);
    check(opened == SQLITE_OK, "Could not open the handoff fixture database");
    sqlite3_stmt* raw = nullptr;
    const auto prepared = sqlite3_prepare_v2(db, R"SQL(
SELECT 1 FROM git_webhook_events e WHERE e.event_id=?
AND NOT EXISTS (SELECT 1 FROM git_webhook_deliveries d WHERE d.event=e.id)
)SQL",
                                             -1, &raw, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    check(prepared == SQLITE_OK && sqlite3_bind_text(raw, 1, event.data(), static_cast<int>(event.size()), SQLITE_TRANSIENT) == SQLITE_OK,
          "Could not read the fixture webhook");
    return sqlite3_step(raw) == SQLITE_ROW;
}

Json runUntilToolResult(HandoffModel& model, Runtime& runtime, std::string_view message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (auto output = model.result(message); output.is_object()) return output;
        runtime.tickConcurrent();
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("No Git tool result for " + std::string(message));
}

void deletedBotWatchHandoff(std::string_view tool, const std::string& delayedSyncHead) {
    Directory directory;
    const auto path = directory.path / "state.db";
    auto host = std::make_shared<FakeHost>();
    HandoffModel model;
    Sessions sessions;
    const std::string newHead(40, 'b');
    const bool observedDelayedHead = delayedSyncHead == newHead;
    const bool requestsReview = tool == "git_request_review";
    Id retired, sibling, replacement;
    const auto mutation = [&](Id task) {
        Json args{{"host", hostName}, {"repo", repoName}, {"taskId", task}, {"prNumber", 1}};
        if (tool == "git_update_pr")
            args["title"] = "Replacement update";
        else
            args["reviewers"] = {"alice"};
        return Decision{std::string(tool), std::move(args), {}};
    };
    const auto soul = [](std::string_view) { return std::string{}; };
    {
        Store store(path);
        Runtime runtime(store, model, sessions, soul, options(host));
        retired = Tasks(store).submit(intake());
        for (int i = 0; i < 8 && Tasks(store).task(retired).task.state != TaskState::completed; ++i) runtime.tick();
        check(Tasks(store).task(retired).task.state == TaskState::completed && host->createCalls == 1,
              "The original PR task did not finish");
        const auto pending = Inbox(store).inbox(localOwner, "owner");
        check(pending.size() == 1 && pending.at(0).at("prTitle") == "Pull request awaiting review",
              "Expected a pending review on the completed task");
        bool blockedByReview = false;
        try {
            store.deleteBot(localOwner, "owner");
        } catch (const BotBusy&) {
            blockedByReview = true;
        }
        check(blockedByReview && !store.botDeleted("owner"), "A pending Git review did not block Bot deletion");
        Inbox(store).decideInbox(localOwner, target(pending.at(0)), "approve");

        sibling = Tasks(store).submit(intake("sibling", "sibling-project"));
        auto siblingArgs = openArgs("sibling-change");
        siblingArgs["taskId"] = sibling;
        check(model.runTool(store, runtime, "sibling", "watch-sibling", {"git_open_pr", siblingArgs, {}}).at("prNumber") == 2,
              "The sibling did not start watching another PR at the same commit");
        replacement = Tasks(store).submit(intake("replacement", "replacement-project"));
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(Tasks(store).task(sibling).task.state == TaskState::completed &&
                  Tasks(store).task(replacement).task.state == TaskState::completed,
              "The competing PR tasks did not finish setup");
        const auto calls = host->hostCalls;
        const auto blocked = model.runTool(store, runtime, "replacement", "claim-active-watch", mutation(replacement));
        check(blocked.at("ok") == false && host->hostCalls == calls, "An active Bot's PR became available to another task");

        check(GitEvents(store).receiveGitWebhook({hostName, "shared-commit", repoName, "ci", "completed", {}, sha, "Shared SHA", 0}),
              "Could not save the shared-commit event");
        runtime.runConversations();
        store.deleteBot(localOwner, "owner");
        check(GitEvents(store).receiveGitWebhook({hostName, "retired-watch", repoName, "ci", "completed", {1}, sha, "Retired PR", 0}),
              "Could not save an explicit event after deletion");
        runtime.runConversations();
        check(historyCount(store, retired, "git_webhook") == 0 && historyCount(store, sibling, "git_webhook") == 0,
              "Deletion delivered an event to the retired task or changed a shared commit's attribution");

        host->beforeCall = [](const GitContext&) { throw GitRejected("Fixture host rejected the first attempt"); };
        const auto rejected = model.runTool(store, runtime, "sibling", "rejected-handoff", mutation(sibling));
        check(rejected.at("status") == "failed", "A rejected host call did not report a failed handoff");

        host->result["headSha"] = newHead;
        const auto queueEventBeforeWatchTransfer = [&] {
            check(GitEvents(store).receiveGitWebhook({hostName, "before-handoff", repoName, "ci", "completed", {1}, sha, "Old watch", 0}),
                  "Could not queue an explicit pre-handoff event");
        };
        auto webhookQueued = std::make_shared<std::promise<bool>>();
        auto queued = webhookQueued->get_future();
        std::promise<void> releaseHost;
        host->beforeCall = [&, webhookQueued, resume = releaseHost.get_future().share()](const GitContext& context) {
            if (context.botId != "replacement") return;
            const auto saveReviewRequests = [&] {
                if (!requestsReview) return true;
                return GitEvents(store).receiveGitWebhook({hostName,
                                                           "unrelated-review",
                                                           repoName,
                                                           "pull_request",
                                                           "review_requested",
                                                           {1},
                                                           newHead,
                                                           "Requested Bob separately",
                                                           0,
                                                           {},
                                                           "Bob"}) &&
                       GitEvents(store).receiveGitWebhook({hostName,
                                                           "matching-review",
                                                           repoName,
                                                           "pull_request",
                                                           "review_requested",
                                                           {1},
                                                           newHead,
                                                           "Requested Alice",
                                                           0,
                                                           {},
                                                           "Alice"});
            };
            webhookQueued->set_value(
                GitEvents(store).receiveGitWebhook({hostName,
                                                    "shared-head-during-handoff",
                                                    repoName,
                                                    "ci",
                                                    "completed",
                                                    {},
                                                    sha,
                                                    "CI activity for both PRs before synchronization",
                                                    0}) &&
                GitEvents(store).receiveGitWebhook({hostName,
                                                    "ci-before-delayed-sync",
                                                    repoName,
                                                    "ci",
                                                    "completed",
                                                    {},
                                                    delayedSyncHead,
                                                    "CI before its synchronization webhook",
                                                    0}) &&
                GitEvents(store).receiveGitWebhook(
                    {hostName, "during-handoff", repoName, "ci", "completed", {1}, newHead, "New watch before host response", 0}) &&
                saveReviewRequests());
            resume.wait();
        };
        model.queueTool(store, "replacement", "claim-retired-watch", mutation(replacement), queueEventBeforeWatchTransfer);
        runtime.tickConcurrent();
        check(queued.wait_for(std::chrono::seconds(5)) == std::future_status::ready && queued.get(),
              "Could not queue activity during the host call");

        model.queueTool(store, "sibling", "competing-handoff", mutation(sibling));
        const auto competing = runUntilToolResult(model, runtime, "competing-handoff");
        check(competing.at("ok") == false && host->hostCalls == calls, "The competing task reached the host during another Bot's handoff");
        check(webhookWasUndelivered(path, "before-handoff"), "The fixture drained the explicit event before testing the handoff cutoff");

        releaseHost.set_value();
        const auto claimed = runUntilToolResult(model, runtime, "claim-retired-watch");
        check(claimed.value("ok", false) && claimed.value("watchAttached", false) && host->hostCalls == calls + 1,
              "The replacement task could not claim the retired watch via " + std::string(tool) + ": " + claimed.dump());
        host->beforeCall = {};
    }
    {
        Store store(path);
        Runtime runtime(store, model, sessions, soul, options(host));
        runtime.runConversations();
        check(webhookWasUndelivered(path, "ci-before-delayed-sync"), "A future head observation attributed unsynchronized CI activity");
        check(GitEvents(store).receiveGitWebhook({hostName,
                                                  "synchronize-after-handoff",
                                                  repoName,
                                                  "pull_request",
                                                  "synchronize",
                                                  {1},
                                                  delayedSyncHead,
                                                  "Delayed synchronization for the CI head",
                                                  0}) &&
                  GitEvents(store).receiveGitWebhook(
                      {hostName, "after-handoff-explicit", repoName, "ci", "completed", {1}, delayedSyncHead, "New watch", 0}) &&
                  GitEvents(store).receiveGitWebhook(
                      {hostName, "after-handoff-commit", repoName, "ci", "completed", {}, delayedSyncHead, "New head", 0}),
              "Could not enqueue activity after handoff");
        runtime.runConversations();
        check(historyCount(store, retired, "git_webhook") == 0 && historyCount(store, sibling, "git_webhook") == 0,
              "Handoff delivered an old event to the retired task or changed a shared commit's attribution");
        check(webhookWasUndelivered(path, "shared-head-during-handoff"), "Handoff backdated a head change past ambiguous CI activity");
        const auto history = Tasks(store).history(replacement, 0, 200).dump();
        check((history.find("\"ci-before-delayed-sync\"") != std::string::npos) == observedDelayedHead,
              "Delayed synchronization failed to recover matching CI or crossed a conflicting head observation");
        const auto expectedNotifications = (requestsReview ? 6 : 4) + (observedDelayedHead ? 1 : 0);
        check(historyCount(store, replacement, "git_webhook") == expectedNotifications &&
                  history.find("\"during-handoff\"") != std::string::npos &&
                  history.find("synchronize-after-handoff") != std::string::npos &&
                  history.find("after-handoff-explicit") != std::string::npos &&
                  history.find("after-handoff-commit") != std::string::npos && history.find("before-handoff") == std::string::npos,
              "The replacement watch lost new activity or replayed an old event after restart");
        if (requestsReview) {
            const auto inbox = Inbox(store).inbox(localOwner, "replacement");
            const auto matching =
                std::ranges::find_if(inbox, [](const Json& item) { return item.at("prompt") == "Review requested on the Git host."; });
            check(history.find("matching-review") != std::string::npos && history.find("unrelated-review") != std::string::npos &&
                      inbox.size() == 2 && matching != inbox.end(),
                  "The review request lost a conversation event or created a duplicate inbox item");
            Inbox(store).decideInbox(localOwner, target(*matching), "approve");
            const auto remaining = Inbox(store).inbox(localOwner, "replacement");
            check(remaining.size() == 1 && remaining.front().at("prompt") == "Requested Bob separately",
                  "Review-request correlation removed an unrelated inbox item");
        }

        const auto calls = host->hostCalls;
        const auto blocked = model.runTool(store, runtime, "sibling", "reclaim-active-watch", mutation(sibling));
        check(blocked.at("ok") == false && host->hostCalls == calls, "A competing task could claim the replacement's watch");
    }
}
}

int main() {
    try {
        deletedBotWatchHandoff("git_update_pr", std::string(40, 'b'));
        deletedBotWatchHandoff("git_request_review", std::string(40, 'b'));
        deletedBotWatchHandoff("git_update_pr", std::string(40, 'c'));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
