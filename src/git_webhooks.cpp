#include "conversation_storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
bool webhookKind(std::string_view kind) {
    return kind == "pull_request" || kind == "review" || kind == "comment" || kind == "check" || kind == "ci" || kind == "code_scan";
}

bool identifier(std::string_view word) {
    return !word.empty() && word.size() <= 128 &&
           word.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == word.npos;
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
}

bool Store::receiveGitWebhook(const GitWebhook& event) {
    const auto repo = GitRepository::parse(event.host, event.repo);
    const auto& sha = event.headSha;
    const auto& digest = event.payloadDigest;
    if ((!digest.empty() && (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != digest.npos)) ||
        !webhookKind(event.kind) || !identifier(event.eventId) || !identifier(event.action) || event.prNumbers.size() > 100 ||
        event.jobId < 0 || event.feedbackId < 0 ||
        (!sha.empty() && ((sha.size() != 40 && sha.size() != 64) || sha.find_first_not_of("0123456789abcdef") != sha.npos)) ||
        std::ranges::any_of(event.prNumbers, [](Id number) { return number <= 0; }) || (event.prNumbers.empty() && sha.empty()))
        throw std::invalid_argument("Invalid Git webhook event");
    std::lock_guard lock(p_->mutex);
    const auto scrub = p_->sharedRedactor();
    scrub.requirePublic({repo.host, repo.repo, event.eventId, event.action, sha});
    Json data{{"kind", event.kind},
              {"action", event.action},
              {"prNumbers", event.prNumbers},
              {"headSha", sha},
              {"summary", bounded(scrub.text(event.summary), 4000)},
              {"jobId", event.jobId}};
    if (!event.requestedReviewer.empty()) data["requestedReviewer"] = bounded(scrub.text(event.requestedReviewer), 100);
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
    Transaction transaction(p_->db);
    p_->exec("INSERT INTO git_webhook_events(host,event_id,repo,data,payload_digest) VALUES(?,?,?,?,?) ON CONFLICT DO NOTHING",
             {repo.host, event.eventId, repo.repo, data, digest.empty() ? Json(nullptr) : Json(digest)});
    if (sqlite3_changes(p_->db) == 0) return false;
    if (event.kind == "pull_request" && event.action == "synchronize" && !sha.empty()) {
        p_->exec(R"SQL(
INSERT INTO git_pr_heads SELECT ?1,?2,value,?3,?4,1 FROM json_each(?5) WHERE true
 ON CONFLICT(host,repo,pr_number,event_cursor) DO UPDATE SET head_sha=excluded.head_sha,synchronized=1
)SQL",
                 {repo.host, repo.repo, p_->last(), sha, event.prNumbers});
    }
    transaction.commit();
    p_->gitWebhooksDrainedAt.reset();
    return true;
}

void Store::Impl::gitInboxItem(const Json& task, std::string_view key, std::string_view url, std::string_view title,
                               std::string_view prompt) {
    const auto id = task.at("id");
    if (!query("SELECT 1 FROM pr_review_items WHERE task_id=? AND git_key=?", {id, key}).next())
        insertPrReview(task, url, title, prompt, key);
}

bool Store::Impl::canReceiveGitWebhook(std::string_view bot, const Json& event) {
    if (botPaused(bot)) return false;
    const auto repo = GitRepository::parse(field(event, "host"), field(event, "repo"));
    return query(R"SQL(
SELECT 1 FROM git_pr_watches w JOIN tasks t ON t.id=w.task_id AND t.bot_id=w.bot_id
WHERE w.host=? AND w.repo=? AND w.pr_number=? AND w.task_id=? AND w.bot_id=?
AND t.state NOT IN ('cancelled','cancelling')
)SQL",
                 {repo.host, repo.repo, event.at("prNumber"), event.at("taskId"), bot})
               .next() &&
           Policy(*this).allows(bot, Action::repository_read, repo.resource());
}

Id Store::Impl::latestGitWebhookId() const {
    auto row = query("SELECT coalesce(max(id),0) FROM git_webhook_events");
    row.next();
    return row.number(0);
}

void Store::Impl::skipPendingGitWebhooks(std::string_view bot) {
    const auto cursor = latestGitWebhookId();
    exec(R"SQL(
INSERT INTO git_pr_watch_origins(host,repo,pr_number,event_cursor)
SELECT host,repo,pr_number,event_cursor FROM git_pr_watches WHERE bot_id=? AND event_cursor<?
ON CONFLICT DO NOTHING
)SQL",
         {bot, cursor});
    exec("UPDATE git_pr_watches SET event_cursor=?1 WHERE bot_id=?2 AND event_cursor<?1", {cursor, bot});
    gitWebhooksDrainedAt.reset();
}

void Store::Impl::deliverGitWebhooks() {
    std::lock_guard lock(mutex);
    const auto version = dataVersion();
    if (gitWebhooksDrainedAt == version) return;
    constexpr unsigned batchSize = 100;
    Transaction transaction(db);
    auto rows = query(R"SQL(
WITH watches AS (
 SELECT w.host,w.repo,w.pr_number,w.task_id,w.bot_id,w.run,w.url,
        w.event_cursor AS delivery_cursor,coalesce(o.event_cursor,w.event_cursor) AS attribution_cursor
 FROM git_pr_watches w LEFT JOIN git_pr_watch_origins o USING(host,repo,pr_number)
), matches AS (
 SELECT e.id,e.host,e.event_id,e.repo,e.data,w.pr_number,w.task_id,w.bot_id,w.run,w.url,w.delivery_cursor,
        json_array_length(e.data,'$.prNumbers')>0 AS explicit_pr,
        count(*) OVER (PARTITION BY e.id) AS watch_count
 FROM git_webhook_events e JOIN watches w ON w.host=e.host AND w.repo=e.repo AND e.id>w.attribution_cursor
 WHERE EXISTS (SELECT 1 FROM json_each(e.data,'$.prNumbers') p WHERE p.value=w.pr_number)
    OR (json_array_length(e.data,'$.prNumbers')=0 AND json_extract(e.data,'$.headSha') IN (
        (SELECT h.head_sha FROM git_pr_heads h
         WHERE (h.host,h.repo,h.pr_number)=(w.host,w.repo,w.pr_number)
           AND h.event_cursor BETWEEN w.attribution_cursor AND e.id-1
         ORDER BY h.event_cursor DESC LIMIT 1),
        (SELECT CASE WHEN h.synchronized THEN h.head_sha END FROM git_pr_heads h
         WHERE (h.host,h.repo,h.pr_number)=(w.host,w.repo,w.pr_number) AND h.event_cursor>e.id
           AND (h.synchronized OR h.head_sha<>json_extract(e.data,'$.headSha'))
         ORDER BY h.event_cursor LIMIT 1)))
)
SELECT id,host,event_id,repo,data,pr_number,task_id,bot_id,run,url FROM matches m
WHERE id>delivery_cursor AND (explicit_pr OR watch_count=1)
 AND NOT EXISTS (SELECT 1 FROM deleted_bots retired WHERE retired.bot=m.bot_id)
 AND NOT EXISTS (SELECT 1 FROM git_webhook_deliveries d
                 WHERE d.event=m.id AND (NOT m.explicit_pr OR d.pr_number=m.pr_number))
ORDER BY id,pr_number LIMIT ?
)SQL",
                      {batchSize});
    Json deliveries = Json::array();
    while (rows.next()) {
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
    for (auto& event : deliveries) {
        const auto delivery = event.at("deliveryId");
        const auto pr = event.at("prNumber");
        const auto feedback = event.value("feedbackId", Id{});
        const auto kind = field(event, "kind");
        const auto host = field(event, "host");
        const auto repo = field(event, "repo");
        if (feedback && gitFeedbackMutationsInFlight.contains({host, repo, pr.get<Id>()})) continue;
        exec("INSERT INTO git_webhook_deliveries VALUES(?,?)", {delivery, pr});
        if (feedback && query(R"SQL(
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
        const auto task = load(taskId);
        event.erase("botId");
        event.erase("deliveryId");
        event.erase("feedbackId");
        const auto reviewer = field(event, "requestedReviewer");
        event.erase("requestedReviewer");
        const auto action = field(event, "action");
        const auto scrub = sharedRedactor();
        event["summary"] = bounded(scrub.text(field(event, "summary")), 4000);
        const auto key = "webhook/" + std::to_string(delivery.get<Id>()) + "/" + std::to_string(pr.get<Id>());
        auto inboxKey = key;
        const auto url = field(event, "url");
        if (const auto location = event.find("reviewLocation"); location != event.end()) {
            const auto commentId = location->at("commentId").get<Id>();
            location->erase("commentId");
            (*location)["url"] = url + "#discussion_r" + std::to_string(commentId);
            *location = scrub.record(std::move(*location));
        }
        const auto prompt = field(event, "summary");
        std::string title;
        if (kind == "review" && action == "submitted") title = "Pull request review submitted";
        if (kind == "code_scan") title = "Code scanning result";
        if (kind == "pull_request" && (action == "review_requested" || action == "ready_for_review"))
            title = "Pull request awaiting review";
        if (kind == "pull_request" && action == "review_requested" && !reviewer.empty()) {
            auto receipt =
                query(R"SQL(
DELETE FROM git_review_receipts WHERE rowid=(
 SELECT r.rowid FROM git_review_receipts r
 LEFT JOIN pr_review_items i ON i.task_id=r.task_id AND i.git_key=r.inbox_key
 WHERE r.task_id=? AND r.host=? AND r.repo=? AND r.pr_number=? AND r.reviewer=lower(?) AND r.after_event<?
   AND (i.id IS NULL OR (i.status='pending' AND i.target_revision=?))
 ORDER BY r.after_event,r.rowid LIMIT 1
) RETURNING inbox_key
)SQL",
                      {taskId, event.at("host"), event.at("repo"), pr, reviewer, delivery, task.value("currentRevision", Id{0})});
            if (receipt.next()) inboxKey = receipt.string(0);
        }
        if (!title.empty()) gitInboxItem(task, inboxKey, url, title, prompt.empty() ? title : prompt);

        const Json routing{{"environmentId", task.at("environmentId")},
                           {"projectId", task.at("projectId")},
                           {"config", task.at("config")},
                           {"gitEvent", event}};
        exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'git_webhook',?,?)",
             {taskId, unixTime(), scrub.record(event), bot});
        const auto message = last();
        exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root,principal_source)
SELECT ?,?,?,coalesce(max(sequence),0)+1,NULL,'',?,'git-webhook',0,? FROM messages WHERE bot_id=?
)SQL",
             {message, bot, key, routing, static_cast<int>(conversation::PrincipalSource::gitWebhook), bot});
        exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')", {message});
    }
    transaction.commit();
    if (deliveries.size() < batchSize) gitWebhooksDrainedAt = version;
}
}
