#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
bool webhookKind(std::string_view kind) {
    return kind == "pull_request" || kind == "review" || kind == "comment" ||
        kind == "check" || kind == "ci" || kind == "code_scan";
}

bool identifier(std::string_view word) {
    return !word.empty() && word.size() <= 128 &&
        word.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == word.npos;
}
}

bool Store::receiveGitWebhook(const GitWebhook& event) {
    const auto repo = GitRepository::parse(event.host,event.repo);
    const auto& sha = event.headSha;
    const auto& digest = event.payloadDigest;
    if ((!digest.empty() && (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != digest.npos)) ||
        !webhookKind(event.kind) || !identifier(event.eventId) || !identifier(event.action) ||
        event.prNumbers.size() > 100 || event.jobId < 0 ||
        (!sha.empty() && ((sha.size() != 40 && sha.size() != 64) ||
                          sha.find_first_not_of("0123456789abcdef") != sha.npos)) ||
        std::ranges::any_of(event.prNumbers,[](Id number) { return number <= 0; }) ||
        (event.prNumbers.empty() && sha.empty()))
        throw std::invalid_argument("Invalid Git webhook event");
    std::lock_guard lock(p_->mutex);
    const auto scrub = p_->sharedRedactor();
    scrub.requirePublic({repo.host,repo.repo,event.eventId,event.action,sha});
    const Json data{{"kind",event.kind},{"action",event.action},{"prNumbers",event.prNumbers},
        {"headSha",sha},{"summary",bounded(scrub.text(event.summary),4000)},{"jobId",event.jobId}};
    Transaction transaction(p_->db);
    p_->exec("INSERT INTO git_webhook_events(host,event_id,repo,data,payload_digest) VALUES(?,?,?,?,?) ON CONFLICT DO NOTHING",
             {repo.host,event.eventId,repo.repo,data,digest.empty() ? Json(nullptr) : Json(digest)});
    if (sqlite3_changes(p_->db) == 0) return false;
    if (event.kind == "pull_request" && event.action == "synchronize" && !sha.empty()) {
        p_->exec(R"SQL(
INSERT INTO git_pr_heads SELECT ?1,?2,value,?3,?4,1 FROM json_each(?5) WHERE true
 ON CONFLICT(host,repo,pr_number,event_cursor) DO UPDATE SET head_sha=excluded.head_sha,synchronized=1
)SQL",{repo.host,repo.repo,p_->last(),sha,event.prNumbers});
    }
    transaction.commit();
    p_->gitWebhooksDrainedAt.reset();
    return true;
}

void Store::Impl::gitInboxItem(const Json& task, std::string_view key, std::string_view url,
                               std::string_view title, std::string_view prompt) {
    const auto id = task.at("id");
    if (!query("SELECT 1 FROM pr_review_items WHERE task_id=? AND git_key=?",{id,key}).next())
        insertPrReview(task,url,title,prompt,key);
}

bool Store::Impl::canReceiveGitWebhook(std::string_view bot, const Json& event) {
    const auto repo = GitRepository::parse(field(event,"host"),field(event,"repo"));
    return query(R"SQL(
SELECT 1 FROM git_pr_watches w JOIN tasks t ON t.id=w.task_id AND t.bot_id=w.bot_id
WHERE w.host=? AND w.repo=? AND w.pr_number=? AND w.task_id=? AND w.bot_id=?
AND t.state NOT IN ('cancelled','cancelling')
)SQL",{repo.host,repo.repo,event.at("prNumber"),event.at("taskId"),bot}).next() &&
        Access(*this).permits(bot,{"repository",repo.resource(),"read"},true);
}

void Store::Impl::deliverGitWebhooks() {
    std::lock_guard lock(mutex);
    const auto version = dataVersion();
    if (gitWebhooksDrainedAt == version) return;
    constexpr unsigned batchSize = 100;
    Transaction transaction(db);
    auto rows = query(R"SQL(
WITH matches AS (
 SELECT e.id,e.host,e.event_id,e.repo,e.data,w.pr_number,w.task_id,w.bot_id,w.run,w.url,
        json_array_length(e.data,'$.prNumbers')>0 AS explicit_pr,
        count(*) OVER (PARTITION BY e.id) AS watch_count
 FROM git_webhook_events e JOIN git_pr_watches w ON w.host=e.host AND w.repo=e.repo AND e.id>w.event_cursor
 WHERE EXISTS (SELECT 1 FROM json_each(e.data,'$.prNumbers') p WHERE p.value=w.pr_number)
    OR (json_array_length(e.data,'$.prNumbers')=0 AND json_extract(e.data,'$.headSha') IN (
        (SELECT h.head_sha FROM git_pr_heads h
         WHERE (h.host,h.repo,h.pr_number)=(w.host,w.repo,w.pr_number)
           AND h.event_cursor BETWEEN w.event_cursor AND e.id-1
         ORDER BY h.event_cursor DESC LIMIT 1),
        (SELECT CASE WHEN h.synchronized THEN h.head_sha END FROM git_pr_heads h
         WHERE (h.host,h.repo,h.pr_number)=(w.host,w.repo,w.pr_number) AND h.event_cursor>e.id
         ORDER BY h.event_cursor LIMIT 1)))
)
SELECT id,host,event_id,repo,data,pr_number,task_id,bot_id,run,url FROM matches m
WHERE (explicit_pr OR watch_count=1)
 AND NOT EXISTS (SELECT 1 FROM git_webhook_deliveries d
                 WHERE d.event=m.id AND (NOT m.explicit_pr OR d.pr_number=m.pr_number))
ORDER BY id,pr_number LIMIT ?
)SQL",{batchSize});
    Json deliveries = Json::array();
    while (rows.next()) {
        auto event = rows.json(4);
        event.erase("prNumbers");
        event.update({{"deliveryId",rows.number(0)},{"host",rows.string(1)},{"eventId",rows.string(2)},
            {"repo",rows.string(3)},{"prNumber",rows.number(5)},{"taskId",rows.number(6)},{"botId",rows.string(7)},
            {"run",rows.json(8)},{"url",rows.string(9)}});
        deliveries.push_back(std::move(event));
    }
    for (auto& event : deliveries) {
        const auto delivery = event.at("deliveryId");
        const auto pr = event.at("prNumber");
        exec("INSERT INTO git_webhook_deliveries VALUES(?,?)",{delivery,pr});
        const auto taskId = event.at("taskId").get<Id>();
        const auto bot = field(event,"botId");
        if (!canReceiveGitWebhook(bot,event)) continue;
        const auto task = load(taskId);
        event.erase("botId");
        event.erase("deliveryId");
        const auto kind = field(event,"kind");
        const auto action = field(event,"action");
        const auto scrub = sharedRedactor();
        event["summary"] = bounded(scrub.text(field(event,"summary")),4000);
        const auto key = "webhook/" + std::to_string(delivery.get<Id>()) + "/" + pr.dump();
        const auto url = field(event,"url");
        const auto prompt = field(event,"summary");
        std::string title;
        if (kind == "review" && action == "submitted") title = "Pull request review submitted";
        if (kind == "code_scan") title = "Code scanning result";
        if (kind == "pull_request" && (action == "review_requested" || action == "ready_for_review"))
            title = "Pull request awaiting review";
        if (!title.empty()) gitInboxItem(task,key,url,title,prompt.empty() ? title : prompt);

        const Json routing{{"environmentId",task.at("environmentId")},{"projectId",task.at("projectId")},
                           {"config",task.at("config")},{"gitEvent",event}};
        exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'git_webhook',?,?)",
             {taskId,unixTime(),scrub.record(event),bot});
        const auto message = last();
        exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root)
SELECT ?,?,?,coalesce(max(sequence),0)+1,NULL,'',?,'git-webhook',0 FROM messages WHERE bot_id=?
)SQL",{message,bot,key,routing,bot});
        exec("INSERT INTO conversation_turns(message_id,status) VALUES(?,'running')",{message});
    }
    transaction.commit();
    if (deliveries.size() < batchSize) gitWebhooksDrainedAt = version;
}
}
