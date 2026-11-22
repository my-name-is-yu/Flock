#include "flock/web.hpp"
#include "../../tests/fake_git.hpp"

#include <httplib.h>
#include <openssl/hmac.h>

#include <iostream>

using namespace git_fixture;

namespace {
constexpr auto webhookPath = "/api/git/webhooks/github.com";
constexpr auto key = "fixture-signature-key";

std::string sign(std::string_view body) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    const std::string_view secret(key);
    const auto success = HMAC(EVP_sha256(),secret.data(),static_cast<int>(secret.size()),
                              reinterpret_cast<const unsigned char*>(body.data()),body.size(),digest,&size);
    check(success,"could not sign webhook fixture");
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
    Decision decide(const ModelInput& input) override {
        const auto& event = input.event;
        if (event.at("kind") == "intake")
            return {"delegate",{{"instruction","Push the change"},{"doneWhen","Open a pull request"}},"Implement the change",{}};
        if (event.at("kind") == "result") {
            if (!event.contains("gitResults")) return {"git_open_pr",openArgs(),"Publish the Session change",{}};
            return {"complete",{{"report","Opened pull request"}},"Report the published PR",{}};
        }
        const auto& delivery = event.at("gitEvent");
        if (event.at("turn").empty()) {
            ++received;
            if (delivery.at("jobId") == 81) return {"git_read_ci_logs",
                {{"host",delivery.at("host")},{"repo",delivery.at("repo")},{"jobId",81},{"maxBytes",32}},
                "Inspect the failed job",{}};
            return {"reply",{{"text","Review received"}},"Relay a notification",{}};
        }
        const auto& log = event.at("turn").at(0).at("result").at("data");
        return {"reply",{{"text","CI failed. " + log.at("text").get<std::string>()}},"Report the exact CI failure",{}};
    }
};

WebCallbacks callbacks() {
    return {.bots=[] { return Json::array({{{"id","owner"},{"name","Release Bot"}}}); },
            .intake=[](std::string_view bot, std::string_view) { return intake(std::string(bot)); },
            .createBot={}, .updateBot={}, .botAvatar={},
            .authenticate=[](std::string_view header) { return header == "Basic fixture" ? localOwner : Person{}; },
            .githubWebhookSecret=key};
}

Json post(httplib::Client& client, std::string_view type, std::string_view id, const Json& payload,
          int status, std::string signature = {}) {
    const auto body = payload.dump();
    httplib::Headers headers{{"X-GitHub-Event",std::string(type)},{"X-GitHub-Delivery",std::string(id)},
                             {"X-Hub-Signature-256",signature.empty() ? sign(body) : std::move(signature)}};
    const auto response = client.Post(webhookPath,headers,body,"application/json");
    if (!response || response->status != status)
        throw std::runtime_error("Unexpected HTTP status for Git webhook: " + (response ? response->body : "no response"));
    return Json::parse(response->body);
}

void run() {
    Directory directory;
    const auto database = directory.path/"state.db";
    auto host = std::make_shared<FakeHost>();
    Sessions sessions;
    Bot model;
    Id task;
    {
        Store store(database);
        grant(store,"owner","environment","sandbox","launch");
        grant(store,"owner","repository",repoGrant,"pr");
        grant(store,"owner","repository",repoGrant,"read");
        task = store.submit(intake());
        Runtime runtime(store,model,sessions,[](std::string_view) { return BotNotes{}; },options(host));
        for (int i = 0; i < 8; ++i) runtime.tick();
        check(store.task(task).at("state") == "completed" && host->createCalls == 1,"initial watch was not recorded");
    }
    Store store(database);
    const auto server = startWeb(store,0,callbacks());
    httplib::Client client("127.0.0.1",static_cast<int>(server->port()));
    const auto privateEndpoint = client.Get("/api/inbox");
    check(privateEndpoint && privateEndpoint->status == 401,"ordinary Web access became public");
    const Json repository{{"full_name","ORG/Flock"},{"id",41}};
    const Json payload{{"action","completed"},{"repository",repository},
        {"workflow_job",{{"id",81},{"name","Build"},{"status","completed"},{"conclusion","failure"},{"head_sha",sha}}}};
    check(post(client,"workflow_job","job-run-1",payload,401,"sha256=wrong").at("error") == "invalid_webhook_signature",
          "unsigned webhook was accepted");
    check(post(client,"workflow_job","job-run-1",payload,202).at("status") == "received","signed webhook was not enqueued");
    check(post(client,"workflow_job","job-run-1",payload,202).at("status") == "duplicate","retry was not deduplicated");
    check(post(client,"workflow_job","job-run-redelivery",payload,202).at("status") == "duplicate",
          "An existing signed payload was handled twice");
    const Json unrelatedCheck{{"action","completed"},{"repository",repository},
        {"check_run",{{"head_sha",sha},{"conclusion","failure"},
                      {"pull_requests",Json::array({{{"number",1},{"base",{{"repo",{{"id",42}}}}}}})}}}};
    check(post(client,"check_run","foreign-repo",unrelatedCheck,202).at("status") == "ignored",
          "PR number from another repository was routed to this watch");
    check(historyCount(store,task,"git_webhook") == 0,"HTTP intake delivered a webhook");
    Runtime runtime(store,model,sessions,[](std::string_view) { return BotNotes{}; },options(host));
    runtime.runConversations();
    check(model.received == 1 && host->logCalls == 1 &&
          store.history(task).dump().find("CI failed. linker: missing symbol") != std::string::npos,
          "authenticated webhook did not wake the persisted task and produce a CI report");
    const std::string newSha(40,'b');
    auto nextJob = payload;
    nextJob["workflow_job"]["head_sha"] = newSha;
    check(post(client,"workflow_job","job-new-head",nextJob,202).at("status") == "received","New CI job was not saved");
    runtime.runConversations();
    check(model.received == 1,"A CI job with no verified watch was delivered");
    const Json sync{{"action","synchronize"},{"repository",repository},
        {"before",sha},{"after",newSha},{"pull_request",{{"number",1},{"head",{{"sha",newSha}}},{"title","Change"}}}};
    check(post(client,"pull_request","pr-new-head",sync,202).at("status") == "received","PR synchronization was not saved");
    runtime.runConversations();
    check(model.received == 3 && host->logCalls == 2,"Out-of-order PR synchronization did not route the new CI job");

    const Json review{{"action","submitted"},{"repository",repository},
        {"pull_request",{{"number",1},{"head",{{"sha",sha}}},{"title","Change"}}},
        {"review",{{"state","approved"},{"body","Review complete"}}}};
    check(post(client,"pull_request_review","review-1",review,202).at("status") == "received","review not enqueued");
    const Json scan{{"action","appeared"},{"repository",repository},{"ref","refs/pull/1/merge"},
        {"alert",{{"html_url","https://github.com/org/flock/security/code-scanning/1"},
                  {"rule",{{"description","Rule result"}}}}}};
    check(post(client,"code_scanning_alert","scan-1",scan,202).at("status") == "received","scan not enqueued");
    runtime.runConversations();
    const auto inbox = client.Get("/api/inbox?bot=owner",httplib::Headers{{"Authorization","Basic fixture"}});
    check(inbox && inbox->status == 200 && inbox->body.find("Pull request review submitted") != std::string::npos &&
          inbox->body.find("Code scanning result") != std::string::npos,"review or scan missing in the Web inbox");
}
}

int main() {
    try { run(); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
