#include "http_fixture.hpp"
#include "flock/memory_automation.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>

using namespace flockv0;
using namespace flockv0::http_test;
namespace {

void pageAssets(Browser& browser, const std::string& html) {
    const std::regex tags(R"(<(script|link)\b[^>]*>)");
    const std::regex source(R"(\ssrc\s*=\s*["']([^"']+)["'])");
    const std::regex href(R"(\shref\s*=\s*["']([^"']+)["'])");
    const std::regex stylesheet(R"(\srel\s*=\s*["']stylesheet["'])");
    bool hasScript = false, hasStylesheet = false;
    for (auto tag = std::sregex_iterator(html.begin(),html.end(),tags); tag != std::sregex_iterator(); ++tag) {
        const auto markup = tag->str();
        const bool script = (*tag)[1] == "script";
        if (!script && !std::regex_search(markup,stylesheet)) continue;
        std::smatch reference;
        if (!std::regex_search(markup,reference,script ? source : href)) continue;
        auto path = reference[1].str();
        if (!path.starts_with('/')) path.insert(0,"/");
        const auto asset = browser.client.Get(path);
        check(asset && asset->status == 200 && !asset->body.empty(),"missing page asset: " + path);
        const auto contentType = asset->get_header_value("Content-Type");
        const auto mediaType = contentType.substr(0,contentType.find(';'));
        check(script ? mediaType == "text/javascript" || mediaType == "application/javascript" : mediaType == "text/css",
              "incorrect media type for page asset: " + path);
        if (script) hasScript = true;
        else hasStylesheet = true;
    }
    check(hasScript && hasStylesheet,"landing page does not load its script and stylesheet");
}

void boundary(Browser& browser) {
    auto landing = browser.client.Get("/");
    check(landing && landing->status == 200 && !landing->body.empty() &&
          landing->get_header_value("Content-Type").starts_with("text/html"),"Flock did not serve its landing page");
    check(landing->get_header_value("X-Frame-Options") == "DENY" &&
          landing->get_header_value("Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos,
          "missing frame protection");
    pageAssets(browser,landing->body);
    for (const auto* path : {"/event-history.mjs","/live-updates.js","/avatars.js","/bot-editor.js","/composer-drafts.mjs","/flock.svg",
                            "/vendor/open-bot-avatar/index.js","/vendor/open-bot-avatar/dom/index.js"}) {
        auto asset = browser.client.Get(path);
        check(asset && asset->status == 200 && !asset->body.empty(),"missing embedded browser asset");
    }
    const auto draft = R"({"botId":"release","request":"do not accept this"})";
    Browser::checked(browser.client.Get("/api/bots",httplib::Headers{{"Host","rebound.invalid"}}),403);
    Browser::checked(browser.client.Get("/",httplib::Headers{{"Host","rebound.invalid"}}),403);
    Browser::checked(browser.client.Get("/api/bots",httplib::Headers{{"Origin","https://other.invalid"}}),403);
    Browser::checked(browser.client.Get("/api/bots",httplib::Headers{{"Sec-Fetch-Site","same-site"}}),403);
    Browser::checked(browser.client.Post("/api/requests",draft,"application/json"),403);
    Browser::checked(browser.client.Post("/api/requests",{{"Origin",browser.origin}},draft,"application/json"),403);
    Browser::checked(browser.client.Post("/api/requests",{{"Origin","null"},{"X-Flock-Action","1"}},draft,"application/json"),403);
    Browser::checked(browser.client.Post("/api/requests",browser.headers(),draft,"text/plain"),415);
    Browser::checked(browser.client.Post("/api/requests",browser.headers(),"{","application/json"),400);
    Browser::checked(browser.client.Post("/api/requests",browser.headers(),std::string(140000,'x'),"application/json"),413);
    const auto preflight = browser.client.Options("/api/requests",{{"Origin","https://other.invalid"}});
    check(preflight && preflight->status == 403 && !preflight->has_header("Access-Control-Allow-Origin"),"cross-origin preflight was permitted");
    Browser::checked(browser.client.Get("/api/conversation?bot=release&before=-1"),400);
    Browser::checked(browser.client.Get("/api/view?bot=missing"),404);
    check(browser.get("/api/view?bot=release").at("composerPerson").is_null(),"local composer was given an authenticated identity");
    for (const std::string path : {"/api/compactions","/api/workflows"}) {
        check(browser.get(path+"?bot=release").is_array(),"Missing inspector collection");
        Browser::checked(browser.client.Get(path+"?bot=missing"),404);
    }
    browser.post("/api/requests",{{"botId","broken"},{"request","test"}},422);
}

void checkPresence(Browser& browser, const Json& list, const std::string& bot, std::string_view expected) {
    const auto row = std::find_if(list.begin(),list.end(),[&](const Json& candidate) {
        return candidate.at("id") == bot;
    });
    check(row != list.end(),"Bot list is missing " + bot);
    check(row->at("presence") == expected,"unexpected Bot list presence for " + bot);
    const auto view = browser.get("/api/view?bot=" + bot);
    check(view.at("presence") == expected,"unexpected Bot view presence for " + bot);
}

bool hasEvent(const Json& events, std::string_view kind, std::string_view field, std::string_view text) {
    for (const auto& event : events) {
        if (event["kind"] != kind) continue;
        if (event.at("data").value(field,std::string{}) == text) return true;
    }
    return false;
}

void conversationPages(Browser& browser, Store& cli, Id release, Id next, Id triage,
                       const std::map<Id,std::string>& earlierRequests) {
    std::map<Id,std::string> expectedRequests = {
        {release,"下書きの掲載範囲を確認する"},
        {next,"次の版の変更点を調べる"},
    };
    expectedRequests.insert(earlierRequests.begin(),earlierRequests.end());
    for (int i = 0; i < 43; ++i) {
        const auto text = "Paged request " + std::to_string(i);
        const auto id = cli.submit({"release",text,"local-env","release"});
        expectedRequests.emplace(id,text);
    }

    auto page = browser.get("/api/view?bot=release");
    check(page.at("olderBefore").get<Id>() > 0,"conversation did not offer older history");
    const std::map<std::string,std::pair<std::string,std::string>> expectedResults{
        {"report",{"report","T3 finished while the browser was closed"}},
        {"completed",{"text","Draft complete"}},
    };
    std::set<std::string> foundResults;
    std::set<Id> cursors;
    Id oldestSeen = 0;
    do {
        const auto& events = page.at("events");
        Id previous = 0;
        for (const auto& event : events) {
            const auto id = event.at("id").get<Id>();
            check(id > previous && (oldestSeen == 0 || id < oldestSeen),"conversation order or page boundary was incorrect");
            previous = id;
            const auto task = event.at("taskId").get<Id>();
            check(task != triage,"conversation included another Bot's history");
            const auto kind = event.at("kind").get<std::string>();
            if (const auto expected = expectedResults.find(kind); task == release && expected != expectedResults.end()) {
                check(foundResults.insert(kind).second &&
                      event.at("data").at(expected->second.first) == expected->second.second,
                      "conversation returned a wrong or repeated report or completion");
            }
            if (kind != "request") continue;
            const auto request = expectedRequests.find(task);
            const auto& data = event.at("data");
            check(request != expectedRequests.end() && data.at("text") == request->second,
                  "conversation returned an unexpected or repeated request");
            expectedRequests.erase(request);
        }
        const auto before = page.at("olderBefore").get<Id>();
        if (before == 0) break;
        check(!events.empty() && cursors.insert(before).second,"older conversation cursor did not advance");
        oldestSeen = events.front().at("id").get<Id>();
        page = browser.get("/api/conversation?bot=release&before=" + std::to_string(before));
    } while (true);
    check(expectedRequests.empty(),"conversation pagination skipped requests");
    check(foundResults.size() == expectedResults.size(),"conversation pagination skipped a report or completion");
}

void sessionPages(Browser& browser, Store& cli, Runtime& runtime, FixedT3& t3, Id triage) {
    const auto original = browser.get("/api/view?bot=triage");
    const auto viewPath = [](const Json& page) {
        return "/api/view?bot=triage&changesAfter=" + page.at("historyCursor").dump() +
            "&sessionsAfter=" + page.at("sessionCursor").dump();
    };
    std::set<Id> expectedTasks = {triage};
    Id failed = 0, historical = 0;
    for (int i = 0; i < 8; ++i) {
        const auto id = std::to_string(i);
        const auto task = cli.submit({"triage","Session " + id,"linked env","separate-" + id});
        expectedTasks.insert(task);
        if (i == 0) failed = task;
        if (i == 1) historical = task;
    }
    const auto allRunsStarted = [&] {
        return std::ranges::all_of(expectedTasks,[&](Id id) { return cli.task(id).at("runs").size() == 1; });
    };
    for (int attempt = 0; attempt < 20 && !allRunsStarted(); ++attempt) runtime.tick();
    check(allRunsStarted(),"session fixtures did not each start one Run");

    const auto updates = browser.get(viewPath(original));
    check(updates.contains("historyCursor") && updates.contains("changedMessageIds") && updates.contains("sessionCursor"),
          "a view refresh did not return both history cursors");
    auto expectedAdded = expectedTasks;
    expectedAdded.erase(triage);
    for (const auto& task : updates.at("sessionTasks"))
        check(expectedAdded.erase(task.at("id").get<Id>()) == 1,"refresh returned an unexpected Session task");
    check(expectedAdded.empty(),"refresh missed new Sessions outside the newest page");
    check(browser.get(viewPath(updates)).at("sessionTasks").empty(),
          "refresh returned unchanged Session history");

    t3.observations["thread-separate-0"] = {.kind=Observation::Kind::Finished,.status="failed",
        .report="The health check failed",.resultRunId="distinct-result-run",.settled=true};
    t3.observations["thread-separate-1"] = {.kind=Observation::Kind::Finished,
        .report="Historical report without a status",.settled=true};
    tick(runtime,3);
    const auto settled = browser.get(viewPath(updates));
    const auto& settledTasks = settled.at("sessionTasks");
    const std::map<Id,std::string_view> states = {{failed,"failed"},{historical,"ended"}};
    check(settled.at("sessionCursor") > updates.at("sessionCursor") && settledTasks.size() == states.size(),
          "settled Runs did not advance the Session cursor");
    for (const auto& task : settledTasks)
        check(task.at("runCount") == 1 && task.at("sessions").at(0).at("state") == states.at(task.at("id").get<Id>()),
              "a settled Run did not refresh its Session status with the same run count");

    auto page = browser.get("/api/view?bot=triage");
    check(page.at("sessionsBefore").get<Id>() > 0,"session list did not offer older history");
    std::set<Id> cursors;
    do {
        const auto& tasks = page.at("sessionTasks");
        check(!tasks.empty(),"session page contained no tasks");
        for (const auto& task : tasks) {
            const auto id = task.at("id").get<Id>();
            check(expectedTasks.erase(id) == 1,"session page returned an unexpected or repeated task");
            check(!task.at("sessions").empty(),"session task contained no links to work");
            if (id == triage || id == failed || id == historical) {
                const auto expected = id == triage ? "completed" : id == failed ? "failed" : "ended";
                check(task.at("sessions").at(0).at("state") == expected,"Session history lost its terminal state");
            }
        }
        const auto before = page.at("sessionsBefore").get<Id>();
        if (before == 0) break;
        check(cursors.insert(before).second,"session cursor did not advance");
        page = browser.get("/api/sessions?bot=triage&before=" + std::to_string(before));
    } while (true);
    check(expectedTasks.empty(),"session pagination skipped tasks");
}

void failedCompletion(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    t3.observations["thread-release"] = {.kind=Observation::Kind::Finished,.status="failed",
        .report="The health check failed",.resultRunId="failed-result",.itemId="failed-report",.settled=true};
    store.submit({"release","Verify staging","local-env","release"});
    tick(runtime,8);

    const auto server = startWeb(store,0,callbacks());
    Browser browser(*server);
    const auto events = browser.get("/api/conversation?bot=release").at("events");
    check(hasEvent(events,"completed","runStatus","failed"),"the final Bot message lost its failed Session outcome");
}

struct PeerModel final : Model {
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        if (input.event["kind"] == "intake")
            return {"send_bot_message",{{"botId","triage"},{"kind","consult"},{"text","判定基準を教えて"}},
                "Triage owns the criteria"};
        if (input.event["kind"] == "bot_request")
            return {"reply_to_bot",{{"kind","result"},{"text","再現条件を確認してください"}},
                "The requested criteria are already known"};
        check(input.event["kind"] == "bot_result","unexpected peer history fixture event");
        return {"complete",{{"report","判定基準を確認しました"}},"Triage answered the request"};
    }
};

std::vector<PeerBot> fixturePeers(std::string_view sender) {
    if (sender != "release") return {};
    return {{"Triage",{"triage","","local-env","release"}}};
}

Id seedPeerHistory(Browser& browser, Runtime& runtime, const FixedT3& t3) {
    const auto task = browser.request("release","判定基準の確認");
    Json waiting;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        runtime.tick();
        waiting = browser.get("/api/view?bot=release");
        if (waiting.at("presence") == "peer_waiting") break;
    }
    check(waiting.at("waits").empty() && waiting.at("presence") == "peer_waiting",
          "waiting on a peer was presented as a user action");
    tick(runtime,8);
    const auto finished = browser.get("/api/view?bot=release");
    check(hasEvent(finished.at("events"),"completed","text","判定基準を確認しました") && t3.launches.empty(),
          "consultation did not show the result without starting a T3 Session");
    return task;
}

void peerHistory(const std::filesystem::path& path) {
    Store store(path);
    PeerModel model;
    FixedT3 t3;
    RuntimeOptions options;
    options.peers = fixturePeers;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },options);
    const auto server = startWeb(store,0,callbacks());
    Browser browser(*server);
    seedPeerHistory(browser,runtime,t3);

    for (int i = 0; i < 2001; ++i) store.submit({"release","Newer request " + std::to_string(i),"local-env","release"});
    const auto newest = browser.get("/api/view?bot=release");
    auto cursor = newest.at("olderBefore").get<Id>();
    check(cursor > 0,"missing older peer history cursor");
    check(newest.at("queued").size() == 2000,"overview lost its pending queue");
    const auto blocker = newest.at("queued").front().at("blockedBy").at("id");
    check(std::all_of(newest.at("queued").begin(),newest.at("queued").end(),[&](const Json& row) {
        return row.at("blockedBy").at("id") == blocker;
    }),"overview reported a different head for the same project");
    const auto recipient = browser.get("/api/view?bot=triage");
    const auto peerMessages = [](const Json& page) {
        Json messages = Json::array();
        for (const auto& event : page.at("events"))
            if (event.at("kind") == "bot_message") messages.push_back(event.at("data"));
        return messages;
    };
    auto senderExchange = peerMessages(newest);
    while (cursor > 0) {
        const auto older = browser.get("/api/conversation?bot=release&before=" + std::to_string(cursor));
        const auto messages = peerMessages(older);
        senderExchange.insert(senderExchange.begin(),messages.begin(),messages.end());
        const auto next = older.at("olderBefore").get<Id>();
        check(0 <= next && next < cursor,"older peer history cursor did not advance");
        cursor = next;
    }
    const auto recipientExchange = peerMessages(recipient);
    const auto checkExchange = [](const Json& exchange) {
        check(exchange.size() == 2,"peer history must show one request and one reply");
        const auto& request = exchange.at(0);
        const auto& reply = exchange.at(1);
        check(request.at("messageType") == "request" && request.at("requestKind") == "consult" &&
              request.at("fromBotId") == "release" && request.at("toBotId") == "triage" &&
              request.at("text") == "判定基準を教えて","peer history lost the consultation request");
        check(reply.at("messageType") == "result" && reply.at("fromBotId") == "triage" &&
              reply.at("toBotId") == "release" && reply.at("text") == "再現条件を確認してください",
              "peer history lost the consultation reply");
        check(!request.at("exchangeId").get<std::string>().empty() &&
              !request.at("messageId").get<std::string>().empty() &&
              request.at("messageId") != reply.at("messageId") &&
              request.at("exchangeId") == reply.at("exchangeId") && reply.at("replyTo") == request.at("messageId"),
              "peer history lost its message or thread identity");
    };
    checkExchange(senderExchange);
    checkExchange(recipientExchange);
    check(senderExchange.at(0).at("exchangeId") == recipientExchange.at(0).at("exchangeId") &&
          senderExchange.at(0).at("messageId") == recipientExchange.at(0).at("messageId") &&
          senderExchange.at(1).at("messageId") == recipientExchange.at(1).at("messageId"),
          "sender and recipient see different message identities");
    check(peerMessages(browser.get("/api/view?bot=settings")).empty(),
          "peer history appeared in a third Bot's conversation");
}

void peerConversation(const std::filesystem::path& path) {
    constexpr auto primary = "/api/peer-conversation?bot=release&peer=triage";
    constexpr auto reverse = "/api/peer-conversation?bot=triage&peer=release";
    constexpr auto separate = "/api/peer-conversation?bot=release&peer=settings";
    std::atomic_bool allowed = true, configured = true;
    RuntimeOptions options;
    options.peers = [&](std::string_view sender) -> std::vector<PeerBot> {
        if (sender != "release") return {};
        auto peers = allowed ? fixturePeers(sender) : std::vector<PeerBot>{};
        peers.push_back({"Settings",{"settings","","local-env","settings"}});
        return peers;
    };
    auto web = callbacks();
    web.bots = [all = web.bots,&configured] {
        auto bots = all();
        if (!configured) {
            const auto archived = std::ranges::find_if(bots,[](const Json& bot) { return bot.at("id") == "triage"; });
            bots.erase(archived);
        }
        return bots;
    };
    web.peers = options.peers;
    PeerModel model;
    FixedT3 t3;
    Json history;
    {
        Store store(path);
        Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },options);
        const auto server = startWeb(store,0,web);
        Browser browser(*server);
        const auto empty = browser.get(primary);
        check(empty.at("messages").empty() && empty.at("hasOlder") == false &&
              !empty.contains("canSend") && browser.get(reverse) == empty,
              "an unused permitted pair did not open as the same read-only history");
        browser.post("/api/peer-messages",{{"botId","release"},{"peerId","triage"},
            {"clientMessageId","direct-send"},{"text","Send directly to the peer"}},404);
        runtime.tick();
        check(store.tasks().empty() && t3.launches.empty(),"opening a DM or posting to the old route started work");
        Browser::checked(browser.client.Get("/api/peer-conversation?bot=triage&peer=settings"),403);

        seedPeerHistory(browser,runtime,t3);
        history = browser.get(primary).at("messages");
        check(history.size() == 2 && history == browser.get(reverse).at("messages"),
              "an exchange was missing, duplicated, or different from the opposite Bot page");
        check(history.at(0).at("data").at("text") == "判定基準を教えて" &&
              history.at(1).at("data").at("text") == "再現条件を確認してください" &&
              history.at(0).at("id").get<Id>() < history.at(1).at("id").get<Id>(),
              "the conversation lost its messages or order");
        check(browser.get(separate).at("messages").empty(),"an exchange leaked to another allowed pair");
        const auto before = std::to_string(history.back().at("id").get<Id>());
        check(browser.get(std::string(reverse) + "&before=" + before).at("messages") == Json::array({history.front()}),
              "the peer history cursor repeated the anchor or skipped older messages");

        const auto checkProjection = [](const Json& messages) {
            for (const auto& message : messages) {
                if (message.at("kind") != "bot_message") continue;
                check(!message.contains("taskId") && !message.contains("taskBotId") &&
                      !message.at("data").contains("evidence"),"peer chat leaked internal task information");
            }
        };
        checkProjection(history);
        checkProjection(browser.get("/api/view?bot=release").at("events"));
        checkProjection(browser.get("/api/view?bot=triage").at("events"));
    }
    {
        Store store(path);
        const auto server = startWeb(store,0,web);
        Browser browser(*server);
        check(browser.get(primary).at("messages") == history,"restarting lost the conversation");
        allowed = false;
        check(browser.get(reverse).at("messages") == history,"removing a grant lost existing history");
        Browser::checked(browser.client.Get("/api/peer-conversation?bot=release&peer=broken"),403);
        Browser::checked(browser.client.Get("/api/peer-conversation?bot=release&peer=unknown"),404);
        const auto bots = browser.get("/api/bots");
        const auto triage = std::ranges::find_if(bots,[](const Json& bot) { return bot.at("id") == "triage"; });
        check(triage != bots.end() && triage->at("peers") == Json::array({Json{{"id","release"}}}),
              "the recipient lost its historical partner after the grant was removed");
        configured = false;
        check(browser.get(primary).at("messages") == history,"removing a peer's configuration lost history");
        const auto beforeFirst = std::to_string(history.front().at("id").get<Id>());
        check(browser.get(std::string(primary) + "&before=" + beforeFirst) ==
              Json{{"messages",Json::array()},{"hasOlder",false}},"an empty page of archived history could not be read");
        const auto list = browser.get("/api/bots");
        const auto release = std::ranges::find_if(list,[](const Json& bot) { return bot.at("id") == "release"; });
        check(release != list.end() && std::ranges::any_of(release->at("peers"),[](const Json& peer) {
            return peer == Json{{"id","triage"}};
        }),"the picker lost the archived conversation or exposed a send action");
    }
}

void peerConversationAnchor(const std::filesystem::path& path) {
    Store store(path);
    PeerModel model;
    FixedT3 t3;
    RuntimeOptions options;
    options.peers = fixturePeers;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },options);
    for (int i = 0; i < 36; ++i) {
        store.submit({"release","Check release criteria","local-env","release"});
        tick(runtime,8);
    }

    auto web = callbacks();
    web.peers = [](std::string_view bot) {
        auto peers = fixturePeers(bot);
        if (bot == "release") peers.push_back({"Settings",{"settings","","local-env","settings"}});
        return peers;
    };
    web.authenticate = [](std::string_view credential) {
        return credential == "Bearer reader" ? Person{"reader"} : Person{};
    };
    const Person owner{"fixture",true};
    auto access = store.policy();
    access.setMember(owner,"release","reader",true);
    access.setMember(owner,"settings","reader",true);
    const auto server = startWeb(store,0,web);
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization","Bearer reader"}});

    const auto recipient = store.conversation("triage",0,200);
    const auto sender = store.peerConversation("release","triage",0,200);
    check(sender.size() == 72,"historical DM fixture did not span multiple pages");
    const auto& target = sender.at(24);
    const auto targetId = target.at("id").get<Id>();
    const auto copy = std::ranges::find_if(recipient,[&](const Json& event) {
        return event.at("kind") == "bot_message" &&
            event.at("data").at("messageId") == target.at("data").at("messageId");
    });
    check(copy != recipient.end() && copy->at("id") != targetId,"fixture has no recipient copy of the anchor");

    const std::string primary = "/api/peer-conversation?bot=release&peer=triage";
    const std::string reverse = "/api/peer-conversation?bot=triage&peer=release";
    const auto anchor = "&anchor=" + std::to_string(targetId);
    Browser::checked(browser.client.Get(primary + anchor),403);
    access.setMember(owner,"triage","reader",true);

    const auto latest = browser.get(primary);
    check(latest.at("messages").front().at("id").get<Id>() > targetId,"fixture anchor appeared in the latest page");
    const auto page = browser.get(primary + anchor);
    const auto& messages = page.at("messages");
    check(page.at("anchorId") == targetId && page.at("hasOlder") == true && messages.size() == 36 &&
        messages.at(18).at("id") == targetId &&
        messages.at(19).at("data").at("replyTo") == messages.at(18).at("data").at("messageId"),
        "the bounded historical page missed the precise message or its reply");
    for (std::size_t i = 0; i < messages.size(); ++i)
        check(messages.at(i).at("id") == sender.at(i + 6).at("id"),"historical pages skipped or reordered messages");
    const auto reverseAnchor = "&anchor=" + std::to_string(copy->at("id").get<Id>());
    check(browser.get(reverse + reverseAnchor) == page,"the recipient anchor did not resolve to the same shared DM page");
    const auto before = "&before=" + std::to_string(messages.front().at("id").get<Id>());
    const auto older = browser.get(primary + before);
    check(older.at("messages").size() == 6 && older.at("hasOlder") == false,
        "earlier pagination repeated or skipped messages around the anchor");
    Browser::checked(browser.client.Get(primary + anchor + before),400);
    Browser::checked(browser.client.Get(primary + "&anchor=-1"),400);
    check(Browser::checked(browser.client.Get("/api/peer-conversation?bot=release&peer=settings" + anchor),404)
        == Json{{"error","peer_message_not_found"}},"a different readable pair resolved the anchor");
}

void exercise(const std::filesystem::path& path) {
    Store store(path);
    Store cli(path);
    FixedModel model;
    FixedT3 t3;
    const auto notes = [](std::string_view) { return std::string{}; };
    auto runtime = std::make_unique<Runtime>(store,model,t3,notes);
    auto server = startWeb(store,0,callbacks());
    const auto port = server->port();
    Browser browser(*server);
    boundary(browser);
    check(cli.tasks().empty(),"rejected HTTP requests still submitted work");
    const auto release = browser.request("release","下書きの掲載範囲を確認する");
    const auto triage = browser.request("triage","昨日届いた不具合を確認する");
    const auto settings = browser.request("settings","接続設定を確認する");
    tick(*runtime,3);
    t3.observations["thread-release"].kind = Observation::Kind::Question;
    t3.observations["thread-release"].question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})};
    t3.observations["thread-triage"].kind = Observation::Kind::ExternalInput;
    t3.observations["thread-triage"].status = "権限の承認が必要";
    tick(*runtime,2);

    const auto releasePage = browser.get("/api/view?bot=release");
    const auto triagePage = browser.get("/api/view?bot=triage");
    auto waits = releasePage.at("waits");
    check(waits.size() == 1 && waits[0]["id"] == release &&
          waits[0]["wait"]["text"] == "利用者向けの変更だけを載せますか？" &&
          waits[0]["wait"]["choices"] == Json::array({"利用者向けのみ","すべて"}),
          "release Bot question or available answers were incorrect");
    check(triagePage["waits"].size() == 1 && triagePage["waits"][0]["id"] == triage,"T3 wait belongs to wrong Bot");
    check(releasePage["bot"]["soul"] == "利用者向けのリリースノートをまとめる。" &&
          triagePage["bot"]["soul"] == "報告された問題を整理する。" && triagePage["bot"]["cron"].empty(),
          "selected Bots showed incorrect settings");
    const auto& schedule = releasePage.at("bot").at("cron").at(0);
    check(schedule == Json{{"id","daily"},{"schedule","0 9 * * *"},{"timezone","Asia/Tokyo"},
          {"request","前日の変更を確認する"},{"active",false}},
          "Release's scheduled request was incorrect");
    check(releasePage["sessionTasks"][0]["sessions"][0]["link"] == "https://app.t3.codes/threads/local-env/thread-release" &&
          triagePage["waits"][0]["wait"]["run"]["link"] == "http://localhost:3773/linked%20env/thread-triage" &&
          triagePage["sessionTasks"][0]["sessions"][0]["link"] == "http://localhost:3773/linked%20env/thread-triage",
          "T3 links did not preserve environment and Thread ownership");
    const auto settingsPage = browser.get("/api/view?bot=settings");
    check(settingsPage["waits"][0]["id"] == settings && settingsPage["waits"][0]["wait"]["reason"] == "settings",
          "configuration failure was not classified");
    const auto list = browser.get("/api/bots");
    const auto releaseRow = std::find_if(list.begin(),list.end(),[](const Json& bot) { return bot.at("id") == "release"; });
    check(releaseRow != list.end() && releaseRow->at("preview") == "利用者向けの変更だけを載せますか？",
          "Bot list does not show Release's latest question");
    check(releaseRow->at("latestIncomingEventId") == waits[0]["wait"]["messageId"],
          "a Bot question did not update the incoming event ID");
    const Id waitId = waits[0]["wait"]["id"];
    std::map<Id,std::string> earlierRequests;
    for (int i = 0; i < 45; ++i) {
        const auto text = "Earlier request " + std::to_string(i);
        earlierRequests.emplace(cli.submit({"release",text,"local-env","release"}),text);
    }
    const auto beforeAnswer = browser.get("/api/view?bot=release");
    check(std::none_of(beforeAnswer["events"].begin(),beforeAnswer["events"].end(),[&](const Json& row) {
        return row["kind"] == "waiting" && row["data"]["id"] == waitId;
    }),"question fixture did not cross a history page boundary");

    const auto next = browser.post("/api/requests",{{"botId","release"},{"request","次の版の変更点を調べる"},
        {"environmentId","ignored"},{"projectId","ignored"},{"trigger",{{"triggerKind","ignored"}}}},201).at("taskId").get<Id>();
    const auto afterSubmit = browser.get("/api/view?bot=release");
    check(afterSubmit["waits"][0]["wait"]["id"] == waitId && afterSubmit["queued"][0]["blockedBy"]["id"] == release,
          "new request changed the original wait or lost the queue blocker");
    checkPresence(browser,browser.get("/api/bots"),"release","waiting");
    check(cli.task(next)["environmentId"] == "local-env" && !cli.task(next).contains("trigger"),"client changed routing or schedule");
    const auto& visibleWait = afterSubmit.at("waits").at(0);
    Json reply = {{"botId","triage"},{"taskId",release},{"waitId",waitId},{"operation","answer"},{"text","すべて"},
                  {"targetRevision",visibleWait.at("currentRevision")},{"targetGeneration",visibleWait.at("generation")}};
    browser.post("/api/operations",reply,404);
    reply["botId"] = "release";
    cli.operate(localOwner,TaskTarget::from(cli.task(release),waitId),"answer",{{"text","利用者向けのみ"}});
    const auto answered = browser.get("/api/view?bot=release");
    check(std::any_of(answered["events"].begin(),answered["events"].end(),[&](const Json& row) {
        return row["kind"] == "user_answer" && row["taskId"] == release &&
               row["data"]["question"] == "利用者向けの変更だけを載せますか？";
    }),"the answer did not carry the question from the older page");
    browser.post("/api/operations",reply,409);
    tick(*runtime,2);
    check(t3.replies.size() == 1 && t3.replies[0].target.threadId == "thread-release" &&
          t3.replies[0].target.runId == "run-release-1" && t3.replies[0].questionId == "scope-question" &&
          t3.replies[0].answers["scope"] == "利用者向けのみ" && cli.task(next)["state"] == "queued",
          "stale reply affected the active Run or another task");
    check(browser.get("/api/view?bot=release")["waits"].empty(),"resolved wait stayed selectable");
    const auto resumed = browser.get("/api/bots");
    checkPresence(browser,resumed,"release","working");
    checkPresence(browser,resumed,"triage","waiting");

    server.reset();
    runtime.reset();
    auto& finished = t3.observations["thread-release"];
    finished.kind = Observation::Kind::Finished;
    finished.report = "T3 finished while the browser was closed";
    finished.status = "completed";
    finished.settled = true;
    runtime = std::make_unique<Runtime>(store,model,t3,notes);
    tick(*runtime,2);
    auto updatedBots = callbacks();
    updatedBots.intake = [read = updatedBots.intake](std::string_view bot, std::string_view request) {
        auto routing = read(bot,request);
        if (bot == "settings") routing.config["session"] = {{"runtimeMode","approval-required"}};
        return routing;
    };
    server = startWeb(store,port,std::move(updatedBots));
    Browser reopened(*server);
    const auto& settingsWait = settingsPage.at("waits").at(0);
    reopened.post("/api/operations",{{"botId","settings"},{"taskId",settings},
        {"waitId",settingsWait.at("wait").at("id")},{"operation","reconcile"},
        {"targetRevision",settingsWait.at("currentRevision")},{"targetGeneration",settingsWait.at("generation")},
        {"session",{{"runtimeMode","invalid-client-setting"}}}});
    tick(*runtime,2);
    check(cli.task(settings)["state"] == "running" && t3.launches["settings"] == 1,
          "Web reconciliation did not start the Session with the corrected Bot settings");
    const auto page = reopened.get("/api/view?bot=release");
    check(hasEvent(page["events"],"report","report","T3 finished while the browser was closed") &&
          hasEvent(page["events"],"completed","text","Draft complete") &&
          hasEvent(page["events"],"completed","runStatus","completed"),
          "reopened UI lost the Session result or Bot decision");
    const auto external = reopened.get("/api/view?bot=triage").at("waits").at(0);
    check(external["wait"]["run"]["link"] == "http://localhost:3773/linked%20env/thread-triage",
          "restart lost the active T3 approval link");
    const Id externalId = external["wait"]["id"];
    t3.observations["thread-triage"] = {};
    tick(*runtime,1);
    check(reopened.get("/api/view?bot=triage")["waits"].empty(),"resolved T3 wait stayed visible");
    reopened.post("/api/operations",{{"botId","triage"},{"taskId",triage},{"waitId",externalId},{"operation","cancel"},
        {"targetRevision",external.at("currentRevision")},{"targetGeneration",external.at("generation")}},409);
    check(cli.task(triage)["state"] == "running","stale cancellation affected a resumed Run");

    auto& triageQuestion = t3.observations["thread-triage"];
    triageQuestion.kind = Observation::Kind::Question;
    triageQuestion.question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})};
    tick(*runtime,2);
    const auto question = reopened.get("/api/view?bot=triage").at("waits").at(0);
    const Json answer = {{"botId","triage"},{"taskId",triage},{"waitId",question.at("wait").at("id")},
        {"operation","answer"},{"text","すべて"},{"targetRevision",question.at("currentRevision")},
        {"targetGeneration",question.at("generation")}};
    reopened.post("/api/operations",answer);
    reopened.post("/api/operations",answer,409);
    tick(*runtime,2);
    check(t3.replies.size() == 2 && t3.replies.back().target.threadId == "thread-triage" &&
          t3.replies.back().target.runId == "run-triage-1" && t3.replies.back().questionId == "scope-question" &&
          t3.replies.back().answers["scope"] == "すべて","a Web answer did not resume its own question exactly once");

    auto& completedTriage = t3.observations["thread-triage"];
    completedTriage.kind = Observation::Kind::Finished;
    completedTriage.status = "completed";
    completedTriage.report = "Triage evidence is complete";
    completedTriage.resultRunId = "run-triage-1-result";
    completedTriage.itemId = "triage-result-item";
    completedTriage.settled = true;
    tick(*runtime,3);
    const auto completedTriagePage = reopened.get("/api/view?bot=triage");
    const auto completedTriageEvent = std::find_if(completedTriagePage["events"].begin(),completedTriagePage["events"].end(),
        [&](const Json& event) { return event["kind"] == "completed" && event["taskId"] == triage; });
    check(completedTriageEvent != completedTriagePage["events"].end() &&
          (*completedTriageEvent)["data"]["run"]["link"] == "http://localhost:3773/linked%20env/thread-triage",
          "completed source lost its saved local T3 link");

    conversationPages(reopened,cli,release,next,triage,earlierRequests);
    sessionPages(reopened,cli,*runtime,t3,triage);
}

void sidebarReplies(const std::filesystem::path& path) {
    struct ReplyModel : Model {
        ModelTurn decide(const ModelInput&) override {
            return {{{"reply",{{"text","Same reply"}},"Reply to the message"}}};
        }
    } model;
    Store store(path);
    FixedT3 t3;
    Runtime runtime(store,model,t3,{});
    const auto cb = callbacks();
    const auto server = startWeb(store,0,cb);
    Browser browser(*server);
    const auto release = [&] {
        const auto bots = browser.get("/api/bots");
        const auto row = std::ranges::find_if(bots,[](const Json& bot) { return bot.at("id") == "release"; });
        check(row != bots.end(),"Release was not listed");
        return *row;
    };
    const auto send = [&](unsigned sequence) {
        store.receive(Person{"owner",true},cb.intake("release","Same request"),
                      "sidebar-" + std::to_string(sequence),"Same request");
    };
    send(0);
    check(release().at("latestIncomingEventId") == 0,"an outgoing message appeared as an incoming reply");
    tick(runtime,3);
    const auto first = release();
    const auto incoming = first.at("latestIncomingEventId").get<Id>();
    const auto events = browser.get("/api/conversation?bot=release").at("events");
    check(incoming > 0 && events.back().at("kind") == "conversation_reply" && events.back().at("id") == incoming,
          "the sidebar did not identify the visible conversation reply");
    send(1);
    check(release().at("latestIncomingEventId") == incoming,"an outgoing message advanced the incoming reply");
    tick(runtime,3);
    const auto second = release();
    check(second.at("preview") == first.at("preview") && second.at("latestIncomingEventId").get<Id>() > incoming,
          "two replies with the same text did not have distinct incoming IDs");
    for (unsigned sequence = 2; sequence < 16; ++sequence) send(sequence);
    check(release().at("latestIncomingEventId") == second.at("latestIncomingEventId"),
          "outgoing messages hid the latest incoming reply past the preview window");
}

void readChanges(Browser& stream, const std::string& path, const std::function<bool(const Json&)>& next,
                 const std::function<void()>& onPing = {}) {
    using namespace std::chrono_literals;
    stream.client.set_read_timeout(2);
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    bool ready = false;
    bool finished = false;
    std::string buffer;
    const auto result = stream.client.Get(path,[&](const httplib::Response& response) {
        return response.status == 200 && response.get_header_value("Content-Type") == "text/event-stream";
    },[&](const char* data, std::size_t size) {
        buffer.append(data,size);
        for (auto end = buffer.find("\n\n"); end != std::string::npos; end = buffer.find("\n\n")) {
            const auto frame = buffer.substr(0,end);
            buffer.erase(0,end + 2);
            if (frame.find("event: ready\n") != std::string::npos) ready = true;
            if (onPing && frame.starts_with("event: ping\n")) onPing();
            constexpr std::string_view prefix = "event: change\ndata: ";
            if (!frame.starts_with(prefix)) continue;
            check(ready,"received an invalidation before the stream was ready");
            finished = !next(Json::parse(frame.substr(prefix.size())));
            if (finished) return false;
        }
        return std::chrono::steady_clock::now() < deadline;
    });
    check(!result && ready && finished,"stream missed an invalidation");
}

void recurringRequestStatus(const std::filesystem::path& path) {
    constexpr Seconds now = 2'000'000'000;
    const Person owner{"fixture",true};
    auto web = callbacks();
    auto cards = web.bots();
    auto& cron = cards.at(0).at("cron");
    cron.push_back({{"id","invalid"},{"schedule","0 9 * * Mon"},{"timezone","Asia/Tokyo"},{"request","Invalid weekday"}});
    cron.push_back({{"id","edited"},{"schedule","0 7 * * *"},{"timezone","Etc/UTC"},{"request","Check staging"}});
    web.bots = [cards] { return cards; };
    Store store(path);
    const auto errors = store.syncSchedules("release",{
        {"daily","0 9 * * *","Asia/Tokyo",web.intake("release","前日の変更を確認する")},
        {"invalid","0 9 * * Mon","Asia/Tokyo",web.intake("release","Invalid weekday")},
        {"edited","0 8 * * *","Etc/UTC",web.intake("release","Check staging")},
    },now);
    check(errors.size() == 1 && errors.front().id == "invalid","fixture accepted an unsupported weekday");
    const auto rows = store.automations(owner,"release").at("automations");
    const auto daily = std::ranges::find_if(rows,[](const Json& row) { return row.at("configId") == "daily"; });
    check(daily != rows.end(),"fixture did not activate the configured schedule");
    for (int i = 0; i < 20; ++i) {
        store.createAutomation(owner,web.intake("release","Check staging"),{
            {"kind","reminder"},{"timezone","Etc/UTC"},{"fireAt",now + 60},{"request","Check staging"},
        },now);
    }

    const auto server = startWeb(store,0,web);
    Browser browser(*server);
    const auto requests = [&] { return browser.get("/api/view?bot=release").at("bot").at("cron"); };
    const auto configured = requests();
    check(configured.at(0).at("active") == true,"newer automations hid an active configured schedule");
    check(configured.at(1).at("active") == false && configured.at(1).at("schedule") == "0 9 * * Mon" &&
          configured.at(1).at("timezone") == "Asia/Tokyo","invalid cron lost the original configuration or appeared active");
    check(configured.at(2).at("active") == false,"an unapplied edit appeared active");

    Browser stream(*server);
    unsigned updates = 0;
    readChanges(stream,"/api/events?bot=release",[&](const Json& change) {
        check(change.at("bot") == "release","schedule update invalidated a different Bot");
        if (updates++ == 0) {
            Store(path).cancelAutomation(owner,"release",daily->at("id").get<Id>());
            return true;
        }
        check(requests().at(0).at("active") == false,"canceled schedule still appeared active");
        return false;
    });
}

void eventStream(const std::filesystem::path& path) {
    Store store(path);
    Store cli(path);
    auto web = callbacks();
    const auto server = startWeb(store,0,web);
    Browser browser(*server);
    Browser stream(*server);
    check(browser.get("/api/view?bot=release").at("canDecide") == true,"local owner lost conversation access");
    Browser::checked(browser.client.Get("/api/events?peer=triage"),400);
    Browser::checked(browser.client.Get("/api/events",httplib::Headers{{"Origin","http://example.test"}}),403);

    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    unsigned changes = 0;
    Id releaseTask = 0;
    Json botsBeforeApproval;
    readChanges(stream,"/api/events?bot=release",[&](const Json& change) {
        if (changes >= 3) {
            if (changes == 3) {
                check(change == Json{{"list",true}},"another Bot invalidated the selected view");
                const auto task = cli.task(releaseTask);
                cli.requestPrReview(releaseTask,task.value("currentRevision",Id{0}),task.at("generation"),
                                    "https://example.test/pull/1","Review the draft","Is this ready?");
            } else if (changes == 4) {
                check(change == Json{{"list",true},{"bot","release"}},"the PR review source did not invalidate its conversation");
                const auto items = browser.get("/api/inbox?bot=release").at("items");
                check(items.size() == 1 && items.at(0).at("itemKind") == "pr_review",
                      "the update did not expose the pending PR review");
                const auto events = browser.get("/api/view?bot=release").at("events");
                check(std::ranges::any_of(events,[&](const auto& event) {
                    return event.at("id") == items.at(0).at("conversationEventId") &&
                        event.at("kind") == "pr_review_requested" &&
                        event.at("data") == Json{{"itemId",items.at(0).at("itemId")},{"title","Review the draft"},
                            {"text","Is this ready?"},{"prUrl","https://example.test/pull/1"}};
                }),"the PR review did not have an exact reply source in the Web view");
                botsBeforeApproval = browser.get("/api/bots");
                auto approve = items.at(0);
                approve["decision"] = "approve";
                browser.post("/api/inbox/decide",approve);
            } else {
                check(change == Json{{"list",true}},"an inbox-only approval invalidated the selected view");
                check(browser.get("/api/bots") == botsBeforeApproval &&
                      browser.get("/api/inbox?bot=release").at("items").empty(),
                      "the Inbox-only invalidation did not remove the approved review");
                return false;
            }
            ++changes;
            return true;
        }
        check(change == Json{{"list",true},{"bot","release"}},"unexpected SSE invalidation scope or payload");
        if (changes == 0) {
            releaseTask = cli.submit(web.intake("release","CLI write"));
        } else if (changes == 1) {
            tick(runtime,3);
        } else {
            check(browser.get("/api/view?bot=release").at("presence") == "working",
                "a state-only Runtime change did not reach the view");
            cli.submit(web.intake("triage","Other Bot"));
        }
        ++changes;
        return true;
    });
}

void conversationPermissions(const std::filesystem::path& path) {
    Store store(path);
    const Person owner{"fixture",true};
    auto access = store.policy();
    access.setMember(owner,"release","reader",true);
    access.setMember(owner,"release","decider",true);
    auto web = callbacks();
    web.authenticate = [](std::string_view credential) {
        if (credential == "Bearer reader") return Person{"reader"};
        if (credential == "Bearer decider") return Person{"decider"};
        return Person{};
    };
    const Json message = {{"botId","release"},{"clientMessageId","permission-test"},{"text","Check the next release"}};
    {
        const auto server = startWeb(store,0,web);
        Browser decider(*server);
        decider.client.set_default_headers({{"Authorization","Bearer decider"}});
        check(decider.get("/api/view?bot=release").at("canDecide") == true,
              "custom authentication without a resolver prevented reads");
        decider.post("/api/messages",message,503);
        check(store.conversation("release").empty(),"a listener without a resolver queued a message");
    }
    bool deciderAvailable = true;
    web.resolveIdentity = [&](std::string_view person) -> std::optional<bool> {
        if (person == "reader" || (person == "decider" && deciderAvailable)) return false;
        return std::nullopt;
    };

    const auto taskId = store.submit(web.intake("release","Draft release notes"));
    struct ConversationModel : FixedModel {
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("kind") == "conversation_turn")
                return {{{"reply",{{"text","Ready to check the release"}},"Reply to the user"}}};
            return FixedModel::decide(input);
        }
    } model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    tick(runtime,3);
    auto& observation = t3.observations["thread-release"];
    observation.kind = Observation::Kind::Question;
    observation.question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})};
    tick(runtime,2);

    const auto server = startWeb(store,0,web);
    Browser reader(*server), decider(*server), stream(*server);
    reader.client.set_default_headers({{"Authorization","Bearer reader"}});
    stream.client.set_default_headers({{"Authorization","Bearer reader"}});
    decider.client.set_default_headers({{"Authorization","Bearer decider"}});
    const auto viewPath = "/api/view?bot=release";
    const auto readable = reader.get(viewPath);
    const auto writable = decider.get(viewPath);
    check(readable.at("canDecide") == true && writable.at("canDecide") == true,"conversation permission did not follow the caller");
    check(!readable.at("events").empty() && readable.at("events") == writable.at("events") &&
          readable.at("waits") == writable.at("waits"),"reader lost conversation or wait history");

    const auto& wait = readable.at("waits").at(0);
    const Json answer = {{"botId","release"},{"taskId",taskId},{"waitId",wait.at("wait").at("id")},
        {"operation","answer"},{"text","すべて"},{"targetRevision",wait.at("currentRevision")},
        {"targetGeneration",wait.at("generation")}};
    auto memberMessage = message;
    memberMessage["clientMessageId"] = "shared-member-message";
    reader.post("/api/messages",memberMessage,201);
    runtime.runConversations();
    access.setMember(owner,"release","reader",false);
    Browser::checked(reader.client.Get(viewPath),403);
    reader.post("/api/operations",answer,403);
    access.setMember(owner,"release","reader",true);

    decider.post("/api/operations",answer);
    const auto authorized = decider.post("/api/messages",message,201).at("messageId");
    runtime.runConversations();
    const auto assertReply = [&](const Json& id, std::string_view route) {
        const auto events = reader.get(viewPath).at("events");
        check(std::ranges::any_of(events,[&](const Json& event) {
            return event.at("kind") == "conversation_reply" && event.at("data").at("replyToMessageId") == id &&
                   event.at("data").at("route") == route;
        }),"Queued message did not use the custom identity resolver");
    };
    assertReply(authorized,"reply");

    auto queued = message;
    queued["clientMessageId"] = "removed-before-run";
    const auto revoked = decider.post("/api/messages",queued,201).at("messageId");
    deciderAvailable = false;
    runtime.runConversations();
    assertReply(revoked,"access");
}

void retryEventProjection(const std::filesystem::path& path) {
    Store store(path);
    std::atomic<bool> peersAvailable = false;
    auto web = callbacks();
    web.peers = [&](std::string_view bot) {
        if (!peersAvailable.load()) throw std::runtime_error("profiles unavailable");
        return fixturePeers(bot);
    };
    const auto server = startWeb(store,0,web);
    Browser stream(*server);
    readChanges(stream,"/api/events?bot=release",[&](const Json& change) {
        if (!peersAvailable.exchange(true)) {
            check(change == Json{{"bot","release"}},"a failed list projection blocked a healthy Bot view");
            return true;
        }
        check(change == Json{{"list",true}},"list projection did not recover independently");
        return false;
    });
}

void retryHistoryInvalidation(const std::filesystem::path& path) {
    Store store(path);
    auto web = callbacks();
    const auto task = store.submit(web.intake("release","Release notes"));
    const auto intake = web.intake("release","Follow up");
    const auto message = store.receive(Person{"owner",true},intake,"old","Follow up").at("messageId").get<Id>();
    for (int i = 0; i < 40; ++i)
        store.receive(Person{"owner",true},intake,"new-" + std::to_string(i),"Newer message");

    std::atomic<bool> projectionUnavailable = false;
    std::atomic<bool> projectionFailed = false;
    web.bots = [bots = web.bots,&projectionUnavailable,&projectionFailed] {
        if (projectionUnavailable.load()) {
            projectionFailed = true;
            throw std::runtime_error("Bot profiles unavailable");
        }
        return bots();
    };
    const auto server = startWeb(store,0,web);
    Browser browser(*server), stream(*server);
    const std::string viewPath = "/api/view?bot=release";
    const auto olderPath = "/api/conversation?bot=release&before=" + std::to_string(message + 1);
    auto baseline = browser.get(viewPath);
    const auto changesPath = viewPath + "&changesAfter=" + baseline.at("historyCursor").dump();
    baseline.erase("historyCursor");
    check(baseline.at("events").front().at("id").get<Id>() > message &&
          browser.get(olderPath).at("events").back().at("data").at("deliveredToT3") == false,
          "expected an undelivered message outside the newest view");

    bool initial = true;
    unsigned pingsDuringFailure = 0;
    readChanges(stream,"/api/events?bot=release",[&](const Json& change) {
        if (std::exchange(initial,false)) {
            check(change == Json{{"list",true},{"bot","release"}},"missing initial view scopes");
            projectionUnavailable = true;

            // Isolate the persisted effect of steering an old message from any other Runtime writes.
            sqlite3* raw = nullptr;
            const auto opened = sqlite3_open_v2(path.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
            const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
            check(opened == SQLITE_OK,"could not open the fixture writer");
            sqlite3_busy_timeout(db.get(),2000);
            const auto write = "INSERT INTO journal(task_id,time,kind,data) VALUES(" + std::to_string(task) +
                ",unixepoch(),'instruction_steered',json_object('messageId'," + std::to_string(message) + "))";
            check(sqlite3_exec(db.get(),write.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,
                  "could not record the historical delivery");
            return true;
        }
        check(!projectionUnavailable && change == Json{{"bot","release"}},
              "recovery did not invalidate only the affected Bot view");
        auto current = browser.get(viewPath);
        current.erase("historyCursor");
        check(current == baseline,"the history-only write changed the newest view");
        check(browser.get(changesPath).at("changedMessageIds") == Json::array({message}) &&
              browser.get(olderPath).at("events").back().at("data").at("deliveredToT3") == true,
              "the recovered notification did not expose the updated historical message");
        return false;
    },[&] {
        if (projectionFailed && ++pingsDuringFailure == 2) projectionUnavailable = false;
    });
}

void peerEventStream(const std::filesystem::path& path) {
    Store store(path);
    auto web = callbacks();
    web.peers = fixturePeers;
    RuntimeOptions options;
    options.peers = fixturePeers;
    PeerModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },options);
    const auto server = startWeb(store,0,web);
    Browser browser(*server);
    Browser stream(*server);
    bool initial = true;
    readChanges(stream,"/api/events?bot=release&peer=triage",[&](const Json& change) {
        const Json pair = {{"bot","release"},{"peer","triage"}};
        if (change.contains("dm")) check(change.at("dm") == pair,"the DM stream sent the wrong scope");
        if (std::exchange(initial,false)) {
            check(change == Json{{"list",true},{"bot","release"},{"dm",pair}},"missing initial DM scope");
            seedPeerHistory(browser,runtime,t3);
            return true;
        }
        return !change.contains("dm");
    });
}
void botProfileEndpoints(const std::filesystem::path& path) {
    Store store(path);
    std::map<std::string,Json,std::less<>> profiles;
    std::map<std::string,AvatarImage,std::less<>> images;
    unsigned counter = 0;

    WebCallbacks cb;
    cb.bots = [&] {
        Json list = Json::array();
        for (const auto& [id,card] : profiles) list.push_back(card);
        return list;
    };
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        return {std::string(bot),std::string(request),"local-env","proj",Json::object()};
    };
    cb.createBot = [&](const Json& profile, const std::optional<AvatarImage>& image) {
        const auto id = "bot-" + std::to_string(++counter);
        Json avatar = profile.at("avatar");
        if (image) {
            images.insert_or_assign(id,*image);
            avatar["version"] = "v" + std::to_string(counter);
            avatar["file"] = image->format().filename;
        }
        Json card = {{"id",id},{"name",profile.at("name")},{"role",profile.value("role","custom")},
                     {"soul",""},{"cron",Json::array()},{"avatar",avatar},{"secret",privateValue}};
        profiles[id] = card;
        return card;
    };
    cb.updateBot = [&](std::string_view botId, const Json& profile, const std::optional<AvatarImage>& image) {
        auto& card = profiles.at(std::string(botId));
        if (!profile.contains("avatar")) {
            card["name"] = profile.at("name");
            return card;
        }
        Json avatar = profile.at("avatar");
        if (avatar.at("kind") == "upload") {
            if (image) {
                images.insert_or_assign(std::string(botId),*image);
                avatar["version"] = "v" + std::to_string(++counter);
                avatar["file"] = image->format().filename;
            } else {
                const auto& existing = card.at("avatar");
                if (existing.value("kind","") != "upload") throw Conflict("Uploaded image is required");
                avatar["version"] = existing.at("version");
                avatar["file"] = existing.at("file");
            }
        } else {
            images.erase(std::string(botId));
        }
        card["name"] = profile.at("name");
        card["avatar"] = avatar;
        return card;
    };
    cb.botAvatar = [&](std::string_view botId) -> std::optional<AvatarImage> {
        const auto found = images.find(botId);
        if (found == images.end()) return std::nullopt;
        return found->second;
    };

    const auto server = startWeb(store,0,std::move(cb));
    Browser browser(*server);

    // 1x1 transparent PNG (67 bytes) base64
    const std::string validPngBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=";
    // 1x1 GIF base64
    const std::string validGifBase64 = "R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7";
    // SVG payload base64 (must be rejected)
    const std::string svgBase64 = "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciPjwvc3ZnPg==";

    const auto created = browser.post("/api/bots",{
        {"name","Scout"},
        {"role","reviewer"},
        {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}},
    },201);
    check(created.at("id") == "bot-1" && created.at("name") == "Scout" && created.at("role") == "reviewer" &&
          created.at("avatar").at("kind") == "shape" && created.at("avatar").at("example") == "sprout",
          "POST /api/bots did not return sanitized created Bot");

    const auto uploaded = browser.post("/api/bots/profile",{
        {"botId","bot-1"},
        {"name","Scout Photo"},
        {"avatar",{{"kind","upload"},{"color","#75b8e8"},{"filename","photo.png"},{"imageData",validPngBase64}}},
    },200);
    check(uploaded.at("name") == "Scout Photo" && uploaded.at("avatar").at("kind") == "upload" &&
          uploaded.at("avatar").at("filename") == "photo.png" && !uploaded.at("avatar").contains("file"),
          "POST /api/bots/profile leaked internal filename or missed upload metadata");
    const auto originalUrl = uploaded.at("avatar").at("imageUrl").get<std::string>();
    check(originalUrl.starts_with("/api/bot-avatar?bot=bot-1&v="),"missing projected avatar imageUrl");

    const auto fetchedImage = browser.client.Get(originalUrl);
    check(fetchedImage && fetchedImage->status == 200 &&
          fetchedImage->get_header_value("Content-Type") == "image/png" &&
          fetchedImage->get_header_value("X-Content-Type-Options") == "nosniff" &&
          fetchedImage->body.starts_with("\x89PNG"),
          "GET /api/bot-avatar did not serve verified PNG");

    auto paddedPng = validPngBase64;
    paddedPng[paddedPng.size()-2] = 'J';
    const auto padded = browser.post("/api/bots/profile",{
        {"botId","bot-1"},
        {"name","Scout Photo"},
        {"avatar",{{"kind","upload"},{"color","#75b8e8"},{"filename","photo.png"},{"imageData",paddedPng}}},
    },200);
    const auto imageUrl = padded.at("avatar").at("imageUrl").get<std::string>();
    const auto paddedImage = browser.client.Get(imageUrl);
    check(paddedImage && paddedImage->status == 200 && paddedImage->body == fetchedImage->body,
          "Unused Base64 bits changed the accepted avatar bytes");

    // Rename without re-uploading keeps imageUrl intact.
    const auto renamed = browser.post("/api/bots/profile",{
        {"botId","bot-1"},
        {"name","Scout Kept"},
        {"avatar",{{"kind","upload"},{"color","#75b8e8"},{"filename","photo.png"}}},
    },200);
    check(renamed.at("avatar").at("imageUrl") == imageUrl,"rename did not preserve uploaded avatar URL");

    // A name-only update may omit the avatar entirely.
    const auto nameOnly = browser.post("/api/bots/profile",{{"botId","bot-1"},{"name","Scout Name"}},200);
    check(nameOnly.at("name") == "Scout Name" && nameOnly.at("avatar").at("imageUrl") == imageUrl,
          "name-only update changed the avatar");

    // Replace with GIF updates version and content-type.
    const auto replaced = browser.post("/api/bots/profile",{
        {"botId","bot-1"},
        {"name","Scout GIF"},
        {"avatar",{{"kind","upload"},{"color","#75b8e8"},{"filename","badge.gif"},{"imageData",validGifBase64}}},
    },200);
    const auto gifUrl = replaced.at("avatar").at("imageUrl").get<std::string>();
    check(gifUrl != imageUrl,"replacing uploaded image did not rotate version URL");
    const auto fetchedGif = browser.client.Get(gifUrl);
    check(fetchedGif && fetchedGif->status == 200 &&
          fetchedGif->get_header_value("Content-Type") == "image/gif",
          "GET /api/bot-avatar did not serve replaced GIF");

    // Switch to icon removes image.
    const auto iconed = browser.post("/api/bots/profile",{
        {"botId","bot-1"},
        {"name","Scout Icon"},
        {"avatar",{{"kind","icon"},{"icon","sparkles"},{"color","#b5a3ef"}}},
    },200);
    check(iconed.at("avatar").at("kind") == "icon" && iconed.at("avatar").at("icon") == "sparkles",
          "switching to icon avatar failed");
    Browser::checked(browser.client.Get("/api/bot-avatar?bot=bot-1"),404);

    // Reject invalid inputs cleanly.
    const auto rejected = browser.post("/api/bots",{
        {"name","Bad SVG"},
        {"role","custom"},
        {"avatar",{{"kind","upload"},{"imageData",svgBase64}}},
    },400);
    check(rejected.at("error") == "invalid_image","unsupported image lost its HTTP error code");
    browser.post("/api/bots",{
        {"name",""},
        {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}},
    },400);
    browser.post("/api/bots",{
        {"name","TooLongNameThatExceedsThirtyTwoCharactersLimit"},
        {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}},
    },400);
    browser.post("/api/bots/profile",{
        {"botId","missing"},
        {"name","Ghost"},
        {"avatar",{{"kind","shape"},{"example","sprout"},{"color","#69c9ac"}}},
    },404);
}
void accessEndpoints(const std::filesystem::path& path) {
    Store store(path);
    store.policy().changeNeverRule(localOwner,Action::repository_read,"github.com/org/private",true);
    auto cb = callbacks();
    cb.intake = [original = cb.intake](std::string_view bot, std::string_view request) {
        auto intake = original(bot,request);
        intake.config.erase("token");
        return intake;
    };
    const auto file = path.parent_path()/"identities.json";
    const Json accounts = Json::array({{{"authorization","Bearer fixture-owner"},{"person","owner"},{"adminRoot",true}},
                                       {{"authorization","Bearer fixture-alice"},{"person","alice"}}});
    std::ofstream(file) << accounts;
    std::filesystem::permissions(file,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
    configureWebIdentities(store,file,cb);
    const auto link = path.parent_path()/"identity-link";
    std::filesystem::create_symlink(file,link);
    bool rejected = false;
    try { configureWebIdentities(store,link,cb); } catch (const Conflict&) { rejected = true; }
    check(rejected,"symlink identity file accepted");
    auto server = startWeb(store,0,cb);
    Browser browser(*server);
    Browser::checked(browser.client.Get("/api/access?bot=release"),401);
    browser.client.set_default_headers({{"Authorization","Bearer fixture-owner"}});
    check(browser.get("/api/view?bot=release").at("composerPerson")=="owner","composer identity did not match owner");
    Browser::checked(browser.client.Get("/api/access?bot=release&bot=triage"),400);
    auto crossOrigin = browser.headers();
    crossOrigin.erase("Origin");
    crossOrigin.emplace("Origin","https://example.invalid");
    const Json share{{"botId","release"},{"person","alice"},{"add",true}};
    Browser::checked(browser.client.Post("/api/access/members",crossOrigin,share.dump(),"application/json"),403);
    const auto secretRequest = store.policy().requestSecret("release",{"fixture-token"});
    browser.post("/api/access/secret",{{"botId","release"},{"requestId",secretRequest},{"value",privateValue}});
    browser.post("/api/access/secret",{{"botId","release"},{"requestId",secretRequest},{"value",privateValue}},403);
    browser.post("/api/access/members",share);
    const auto state = browser.get("/api/access?bot=release");
    check(state.at("mode") == "full" && state.at("rules").size() == 1 &&
          state.at("members") == Json::array({"alice"}),"policy settings view lost rules or membership");
    browser.client.set_default_headers({{"Authorization","Bearer fixture-alice"}});
    check(browser.get("/api/view?bot=release").at("composerPerson")=="alice","composer identity did not match viewer");
    browser.get("/api/conversation?bot=release");
    check(browser.get("/api/bots").size()==1,"view-only person listed unrelated Bots");
    const auto list = browser.client.Get("/api/bots");
    const auto view = browser.client.Get("/api/view?bot=release");
    check(list && view && list->get_header_value("X-Flock-Viewer") == "\"alice\"" &&
          view->get_header_value("X-Flock-Viewer") == "\"alice\"",
          "list and conversation did not expose the same authenticated viewer scope");
    Browser::checked(browser.client.Get("/api/conversation?bot=triage"),403);
    Browser::checked(browser.client.Get("/api/workflows?bot=triage"),403);
    Browser::checked(browser.client.Get("/api/compactions?bot=triage"),403);
    Browser::checked(browser.client.Get("/api/events?bot=triage"),403);
    Browser::checked(browser.client.Get("/api/events?bot=release&peer=triage"),403);
    browser.post("/api/messages",{{"botId","release"},{"text","hello"},{"clientMessageId","1"}},201);
    browser.post("/api/access/members",{{"botId","release"},{"person","owner"},{"add",false}},403);
    browser.post("/api/access/secret",{{"botId","release"},{"requestId",1},{"value",privateValue}},403);
}

} // namespace

int main() {
    try {
        TestDirectory directory;
        exercise(directory.path / "state.db");
        sidebarReplies(directory.path / "sidebar.db");
        failedCompletion(directory.path / "failed-completion.db");
        recurringRequestStatus(directory.path / "recurring-requests.db");
        eventStream(directory.path / "events.db");
        conversationPermissions(directory.path / "conversation-permissions.db");
        retryEventProjection(directory.path / "retry-events.db");
        retryHistoryInvalidation(directory.path / "retry-history.db");
        peerEventStream(directory.path / "peer-events.db");
        peerHistory(directory.path / "peers.db");
        peerConversation(directory.path / "peer-conversations.db");
        peerConversationAnchor(directory.path / "peer-conversation-anchor.db");
        accessEndpoints(directory.path / "access.db");
        boardInboxEndpoints(directory.path / "board-inbox.db");
        channelEndpoints(directory.path / "channels.db");
        botProfileEndpoints(directory.path / "profiles.db");
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
