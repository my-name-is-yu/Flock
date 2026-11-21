#include "flock/web.hpp"

#include <httplib.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>

using namespace flockv0;
namespace {
constexpr auto privateValue = "private-token-value";

void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct TestDirectory {
    std::filesystem::path path;
    TestDirectory() {
        char name[] = "/tmp/flock-web-XXXXXX";
        const auto directory = mkdtemp(name);
        check(directory != nullptr,"could not create fixture directory");
        path = directory;
    }
    ~TestDirectory() { std::error_code error; std::filesystem::remove_all(path,error); }
};

struct FixedModel : Model {
    Decision decide(const ModelInput& input) override {
        if (input.event["kind"] == "intake")
            return {"delegate",{{"instruction","Read the working notes"},{"doneWhen","Summarize user-facing changes"}},"The request needs a working report"};
        if (input.event["kind"] == "question") {
            if (input.event.contains("userAnswer")) {
                return {"answer_session_question",{{"requestId","scope-question"},
                    {"answers",{{"scope",input.event["userAnswer"]}}}},"The user selected the scope"};
            }
            return {"ask_user",{{"question","利用者向けの変更だけを載せますか？"},{"choices",{"利用者向けのみ","すべて"}}},"掲載範囲の指定が必要"};
        }
        return {"complete",{{"report","Draft complete"}},"The Session report matches the criterion"};
    }
};

struct FixedT3 : T3 {
    std::map<std::string,Observation,std::less<>> observations;
    std::vector<Command> replies;
    std::map<std::string,unsigned,std::less<>> launches;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    Delivery send(const Command& command) override {
        if (command.tool == "delegate") {
            if (command.projectId == "settings") return {Delivery::Kind::Settings,{},privateValue};
            const auto& project = command.projectId;
            const auto link = project == "triage" ? "http://localhost:3773/linked%20env/thread-triage" : "javascript:unexpected";
            return {Delivery::Kind::Accepted,{"thread-" + project,"run-" + project + "-" + std::to_string(++launches[project]),link},{}};
        }
        check(command.tool == "answer_session_question","unexpected command in fixture");
        replies.push_back(command);
        observations.erase(command.target.threadId);
        return {Delivery::Kind::Accepted,{},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return observations[run.threadId]; }
    void interrupt(const std::string&, const Json&, const RunRef&) override {}
};

WebCallbacks callbacks() {
    return {
        [] {
            return Json::array({
                {{"id","release"},{"name","Release"},{"soul","利用者向けのリリースノートをまとめる。"},
                    {"cron",Json::array({{{"id","daily"},{"schedule","0 9 * * *"},{"timezone","Asia/Tokyo"},
                        {"request","前日の変更を確認する"},{"token",privateValue}}})},
                    {"avatar",{{"example","mug"},{"color","#982fff"},{"secret",privateValue}}},{"token",privateValue}},
                {{"id","triage"},{"name","Triage"},{"soul","報告された問題を整理する。"},{"cron",Json::array()},{"avatar","sprout"}},
                {{"id","settings"},{"name","Settings"},{"soul","設定エラーの確認"}},
                {{"id","broken"},{"name","Broken"}},
            });
        },
        [](std::string_view bot, std::string_view request) -> Intake {
            if (bot == "broken") throw std::runtime_error(privateValue);
            return {std::string(bot),std::string(request),bot == "triage" ? "linked env" : "local-env",
                std::string(bot),{{"token",privateValue},{"runTimeoutSeconds",1800}}, {}, 0};
        }
    };
}

class Browser {
public:
    explicit Browser(const WebServer& server) : client("127.0.0.1",static_cast<int>(server.port())),
        origin("http://127.0.0.1:" + std::to_string(server.port())) {}

    Json get(const std::string& path) { return checked(client.Get(path),200); }
    Json post(const std::string& path, const Json& data, int status = 200) {
        return checked(client.Post(path,headers(),data.dump(),"application/json"),status);
    }
    Id request(std::string_view bot, std::string_view text) {
        return post("/api/requests",{{"botId",bot},{"request",text}},201).at("taskId");
    }
    httplib::Headers headers() const { return {{"Origin",origin},{"Sec-Fetch-Site","same-origin"},{"X-Flock-Action","1"}}; }
    static Json checked(const httplib::Result& response, int status) {
        if (!response) throw std::runtime_error("HTTP request failed: " + httplib::to_string(response.error()));
        if (response->status != status)
            throw std::runtime_error("Unexpected HTTP " + std::to_string(response->status) + " from " + response->body.substr(0,240));
        check(response->body.find(privateValue) == std::string::npos,"private connection value entered HTTP response");
        return Json::parse(response->body);
    }
    httplib::Client client;
    std::string origin;
};

void boundary(Browser& browser) {
    auto landing = browser.client.Get("/");
    check(landing && landing->status == 200 && !landing->body.empty() &&
          landing->get_header_value("Content-Type").starts_with("text/html"),"Flock did not serve its landing page");
    check(landing->get_header_value("X-Frame-Options") == "DENY" &&
          landing->get_header_value("Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos,
          "missing frame protection");
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
    browser.post("/api/requests",{{"botId","broken"},{"request","test"}},422);
}

void tick(Runtime& runtime, int count) { for (int i = 0; i < count; ++i) runtime.tick(); }

bool hasEvent(const Json& events, std::string_view kind, std::string_view field, std::string_view text) {
    for (const auto& event : events) {
        if (event["kind"] != kind) continue;
        if (event.at("data").value(field,std::string{}) == text) return true;
    }
    return false;
}

void conversationPages(Browser& browser, Store& cli, Id release, Id next, Id triage) {
    std::map<Id,std::string> expectedRequests = {
        {release,"下書きの掲載範囲を確認する"},
        {next,"次の版の変更点を調べる"},
    };
    for (int i = 0; i < 43; ++i) {
        const auto text = "Paged request " + std::to_string(i);
        const auto id = cli.submit({"release",text,"local-env","release",Json::object(),"daily",1700000000 + i});
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
            if (task != release && task != next)
                check(data.at("source") == "cron" && data.at("scheduleId") == "daily","scheduled request lost its origin");
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

void sessionPages(Browser& browser, Store& cli, Runtime& runtime, Id triage) {
    std::set<Id> expectedTasks = {triage};
    for (int i = 0; i < 8; ++i) {
        const auto id = std::to_string(i);
        expectedTasks.insert(cli.submit({"triage","Session " + id,"linked env","separate-" + id}));
    }
    const auto allRunsStarted = [&] {
        return std::ranges::all_of(expectedTasks,[&](Id id) { return cli.task(id).at("runs").size() == 1; });
    };
    for (int attempt = 0; attempt < 20 && !allRunsStarted(); ++attempt) runtime.tick();
    check(allRunsStarted(),"session fixtures did not each start one Run");

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
        }
        const auto before = page.at("sessionsBefore").get<Id>();
        if (before == 0) break;
        check(cursors.insert(before).second,"session cursor did not advance");
        page = browser.get("/api/sessions?bot=triage&before=" + std::to_string(before));
    } while (true);
    check(expectedTasks.empty(),"session pagination skipped tasks");
}

void peerHistory(const std::filesystem::path& path) {
    struct PeerModel final : Model {
        Decision decide(const ModelInput& input) override {
            if (input.event["kind"] == "intake") {
                return {"send_bot_message",{{"botId","triage"},{"kind","consult"},{"text","判定基準を教えて"}},
                    "Triage owns the criteria"};
            }
            if (input.event["kind"] == "bot_request") {
                return {"reply_to_bot",{{"kind","result"},{"text","再現条件を確認してください"}},
                    "The requested criteria are already known"};
            }
            check(input.event["kind"] == "bot_result","unexpected peer history fixture event");
            return {"complete",{{"report","判定基準を確認しました"}},"Triage answered the request"};
        }
    } model;

    Store store(path);
    FixedT3 t3;
    RuntimeOptions options;
    options.peers = [](std::string_view sender) -> std::vector<PeerBot> {
        if (sender != "release") return {};
        return {{"Triage",{"triage","","local-env","release"}}};
    };
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; },options);
    const auto server = startWeb(store,0,callbacks());
    Browser browser(*server);
    browser.request("release","判定基準の確認");
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

void exercise(const std::filesystem::path& path) {
    Store store(path);
    Store cli(path);
    FixedModel model;
    FixedT3 t3;
    const auto notes = [](std::string_view) { return BotNotes{}; };
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
    check(schedule.at("schedule") == "0 9 * * *" && schedule.at("timezone") == "Asia/Tokyo" &&
          schedule.at("request") == "前日の変更を確認する","Release's scheduled request was incorrect");
    check(releasePage["sessionTasks"][0]["sessions"][0]["link"] == "https://app.t3.codes/threads/local-env/thread-release" &&
          triagePage["waits"][0]["wait"]["run"]["link"] == "http://localhost:3773/linked%20env/thread-triage",
          "T3 links did not preserve environment and Thread ownership");
    const auto settingsPage = browser.get("/api/view?bot=settings");
    check(settingsPage["waits"][0]["id"] == settings && settingsPage["waits"][0]["wait"]["reason"] == "settings",
          "configuration failure was not classified");
    const auto list = browser.get("/api/bots");
    const auto releaseRow = std::find_if(list.begin(),list.end(),[](const Json& bot) { return bot.at("id") == "release"; });
    check(releaseRow != list.end() && releaseRow->at("preview") == "利用者向けの変更だけを載せますか？",
          "Bot list does not show Release's latest question");
    const Id waitId = waits[0]["wait"]["id"];
    const auto next = browser.post("/api/requests",{{"botId","release"},{"request","次の版の変更点を調べる"},
        {"environmentId","ignored"},{"projectId","ignored"},{"scheduleId","ignored"}},201).at("taskId").get<Id>();
    const auto afterSubmit = browser.get("/api/view?bot=release");
    check(afterSubmit["waits"][0]["wait"]["id"] == waitId && afterSubmit["queued"][0]["blockedBy"]["id"] == release,
          "new request changed the original wait or lost the queue blocker");
    check(cli.task(next)["environmentId"] == "local-env" && cli.task(next)["scheduleId"] == "","client changed routing or schedule");
    Json reply = {{"botId","triage"},{"taskId",release},{"waitId",waitId},{"operation","answer"},{"text","すべて"}};
    browser.post("/api/operations",reply,404);
    reply["botId"] = "release";
    cli.operate(release,waitId,"answer",{{"text","利用者向けのみ"}});
    browser.post("/api/operations",reply,409);
    tick(*runtime,2);
    check(t3.replies.size() == 1 && t3.replies[0].target.threadId == "thread-release" &&
          t3.replies[0].target.runId == "run-release-1" && t3.replies[0].questionId == "scope-question" &&
          t3.replies[0].answers["scope"] == "利用者向けのみ" && cli.task(next)["state"] == "queued",
          "stale reply affected the active Run or another task");
    check(browser.get("/api/view?bot=release")["waits"].empty(),"resolved wait stayed selectable");

    server.reset();
    runtime.reset();
    auto& finished = t3.observations["thread-release"];
    finished.kind = Observation::Kind::Finished;
    finished.report = "T3 finished while the browser was closed";
    finished.status = "completed";
    finished.settled = true;
    runtime = std::make_unique<Runtime>(store,model,t3,notes);
    tick(*runtime,2);
    server = startWeb(store,port,callbacks());
    Browser reopened(*server);
    const auto page = reopened.get("/api/view?bot=release");
    check(hasEvent(page["events"],"report","report","T3 finished while the browser was closed") &&
          hasEvent(page["events"],"completed","text","Draft complete"),
          "reopened UI lost the Session result or Bot decision");
    const auto external = reopened.get("/api/view?bot=triage").at("waits").at(0);
    check(external["wait"]["run"]["link"] == "http://localhost:3773/linked%20env/thread-triage",
          "restart lost the active T3 approval link");
    const Id externalId = external["wait"]["id"];
    t3.observations["thread-triage"] = {};
    tick(*runtime,1);
    check(reopened.get("/api/view?bot=triage")["waits"].empty(),"resolved T3 wait stayed visible");
    reopened.post("/api/operations",{{"botId","triage"},{"taskId",triage},{"waitId",externalId},{"operation","cancel"}},409);
    check(cli.task(triage)["state"] == "running","stale cancellation affected a resumed Run");

    auto& triageQuestion = t3.observations["thread-triage"];
    triageQuestion.kind = Observation::Kind::Question;
    triageQuestion.question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})};
    tick(*runtime,2);
    const auto questionId = reopened.get("/api/view?bot=triage")["waits"][0]["wait"]["id"];
    const Json answer = {{"botId","triage"},{"taskId",triage},{"waitId",questionId},{"operation","answer"},{"text","すべて"}};
    reopened.post("/api/operations",answer);
    reopened.post("/api/operations",answer,409);
    tick(*runtime,2);
    check(t3.replies.size() == 2 && t3.replies.back().target.threadId == "thread-triage" &&
          t3.replies.back().target.runId == "run-triage-1" && t3.replies.back().questionId == "scope-question" &&
          t3.replies.back().answers["scope"] == "すべて","a Web answer did not resume its own question exactly once");

    conversationPages(reopened,cli,release,next,triage);
    sessionPages(reopened,cli,*runtime,triage);
}
} // namespace

int main() {
    try {
        TestDirectory directory;
        exercise(directory.path / "state.db");
        peerHistory(directory.path / "peers.db");
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
