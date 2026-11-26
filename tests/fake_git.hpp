#pragma once
#include "flock/task_history.hpp"
#include "flock/git_host.hpp"
#include "flock/board_inbox.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/runtime_options.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace git_fixture {
using namespace flockv0;
inline constexpr auto hostName = "github.com";
inline constexpr auto repoName = "org/flock";
inline constexpr auto repositoryTarget = "github.com/org/flock";
inline const std::string sha(40, 'a');

inline void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        char name[] = "/tmp/flock-git-XXXXXX";
        const auto directory = mkdtemp(name);
        check(directory != nullptr, "could not create fixture directory");
        path = directory;
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

class FakeHost final : public GitHost {
public:
    enum class Create { success, lostResponse, lostRequest, rejected };
    enum class State { open, closed, merged };
    struct Entry {
        GitPullRequest pr;
        std::string title;
        State state;
    };
    Create create = Create::success;
    State state = State::open;
    bool upstreamPermitted = true;
    bool rejectComment = false;
    bool storedDiscoveryPermitted = true;
    GitLog log{"linker: missing symbol in build\nextra output the adapter failed to trim", false};
    Json result{{"headSha", sha}, {"status", "ok"}};
    int createCalls = 0, lookupCalls = 0, logCalls = 0, hostCalls = 0, comments = 0, edits = 0;
    std::vector<Entry> prs;
    std::map<Id, GitLog> jobLogs;
    std::vector<std::string> prStateUpdates;
    std::function<void(std::string_view)> onPrStateUpdate;
    std::function<void(const GitContext&)> beforeCall;
    std::function<void(const GitContext&, const GitOpenPr&)> inspect;
    std::string_view name() const override { return hostName; }
    std::pair<std::vector<GitRepository>, bool> repositories(const GitContext&, std::string_view query, Id page) override {
        if (page == 11) return {{}, true};
        if (page != 1 && page != 12) return {};
        if (std::string_view(repoName).find(query) == std::string_view::npos) return {};
        return {{{hostName, repoName}}, false};
    }

    bool permitsStoredRead(const GitContext& context, const GitRepository& repo, const Decision& read) override {
        return permits(context, repo, "read") && (read.tool != "git_find_repos" || storedDiscoveryPermitted);
    }

    bool permits(const GitContext&, const GitRepository&, std::string_view) const override { return upstreamPermitted; }
    GitPullRequest createPullRequest(const GitContext& context, const GitOpenPr& request) override {
        ++createCalls;
        if (inspect) inspect(context, request);
        if (create == Create::rejected) throw GitRejected("Branch does not exist");
        if (create == Create::lostRequest) throw Conflict("No response");
        const auto number = static_cast<Id>(prs.size() + 1);
        GitPullRequest pr{request.target,      number, "https://" + request.target.base.resource() + "/pull/" + std::to_string(number),
                          request.operationId, sha,    state == State::open};
        prs.push_back({pr, request.title, state});
        if (create == Create::lostResponse) throw Conflict("Lost response");
        return pr;
    }
    std::vector<GitPullRequest> findPullRequests(const GitContext&, const GitPrTarget&) override {
        ++lookupCalls;
        std::vector<GitPullRequest> result;
        for (const auto& entry : prs) result.push_back(entry.pr);
        return result;
    }
    GitLog readCiLogs(const GitContext&, const GitRepository&, Id job, std::size_t maxBytes) override {
        ++logCalls;
        check(maxBytes == 32, "log byte limit was not passed to the adapter");
        if (!jobLogs.empty()) return jobLogs.at(job);
        check(job == 81, "wrong CI job");
        return log;
    }
    Json call(const GitContext& context, const GitRepository&, const Decision& decision) override {
        if (beforeCall) beforeCall(context);
        ++hostCalls;
        const auto& args = decision.args;
        if (decision.tool == "git_update_pr" && args.contains("state")) {
            auto& pr = prs.at(args.at("prNumber").get<Id>() - 1);
            const auto state = args.at("state").get<std::string>();
            pr.state = state == "closed" ? State::closed : State::open;
            prStateUpdates.push_back(state);
            if (onPrStateUpdate) onPrStateUpdate(state);
            auto updated = result;
            updated["state"] = state;
            return updated;
        }
        if (decision.tool == "git_comment") {
            if (rejectComment) throw GitRejected("Comment rejected");
            if (args.contains("commentId")) {
                check(args.at("commentId") == 101 && comments == 1 && args.at("target") == commentTarget &&
                          args.at("number") == commentNumber,
                      "wrong comment edited");
                ++edits;
            } else {
                ++comments;
                commentTarget = args.at("target");
                commentNumber = args.at("number");
            }
            return {{"id", 101}, {"body", args.at("body")}};
        }
        return result;
    }
private:
    Json commentTarget, commentNumber;
};

inline Intake intake(std::string bot = "owner", std::string project = "project") {
    return {std::move(bot), "Ship the tested change", "sandbox", std::move(project), {{"modelProvider", "chatgpt"}, {"model", "fixture"}}};
}

inline RuntimeOptions options(const std::shared_ptr<FakeHost>& host) {
    return {.gitHost = host};
}

inline Json openArgs(std::string branch = "change", std::string repo = repoName) {
    return {{"host", hostName}, {"repo", std::move(repo)}, {"baseRef", "main"}, {"headRef", std::move(branch)}, {"title", "Change"}};
}

inline InboxTarget target(const Json& row) {
    return {row.at("botId"),           parseInboxKind(row.at("itemKind").get<std::string>()),
            row.at("itemId"),          row.at("waitId"),
            row.at("taskId"),          row.at("targetRevision"),
            row.at("targetGeneration")};
}

inline unsigned historyCount(Store& store, Id task, std::string_view kind) {
    const auto records = TaskHistory(store).history(task, 0, 200);
    return std::ranges::count_if(records, [&](const Json& row) { return row.at("kind") == kind; });
}
}
