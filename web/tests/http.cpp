#include "flock/web.hpp"

#include <httplib.h>

#include <filesystem>
#include <iostream>
#include <map>

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
    unsigned launches = 0;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    Delivery send(const Command& command) override {
        if (command.tool == "delegate") {
            ++launches;
            if (command.projectId == "settings") return {Delivery::Kind::Settings,{},privateValue};
            const auto& project = command.projectId;
            return {Delivery::Kind::Accepted,{"thread-" + project,"run-" + std::to_string(launches),"javascript:unexpected"},{}};
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
    check(landing && landing->status == 200 && landing->body.find("/app.js") != std::string::npos,"missing embedded UI");
    check(landing->get_header_value("X-Frame-Options") == "DENY" &&
          landing->get_header_value("Content-Security-Policy").find("frame-ancestors 'none'") != std::string::npos,
          "missing frame protection");
    for (const auto* path : {"/app.js","/avatars.js","/theme.js","/style.css","/sidebar.css","/conversation.css",
                            "/composer.css","/dialog.css","/flock.svg",
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
    browser.post("/api/requests",{{"botId","broken"},{"request","test"}},422);
}

void samePresence(Browser& browser) {
    for (const auto& row : browser.get("/api/bots")) {
        const auto view = browser.get("/api/view?bot=" + row["id"].get<std::string>());
        check(row["presence"] == view["presence"],"Bot list presence differs from the Bot view");
    }
}

void tick(Runtime& runtime, int count) { for (int i = 0; i < count; ++i) runtime.tick(); }

bool hasEvent(const Json& events, std::string_view kind, std::string_view text) {
    for (const auto& event : events) {
        if (event["kind"] != kind) continue;
        const auto& data = event["data"];
        if (data.value("text", data.value("report", std::string{})) == text) return true;
    }
    return false;
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
    check(store.tasks().empty(),"rejected requests changed SQLite");
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
    check(waits.size() == 1 && waits[0]["id"] == release && waits[0]["wait"]["choices"].size() == 2,"release Bot question was missing");
    check(triagePage["waits"].size() == 1 && triagePage["waits"][0]["id"] == triage,"T3 wait belongs to wrong Bot");
    check(releasePage["bot"]["cron"][0]["timezone"] == "Asia/Tokyo" && releasePage["bot"]["soul"] != triagePage["bot"]["soul"],
          "Bot settings were mixed");
    check(releasePage["sessionTasks"][0]["sessions"][0]["link"] == "https://app.t3.codes/threads/local-env/thread-release" &&
          triagePage["waits"][0]["wait"]["run"]["link"] == "https://app.t3.codes/threads/linked%20env/thread-triage",
          "T3 links did not preserve environment and Thread ownership");
    const auto settingsPage = browser.get("/api/view?bot=settings");
    check(settingsPage["waits"][0]["id"] == settings && settingsPage["waits"][0]["wait"]["reason"] == "settings",
          "configuration failure was not classified");
    const auto list = browser.get("/api/bots");
    check(list[0]["preview"] == "利用者向けの変更だけを載せますか？","Bot list does not show its latest question");
    samePresence(browser);
    check(list[0]["presence"] == "waiting" && list[3]["id"] == "broken" && list[3]["presence"] == "idle",
          "Bot list presence did not follow task states");
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
          t3.replies[0].answers["scope"] == "利用者向けのみ" && cli.task(next)["state"] == "queued",
          "stale reply affected the active Run or another task");
    check(browser.get("/api/view?bot=release")["waits"].empty(),"resolved wait stayed selectable");
    samePresence(browser);
    check(browser.get("/api/bots")[0]["presence"] == "working","resumed Bot was not listed as working");

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
    check(hasEvent(page["events"],"report",finished.report) && hasEvent(page["events"],"completed","Draft complete"),
          "reopened UI lost the Session result or Bot decision");
    const auto external = reopened.get("/api/view?bot=triage").at("waits").at(0);
    check(external["wait"]["run"]["link"] == "https://app.t3.codes/threads/linked%20env/thread-triage",
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
          t3.replies.back().answers["scope"] == "すべて","a Web answer did not resume its own question exactly once");

    for (int i = 0; i < 43; ++i) {
        cli.submit({"release","Paged request " + std::to_string(i),"local-env","release",Json::object(),"daily",1700000000 + i});
    }
    const auto newest = reopened.get("/api/view?bot=release");
    const auto cursor = newest.at("olderBefore").get<Id>();
    check(cursor > 0 && newest["events"].front()["id"] == cursor,"missing exclusive older cursor");
    const auto older = reopened.get("/api/conversation?bot=release&before=" + std::to_string(cursor));
    check(older["events"].back()["id"].get<Id>() < cursor &&
          hasEvent(older["events"],"request","下書きの掲載範囲を確認する") &&
          !hasEvent(older["events"],"request","昨日届いた不具合を確認する"),"older page was out of scope or lost events");
    check(newest["events"].back()["data"]["source"] == "cron","scheduled origin did not survive HTTP projection");

    for (int i = 0; i < 8; ++i) {
        const auto id = std::to_string(i);
        cli.submit({"release","Session " + id,"local-env","separate-" + id});
    }
    tick(*runtime,3);
    const auto sessionPage = reopened.get("/api/view?bot=release");
    const auto sessionCursor = sessionPage.at("sessionsBefore").get<Id>();
    check(sessionPage["sessionTasks"].size() == 6 && sessionCursor > 0,"missing session pagination");
    const auto olderSessions = reopened.get("/api/sessions?bot=release&before=" + std::to_string(sessionCursor));
    check(!olderSessions["sessionTasks"].empty() && olderSessions["sessionTasks"].front()["id"].get<Id>() < sessionCursor,
          "session page duplicated newer tasks");
}
} // namespace

int main() {
    try { TestDirectory directory; exercise(directory.path / "state.db"); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
