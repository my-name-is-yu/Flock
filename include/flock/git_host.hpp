#pragma once
#include "flock/json.hpp"
#include "flock/run_ref.hpp"
#include "flock/types.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
struct Decision;
struct GitRepository {
    std::string host, repo;
    std::string resource() const { return host + "/" + repo; }
    static GitRepository parse(std::string_view host, std::string_view repo);
    bool operator==(const GitRepository&) const = default;
};

struct GitPrTarget {
    GitRepository base, head;
    std::string baseRef, headRef;
    bool operator==(const GitPrTarget&) const = default;
};

struct GitPullRequest {
    GitPrTarget target;
    Id number;
    std::string url, operationId, headSha;
};

struct GitOpenPr {
    GitPrTarget target;
    std::string operationId, title, body;
    bool draft;
};

struct GitContext {
    Id taskId;
    std::string botId, environmentId, projectId;
    RunRef run;
};

struct GitRejected : Conflict {
    using Conflict::Conflict;
};

struct GitLog {
    std::string text;
    bool truncated = false;
};

class GitHost {
public:
    virtual ~GitHost() = default;
    virtual std::string_view name() const = 0;
    virtual bool permits(const GitContext&, const GitRepository&, std::string_view operation) const = 0;
    virtual bool permitsStoredRead(const GitContext& context, const GitRepository& repo, const Decision&) {
        return permits(context, repo, "read");
    }
    virtual std::pair<std::vector<GitRepository>, bool> repositories(const GitContext&, std::string_view, Id) { return {}; }
    virtual GitPullRequest createPullRequest(const GitContext&, const GitOpenPr&) = 0;
    virtual std::optional<GitPullRequest> readPullRequest(const GitContext&, const GitRepository&, Id) { return std::nullopt; }
    virtual std::vector<GitPullRequest> findPullRequests(const GitContext&, const GitPrTarget&) = 0;
    virtual GitLog readCiLogs(const GitContext&, const GitRepository&, Id jobId, std::size_t maxBytes) = 0;
    virtual Json call(const GitContext&, const GitRepository&, const Decision&) = 0;
};

enum class GitFollowup { none, failedCheck, changesRequested, reviewComment };

struct GitReviewLocation {
    Id commentId = 0;
    std::string path, side;
    Id line = 0;
    bool originalLine = false;
};

struct GitWebhook {
    std::string host, eventId, repo, kind, action;
    std::vector<Id> prNumbers;
    std::string headSha, summary;
    Id jobId = 0;
    std::string payloadDigest = {};
    GitFollowup followup = GitFollowup::none;
    Id feedbackId = 0;
    std::optional<GitReviewLocation> reviewLocation = {};
};

const std::vector<std::string>& gitTools();
bool isGitTool(std::string_view tool);
std::string_view gitOperation(const Decision&);
void appendReadOnlyGitTools(std::vector<std::string>& tools);
void appendGitNotificationTools(const Json& event, std::vector<std::string>& tools);
bool gitNotificationToolAllowed(const Decision&, const Json& event);
Json gitToolSchema(std::string_view tool, bool readOnly = false);

}
