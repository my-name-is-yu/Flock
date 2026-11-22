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
    if (!webhookKind(event.kind) || !identifier(event.eventId) || !identifier(event.action) ||
        event.summary.size() > 4000 || event.prNumbers.size() > 100 || event.jobId < 0 ||
        (!sha.empty() && ((sha.size() != 40 && sha.size() != 64) ||
                          sha.find_first_not_of("0123456789abcdef") != sha.npos)) ||
        std::ranges::any_of(event.prNumbers,[](Id number) { return number <= 0; }) ||
        (event.prNumbers.empty() && sha.empty()))
        throw std::invalid_argument("Invalid Git webhook event");
    const Json data{{"kind",event.kind},{"action",event.action},{"prNumbers",event.prNumbers},
        {"headSha",sha},{"summary",event.summary},{"jobId",event.jobId}};
    std::lock_guard lock(p_->mutex);
    p_->exec("INSERT OR IGNORE INTO git_webhook_events(host,event_id,repo,data,received_at) VALUES(?,?,?,?,?)",
             {repo.host,event.eventId,repo.repo,data,unixTime()});
    return sqlite3_changes(p_->db) == 1;
}

void Store::Impl::gitInboxItem(const Json& task, std::string_view key, std::string_view url,
                               std::string_view title, std::string_view prompt) {
    const auto id = task.at("id");
    if (query("SELECT 1 FROM git_inbox_items WHERE task_id=? AND dedupe=?",{id,key}).next()) return;
    const auto item = insertPrReview(task,url,title,prompt);
    exec("INSERT INTO git_inbox_items(task_id,dedupe,inbox_id) VALUES(?,?,?)",{id,key,item});
}

void Store::Impl::deliverGitWebhooks() {
    std::lock_guard lock(mutex);
    Transaction transaction(db);
    auto rows = query(R"SQL(
SELECT e.id,e.host,e.event_id,e.repo,e.data,w.pr_number,w.task_id,w.bot_id,w.run,w.url
FROM git_webhook_events e JOIN git_pr_watches w ON w.host=e.host AND w.repo=e.repo
WHERE NOT EXISTS (SELECT 1 FROM git_webhook_deliveries d WHERE d.event=e.id AND d.pr_number=w.pr_number)
AND (EXISTS (SELECT 1 FROM json_each(e.data,'$.prNumbers') p WHERE p.value=w.pr_number)
 OR (json_array_length(e.data,'$.prNumbers')=0 AND w.head_sha<>'' AND w.head_sha=json_extract(e.data,'$.headSha')
 AND (SELECT count(*) FROM git_pr_watches sibling
      WHERE sibling.host=w.host AND sibling.repo=w.repo AND sibling.head_sha=w.head_sha)=1))
ORDER BY e.id,w.pr_number LIMIT 100
)SQL");
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
        const auto repo = GitRepository::parse(field(event,"host"),field(event,"repo"));
        const auto taskId = event.at("taskId").get<Id>();
        const auto task = load(taskId);
        const auto bot = field(task,"botId");
        if (bot != field(event,"botId") || task.at("state") == "cancelled" || task.at("state") == "cancelling" ||
            !Access(*this).permits(bot,{"repository",repo.resource(),"read"},true)) continue;
        event.erase("botId");
        event.erase("deliveryId");
        const auto kind = field(event,"kind");
        const auto action = field(event,"action");
        if (kind == "pull_request" && !field(event,"headSha").empty())
            exec("UPDATE git_pr_watches SET head_sha=? WHERE host=? AND repo=? AND pr_number=?",
                 {event.at("headSha"),repo.host,repo.repo,pr});
        const auto scrub = redactor(bot);
        event["summary"] = scrub.text(field(event,"summary"));
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
             {taskId,unixTime(),event,bot});
        const auto message = last();
        exec(R"SQL(
INSERT INTO messages(id,bot_id,client_message_id,sequence,reply_to,text,routing,person_id,person_root)
SELECT ?,?,?,coalesce(max(sequence),0)+1,NULL,'',?,'git-webhook',0 FROM messages WHERE bot_id=?
)SQL",{message,bot,key,routing,bot});
    }
    transaction.commit();
}
}
