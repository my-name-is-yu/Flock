#include "git_github.hpp"
#include "flock/decision.hpp"
#include "flock/git_host.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flockv0 {
namespace {
constexpr std::string_view api = "https://api.github.com";
constexpr std::string_view markerPrefix = "<!-- flock-pr-operation: ";
constexpr std::string_view markerSuffix = " -->";
constexpr std::size_t responseLimit = 2 * 1024 * 1024;

std::string str(const Json& json, const char* key) {
    const auto it = json.find(key);
    return it != json.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string marker(std::string_view operationId) {
    return std::string(markerPrefix) + std::string(operationId) + std::string(markerSuffix);
}

std::string correlation(std::string_view body) {
    const auto start = body.rfind(markerPrefix);
    if (start == body.npos) return {};
    const auto tail = body.substr(start + markerPrefix.size());
    if (!tail.ends_with(markerSuffix)) return {};
    const auto id = tail.substr(0, tail.size() - markerSuffix.size());
    if (id.size() != 38 || !id.starts_with("flock-") || id.substr(6).find_first_not_of("0123456789abcdef") != id.npos) return {};
    return std::string(id);
}

std::string encode(std::string_view input) {
    std::string result;
    constexpr std::string_view unreserved = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._~";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : input) {
        if (unreserved.find(c) != unreserved.npos)
            result += static_cast<char>(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

struct Buffer {
    std::string text;
    std::size_t maxBytes;
    bool truncated = false;
};

struct HttpResponse {
    Buffer body;
    long status;
    std::string redirect;
};

using CurlUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using CurlText = std::unique_ptr<char, decltype(&curl_free)>;

CurlText urlPart(const CurlUrl& url, CURLUPart part) {
    char* raw = nullptr;
    curl_url_get(url.get(), part, &raw, 0);
    return CurlText(raw, curl_free);
}

bool validLogUrl(const std::string& location) {
    const CurlUrl url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, location.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK) return false;
    const auto scheme = urlPart(url, CURLUPART_SCHEME);
    const auto host = urlPart(url, CURLUPART_HOST);
    if (!scheme || std::string_view(scheme.get()) != "https" || !host || urlPart(url, CURLUPART_PORT)) return false;
    const std::string_view name(host.get());
    constexpr std::string_view blob = ".blob.core.windows.net";
    constexpr std::string_view storage = "productionresultssa";
    if (name.ends_with(".actions.githubusercontent.com")) return true;
    if (!name.starts_with(storage) || !name.ends_with(blob)) return false;
    const auto number = name.substr(storage.size(), name.size() - storage.size() - blob.size());
    return !number.empty() && number.find_first_not_of("0123456789") == number.npos;
}

void requireSuccess(long status) {
    if (status == 400 || status == 401 || status == 403 || status == 404 || status == 422 || status == 429)
        throw GitRejected("GitHub rejected the request (HTTP " + std::to_string(status) + ")");
    if (status < 200 || status >= 300) throw Conflict("GitHub returned HTTP " + std::to_string(status));
}

std::size_t receive(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& buffer = *static_cast<Buffer*>(context);
    const auto length = size * count;
    const auto bytes = std::min(length, buffer.maxBytes - buffer.text.size());
    try {
        buffer.text.append(data, bytes);
    } catch (...) {
        return 0;
    }
    buffer.truncated = bytes != length;
    return bytes;
}

Json pick(const Json& object, std::initializer_list<const char*> keys) {
    auto result = Json::object();
    for (const auto* key : keys) {
        const auto value = object.find(key);
        if (value == object.end()) continue;
        result[key] = *value;
    }
    return result;
}

std::string path(const GitRepository& repo) {
    return "/repos/" + repo.repo;
}
std::string id(const Json& args, const char* key) {
    return std::to_string(args.at(key).get<Id>());
}

GitPullRequest pull(const Json& value) {
    const auto& base = value.at("base");
    const auto& head = value.at("head");
    const auto baseRepo = GitRepository::parse("github.com", base.at("repo").at("full_name").get<std::string>());
    const auto headRepo = GitRepository::parse("github.com", head.at("repo").at("full_name").get<std::string>());
    const auto number = value.at("number").get<Id>();
    return {{baseRepo, headRepo, str(base, "ref"), str(head, "ref")},
            number,
            "https://" + baseRepo.resource() + "/pull/" + std::to_string(number),
            correlation(str(value, "body")),
            str(head, "sha"),
            value.at("state") == "open" && !value.value("draft", false)};
}

Json compactPr(const Json& value) {
    auto result = pick(value, {"number", "html_url", "title", "state", "draft", "merged", "mergeable", "review_comments"});
    result["headSha"] = str(value.at("head"), "sha");
    return result;
}

Json compactIssue(const Json& value) {
    auto result = pick(value, {"number", "html_url", "title", "state", "body", "comments", "updated_at"});
    result["labels"] = Json::array();
    for (const auto& label : value.at("labels")) result["labels"].push_back(label.at("name"));
    result["assignees"] = Json::array();
    for (const auto& user : value.at("assignees")) result["assignees"].push_back(user.at("login"));
    return result;
}

Json compactComment(const Json& value) {
    auto result = pick(value, {"id", "html_url", "body", "updated_at"});
    result["author"] = str(value.at("user"), "login");
    return result;
}

class GitHub final : public GitHost {
public:
    explicit GitHub(std::string token) : token_(std::move(token)) {
        if (token_.empty() || token_.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != token_.npos)
            throw Conflict("Invalid GitHub token");
        static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (initialized != CURLE_OK) throw Conflict("GitHub HTTP initialization failed");
    }
    std::string_view name() const override { return "github.com"; }
    void verifyToken() const { (void)json("GET", "/rate_limit"); }
    bool permits(const GitContext&, const GitRepository& repository, std::string_view) const override { return repository.host == name(); }
    bool permitsStoredRead(const GitContext& context, const GitRepository& repo, const Decision& read) override {
        if (repo.host != name()) return false;
        if (read.tool == "git_find_repos")
            json("GET", path(repo));
        else if (read.tool == "git_read_ci_logs")
            jobLogUrl(repo, read.args.at("jobId"));
        else
            call(context, repo, read);
        return true;
    }

    std::pair<std::vector<GitRepository>, bool> repositories(const GitContext&, std::string_view query, Id page) override {
        std::vector<GitRepository> matches;
        const auto response = json("GET", "/user/repos?sort=full_name&per_page=100&page=" + std::to_string(page));
        if (!response.is_array()) throw Conflict("Expected a list of GitHub repositories");
        for (const auto& item : response) {
            auto repo = GitRepository::parse(name(), str(item, "full_name"));
            if (repo.repo.find(query) != repo.repo.npos) matches.push_back(std::move(repo));
        }
        return {std::move(matches), response.size() == 100};
    }

    GitPullRequest createPullRequest(const GitContext&, const GitOpenPr& request) override {
        const auto& [base, head, baseRef, headRef] = request.target;
        const auto owner = head.repo.substr(0, head.repo.find('/'));
        Json body{{"base", baseRef},
                  {"head", head == base ? headRef : owner + ":" + headRef},
                  {"title", request.title},
                  {"body", request.body + "\n\n" + marker(request.operationId)},
                  {"draft", request.draft}};
        if (base != head && base.repo.starts_with(owner + "/")) body["head_repo"] = head.repo.substr(owner.size() + 1);
        return pull(json("POST", path(base) + "/pulls", body));
    }

    std::vector<GitPullRequest> findPullRequests(const GitContext&, const GitPrTarget& target) override {
        const auto& [base, head, baseRef, headRef] = target;
        const auto owner = head.repo.substr(0, head.repo.find('/'));
        const auto url =
            path(base) + "/pulls?state=all&per_page=30&head=" + encode(owner + ":" + headRef) + "&base=" + encode(baseRef) + "&page=";
        std::vector<GitPullRequest> results;
        for (int page = 1; page <= 50; ++page) {
            const auto response = json("GET", url + std::to_string(page));
            if (!response.is_array()) throw Conflict("Expected a list of GitHub pull requests");
            for (const auto& raw : response) {
                if (raw.at("head").at("repo").is_null() || str(raw, "body").find(markerPrefix) == std::string::npos) continue;
                auto candidate = pull(raw);
                if (candidate.target == target) results.push_back(std::move(candidate));
            }
            if (response.size() < 30) return results;
        }
        throw Conflict("GitHub pull request lookup exceeded its page limit");
    }

    GitLog readCiLogs(const GitContext&, const GitRepository& repo, Id job, std::size_t maxBytes) override {
        auto download = request("GET", jobLogUrl(repo, job), nullptr, maxBytes, false);
        requireSuccess(download.status);
        return {std::move(download.body.text), download.body.truncated};
    }

    Json call(const GitContext&, const GitRepository& repo, const Decision& decision) override {
        const auto& tool = decision.tool;
        const auto& args = decision.args;
        const auto root = path(repo);
        if (tool == "git_repo_info")
            return pick(json("GET", root), {"full_name", "html_url", "description", "default_branch", "private", "archived"});
        if (tool == "git_commit_status") {
            const auto commit = root + "/commits/" + encode(args.at("sha").get<std::string>());
            const auto status = json("GET", commit + "/status?per_page=30");
            const auto checks = json("GET", commit + "/check-runs?per_page=30");
            Json result{{"state", status.at("state")},
                        {"totalStatuses", status.at("total_count")},
                        {"totalChecks", checks.at("total_count")},
                        {"statuses", Json::array()},
                        {"checks", Json::array()}};
            for (const auto& item : status.at("statuses"))
                result["statuses"].push_back(pick(item, {"context", "state", "description", "target_url"}));
            for (const auto& item : checks.at("check_runs"))
                result["checks"].push_back(pick(item, {"id", "name", "status", "conclusion", "html_url", "details_url"}));
            return result;
        }
        if (tool == "git_comment") return comment(root, args);
        if (tool == "git_manage_issue") return issue(root, args);
        const auto pr = root + "/pulls/" + id(args, "prNumber");
        if (tool == "git_pr_status") return compactPr(json("GET", pr));
        if (tool == "git_update_pr") {
            auto body = pick(args, {"title", "state"});
            if (args.contains("baseRef")) body["base"] = args.at("baseRef");
            if (args.contains("body")) {
                const auto operation = correlation(str(json("GET", pr), "body"));
                body["body"] = args.at("body");
                if (!operation.empty()) body["body"] = str(body, "body") + "\n\n" + marker(operation);
            }
            return compactPr(json("PATCH", pr, body));
        }
        if (tool == "git_request_review")
            return compactPr(json("POST", pr + "/requested_reviewers", {{"reviewers", args.at("reviewers")}}));
        if (tool == "git_submit_review") {
            const auto verdict = str(args, "verdict");
            const auto value = verdict == "approve" ? "APPROVE" : verdict == "request_changes" ? "REQUEST_CHANGES" : "COMMENT";
            return pick(json("POST", pr + "/reviews", {{"event", value}, {"body", str(args, "body")}}),
                        {"id", "html_url", "state", "submitted_at"});
        }
        throw GitRejected("Unsupported GitHub operation");
    }
private:
    std::string jobLogUrl(const GitRepository& repo, Id job) const {
        const auto endpoint = std::string(api) + path(repo) + "/actions/jobs/" + std::to_string(job) + "/logs";
        const auto response = request("GET", endpoint, nullptr, 8192, true);
        if (response.status != 302) {
            requireSuccess(response.status);
            throw Conflict("GitHub did not return a job log download URL");
        }
        if (!validLogUrl(response.redirect)) throw Conflict("GitHub returned an unexpected job log destination");
        return response.redirect;
    }

    HttpResponse request(const char* method, const std::string& url, const Json& data, std::size_t maxBytes, bool authenticated) const {
        const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) throw Conflict("Could not open a GitHub connection");
        Buffer response{{}, maxBytes};
        auto options = curl.get();
        curl_easy_setopt(options, CURLOPT_URL, url.c_str());
        curl_easy_setopt(options, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(options, CURLOPT_USERAGENT, "Flock");
        if (authenticated) {
            curl_easy_setopt(options, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
            curl_easy_setopt(options, CURLOPT_XOAUTH2_BEARER, token_.c_str());
        }
        curl_easy_setopt(options, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(options, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(options, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(options, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(options, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(options, CURLOPT_WRITEDATA, &response);
        curl_slist* headers = nullptr;
        for (const char* header :
             {"Accept: application/vnd.github+json", "Content-Type: application/json", "X-GitHub-Api-Version: 2022-11-28"}) {
            auto* next = curl_slist_append(headers, header);
            if (!next) {
                curl_slist_free_all(headers);
                throw Conflict("Could not prepare GitHub request headers");
            }
            headers = next;
        }
        const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> ownedHeaders(headers, curl_slist_free_all);
        if (authenticated) curl_easy_setopt(options, CURLOPT_HTTPHEADER, headers);
        const auto body = data.is_null() ? std::string{} : data.dump();
        if (!data.is_null()) {
            curl_easy_setopt(options, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(options, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
        const auto sent = curl_easy_perform(options);
        long status = 0;
        if (curl_easy_getinfo(options, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK)
            throw Conflict("Could not read the GitHub response status");
        if (sent != CURLE_OK && !(sent == CURLE_WRITE_ERROR && response.truncated && !authenticated))
            throw Conflict("GitHub request or response was interrupted");
        char* redirect = nullptr;
        if (status == 302 && curl_easy_getinfo(options, CURLINFO_REDIRECT_URL, &redirect) != CURLE_OK)
            throw Conflict("Could not read the GitHub redirect");
        return {std::move(response), status, redirect ? redirect : ""};
    }

    Json json(const char* method, const std::string& endpoint, const Json& data = nullptr) const {
        const auto response = request(method, std::string(api) + endpoint, data, responseLimit, true);
        requireSuccess(response.status);
        auto parsed = Json::parse(response.body.text, nullptr, false);
        if (parsed.is_discarded()) throw Conflict("GitHub returned malformed JSON");
        return parsed;
    }

    Json comment(const std::string& root, const Json& args) const {
        const auto target = root + "/issues/" + id(args, "number");
        validateIssueKind(target, args.at("target") == "pr");
        const Json body{{"body", args.at("body")}};
        if (!args.contains("commentId")) return compactComment(json("POST", target + "/comments", body));
        const auto endpoint = root + "/issues/comments/" + id(args, "commentId");
        auto issueUrl = str(json("GET", endpoint), "issue_url");
        std::ranges::transform(issueUrl, issueUrl.begin(), [](unsigned char c) { return std::tolower(c); });
        if (issueUrl != std::string(api) + target) throw GitRejected("Comment does not belong to the selected PR or issue");
        return compactComment(json("PATCH", endpoint, body));
    }

    void validateIssueKind(const std::string& target, bool pullRequest) const {
        const auto value = json("GET", target);
        if (value.contains("pull_request") != pullRequest) throw GitRejected("The selected GitHub number has a different target kind");
    }

    Json issue(const std::string& root, const Json& args) const {
        const auto action = str(args, "action");
        const auto issues = root + "/issues";
        const auto target = args.contains("issueNumber") ? issues + "/" + id(args, "issueNumber") : issues;
        const auto pageNumber = args.value("page", Id{1});
        const auto page = "&per_page=30&page=" + std::to_string(pageNumber);
        const auto continuation = [pageNumber](const Json& response) -> Json {
            return response.size() == 30 ? Json(pageNumber + 1) : Json(nullptr);
        };
        if (action == "list") {
            const auto response = json("GET", issues + "?state=" + args.value("state", "open") + page);
            auto items = Json::array();
            for (const auto& value : response)
                if (!value.contains("pull_request")) items.push_back(compactIssue(value));
            return {{"items", items}, {"nextPage", continuation(response)}};
        }
        if (action == "read") {
            const auto value = json("GET", target);
            if (value.contains("pull_request")) throw GitRejected("The selected number is a pull request");
            return compactIssue(value);
        }
        if (action == "create") return compactIssue(json("POST", target, pick(args, {"title", "body", "labels", "assignees"})));
        validateIssueKind(target, false);
        if (action == "update") return compactIssue(json("PATCH", target, pick(args, {"title", "body", "state", "labels", "assignees"})));
        const auto response = json("GET", target + "/comments?sort=created" + page);
        auto items = Json::array();
        for (const auto& value : response) items.push_back(compactComment(value));
        return {{"items", items}, {"nextPage", continuation(response)}};
    }

    const std::string token_;
};
}

std::shared_ptr<GitHost> makeGitHubHost(std::string token) {
    return std::make_shared<GitHub>(std::move(token));
}

void verifyGitHubToken(std::string token) {
    GitHub(std::move(token)).verifyToken();
}
}
