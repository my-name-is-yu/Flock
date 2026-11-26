#include "flock/git_events.hpp"
#include "secret_storage.hpp"
#include "git_storage.hpp"
#include "conversation_storage.hpp"
#include "flock/git_host.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/task.hpp"
#include "t3_credentials.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "flock/policy.hpp"
#include "bot_lifecycle_storage.hpp"
#include "task_records.hpp"
#include <sqlite3.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
bool webhookKind(std::string_view kind) {
    return kind == "pull_request" || kind == "review" || kind == "comment" || kind == "check" || kind == "ci" || kind == "code_scan";
}

bool identifier(std::string_view word) {
    return !word.empty() && word.size() <= 128 &&
           word.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == word.npos;
}

bool isLowerHex(std::string_view value) {
    return value.find_first_not_of("0123456789abcdef") == value.npos;
}

bool hasValidWebhookTarget(const GitWebhook& event) {
    const auto& sha = event.headSha;
    if (event.prNumbers.empty() && sha.empty()) return false;
    if (event.prNumbers.size() > 100) return false;
    if (!sha.empty() && ((sha.size() != 40 && sha.size() != 64) || !isLowerHex(sha))) return false;
    return std::ranges::all_of(event.prNumbers, [](Id number) { return number > 0; });
}

std::string_view followupType(GitFollowup followup) {
    switch (followup) {
    case GitFollowup::failedCheck: return "failed_check";
    case GitFollowup::changesRequested: return "changes_requested";
    case GitFollowup::reviewComment: return "review_comment";
    case GitFollowup::none: return {};
    }
    throw std::invalid_argument("Invalid Git follow-up type");
}

std::string priorLifecycleAction(Database& database, Id task, Id message, const GitRepository& repo, Id pr) {
    auto event = database.query(R"SQL(
SELECT json_extract(data,'$.action') AS action
FROM journal
WHERE task_id=? AND id<? AND kind='git_webhook' AND json_extract(data,'$.kind')='pull_request'
  AND (json_extract(data,'$.host'),json_extract(data,'$.repo'),json_extract(data,'$.prNumber'))=(?,?,?)
  AND action IN ('merged','closed','reopened')
ORDER BY (action='merged') DESC,id DESC LIMIT 1
)SQL",
                                {task, message, repo.host, repo.repo, pr});
    return event.next() ? event.string(0) : std::string{};
}
bool gitPrEndedEvent(const Json& event) {
    const auto action = field(event, "action");
    return field(event, "kind") == "pull_request" && (action == "closed" || action == "merged");
}
}

bool GitStorage::receiveGitWebhook(const GitWebhook& event) {
    const auto repo = GitRepository::parse(event.host, event.repo);
    const auto& sha = event.headSha;
    const auto& digest = event.payloadDigest;
    const bool validDigest = digest.empty() || (digest.size() == 64 && isLowerHex(digest));
    const bool validEventIdentity = webhookKind(event.kind) && identifier(event.eventId) && identifier(event.action);
    const bool validRelatedIds = event.jobId >= 0 && event.feedbackId >= 0;
    if (!validDigest || !validEventIdentity || !validRelatedIds || !hasValidWebhookTarget(event))
        throw std::invalid_argument("Invalid Git webhook event");
    std::lock_guard lock(database.mutex);
    const auto scrub = secretStorage.sharedRedactor();
    scrub.requirePublic({repo.host, repo.repo, event.eventId, event.action, sha});
    Json data{{"kind", event.kind},
              {"action", event.action},
              {"prNumbers", event.prNumbers},
              {"headSha", sha},
              {"summary", bounded(scrub.text(event.summary), 4000)},
              {"jobId", event.jobId}};
    if (const auto followup = followupType(event.followup); !followup.empty()) data["followup"] = followup;
    if (event.feedbackId) data["feedbackId"] = event.feedbackId;
    if (event.reviewLocation) {
        const auto& [comment, path, side, line, original] = *event.reviewLocation;
        if (event.kind != "comment" || comment <= 0 || line < 0) throw std::invalid_argument("Invalid Git review comment location");
        Json location{{"commentId", comment}, {"path", bounded(redactT3Tokens(scrub.text(path)), 600)}};
        if (line) location.update({{"line", line}, {"originalLine", original}});
        if (side == "LEFT" || side == "RIGHT") location["side"] = side;
        data["reviewLocation"] = std::move(location);
    }
    Transaction transaction(database.db);
    database.exec("INSERT INTO git_webhook_events(host,event_id,repo,data,payload_digest) VALUES(?,?,?,?,?) ON CONFLICT DO NOTHING",
                  {repo.host, event.eventId, repo.repo, data, digest.empty() ? Json(nullptr) : Json(digest)});
    if (sqlite3_changes(database.db) == 0) return false;
    if (event.kind == "pull_request" && event.action == "synchronize" && !sha.empty()) {
        database.exec(R"SQL(
INSERT INTO git_pr_heads SELECT ?1,?2,value,?3,?4,1 FROM json_each(?5) WHERE true
 ON CONFLICT(host,repo,pr_number,event_cursor) DO UPDATE SET head_sha=excluded.head_sha,synchronized=1
)SQL",
                      {repo.host, repo.repo, database.last(), sha, event.prNumbers});
    }
    transaction.commit();
    gitWebhooksDrainedAt.reset();
    return true;
}

bool GitStorage::canReceiveGitWebhook(std::string_view bot, const Json& event) {
    if (botLifecycle.botPaused(bot)) return false;
    const auto repo = GitRepository::parse(field(event, "host"), field(event, "repo"));
    return database
               .query(R"SQL(
SELECT 1 FROM git_pr_watches w JOIN tasks t ON t.id=w.task_id AND t.bot_id=w.bot_id
WHERE w.host=? AND w.repo=? AND w.pr_number=? AND w.task_id=? AND w.bot_id=?
AND t.state NOT IN ('completed','cancelled','cancelling')
)SQL",
                      {repo.host, repo.repo, event.at("prNumber"), event.at("taskId"), bot})
               .next() &&
           permissions.allows(bot, Action::repository_read, repo.resource());
}

Json GitStorage::watchedPullRequests(const conversation::Message& message) {
    const auto task = message.routing.at("gitEvent").at("taskId").get<Id>();
    auto watches = database.query(
        "SELECT host,repo,pr_number FROM git_pr_watches WHERE task_id=? AND bot_id=? ORDER BY host,repo,pr_number", {task, message.bot});
    auto pullRequests = Json::array();
    while (watches.next()) {
        const GitRepository repo{watches.string(0), watches.string(1)};
        if (!permissions.allows(message.bot, Action::repository_read, repo.resource())) continue;
        const auto pr = watches.number(2);
        Json pullRequest{{"host", repo.host}, {"repo", repo.repo}, {"prNumber", pr}};
        if (auto action = priorLifecycleAction(database, task, message.id, repo, pr); !action.empty())
            pullRequest["priorLifecycleAction"] = std::move(action);
        pullRequests.push_back(std::move(pullRequest));
    }
    return pullRequests;
}

bool GitStorage::gitNotificationCanComplete(const conversation::Message& message) {
    const auto& event = message.routing.at("gitEvent");
    if (gitPrEndedEvent(event)) return true;
    const auto kind = field(event, "kind");
    if ((kind != "ci" && kind != "check") || field(event, "action") != "completed") return false;
    const GitRepository repo{field(event, "host"), field(event, "repo")};
    return priorLifecycleAction(database, event.at("taskId").get<Id>(), message.id, repo, event.at("prNumber").get<Id>()) == "merged";
}

Id GitStorage::latestGitWebhookId() const {
    auto row = database.query("SELECT coalesce(max(id),0) FROM git_webhook_events");
    row.next();
    return row.number(0);
}

void GitStorage::deliverGitWebhooks() {
    std::lock_guard lock(database.mutex);
    const auto version = database.dataVersion();
    if (gitWebhooksDrainedAt == version) return;
    constexpr unsigned batchSize = 100;
    Transaction transaction(database.db);
    auto rows = database.query(R"SQL(
WITH matches AS (
 SELECT e.id,e.host,e.event_id,e.repo,e.data,w.pr_number,w.task_id,w.bot_id,w.run,w.url,
        t.state='deciding' AND coalesce(json_extract(task_event.data,'$.kind'),'')<>'external_check' AS deciding,
        json_array_length(e.data,'$.prNumbers')>0 AS explicit_pr,
        count(*) OVER event_watches AS watch_count, min(w.pr_number) OVER event_watches AS first_watch
 FROM git_webhook_events e JOIN git_pr_watches w ON w.host=e.host AND w.repo=e.repo AND e.id>w.event_cursor
 JOIN tasks t ON t.id=w.task_id
 LEFT JOIN events task_event ON task_event.task_id=t.id AND task_event.id=json_extract(t.data,'$.eventId')
 WHERE EXISTS (SELECT 1 FROM json_each(e.data,'$.prNumbers') p WHERE p.value=w.pr_number)
    OR (json_array_length(e.data,'$.prNumbers')=0 AND json_extract(e.data,'$.headSha') IN (
        (SELECT h.head_sha FROM git_pr_heads h
         WHERE (h.host,h.repo,h.pr_number)=(w.host,w.repo,w.pr_number)
           AND h.event_cursor BETWEEN w.event_cursor AND e.id-1
         ORDER BY h.event_cursor DESC LIMIT 1),
        (SELECT CASE WHEN h.synchronized THEN h.head_sha END FROM git_pr_heads h
         WHERE (h.host,h.repo,h.pr_number)=(w.host,w.repo,w.pr_number) AND h.event_cursor>e.id
           AND (h.synchronized OR h.head_sha<>json_extract(e.data,'$.headSha'))
         ORDER BY h.event_cursor LIMIT 1)))
 WINDOW event_watches AS (PARTITION BY e.id)
)
SELECT id,host,event_id,repo,data,pr_number,task_id,bot_id,run,url,
       deciding AND (explicit_pr OR watch_count=1) AS deferred, NOT explicit_pr AND watch_count>1 AS ambiguous
FROM matches m
WHERE (explicit_pr OR pr_number=first_watch)
 AND NOT EXISTS (SELECT 1 FROM git_webhook_deliveries d
                 WHERE d.event=m.id AND (NOT m.explicit_pr OR d.pr_number=m.pr_number))
ORDER BY deferred,id,pr_number LIMIT ?
)SQL",
                               {batchSize});
    Json deliveries = Json::array();
    std::vector<std::pair<Id, Id>> ambiguous;
    bool deferred = false;
    while (rows.next()) {
        if (rows.number(10)) {
            deferred = true;
            break;
        }
        if (rows.number(11)) {
            ambiguous.emplace_back(rows.number(0), rows.number(5));
            continue;
        }
        auto event = rows.json(4);
        event.erase("prNumbers");
        event.update({{"deliveryId", rows.number(0)},
                      {"host", rows.string(1)},
                      {"eventId", rows.string(2)},
                      {"repo", rows.string(3)},
                      {"prNumber", rows.number(5)},
                      {"taskId", rows.number(6)},
                      {"botId", rows.string(7)},
                      {"run", rows.json(8)},
                      {"url", rows.string(9)}});
        deliveries.push_back(std::move(event));
    }
    for (const auto& [event, pr] : ambiguous) database.exec("INSERT INTO git_webhook_deliveries VALUES(?,?)", {event, pr});
    for (auto& event : deliveries) {
        const auto delivery = event.at("deliveryId");
        const auto pr = event.at("prNumber");
        const auto feedback = event.value("feedbackId", Id{});
        const auto kind = field(event, "kind");
        const auto host = field(event, "host");
        const auto repo = field(event, "repo");
        if (feedback && gitFeedbackMutationsInFlight.contains({host, repo, pr.get<Id>()})) continue;
        database.exec("INSERT INTO git_webhook_deliveries VALUES(?,?)", {delivery, pr});
        if (feedback && database
                            .query(R"SQL(
SELECT 1 FROM git_mutations
WHERE json_extract(result,'$.data.id')=?1
  AND json_extract(invocation,'$[2].host')=?2
  AND json_extract(invocation,'$[2].repo')=?3
  AND ((?5='review' AND json_extract(invocation,'$[1]')='git_submit_review'
        AND json_extract(invocation,'$[2].prNumber')=?4)
    OR (?5='comment' AND json_extract(invocation,'$[1]')='git_comment'
        AND json_extract(invocation,'$[2].target')='pr'
        AND json_extract(invocation,'$[2].number')=?4
        AND json_type(invocation,'$[2].commentId') IS NULL))
)SQL",
                                   {feedback, host, repo, pr, kind})
                            .next())
            continue;
        const auto taskId = event.at("taskId").get<Id>();
        const auto bot = field(event, "botId");
        if (!canReceiveGitWebhook(bot, event)) continue;
        const auto task = taskRecords.load(taskId);
        event.erase("botId");
        event.erase("deliveryId");
        event.erase("feedbackId");
        const auto scrub = secretStorage.sharedRedactor();
        event["summary"] = bounded(scrub.text(field(event, "summary")), 4000);
        const auto key = "webhook/" + std::to_string(delivery.get<Id>()) + "/" + std::to_string(pr.get<Id>());
        const auto url = field(event, "url");
        if (const auto location = event.find("reviewLocation"); location != event.end()) {
            const auto commentId = location->at("commentId").get<Id>();
            location->erase("commentId");
            (*location)["url"] = url + "#discussion_r" + std::to_string(commentId);
            *location = scrub.record(std::move(*location));
        }
        const Json routing{
            {"environmentId", task.environmentId}, {"projectId", task.projectId}, {"config", task.config}, {"gitEvent", event}};
        database.exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'git_webhook',?,?)",
                      {taskId, unixTime(), scrub.record(event), bot});
        const auto message = database.last();
        database.exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root,principal_source)
SELECT ?,?,?,coalesce(max(sequence),0)+1,NULL,'',?,'git-webhook',0,? FROM messages WHERE bot_id=?
)SQL",
                      {message, bot, key, routing, static_cast<int>(conversation::PrincipalSource::gitWebhook), bot});
        database.exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')", {message});
    }
    transaction.commit();
    if (!deferred && deliveries.size() + ambiguous.size() < batchSize) gitWebhooksDrainedAt = version;
}

bool GitEvents::receiveGitWebhook(const GitWebhook& event) {
    return git_.receiveGitWebhook(event);
}
}
