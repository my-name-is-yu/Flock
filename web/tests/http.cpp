#include "flock/web.hpp"

#include <httplib.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <map>

using namespace flockv0;
namespace {
constexpr auto privateValue = "private-token-value";
constexpr auto proposedOptions = "1. Reconcile manually\n2. Summarize the differences first\n3. Review after publication";

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
            return {"ask_user",{{"question","Should I include only user-facing changes?"},{"choices",{"User-facing only","All changes"}}},"The publication scope needs to be specified"};
        }
        if (input.task["request"] == "Compare the three options")
            return {"complete",{{"report",proposedOptions}},"The Session compared all three options"};
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
            const auto link = project == "triage" ? "http://localhost:3773/linked%20env/thread-triage" : "javascript:unexpected";
            return {Delivery::Kind::Accepted,{"thread-" + project,"run-" + std::to_string(launches),link},{}};
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

WebCallbacks callbacks(const std::atomic_bool* intakeDisabled = nullptr) {
    return {
        [] {
            return Json::array({
                {{"id","release"},{"name","Release"},{"soul","Write release notes for users."},
                    {"cron",Json::array({{{"id","daily"},{"schedule","0 9 * * *"},{"timezone","Asia/Tokyo"},
                        {"request","Review yesterday's changes"},{"token",privateValue}}})},
                    {"avatar",{{"example","mug"},{"color","#982fff"},{"secret",privateValue}}},{"token",privateValue}},
                {{"id","triage"},{"name","Triage"},{"soul","Triage reported issues."},{"cron",Json::array()},{"avatar","sprout"}},
                {{"id","settings"},{"name","Settings"},{"soul","Investigate configuration errors."}},
                {{"id","broken"},{"name","Broken"}},
            });
        },
        [intakeDisabled](std::string_view bot, std::string_view request) -> Intake {
            if (bot == "broken" || (intakeDisabled && intakeDisabled->load())) throw std::runtime_error(privateValue);
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
    for (const auto* path : {"/app.js","/avatars.js","/style.css","/flock.svg",
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
    Browser::checked(browser.client.Post("/api/messages",draft,"application/json"),403);
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

bool hasEvent(const Json& events, std::string_view kind, std::string_view text) {
    for (const auto& event : events) {
        if (event["kind"] != kind) continue;
        const auto& data = event["data"];
        if (data.value("text", data.value("report", std::string{})) == text) return true;
    }
    return false;
}

void conversationEntry(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    std::atomic_bool disableIntake = false;
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; });
    auto server = startWeb(store,0,callbacks(&disableIntake));
    Browser browser(*server);
    const auto taskId = browser.request("release","Compare the three options");
    tick(runtime,3);
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed", .report = proposedOptions, .settled = true
    };
    tick(runtime,2);
    check(store.task(taskId)["state"] == "completed","fixture did not produce a completed comparison");

    const Json message = {{"botId","release"},{"clientMessageId","options-more"},{"text","Explain option 2 in more detail"}};
    const auto receipt = browser.post("/api/messages",message,201);
    check(receipt["status"] == "received" && receipt["messageId"].is_number_integer(),"conversation receipt was missing");
    disableIntake = true;
    check(browser.post("/api/messages",message,201) == receipt,"retry did not recover its receipt with changed Bot settings");
    auto changed = message;
    changed["text"] = "Explain option 3 in more detail";
    browser.post("/api/messages",changed,409);
    changed = message;
    changed["replyToMessageId"] = 1;
    browser.post("/api/messages",changed,409);
    browser.post("/api/messages",{{"botId","release"},{"text","Additional clarification"}},400);

    const auto page = browser.get("/api/view?bot=release");
    unsigned questions = 0, answers = 0;
    for (const auto& event : page["events"]) {
        const auto& data = event["data"];
        if (event["kind"] == "conversation_message" && data["messageId"] == receipt["messageId"]) ++questions;
        if (event["kind"] == "conversation_reply" && data["replyToMessageId"] == receipt["messageId"]) {
            ++answers;
            check(data["route"] == "respond" && data["text"].get<std::string>().find("Summarize the differences first") != std::string::npos,
                  "answer did not reference the second option");
            check(data["source"]["taskId"] == taskId && data["source"]["messageId"].is_number_integer(),
                  "answer source was not visible in the conversation");
        }
    }
    check(questions == 1 && answers == 1 && store.tasks().size() == 1 && store.task(taskId)["runs"].size() == 1 && t3.launches == 1,
          "read-only conversation or retry created extra messages, Tasks or Runs");
    const auto task = browser.get("/api/task?bot=release&taskId=" + std::to_string(taskId));
    check(task["id"] == taskId && task["runs"].size() == 1 && !task.contains("config"),"task details omitted a Run or exposed config");
    check(task["result"]["kind"] == "completed" && task["result"]["data"]["text"] == proposedOptions &&
        task["result"]["data"]["run"]["runId"] == task["runs"][0]["runId"],
        "task details lost its completion or source Run");
    Browser::checked(browser.client.Get("/api/task?bot=triage&taskId=" + std::to_string(taskId)),404);

    const Json status = {{"botId","release"},{"clientMessageId","with-invalid-settings"},{"text","What's the status?"}};
    browser.post("/api/messages",status,201);
    check(store.tasks().size() == 1,"read-only request with changed settings created work");
    disableIntake = false;
    const auto firstWaitTask = browser.request("triage","Decide what to publish");
    const auto secondWaitTask = store.submit({"triage","Decide what to publish next time","linked env","next-scope"});
    tick(runtime,3);
    t3.observations["thread-triage"] = {
        .kind = Observation::Kind::Question,
        .question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})}
    };
    t3.observations["thread-next-scope"] = t3.observations.at("thread-triage");
    tick(runtime,2);
    const auto waitPage = browser.get("/api/view?bot=triage");
    Id question = 0;
    for (const auto& row : waitPage["waits"]) {
        if (row["id"] == firstWaitTask) question = row["wait"]["messageId"];
    }
    check(question > 0 && waitPage["waits"].size() == 2,"HTTP projection lacks a stable question anchor");
    const Json reply = {{"botId","triage"},{"clientMessageId","answer-first"},{"text","User-facing only"},{"replyToMessageId",question}};
    const auto answerReceipt = browser.post("/api/messages",reply,201);
    tick(runtime,2);
    check(browser.post("/api/messages",reply,201) == answerReceipt,"exact answer retry lost its receipt");
    auto delayed = reply;
    delayed["clientMessageId"] = "delayed-answer-first";
    delayed["text"] = "All changes";
    const auto delayedResponse = browser.client.Post("/api/messages",browser.headers(),delayed.dump(),"application/json");
    check(delayedResponse && (delayedResponse->status == 201 || delayedResponse->status == 409),"stale answer was not handled");
    tick(runtime,2);
    check(t3.replies.size() == 1 && t3.replies[0].target.threadId == "thread-triage" &&
        store.task(secondWaitTask)["wait"]["reason"] == "user_question" && store.tasks().size() == 3,
        "exact or stale answer changed another wait or created a Task");
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
    const auto release = browser.request("release","Check what to include in the draft");
    const auto triage = browser.request("triage","Review the bugs reported yesterday");
    const auto settings = browser.request("settings","Check the connection settings");
    tick(*runtime,3);
    t3.observations["thread-release"].kind = Observation::Kind::Question;
    t3.observations["thread-release"].question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})};
    t3.observations["thread-triage"].kind = Observation::Kind::ExternalInput;
    t3.observations["thread-triage"].status = "Permission approval required";
    tick(*runtime,2);

    const auto releasePage = browser.get("/api/view?bot=release");
    const auto triagePage = browser.get("/api/view?bot=triage");
    auto waits = releasePage.at("waits");
    check(waits.size() == 1 && waits[0]["id"] == release && waits[0]["wait"]["choices"].size() == 2,"release Bot question was missing");
    check(triagePage["waits"].size() == 1 && triagePage["waits"][0]["id"] == triage,"T3 wait belongs to wrong Bot");
    check(releasePage["bot"]["cron"][0]["timezone"] == "Asia/Tokyo" && releasePage["bot"]["soul"] != triagePage["bot"]["soul"],
          "Bot settings were mixed");
    check(releasePage["sessionTasks"][0]["sessions"][0]["link"] == "https://app.t3.codes/threads/local-env/thread-release" &&
          triagePage["waits"][0]["wait"]["run"]["link"] == "http://localhost:3773/linked%20env/thread-triage",
          "T3 links did not preserve environment and Thread ownership");
    const auto settingsPage = browser.get("/api/view?bot=settings");
    check(settingsPage["waits"][0]["id"] == settings && settingsPage["waits"][0]["wait"]["reason"] == "settings",
          "configuration failure was not classified");
    const auto list = browser.get("/api/bots");
    check(list[0]["preview"] == "Should I include only user-facing changes?","Bot list does not show its latest question");
    const Id waitId = waits[0]["wait"]["id"];
    const auto next = browser.post("/api/requests",{{"botId","release"},{"request","Look into the changes for the next release"},
        {"environmentId","ignored"},{"projectId","ignored"},{"scheduleId","ignored"}},201).at("taskId").get<Id>();
    const auto afterSubmit = browser.get("/api/view?bot=release");
    check(afterSubmit["waits"][0]["wait"]["id"] == waitId && afterSubmit["queued"][0]["blockedBy"]["id"] == release,
          "new request changed the original wait or lost the queue blocker");
    check(cli.task(next)["environmentId"] == "local-env" && cli.task(next)["scheduleId"] == "","client changed routing or schedule");
    Json reply = {{"botId","triage"},{"taskId",release},{"waitId",waitId},{"operation","answer"},{"text","All changes"}};
    browser.post("/api/operations",reply,404);
    reply["botId"] = "release";
    cli.operate(release,waitId,"answer",{{"text","User-facing only"}});
    browser.post("/api/operations",reply,409);
    tick(*runtime,2);
    check(t3.replies.size() == 1 && t3.replies[0].target.threadId == "thread-release" &&
          t3.replies[0].answers["scope"] == "User-facing only" && cli.task(next)["state"] == "queued",
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
    check(hasEvent(page["events"],"report",finished.report) && hasEvent(page["events"],"completed","Draft complete"),
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
    const Json answer = {{"botId","triage"},{"taskId",triage},{"waitId",questionId},{"operation","answer"},{"text","All changes"}};
    reopened.post("/api/operations",answer);
    reopened.post("/api/operations",answer,409);
    tick(*runtime,2);
    check(t3.replies.size() == 2 && t3.replies.back().target.threadId == "thread-triage" &&
          t3.replies.back().answers["scope"] == "All changes","a Web answer did not resume its own question exactly once");

    for (int i = 0; i < 43; ++i) {
        cli.submit({"release","Paged request " + std::to_string(i),"local-env","release",Json::object(),"daily",1700000000 + i});
    }
    const auto newest = reopened.get("/api/view?bot=release");
    const auto cursor = newest.at("olderBefore").get<Id>();
    check(cursor > 0 && newest["events"].front()["id"] == cursor,"missing exclusive older cursor");
    const auto older = reopened.get("/api/conversation?bot=release&before=" + std::to_string(cursor));
    check(older["events"].back()["id"].get<Id>() < cursor &&
          hasEvent(older["events"],"request","Check what to include in the draft") &&
          !hasEvent(older["events"],"request","Review the bugs reported yesterday"),"older page was out of scope or lost events");
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
    try {
        TestDirectory directory;
        exercise(directory.path / "state.db");
        conversationEntry(directory.path / "conversation.db");
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
