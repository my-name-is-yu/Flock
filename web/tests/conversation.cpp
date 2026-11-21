#include "flock/web.hpp"

#ifdef FLOCK_TEST_CHATGPT
#include "bots.hpp"
#include "model.hpp"
#endif

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>

using namespace flockv0;
namespace {
constexpr auto privateValue = "private-token-value";
constexpr auto comparisonRequest = "3案を比較して";
constexpr auto compoundExplanation = "2案目を詳しく。2案目を要約して";
constexpr auto missingBudgetQuestion = "2案目にいくらかかる？";
constexpr auto budgetDetail = "明細は100件です";
constexpr auto proposedOptions = "1. 手動で照合する\n"
    "2. 差分を先に集計する\n"
    "   - 前回と今回の明細を ID で突き合わせる。\n"
    "   - 追加・削除・更新の件数を出し、金額が変わった明細を人が確認する。\n"
    "3. 公開後に見直す";

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
    unsigned routed = 0, explained = 0, summarized = 0;
    Id comparisonResult = 0;
    Id estimateClarification = 0;
    std::size_t summarySourceSize = 0;

    Decision decide(const ModelInput& input) override {
        if (input.event["kind"] == "route_conversation") return routeMessage(input);
        if (input.event["kind"] == "answer_conversation_result") return answerResult(input);
        if (input.event["kind"] == "intake")
            return {"delegate",{{"instruction","Read the working notes"},{"doneWhen","Summarize user-facing changes"}},"The request needs a working report"};
        if (input.event["kind"] == "question") {
            if (input.event.contains("userAnswer")) {
                return {"answer_session_question",{{"requestId","scope-question"},
                    {"answers",{{"scope",input.event["userAnswer"]}}}},"The user selected the scope"};
            }
            return {"ask_user",{{"question","利用者向けの変更だけを載せますか？"},{"choices",{"利用者向けのみ","すべて"}}},"掲載範囲の指定が必要"};
        }
        if (input.task["request"] == comparisonRequest)
            return {"complete",{{"report",proposedOptions}},"The Session compared all three options"};
        return {"complete",{{"report","Draft complete"}},"The Session report matches the criterion"};
    }

    Decision routeMessage(const ModelInput& input) {
        ++routed;
        const auto& event = input.event;
        check(event.dump().find(privateValue) == std::string::npos &&
              input.history.dump().find(privateValue) == std::string::npos,"Connection settings entered model-visible context");
        Json route;
        if (event.at("text") == budgetDetail) {
            const auto& pending = event.at("pendingClarification");
            check(pending.at("messageId") == estimateClarification &&
                  pending.at("result").at("messageId") == comparisonResult,
                  "The ordinary reply lost its pending question or selected result");
            return {"route_conversation",{{"routes",Json::array({{{"kind","answer_result"}}})},
                    {"resolution","resolved"},{"clarificationMessageId",estimateClarification}},{}};
        }
        if (event.at("text") == "2案目を詳しく" || event.at("text") == "2案目を要約して" ||
            event.at("text") == compoundExplanation ||
            event.at("text") == missingBudgetQuestion) {
            const auto& results = event.at("results");
            const auto selected = std::ranges::find_if(results,[&](const Json& result) {
                return result.at("messageId") == comparisonResult && result.at("request") == comparisonRequest &&
                       result.at("text") == proposedOptions;
            });
            check(selected != results.end(),"The comparison was not available to the model fixture");
            route = {{"kind","answer_result"},{"resultMessageId",selected->at("messageId")},{"resultOption",2}};
            if (event.at("text") == compoundExplanation) {
                auto summary = route;
                route["segment"] = "2案目を詳しく";
                summary["segment"] = "2案目を要約して";
                return {"route_conversation",{{"routes",Json::array({route,summary})}},"Explain and summarize"};
            }
        } else if (event.at("text") == "Who are you?") {
            route = {{"kind","respond"},{"text","I'm the release Bot."}};
        } else if (event.at("text") == "進捗どうなってる" || event.at("text") == "進捗は？") {
            route = {{"kind","status"}};
        } else if (event.at("text") == "Prepare the notes in local-env or backup") {
            route = {{"kind","create_task"}};
        } else {
            const auto& anchor = event.at("reply");
            check(anchor.at("kind") == "wait" && event.at("text") == "利用者向けのみ",
                  "Unexpected route in HTTP fixture");
            route = {{"kind","answer_wait"},{"taskId",anchor.at("taskId")}};
        }
        return {"route_conversation",{{"routes",Json::array({route})}},{}};
    }

    Decision answerResult(const ModelInput& input) {
        const auto& source = input.event.at("source").at("text").get_ref<const std::string&>();
        check(source.find("ID で突き合わせる") != std::string::npos &&
              source.find("金額が変わった明細を人が確認する") != std::string::npos &&
              source.find("手動で照合する") == std::string::npos && source.find("公開後に見直す") == std::string::npos,
              "The result answer did not receive only the selected option");
        const auto question = input.event.at("text").get<std::string>();
        if (question.find(budgetDetail) != std::string::npos) {
            check(question.starts_with(missingBudgetQuestion),"The original estimate question was lost");
            return {"answer_conversation_result",{{"text","100件の明細を照合し、金額が変わった分の確認時間を見積もります。"}},{}};
        }
        if (question == "2案目を詳しく") {
            ++explained;
            return {"answer_conversation_result",{{"text","差分を先に集計する案です。ID ごとに前回と今回の記録を比べるので、"
                    "追加・削除・更新を分けて数えられます。そのうち金額が変わった更新だけを人が確かめる手順です。"}},{}};
        }
        if (input.event.at("text") == missingBudgetQuestion)
            return {"clarify_conversation_result",{{"text","見積もりに必要な明細の件数を教えてください。"}},{}};
        check(input.event.at("text") == "2案目を要約して","The answer model lost the user's summary request");
        ++summarized;
        summarySourceSize = source.size();
        return {"answer_conversation_result",{{"text","ID で明細の差分を集計し、金額変更を人が確認する案です。"}},{}};
    }
};

struct FixedT3 : T3 {
    std::map<std::string,Observation,std::less<>> observations;
    std::map<std::string,unsigned> projectLaunches;
    std::vector<Command> replies;
    unsigned launches = 0, interruptions = 0;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    Delivery send(const Command& command) override {
        if (command.tool == "delegate") {
            ++launches;
            if (command.projectId == "settings") return {Delivery::Kind::Settings,{},privateValue};
            const auto& project = command.projectId;
            const auto sequence = ++projectLaunches[project];
            auto thread = "thread-" + project;
            if (sequence > 1) thread += "-" + std::to_string(sequence);
            const auto link = project == "triage" ? "http://localhost:3773/linked%20env/" + thread : "javascript:unexpected";
            return {Delivery::Kind::Accepted,{std::move(thread),"run-" + std::to_string(launches),link},{}};
        }
        check(command.tool == "answer_session_question","unexpected command in fixture");
        replies.push_back(command);
        observations.erase(command.target.threadId);
        return {Delivery::Kind::Accepted,{},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return observations[run.threadId]; }
    void interrupt(const std::string&, const Json&, const RunRef&) override { ++interruptions; }
};

WebCallbacks callbacks(const std::atomic_bool* intakeDisabled = nullptr) {
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

void tick(Runtime& runtime, int count) { for (int i = 0; i < count; ++i) runtime.tick(); }

bool hasEvent(const Json& events, std::string_view kind, std::string_view text) {
    for (const auto& event : events) {
        if (event["kind"] != kind) continue;
        const auto& data = event["data"];
        if (data.value("text", data.value("report", std::string{})) == text) return true;
    }
    return false;
}

Json conversationReply(const Json& page, Id messageId) {
    Json result;
    for (const auto& event : page.at("events")) {
        if (event["kind"] != "conversation_reply" || event["data"]["replyToMessageId"] != messageId) continue;
        check(result.is_null(),"message received more than one Bot reply");
        result = event.at("data");
    }
    check(!result.is_null(),"message received no Bot reply");
    return result;
}

void destinationChoicesInView(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; });
    auto config = callbacks();
    const auto configuredIntake = config.intake;
    config.intake = [configuredIntake](std::string_view bot, std::string_view request) {
        auto intake = configuredIntake(bot,request);
        intake.config["conversationEnvironments"]["backup"] = "backup-env";
        return intake;
    };
    const auto server = startWeb(store,0,std::move(config));
    Browser browser(*server);
    const auto send = [&](std::string_view key, std::string_view message, Id replyTo) {
        Json input = {{"botId","release"},{"clientMessageId",key},{"text",message}};
        if (replyTo) input["replyToMessageId"] = replyTo;
        return browser.post("/api/messages",input,201);
    };
    send("request-destination","Prepare the notes in local-env or backup",0);
    runtime.routeConversation();
    const auto page = browser.get("/api/view?bot=release");
    const auto& clarifications = page.at("clarifications");
    check(clarifications.size() == 1 && clarifications[0].contains("destinations") &&
          clarifications[0]["destinations"] == Json::array({"local-env","backup","backup-env"}) &&
          store.tasks().empty(),"the Web view lost the configured destination choices");
    const auto receipt = send("choose-backup","backup",clarifications[0].at("messageId"));
    tick(runtime,4);
    const auto resolved = browser.get("/api/view?bot=release");
    const auto tasks = store.tasks("release");
    check(resolved.at("clarifications").empty() && tasks.size() == 1 &&
          conversationReply(resolved,receipt.at("messageId")).at("route") == "create_task" &&
          store.task(tasks[0]["id"]).at("runs").back().at("environmentId") == "backup-env",
          "the selected Web destination did not receive the new Run");
}

void conversationEntry(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    std::atomic_bool disableIntake = false;
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; });
    auto server = startWeb(store,0,callbacks(&disableIntake));
    Browser browser(*server);
    const auto taskId = browser.request("release",comparisonRequest);
    tick(runtime,3);
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed", .report = proposedOptions,
        .itemId = "comparison-report", .settled = true
    };
    tick(runtime,2);
    const auto taskPath = "/api/task?bot=release&taskId=" + std::to_string(taskId);
    const auto completed = browser.get(taskPath);
    check(completed["id"] == taskId && completed["state"] == "completed","task details did not show the completed comparison");
    const auto& result = completed.at("result");
    check(result["kind"] == "completed" && result["taskId"] == taskId && result["data"]["text"] == proposedOptions,
          "task details lost the completion report");
    model.comparisonResult = result.at("id");
    const auto& reportRun = result.at("data").at("run");
    check(reportRun["threadId"] == "thread-release" && reportRun["runId"] == "run-1" && reportRun["itemId"] == "comparison-report",
          "task details did not point to the Run and report item that produced the result");

    const Json message = {{"botId","release"},{"clientMessageId","options-more"},{"text","2案目を詳しく"}};
    const auto receipt = browser.post("/api/messages",message,201);
    const auto messageId = receipt.at("messageId").get<Id>();
    const auto cursor = browser.get("/api/view?bot=release").at("historyCursor").get<Id>();
    const auto since = "&changesAfter=" + std::to_string(cursor);
    check(receipt["status"] == "received" && messageId > 0,"conversation receipt was missing");
    check(model.explained == 0 && store.tasks().size() == 1,"HTTP intake ran model routing before returning its receipt");
    disableIntake = true;
    check(browser.post("/api/messages",message,201) == receipt,"retry did not recover its receipt with changed Bot settings");
    auto changed = message;
    changed["text"] = "3案目を詳しく";
    browser.post("/api/messages",changed,409);
    changed = message;
    changed["replyToMessageId"] = result.at("id");
    browser.post("/api/messages",changed,409);
    browser.post("/api/messages",{{"botId","release"},{"text","追加確認"}},400);

    tick(runtime,3);
    const auto page = browser.get("/api/view?bot=release" + since);
    check(page.at("changedMessageIds") == Json::array({messageId}) &&
          browser.get("/api/view?bot=triage" + since).at("changedMessageIds").empty(),
          "a processed message was not invalidated exclusively for its own Bot");
    unsigned questions = 0, answers = 0;
    for (const auto& event : page.at("events")) {
        if (event["kind"] == "conversation_message") {
            ++questions;
            const auto& data = event.at("data");
            check(event["id"] == messageId && data["messageId"] == messageId && data["text"] == message["text"],
                  "the saved conversation entry did not match the submitted message");
        }
        if (event["kind"] == "conversation_reply") ++answers;
    }
    const auto response = conversationReply(page,messageId);
    const auto responseText = response.at("text").get<std::string>();
    check(response["route"] == "respond" && responseText.find("差分を先に集計する") != std::string::npos &&
          responseText.find("ID ごとに前回と今回の記録を比べる") != std::string::npos &&
          responseText.find("追加・削除・更新を分けて数えられます") != std::string::npos &&
          responseText.find("金額が変わった更新だけを人が確かめる") != std::string::npos &&
          responseText.find("手動で照合する") == std::string::npos && responseText.find("公開後に見直す") == std::string::npos,
          "answer did not explain only the second option");
    check(response["source"]["taskId"] == taskId && response["source"]["messageId"] == result["id"],
          "answer did not point to the completed comparison");
    check(questions == 1 && answers == 1 && store.tasks().size() == 1 && t3.launches == 1,
          "read-only conversation or retry created extra messages, Tasks or Runs");
    const auto task = browser.get(taskPath);
    check(task["runs"].size() == 1 && task["runs"][0]["runId"] == reportRun["runId"] && !task.contains("config"),
          "task details omitted the comparison Run or exposed config");
    Browser::checked(browser.client.Get("/api/task?bot=triage&taskId=" + std::to_string(taskId)),404);

    const auto summary = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","options-summary"},
        {"text","2案目を要約して"}},201);
    tick(runtime,3);
    const auto summarized = conversationReply(browser.get("/api/view?bot=release"),summary.at("messageId"));
    const auto summaryText = summarized.at("text").get<std::string>();
    check(summarized.at("route") == "respond" && summarized.at("source").at("messageId") == result.at("id") &&
          summaryText.find("ID で明細の差分を集計") != std::string::npos &&
          summaryText.find("金額変更を人が確認") != std::string::npos &&
          summaryText.find("手動で照合する") == std::string::npos && summaryText.find("公開後に見直す") == std::string::npos &&
          summaryText.size() < model.summarySourceSize,"The summary did not condense the selected option");
    check(model.explained == 1 && model.summarized == 1 && store.tasks().size() == 1 && t3.launches == 1 &&
          browser.get(taskPath).at("runs") == task.at("runs"),"Two read-only answers added work or missed the model calls");

    const auto estimate = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","estimate"},
        {"text",missingBudgetQuestion}},201);
    runtime.routeConversation();
    const auto estimateReply = conversationReply(browser.get("/api/view?bot=release"),estimate.at("messageId"));
    const auto pending = store.clarifications("release");
    check(estimateReply.at("route") == "clarify" &&
          estimateReply.at("text") == "見積もりに必要な明細の件数を教えてください。" &&
          pending.size() == 1 && pending.front().at("messageId") == estimateReply.at("messageId") &&
          pending.front().at("source").at("messageId") == result.at("id") &&
          store.tasks().size() == 1 && t3.launches == 1 &&
          browser.get(taskPath).at("runs") == task.at("runs"),"Insufficient evidence did not yield a durable read-only clarification");
    model.estimateClarification = estimateReply.at("messageId");
    const auto detail = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","estimate-detail"},
        {"text",budgetDetail}},201);
    runtime.routeConversation();
    const auto estimateAnswer = conversationReply(browser.get("/api/view?bot=release"),detail.at("messageId"));
    check(estimateAnswer.at("route") == "respond" && estimateAnswer.at("source").at("messageId") == result.at("id") &&
          estimateAnswer.at("text").get<std::string>().find("100件") != std::string::npos &&
          store.clarifications("release").empty() && store.tasks().size() == 1 && t3.launches == 1 &&
          browser.get(taskPath).at("runs") == task.at("runs"),"Answering the clarification created work or lost the result");

    const Json status = {{"botId","release"},{"clientMessageId","with-invalid-settings"},{"text","進捗どうなってる"}};
    const auto statusReceipt = browser.post("/api/messages",status,201);
    tick(runtime,3);
    const auto statusReply = conversationReply(browser.get("/api/view?bot=release"),statusReceipt.at("messageId"));
    const auto statusReplyText = statusReply.at("text").get<std::string>();
    check(statusReplyText.find(comparisonRequest) != std::string::npos && statusReplyText.find("完了") != std::string::npos &&
          store.tasks().size() == 1 && t3.launches == 1,"progress inquiry with changed settings failed to report completion without creating work");
    disableIntake = false;
    const auto firstWaitTask = browser.request("triage","掲載する範囲を決める");
    const auto secondWaitTask = store.submit({"triage","次回の掲載範囲を決める","linked env","next-scope"});
    tick(runtime,3);
    t3.observations["thread-triage"] = {
        .kind = Observation::Kind::Question,
        .question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})}
    };
    t3.observations["thread-next-scope"] = t3.observations.at("thread-triage");
    tick(runtime,2);
    const auto waitPage = browser.get("/api/view?bot=triage");
    Json firstWait, secondWait;
    for (const auto& row : waitPage.at("waits")) {
        if (row["id"] == firstWaitTask) firstWait = row.at("wait");
        if (row["id"] == secondWaitTask) secondWait = row.at("wait");
    }
    check(waitPage["waits"].size() == 2 && !firstWait.is_null() && !secondWait.is_null(),
          "the Bot conversation did not expose both pending questions");
    const auto question = firstWait.at("messageId").get<Id>();
    const auto otherQuestion = secondWait.at("messageId").get<Id>();
    check(question > 0 && otherQuestion > 0 && question != otherQuestion,"pending questions did not have distinct reply anchors");
    const Json reply = {{"botId","triage"},{"clientMessageId","answer-first"},{"text","Status\n\nApprove\n\nseparately: keep this answer together"},{"replyToMessageId",question}};
    const auto answerReceipt = browser.post("/api/messages",reply,201);
    tick(runtime,2);
    check(browser.post("/api/messages",reply,201) == answerReceipt,"exact answer retry lost its receipt");
    auto delayed = reply;
    delayed["clientMessageId"] = "delayed-answer-first";
    delayed["text"] = "すべて";
    const auto delayedReceipt = browser.post("/api/messages",delayed,201);
    tick(runtime,2);
    const auto afterAnswer = browser.get("/api/view?bot=triage");
    const auto delayedReply = conversationReply(afterAnswer,delayedReceipt.at("messageId"));
    check(delayedReply.at("route") == "respond" && !delayedReply.at("text").get<std::string>().empty(),
          "Bot did not respond to the late answer");
    check(t3.replies.size() == 1 && t3.replies[0].environmentId == "linked env" &&
          t3.replies[0].target.threadId == "thread-triage" && t3.replies[0].target.runId == firstWait["run"]["runId"].get<std::string>() &&
          t3.replies[0].questionId == "scope-question" && t3.replies[0].answers == Json{{"scope",reply["text"]}},
          "T3 did not receive exactly one matching answer for the original question");
    const auto& waits = afterAnswer.at("waits");
    check(waits.size() == 1 && waits[0]["id"] == secondWaitTask && waits[0]["wait"]["messageId"] == otherQuestion &&
          waits[0]["wait"]["reason"] == "user_question" && store.tasks().size() == 3,
          "exact or stale answer changed another wait or created a Task");
}

void botMediatedConversation(const std::filesystem::path& path, std::string_view note) {
    constexpr auto request = "出荷前に確認してください。\n\n署名付きビルドの確認手順を教えてください。";
    constexpr auto greeting = "こんにちは。";
    constexpr auto greetingReply = "こんにちは。出荷の準備についてお手伝いします。";
    constexpr auto review = "前のバージョンから更新し、署名が有効であることを確認してください。";
    constexpr auto response = "Reviewer に確認しました。前のバージョンから更新し、署名が有効であることを確認してください。";
    Json original;
    struct ForwardingModel final : Model {
        std::function<Decision(const ModelInput&)> script;
        Decision decide(const ModelInput& input) override { return script(input); }
    } model;
    model.script = [&](const ModelInput& input) -> Decision {
        const auto kind = input.event.at("kind");
        if (kind == "route_conversation") {
            if (input.event.at("text") == greeting)
                return {"route_conversation",{{"routes",Json::array({{{"kind","respond"},{"text",greetingReply}}})}}, {}};
            check(input.event.at("text") == request,"the responsible Bot did not receive the user's original message");
            return {"route_conversation",{{"routes",Json::array({{{"kind","create_task"}}})}}, {}};
        }
        if (kind == "intake") {
            check(input.task.at("botId") == "ernest" && input.task.at("request") == request &&
                  input.task.at("forwardableUserMessage") == original &&
                  input.task.at("peers") == Json::array({{{"botId","reviewer"},{"name","Reviewer"}}}) &&
                  std::ranges::find(input.tools,"send_bot_message") != input.tools.end(),
                  "Ernest did not receive the saved user message and choose its permitted peer");
            Json args = {{"botId","reviewer"},{"kind","consult"},{"forwardUserMessage",true}};
            if (!note.empty()) args["text"] = note;
            return {"send_bot_message",std::move(args),"Reviewer knows the release procedure"};
        }
        if (kind == "bot_request") {
            check(input.task.at("botId") == "reviewer" && input.event.at("fromBotId") == "ernest" &&
                  input.event.at("toBotId") == "reviewer" && input.event.value("text",std::string{}) == note &&
                  input.event.at("forwardedMessage") == original,
                  "Reviewer did not receive Ernest's forwarded message and separate note");
            return {"reply_to_bot",{{"kind","result"},{"text",review}},"Answer Ernest's consultation"};
        }
        check(kind == "bot_result" && input.task.at("botId") == "ernest" && input.event.at("fromBotId") == "reviewer" &&
              input.event.at("text") == review,"the peer reply did not return to the responsible Bot");
        return {"complete",{{"report",response}},"Relay the answer in Ernest's chat"};
    };

    Store store(path);
    FixedT3 t3;
    const auto peers = [](std::string_view bot) -> std::vector<PeerBot> {
        if (bot != "ernest") return {};
        return {{"Reviewer",{"reviewer","","local-env","ernest"}}};
    };
    auto web = callbacks();
    web.bots = [] { return Json::array({{{"id","ernest"},{"name","Ernest"}},{{"id","reviewer"},{"name","Reviewer"}}}); };
    web.peers = peers;
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; },RuntimeOptions{.peers=peers});
    const auto server = startWeb(store,0,web);
    Browser browser(*server);
    constexpr auto dm = "/api/peer-conversation?bot=ernest&peer=reviewer";

    const auto hello = browser.post("/api/messages",{{"botId","ernest"},{"clientMessageId","hello"},{"text",greeting}},201);
    runtime.routeConversation();
    check(conversationReply(browser.get("/api/view?bot=ernest"),hello.at("messageId")).at("text") == greetingReply &&
          store.tasks().empty() && browser.get(dm).at("messages").empty(),"ordinary user chat forwarded without a Bot decision");

    const auto receipt = browser.post("/api/messages",{{"botId","ernest"},{"clientMessageId","rollout"},{"text",request}},201);
    check(store.tasks().empty() && browser.get(dm).at("messages").empty(),"HTTP intake started a Bot-to-Bot exchange");
    const auto received = browser.get("/api/view?bot=ernest").at("events");
    const auto userMessage = std::ranges::find_if(received,[&](const Json& event) {
        return event.at("id") == receipt.at("messageId") && event.at("kind") == "conversation_message";
    });
    check(userMessage != received.end() && userMessage->at("data").at("text") == request,
          "the original user message was not saved verbatim");
    original = {{"text",userMessage->at("data").at("text")},{"time",userMessage->at("time")}};
    runtime.routeConversation();
    check(conversationReply(browser.get("/api/view?bot=ernest"),receipt.at("messageId")).at("route") == "create_task" &&
          store.tasks("ernest").size() == 1 && store.tasks("reviewer").empty() && browser.get(dm).at("messages").empty(),
          "routing the user's request bypassed Ernest's decision and contacted Reviewer");

    tick(runtime,8);
    const auto ernest = browser.get("/api/view?bot=ernest");
    const auto& events = ernest.at("events");
    check(ernest.at("bot").at("id") == "ernest" && hasEvent(events,"conversation_message",request) &&
          (hasEvent(events,"completed",response) || hasEvent(events,"conversation_reply",response)),
          "Ernest's normal conversation did not receive the answer to the user's request");
    const auto messages = browser.get(dm).at("messages");
    check(messages.size() == 2 && messages == browser.get("/api/peer-conversation?bot=reviewer&peer=ernest").at("messages"),
          "the exchange was not the same two-message thread from both Bots");
    const auto& forwarded = messages.front().at("data");
    const auto& answered = messages.back().at("data");
    check(forwarded.at("fromBotId") == "ernest" && forwarded.at("toBotId") == "reviewer" &&
          forwarded.value("text",std::string{}) == note && forwarded.at("forwardedMessage") == original &&
          answered.at("fromBotId") == "reviewer" && answered.at("toBotId") == "ernest" && answered.at("text") == review &&
          !forwarded.contains("initiator") && !answered.contains("recipient") && !answered.contains("forwardedMessage") &&
          t3.launches == 0,"the exchange did not preserve the forward, its Bot authors, or the reply");
    check(std::ranges::none_of(messages,[](const Json& row) {
        return row.contains("taskId") || row.contains("taskBotId") || row.at("data").contains("evidence");
    }),"the DM exposed task or evidence details");
}

struct StopModel : FixedModel {
    Id other = 0;
    bool honorSelection = false;
    Decision decide(const ModelInput& input) override {
        if (input.event["kind"] != "route_conversation") return FixedModel::decide(input);
        const auto& event = input.event;
        const auto& reply = event.at("reply");
        if (reply.at("kind") == "clarification" && honorSelection) {
            check(reply.contains("selectedTaskId"),"the model did not receive the user's hidden choice");
            return {"route_conversation",{{"routes",Json::array({{{"kind","append_instruction"},
                    {"taskId",reply.at("selectedTaskId")}}})},{"resolution","resolved"}},{}};
        }
        if (reply.at("kind") == "clarification") {
            return {"route_conversation",{{"routes",Json::array({{{"kind","append_instruction"},{"taskId",other}}})},
                    {"resolution","resolved"}},{}};
        }
        check(event.at("text") == "ログインの件を調整して","Unexpected route in stop fixture");
        Json candidates = Json::array();
        for (const auto& task : event.at("tasks"))
            if (task.at("state") == "running") candidates.push_back(task.at("taskId"));
        return {"route_conversation",{{"routes",Json::array({{{"kind","clarify"},{"text","どちらの依頼ですか？"},
                {"candidates",candidates}}})}},{}};
    }
};

bool exposesTaskNumber(const std::string& text, Id task) {
    return text.find("#" + std::to_string(task)) != std::string::npos;
}

void sameNameStop(const std::filesystem::path& path) {
    Store store(path);
    StopModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return BotNotes{}; });
    auto server = startWeb(store,0,callbacks());
    Browser browser(*server);
    const auto first = store.submit({"triage","ログイン画面の修正","linked env","login-a"});
    const auto second = store.submit({"triage","ログイン画面の修正","linked env","login-b"});
    tick(runtime,4);
    check(store.task(first)["state"] == "running" && store.task(second)["state"] == "running",
          "same-name fixtures did not both start");

    const auto stop = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","stop-login"},
        {"text","「ログイン画面の修正」を中止して"}},201);
    runtime.routeConversation();
    auto page = browser.get("/api/view?bot=triage");
    const auto question = conversationReply(page,stop.at("messageId"));
    const auto questionText = question.at("text").get<std::string>();
    check(question.at("route") == "clarify" && questionText.find("ログイン画面の修正") != std::string::npos &&
          !exposesTaskNumber(questionText,first) && !exposesTaskNumber(questionText,second),
          "same-name stop request was not confirmed by request name");
    check(store.task(first)["state"] == "running" && store.task(second)["state"] == "running",
          "an ambiguous stop request stopped a request before confirmation");
    check(page.at("clarifications").size() == 1,"stop confirmation was not listed as waiting for the user");
    const auto& clarification = page.at("clarifications").at(0);
    const Id anchor = clarification.at("messageId");
    check(clarification.at("candidates").size() == 2,"stop confirmation did not offer both requests");
    for (const auto& candidate : clarification.at("candidates"))
        check(candidate.at("request") == "ログイン画面の修正" && candidate.at("createdAt").get<Seconds>() > 0 &&
              candidate.at("state") == "running","same-name candidates lacked the facts that tell them apart");

    const auto label = "ログイン画面の修正（後から依頼した方）";
    browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","no-anchor"},{"text",label},
        {"candidateTaskId",second}},400);
    const auto forged = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","forged"},{"text",label},
        {"replyToMessageId",anchor},{"candidateTaskId",second + 100}},201);
    runtime.routeConversation();
    check(conversationReply(browser.get("/api/view?bot=triage"),forged.at("messageId")).at("route") == "respond" &&
          store.task(first)["state"] == "running" && store.task(second)["state"] == "running" &&
          store.clarifications("triage").size() == 1,"a choice outside the confirmation reached a request");

    const Json choice = {{"botId","triage"},{"clientMessageId","choose-second"},{"text",label},
                         {"replyToMessageId",anchor},{"candidateTaskId",second}};
    const auto chosen = browser.post("/api/messages",choice,201);
    check(browser.post("/api/messages",choice,201) == chosen,"retrying a choice did not return its receipt");
    auto switched = choice;
    switched["candidateTaskId"] = first;
    browser.post("/api/messages",switched,409);
    runtime.routeConversation();
    page = browser.get("/api/view?bot=triage");
    const auto stopped = conversationReply(page,chosen.at("messageId"));
    const auto stoppedText = stopped.at("text").get<std::string>();
    check(stopped.at("route") == "cancel_task" && stoppedText.find("ログイン画面の修正") != std::string::npos &&
          !exposesTaskNumber(stoppedText,second),"the chosen stop was not confirmed by request name");
    const auto secondState = store.task(second)["state"];
    check(store.task(first)["state"] == "running" && (secondState == "cancelling" || secondState == "cancelled") &&
          store.clarifications("triage").empty(),"the choice did not stop only the selected request");

    const auto stale = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","stale-choice"},{"text",label},
        {"replyToMessageId",anchor},{"candidateTaskId",first}},201);
    runtime.routeConversation();
    const auto staleReply = conversationReply(browser.get("/api/view?bot=triage"),stale.at("messageId"));
    check(staleReply.at("route") == "respond" && store.task(first)["state"] == "running",
          "a reply to a closed confirmation reached another request");

    const auto third = store.submit({"triage","ログイン画面の文言","linked env","login-c"});
    tick(runtime,4);
    check(store.task(third)["state"] == "running","the third request did not start");
    model.other = third;
    const auto adjust = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","adjust"},
        {"text","ログインの件を調整して"}},201);
    runtime.routeConversation();
    const auto adjustQuestion = conversationReply(browser.get("/api/view?bot=triage"),adjust.at("messageId"));
    check(adjustQuestion.at("route") == "clarify","the model clarification fixture did not ask");
    const auto misrouted = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","misrouted"},
        {"text","ログイン画面の修正"},{"replyToMessageId",adjustQuestion.at("messageId")},{"candidateTaskId",first}},201);
    runtime.routeConversation();
    const auto misroutedReply = conversationReply(browser.get("/api/view?bot=triage"),misrouted.at("messageId"));
    const auto untouched = [&](Id task) { return store.task(task).value("pendingInstructions",Json::array()).empty(); };
    check(misroutedReply.at("route") == "clarify" && untouched(first) && untouched(third),
          "routing applied a chosen reply to a request the user did not choose");

    model.honorSelection = true;
    const auto honored = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","honored"},
        {"text","ログイン画面の修正"},{"replyToMessageId",adjustQuestion.at("messageId")},{"candidateTaskId",first}},201);
    runtime.routeConversation();
    const auto honoredReply = conversationReply(browser.get("/api/view?bot=triage"),honored.at("messageId"));
    check(honoredReply.at("route") == "append_instruction" && !untouched(first) && untouched(third),
          "a chosen candidate did not resolve a non-stop clarification for that request only");
}
#ifdef FLOCK_TEST_CHATGPT
void chatGPTConversation(const std::filesystem::path& home) {
    Store store(home / "conversation.sqlite");
    app::ChatGPTModel model(home / "credentials" / "chatgpt");
    const app::Bots bots(home / "bots");
    FixedT3 t3;
    RuntimeOptions options;
    options.peers = [&](std::string_view bot) { return bots.peers(bot); };
    Runtime runtime(store,model,t3,[&](std::string_view bot) { return bots.notes(bot); },options);
    const auto server = startWeb(store,0,{
        [&] { return bots.cards(); },
        [&](std::string_view bot,std::string_view request) { return bots.get(bot).intake(request); },
    });
    Browser browser(*server);
    const auto send = [&](std::string_view key,std::string_view text) -> Id {
        const auto receipt = browser.post("/api/messages",{{"botId","release"},{"clientMessageId",key},{"text",text}},201);
        check(receipt.at("status") == "received","composer did not acknowledge the message");
        return receipt.at("messageId");
    };
    const auto view = [&] { return browser.get("/api/view?bot=release"); };
    const auto task = [&](Id id) { return browser.get("/api/task?bot=release&taskId=" + std::to_string(id)); };
    const auto linkedTask = [&](Id messageId) -> Id {
        const auto page = view();
        const auto& events = page.at("events");
        const auto message = std::ranges::find_if(events,[&](const Json& event) {
            return event.at("kind") == "conversation_message" && event.at("id") == messageId;
        });
        check(message != events.end() && message->at("taskId").is_number_integer(),
              "composer message was not linked to its Task");
        return message->at("taskId");
    };

    const auto request = send("blockers","Summarize the release blockers.");
    check(store.tasks().empty(),"composer created a Task before routing");
    tick(runtime,5);
    check(conversationReply(view(),request).at("route") == "create_task" && store.tasks().size() == 1,
          "ordinary composer request did not create exactly one Task");
    const auto releaseTask = linkedTask(request);
    check(task(releaseTask).at("state") == "running" && t3.launches == 1,
          "the Task did not start one Run");
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed",
        .report = "The signing certificate expired on 18 November.",
        .itemId = "blockers-report", .settled = true
    };
    tick(runtime,3);
    const auto completed = task(releaseTask);
    check(completed.at("state") == "completed","the Session report did not become a completed result");
    const auto& result = completed.at("result");
    check(result.at("data").at("run").at("itemId") == "blockers-report","the result lost the report item");
    t3.observations.clear();

    const auto question = send("why-blocked","Why is the release blocked?");
    tick(runtime,3);
    const auto answer = conversationReply(view(),question);
    check(answer.at("route") == "respond" && answer.at("text") == "The release is blocked by the expired signing certificate." &&
          answer.at("source").at("taskId") == releaseTask && answer.at("source").at("messageId") == result.at("id"),
          "result answer was not grounded in the saved report");
    const auto missing = send("renewal-date","When will the signing certificate be renewed?");
    tick(runtime,3);
    const auto clarification = conversationReply(view(),missing);
    check(clarification.at("route") == "clarify" && clarification.at("text") == "What is the planned certificate renewal date?" &&
          clarification.at("source").at("messageId") == result.at("id"),
          "missing evidence did not produce a clarification linked to the result");
    check(store.tasks().size() == 1 && t3.launches == 1 && task(releaseTask).at("runs") == completed.at("runs"),
          "result follow-ups created a Task or changed Runs");

    const auto consult = send("consult-verifier","Ask Verifier to confirm the release criteria.");
    tick(runtime,8);
    const auto consultTask = linkedTask(consult);
    const auto page = view();
    Json messages = Json::array();
    for (const auto& event : page.at("events"))
        if (event.at("kind") == "bot_message")
            messages.push_back(event.at("data"));
    check(messages.size() == 2,"the originating conversation did not show one request and one peer reply");
    const auto& outgoing = messages.at(0);
    const auto& incoming = messages.at(1);
    check(outgoing.at("fromBotId") == "release" && outgoing.at("toBotId") == "verifier" &&
          outgoing.at("requestKind") == "consult" && incoming.at("fromBotId") == "verifier" &&
          incoming.at("messageType") == "result" && incoming.at("toBotId") == "release" &&
          incoming.at("exchangeId") == outgoing.at("exchangeId") && incoming.at("replyTo") == outgoing.at("messageId") &&
          incoming.at("text") == "Publish only after the certificate is renewed.",
          "Verifier's reply did not return to its original exchange");
    check(task(consultTask).at("state") == "completed" && store.tasks("verifier").size() == 1 && t3.launches == 1,
          "the originating Task did not resume after the consultation");

    const auto first = send("notes","Prepare release notes.");
    const auto second = send("checksums","Check artifact checksums in the backup environment.");
    tick(runtime,5);
    const auto firstTask = task(linkedTask(first));
    const auto secondTask = task(linkedTask(second));
    const auto& firstRuns = firstTask.at("runs");
    const auto& secondRuns = secondTask.at("runs");
    check(!firstRuns.empty() && !secondRuns.empty(),"both composer requests must launch a Run");
    check(firstRuns.back().at("environmentId") == "local-env" && secondRuns.back().at("environmentId") == "backup-env",
          "the composer Runs did not keep each request on its own destination");
    const auto active = view().at("activeSessions");
    check(firstTask.at("state") == "running" && secondTask.at("state") == "running" &&
          active.size() == 2 && t3.launches == 3,"ambiguous-stop fixture requires two active Runs: " +
          firstTask.at("state").dump() + ", " + secondTask.at("state").dump() + ", active=" + active.dump() +
          ", launches=" + std::to_string(t3.launches));
    const auto taskCount = store.tasks().size();
    const auto stop = send("stop","それ止めて");
    tick(runtime,3);
    const auto afterStop = view();
    const auto confirm = conversationReply(afterStop,stop);
    const auto& candidates = confirm.at("candidates");
    check(confirm.at("route") == "clarify" && confirm.at("text") == "どの作業を停止しますか？" && candidates.size() == 2 &&
          std::ranges::all_of(active,[&](const Json& run) {
              return std::ranges::any_of(candidates,[&](const Json& candidate) { return candidate.at("taskId") == run.at("taskId"); });
          }),"ambiguous stop did not ask the user to choose between both running tasks");
    check(afterStop.at("activeSessions") == active && store.tasks().size() == taskCount && t3.launches == 3 &&
          task(firstTask.at("id")).at("runs") == firstTask.at("runs") &&
          task(secondTask.at("id")).at("runs") == secondTask.at("runs") && t3.interruptions == 0,
          "ambiguous stop changed a Run or created work");
}
#endif
void approveModelCallBeforeTask(const std::filesystem::path& path) {
    Store webStore(path);
    auto store = std::make_unique<Store>(path);
    FixedModel model;
    FixedT3 t3;
    const auto start = [&] {
        return std::make_unique<Runtime>(*store,model,t3,[](auto) { return BotNotes{}; });
    };
    auto runtime = start();
    auto webCallbacks = callbacks();
    webCallbacks.intake = [original = webCallbacks.intake](std::string_view bot, std::string_view request) {
        auto intake = original(bot,request);
        intake.config["modelCallLimit"] = 2;
        return intake;
    };
    auto server = startWeb(webStore,0,std::move(webCallbacks));
    Browser browser(*server);
    const auto task = browser.request("release",comparisonRequest);
    tick(*runtime,3);
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished,.status = "completed",.report = proposedOptions,
        .itemId = "comparison-report",.settled = true
    };
    tick(*runtime,2);
    model.comparisonResult = store->latestTaskResult(task).at("id");
    const auto receipt = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","compound"},
        {"text",compoundExplanation}},201);
    runtime->routeConversation();
    const auto waiting = browser.get("/api/view?bot=release");
    const auto wait = waiting.at("clarifications").at(0);
    const auto reply = conversationReply(waiting,receipt.at("messageId"));
    check(wait.at("route") == "model_budget" && wait.at("messageId") == reply.at("messageId") &&
          model.routed == 1 && model.explained == 1 && model.summarized == 0,"third call reached the fixture");
    runtime.reset();
    store = std::make_unique<Store>(path);
    runtime = start();
    tick(*runtime,3);
    check(model.routed == 1 && model.explained == 1 && model.summarized == 0,"waiting message resumed without approval");

    const auto independent = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","independent"},
        {"text","Who are you?"}},201);
    runtime->routeConversation();
    check(conversationReply(browser.get("/api/view?bot=release"),independent.at("messageId")).at("route") == "respond" &&
          model.routed == 2,"a paused message blocked an independent message to the same Bot");

    Json approval = {{"botId","triage"},{"waitMessageId",wait.at("messageId")},{"operation","approve"}};
    browser.post("/api/operations",approval,409);
    approval["botId"] = "release";
    browser.post("/api/operations",approval);
    browser.post("/api/operations",approval,409);
    runtime->routeConversation();
    const auto resumed = browser.get("/api/view?bot=release");
    const auto& answer = resumed.at("events").back().at("data");
    check(resumed.at("clarifications").empty() && answer.at("route") == "compound" &&
          answer.at("replyToMessageId") == receipt.at("messageId") &&
          model.routed == 2 && model.explained == 1 && model.summarized == 1 &&
          store->tasks().size() == 1 && t3.launches == 1,
          "approval did not resume the unfinished explanation from its saved routing decisions");
}

}

int main(int argc,char** argv) {
    try {
#ifdef FLOCK_TEST_CHATGPT
        if (argc == 2) {
            chatGPTConversation(argv[1]);
            std::cout << "web ChatGPT conversation behavior passed\n";
            return 0;
        }
#endif
        TestDirectory directory;
        destinationChoicesInView(directory.path / "destination.sqlite");
        conversationEntry(directory.path / "conversation.sqlite");
        botMediatedConversation(directory.path / "bot-mediated-with-note.sqlite","確認手順の妥当性を確認してください。");
        botMediatedConversation(directory.path / "bot-mediated-without-note.sqlite",{});
        sameNameStop(directory.path / "stop.sqlite");
        approveModelCallBeforeTask(directory.path / "model-budget.sqlite");
        std::cout << "web conversation behavior passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
