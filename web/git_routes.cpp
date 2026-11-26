#include "git_routes.hpp"
#include "flock/git_events.hpp"
#include "flock/git_host.hpp"
#include "flock/json.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace flockv0::web {
namespace {
constexpr std::size_t maxWebhookBytes = 1024 * 1024;

void reply(httplib::Response& response, int status, const Json& result) {
    response.status = status;
    response.set_content(result.dump(), "application/json");
}

std::string hexDigest(const unsigned char* digest, unsigned length) {
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(length * 2);
    for (unsigned i = 0; i < length; ++i) {
        output += hex[digest[i] >> 4];
        output += hex[digest[i] & 15];
    }
    return output;
}

bool verified(const httplib::Request& request, const std::string& secret) {
    if (request.get_header_value_count("X-Hub-Signature-256") != 1) return false;
    const auto signature = request.get_header_value("X-Hub-Signature-256");
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (secret.size() > std::numeric_limits<int>::max() ||
        !HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(request.body.data()),
              request.body.size(), digest, &length))
        return false;
    const auto expected = "sha256=" + hexDigest(digest, length);
    return signature.size() == expected.size() && CRYPTO_memcmp(signature.data(), expected.data(), expected.size()) == 0;
}

std::string str(const Json& data, const char* key) {
    const auto field = data.find(key);
    return field != data.end() && field->is_string() ? field->get<std::string>() : std::string{};
}

Id number(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<Id>::max()) ||
        value.get<Id>() < 1)
        throw std::invalid_argument("Invalid GitHub identifier");
    return value.get<Id>();
}

bool addPulls(GitWebhook& event, const Json& source, const Json& repository) {
    if (!source.contains("pull_requests")) return false;
    const auto& prs = source.at("pull_requests");
    if (!prs.is_array()) throw std::invalid_argument("Invalid GitHub pull request list");
    for (const auto& pr : prs)
        if (pr.at("base").at("repo").at("id") == repository.at("id")) event.prNumbers.push_back(number(pr, "number"));
    return !prs.empty();
}

void pullFromRef(GitWebhook& event, std::string ref) {
    if (!ref.starts_with("refs/pull/")) return;
    const std::string_view rest(ref.data() + 10, ref.size() - 10);
    const auto separator = rest.find('/');
    if (separator == rest.npos || (rest.substr(separator) != "/merge" && rest.substr(separator) != "/head")) return;
    Id value = 0;
    const auto [end, error] = std::from_chars(rest.data(), rest.data() + separator, value);
    if (error == std::errc{} && end == rest.data() + separator && value > 0) event.prNumbers.push_back(value);
}

std::optional<GitWebhook> githubEvent(const httplib::Request& request) {
    const auto type = request.get_header_value("X-GitHub-Event");
    if (type == "ping") return {};
    const auto payload = Json::parse(request.body);
    GitWebhook event;
    event.host = "github.com";
    event.eventId = request.get_header_value("X-GitHub-Delivery");
    event.action = str(payload, "action");
    if (type == "pull_request" || type == "pull_request_review" || type == "pull_request_review_comment") {
        const auto& pr = payload.at("pull_request");
        event.prNumbers.push_back(number(pr, "number"));
        event.kind = type == "pull_request" ? "pull_request" : type == "pull_request_review" ? "review" : "comment";
        event.headSha = str(pr.at("head"), "sha");
        event.summary = str(pr, "title");
        const bool open = str(pr, "state") != "closed";
        if (event.kind == "pull_request" && event.action == "review_requested") {
            const auto reviewer = payload.find("requested_reviewer");
            if (reviewer != payload.end() && reviewer->is_object()) event.requestedReviewer = str(*reviewer, "login");
        }
        if (event.kind == "review") {
            const auto& review = payload.at("review");
            const auto state = str(review, "state");
            const auto body = str(review, "body");
            event.summary = state + "\n" + body;
            event.feedbackId = number(review, "id");
            if (event.action == "submitted" && open) {
                if (state == "changes_requested") event.followup = GitFollowup::changesRequested;
                if (state == "commented" && !body.empty()) event.followup = GitFollowup::reviewComment;
            }
        }
        if (event.kind == "comment") {
            const auto& comment = payload.at("comment");
            event.summary = str(comment, "body");
            GitReviewLocation location{number(comment, "id"), str(comment, "path"), str(comment, "side")};
            if (comment.contains("line") && !comment.at("line").is_null()) {
                location.line = number(comment, "line");
            } else if (comment.contains("original_line") && !comment.at("original_line").is_null()) {
                location.line = number(comment, "original_line");
                location.originalLine = true;
            }
            event.reviewLocation = std::move(location);
            if (event.action == "created" && open && !event.summary.empty()) event.followup = GitFollowup::reviewComment;
        }
    } else if (type == "issue_comment") {
        const auto& issue = payload.at("issue");
        if (!issue.contains("pull_request")) return {};
        event.prNumbers.push_back(number(issue, "number"));
        event.kind = "comment";
        const auto& comment = payload.at("comment");
        event.summary = str(comment, "body");
        event.feedbackId = number(comment, "id");
    } else if (type == "check_run" || type == "check_suite" || type == "workflow_job" || type == "workflow_run") {
        if (event.action != "completed") return {};
        const auto& check = payload.at(type);
        event.kind = type.starts_with("check_") ? "check" : "ci";
        event.headSha = str(check, "head_sha");
        if (addPulls(event, check, payload.at("repository")) && event.prNumbers.empty()) return {};
        const auto conclusion = str(check, "conclusion");
        event.summary = str(check, "name") + " " + str(check, "status") + " " + conclusion;
        if (type == "workflow_job") event.jobId = number(check, "id");
        if ((type == "check_run" || type == "workflow_job") &&
            (conclusion == "failure" || conclusion == "timed_out" || conclusion == "startup_failure"))
            event.followup = GitFollowup::failedCheck;
    } else if (type == "status") {
        event.kind = "check";
        event.action = str(payload, "state");
        event.headSha = str(payload, "sha");
        event.summary = str(payload, "context") + " " + str(payload, "description");
        if (event.action == "failure" || event.action == "error") event.followup = GitFollowup::failedCheck;
    } else if (type == "code_scanning_alert") {
        const auto& alert = payload.at("alert");
        event.kind = "code_scan";
        pullFromRef(event, str(payload, "ref"));
        event.headSha = str(payload, "commit_oid");
        event.summary = str(alert.at("rule"), "description") + "\n" + str(alert, "html_url");
    } else
        return {};
    if (event.prNumbers.empty() && event.headSha.empty()) return {};
    event.repo = payload.at("repository").at("full_name").get<std::string>();
    return event;
}
}

void gitRoutes(httplib::Server& server, Store& store, const WebCallbacks& callbacks) {
    server.Post(R"(/api/git/webhooks/([^/]+))", [&store, &callbacks](const httplib::Request& req, httplib::Response& res) {
        if (req.matches[1].str() != "github.com" || callbacks.githubWebhookSecret.empty())
            return reply(res, 404, {{"error", "unknown_git_webhook"}});
        if (req.body.size() > maxWebhookBytes) return reply(res, 413, {{"error", "payload_too_large"}});
        if (!verified(req, callbacks.githubWebhookSecret)) return reply(res, 401, {{"error", "invalid_webhook_signature"}});
        if (req.get_header_value_count("X-GitHub-Delivery") != 1 || req.get_header_value_count("X-GitHub-Event") != 1)
            return reply(res, 400, {{"error", "invalid_webhook_headers"}});
        try {
            auto event = githubEvent(req);
            if (!event) return reply(res, 202, {{"status", "ignored"}});
            unsigned char digest[SHA256_DIGEST_LENGTH];
            SHA256(reinterpret_cast<const unsigned char*>(req.body.data()), req.body.size(), digest);
            event->payloadDigest = hexDigest(digest, SHA256_DIGEST_LENGTH);
            reply(res, 202, {{"status", GitEvents(store).receiveGitWebhook(*event) ? "received" : "duplicate"}});
        } catch (const Json::exception&) {
            reply(res, 400, {{"error", "invalid_git_webhook"}});
        } catch (const std::invalid_argument&) {
            reply(res, 400, {{"error", "invalid_git_webhook"}});
        } catch (const Conflict&) {
            reply(res, 400, {{"error", "invalid_git_webhook"}});
        }
    });
}
}
