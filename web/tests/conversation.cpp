#include "flock/web.hpp"

#ifdef FLOCK_TEST_CHATGPT
#include "bots.hpp"
#include "model.hpp"
#endif

#include <httplib.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <ranges>

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


std::string statusText(const ModelInput& input) {
    std::string text;
    for (const auto& task : input.task.at("tasks")) {
        if (!text.empty()) text += '\n';
        text += "- " + task.at("request").get<std::string>() + " ／ " + (task.at("state") == "completed" ? "完了" : "作業中");
    }
    return text;
}

struct FixedModel : Model {
    unsigned turns = 0, explained = 0, summarized = 0;
    Id comparisonResult = 0;
    std::size_t summarySourceSize = 0;

    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        if (input.event["kind"] == "conversation_turn") return converse(input);
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

    Decision converse(const ModelInput& input) {
        ++turns;
        const auto& event = input.event;
        check(event.dump().find(privateValue) == std::string::npos && input.task.dump().find(privateValue) == std::string::npos &&
              input.history.dump().find(privateValue) == std::string::npos,"Connection settings entered model-visible context");
        const auto text = event.at("text").get<std::string>();
        const auto& steps = event.at("turn");
        const auto reply = [](std::string message, Json suggestions = nullptr) {
            Json args = {{"text",std::move(message)}};
            if (!suggestions.is_null()) args["suggestions"] = std::move(suggestions);
            return Decision{"reply",std::move(args),"Reply to the user"};
        };
        if (text == "2案目を詳しく" || text == "2案目を要約して" || text == compoundExplanation) {
            if (steps.empty()) return {"read_result",{{"messageId",comparisonResult}},"Read the comparison"};
            const auto& source = steps[0].at("result").at("text").get_ref<const std::string&>();
            check(source == proposedOptions,"The comparison was not readable in full");
            if (text == compoundExplanation && steps.size() == 1)
                return {"read_result",{{"messageId",comparisonResult}},"Read it again for the summary"};
            if (text == "2案目を要約して") {
                ++summarized;
                summarySourceSize = source.size();
                return reply("ID で明細の差分を集計し、金額変更を人が確認する案です。");
            }
            ++explained;
            if (text == compoundExplanation) ++summarized;
            return reply("差分を先に集計する案です。ID ごとに前回と今回の記録を比べるので、"
                         "追加・削除・更新を分けて数えられます。そのうち金額が変わった更新だけを人が確かめる手順です。");
        }
        if (text == missingBudgetQuestion) return reply("見積もりに必要な明細の件数を教えてください。");
        if (text == budgetDetail) {
            check(input.history.dump().find("見積もりに必要な明細の件数") != std::string::npos,
                  "The follow-up did not see the Bot's earlier question");
            return reply("100件の明細を照合し、金額が変わった分の確認時間を見積もります。");
        }
        if (text == "Who are you?") return reply("I'm the release Bot.");
        if (text == "進捗どうなってる") return reply(statusText(input));
        if (text == "Prepare the notes in local-env or backup") {
            Json choices = Json::array();
            for (const auto& environment : input.task.at("environments")) choices.push_back(environment.at("name"));
            return reply("どちらの実行先で準備しますか？",choices);
        }
        if (text == "backup") {
            if (steps.empty()) return {"start_task",{{"request","Prepare the notes"},{"environment","backup"}},"Use the chosen destination"};
            return reply("backup で準備を始めました。");
        }
        const auto& anchor = event.at("replyTo");
        check(anchor.is_object() && anchor.at("kind") == "task_question","Unexpected message in HTTP fixture: " + text);
        if (!steps.empty()) return reply(steps.back().at("result").value("ok",false) ? "回答を伝えました。" : "その質問は終了しています。");
        if (!anchor.at("current").get<bool>()) return reply("その質問は終了しています。");
        const auto answer = text == "利用者向けのみ" ? "すべて" : text;
        return {"answer_task_question",{{"taskId",anchor.at("taskId")},{"waitId",anchor.at("waitId")},{"text",answer},
            {"revisionId",anchor.at("revisionId")},{"generation",anchor.at("generation")}},"Answer the question"};
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
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override { ++interruptions; return InterruptOutcome::Requested; }
};

WebCallbacks callbacks(const std::atomic_bool* intakeDisabled = nullptr) {
    return {
        [] {
            return Json::array({
                {{"id","release"},{"name","Release"},{"soul","利用者向けのリリースノートをまとめる。"},
                    {"cron",Json::array()},
                    {"avatar",{{"example","mug"},{"color","#982fff"},{"secret",privateValue}}},{"token",privateValue}},
                {{"id","triage"},{"name","Triage"},{"soul","報告された問題を整理する。"},{"cron",Json::array()}},
                {{"id","settings"},{"name","Settings"},{"soul","設定エラーの確認"}},
                {{"id","broken"},{"name","Broken"}},
            });
        },
        [intakeDisabled](std::string_view bot, std::string_view request) -> Intake {
            if (bot == "broken" || (intakeDisabled && intakeDisabled->load())) throw std::runtime_error(privateValue);
            return {std::string(bot),std::string(request),bot == "triage" ? "linked env" : "local-env",
                std::string(bot),{{"token",privateValue},{"runTimeoutSeconds",1800}}};
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
        return post("/api/requests",{{"botId",bot},{"clientRequestId",actionId()},{"request",text}},201).at("taskId");
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

Json conversationCompletion(const Json& page, Id taskId) {
    const auto& events = page.at("events");
    const auto completion = std::ranges::find_if(events,[&](const Json& event) {
        return event.at("kind") == "completed" && event.at("taskId") == taskId;
    });
    check(completion != events.end(),"the conversation omitted the completion");
    return *completion;
}

void writeWebIdentities(const std::filesystem::path& path, const Json& accounts) {
    std::ofstream(path) << accounts;
    std::filesystem::permissions(path,std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

void inspectors(const std::filesystem::path& path) {
    struct InspectorModel : Model {
        const std::string credential = "credential-protected-after-inspector-write";
        Id boundary = 0;
        ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
        Decision choose(const ModelInput& input) {
            if (input.event.at("turn").empty() && input.event.at("text")=="Summarize")
                return {"compact_conversation",{{"endMessageId",boundary},{"summary","Preserve the release notes "+credential}},"Summarize earlier work"};
            if (input.event.at("turn").empty() && input.event.at("text")=="Release")
                return {"start_workflow",{{"request","Prepare a release "+credential}},"Use a separate verifier"};
            return {"reply",{{"text","Recorded"}},"Acknowledge the request"};
        }
    } model;
    Store store(path);
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](auto){return std::string{};});
    const auto routing = callbacks().intake("release","");
    auto server = startWeb(store,0,callbacks());
    Browser browser(*server);
    model.boundary = store.receive({"fixture",true},routing,"a","Original release instructions").at("messageId");
    store.receive({"fixture",true},routing,"b","Summarize");
    store.receive({"fixture",true},routing,"c","Release");
    runtime.runConversations();
    auto access = store.policy();
    access.submitSecret({"fixture",true},"release",access.requestSecret("release",{"inspector-token"}),model.credential);
    const auto summaries = browser.get("/api/compactions?bot=release");
    const auto workflows = browser.get("/api/workflows?bot=release");
    const auto view = browser.get("/api/view?bot=release");
    check(summaries.size()==1 && summaries[0]["endMessageId"]==model.boundary &&
          summaries[0]["summary"]=="Preserve the release notes [redacted]" && view.at("compactions")==summaries,
          "summary or its covered range missing from the inspector");
    check(workflows.size()==1 && workflows[0]["request"]=="Prepare a release [redacted]" &&
          workflows[0]["stages"].size()==2 && view.at("workflows")==workflows,
          "workflow stages missing from the Bot view");
    check(browser.get("/api/compactions?bot=triage").empty() && browser.get("/api/workflows?bot=triage").empty(),
          "inspectors mixed Bot histories");
    const auto audit = browser.get("/api/conversation?bot=release");
    check(audit.dump().find("Original release instructions")!=std::string::npos,
          "summary removed the original message from the HTTP audit history");
    bool linked = false;
    for (const auto& event : audit.at("events")) {
        const auto& data = event.at("data");
        if (!data.contains("actions")) continue;
        for (const auto& action : data.at("actions"))
            if (action.at("tool")=="start_workflow" && action.at("workflowId")==workflows[0]["workflowId"])
                linked = data.at("status")=="working";
    }
    check(linked,"workflow start was missing from the conversation reply");
}

void imageAttachmentCountInConversation(const std::filesystem::path& path) {
    Store store(path);
    auto config = callbacks();
    config.authenticate = [](std::string_view credential) {
        if (credential == "Bearer owner") return Person{"owner",true};
        if (credential == "Bearer reader") return Person{"reader",false};
        return Person{};
    };
    config.resolveIdentity = [](std::string_view person) -> std::optional<bool> {
        if (person == "owner") return true;
        if (person == "reader") return false;
        return std::nullopt;
    };
    store.policy().setMember({"owner",true},"release","reader",true);
    const auto server = startWeb(store,0,std::move(config));
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization","Bearer owner"}});
    const Json image = {{"name","private-screenshot.png"},{"mimeType","image/png"},
        {"dataBase64","iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"}};
    const Json audio = {{"name","private-audio.mp3"},{"mimeType","audio/mpeg"},{"dataBase64","SUQzAA=="}};
    const auto submit = [&](std::string_view key, const Json& attachments) {
        return browser.post("/api/messages",{{"botId","release"},{"clientMessageId",key},
            {"text","Look at this"},{"attachments",attachments}},201).at("messageId").get<Id>();
    };
    const auto single = submit("a",Json::array({image,audio}));
    const auto multiple = submit("b",Json::array({image,image}));
    const auto textOnly = submit("c",Json::array());
    auto noncanonicalAudio = audio;
    noncanonicalAudio["dataBase64"] = "SUQzAB==";
    browser.post("/api/messages",{{"botId","release"},{"clientMessageId","invalid-padding"},
        {"text","Look at this"},{"attachments",Json::array({noncanonicalAudio})}},409);
    const auto assertCounts = [](const Json& events, const std::map<Id,int>& expected) {
        auto remaining = expected;
        for (const auto& event : events) {
            const auto match = remaining.find(event.at("id").get<Id>());
            if (match == remaining.end()) continue;
            const auto& data = event.at("data");
            check(data.at("imageAttachmentCount") == match->second,"wrong saved image count in the conversation");
            check(!data.contains("attachments") && !data.contains("dataBase64") &&
                  data.dump().find("private-screenshot") == std::string::npos,
                  "Web conversation exposed private attachment details");
            remaining.erase(match);
        }
        check(remaining.empty(),"messages were missing from the conversation");
    };
    constexpr auto conversation = "/api/conversation?bot=release";
    browser.client.set_default_headers({});
    Browser::checked(browser.client.Get(conversation),401);
    browser.client.set_default_headers({{"Authorization","Bearer reader"}});
    assertCounts(browser.get("/api/view?bot=release").at("events"),{{single,1},{multiple,2},{textOnly,0}});
    assertCounts(browser.get(std::string(conversation) + "&before=" + std::to_string(multiple)).at("events"),{{single,1}});
    Browser::checked(browser.client.Get("/api/conversation?bot=triage"),403);
}

void suggestionsAndEnvironmentsInView(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
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
    const auto question = send("request-destination","Prepare the notes in local-env or backup",0);
    runtime.runConversations();
    const auto page = browser.get("/api/view?bot=release");
    const auto asked = conversationReply(page,question.at("messageId"));
    check(asked.at("suggestions") == Json::array({"local-env","backup"}) && page.at("pausedConversations").empty() &&
          page.at("waits").empty() && store.tasks().empty(),
          "the Bot's question was not an ordinary message with its suggested replies");
    const auto receipt = send("choose-backup","backup",asked.at("messageId"));
    tick(runtime,4);
    const auto resolved = browser.get("/api/view?bot=release");
    const auto tasks = store.tasks("release");
    check(tasks.size() == 1 && conversationReply(resolved,receipt.at("messageId")).at("actions").at(0).at("tool") == "start_task" &&
          store.task(tasks[0]["id"]).at("runs").back().at("environmentId") == "backup-env",
          "the chosen destination did not receive the new Run");
}

void conversationEntry(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    std::atomic_bool disableIntake = false;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    auto server = startWeb(store,0,callbacks(&disableIntake));
    Browser browser(*server);
    const auto taskId = browser.request("release",comparisonRequest);
    tick(runtime,3);
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed", .report = proposedOptions,
        .itemId = "comparison-report", .settled = true
    };
    tick(runtime,2);
    check(store.task(taskId).at("state") == "completed","the comparison did not complete");
    const auto result = conversationCompletion(browser.get("/api/view?bot=release"),taskId);
    check(result.at("data").at("text") == proposedOptions,"the conversation lost the completion report");
    model.comparisonResult = result.at("id");
    const auto& reportRun = result.at("data").at("run");
    check(reportRun["threadId"] == "thread-release" && reportRun["runId"] == "run-1" && reportRun["itemId"] == "comparison-report",
          "the conversation did not link the Run and report item that produced the result");

    const Json message = {{"botId","release"},{"clientMessageId","options-more"},{"text","2案目を詳しく"}};
    const auto receipt = browser.post("/api/messages",message,201);
    const auto messageId = receipt.at("messageId").get<Id>();
    const auto cursor = browser.get("/api/view?bot=release").at("historyCursor").get<Id>();
    const auto since = "&changesAfter=" + std::to_string(cursor);
    check(receipt["status"] == "received" && messageId > 0,"conversation receipt was missing");
    check(model.turns == 0 && store.tasks().size() == 1,"HTTP intake ran the model before returning its receipt");
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
    const auto triage = browser.get("/api/view?bot=triage" + since);
    check(page.at("changedMessageIds") == Json::array({messageId}) && triage.at("changedMessageIds").empty(),
          "a processed message was not invalidated exclusively for its own Bot");
    check(triage.at("events").empty() && triage.at("sessionTasks").empty(),"another Bot could see the comparison");
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
    check(response["route"] == "reply" && responseText.find("差分を先に集計する") != std::string::npos &&
          responseText.find("金額が変わった更新だけを人が確かめる") != std::string::npos &&
          response["actions"].empty(),"answer did not explain the second option without changing work");
    check(questions == 1 && answers == 1 && store.tasks().size() == 1 && t3.launches == 1,
          "read-only conversation or retry created extra messages, Tasks or Runs");
    const auto& sessionTasks = page.at("sessionTasks");
    check(sessionTasks.size() == 1 && sessionTasks.at(0).at("id") == taskId &&
          sessionTasks.at(0).at("sessions").size() == 1 &&
          sessionTasks.at(0).at("sessions").at(0).at("runId") == reportRun.at("runId") &&
          !sessionTasks.at(0).contains("config"),"the Session list omitted the comparison Run or exposed config");
    const auto runs = store.task(taskId).at("runs");

    const auto summary = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","options-summary"},
        {"text","2案目を要約して"}},201);
    tick(runtime,3);
    const auto summarized = conversationReply(browser.get("/api/view?bot=release"),summary.at("messageId"));
    const auto summaryText = summarized.at("text").get<std::string>();
    check(summarized.at("route") == "reply" && summaryText.find("ID で明細の差分を集計") != std::string::npos &&
          summaryText.size() < model.summarySourceSize,"The summary did not condense the selected option");
    check(model.explained == 1 && model.summarized == 1 && store.tasks().size() == 1 && t3.launches == 1 &&
          store.task(taskId).at("runs") == runs,"Two read-only answers added work or missed the model calls");

    const auto estimate = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","estimate"},
        {"text",missingBudgetQuestion}},201);
    runtime.runConversations();
    const auto estimatePage = browser.get("/api/view?bot=release");
    const auto estimateReply = conversationReply(estimatePage,estimate.at("messageId"));
    check(estimateReply.at("text") == "見積もりに必要な明細の件数を教えてください。" &&
          estimatePage.at("pausedConversations").empty() && estimatePage.at("waits").empty() && store.tasks().size() == 1,
          "the Bot's question left pending state or started work");
    const auto detail = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","estimate-detail"},
        {"text",budgetDetail}},201);
    runtime.runConversations();
    const auto estimateAnswer = conversationReply(browser.get("/api/view?bot=release"),detail.at("messageId"));
    check(estimateAnswer.at("text").get<std::string>().find("100件") != std::string::npos &&
          store.tasks().size() == 1 && t3.launches == 1 && store.task(taskId).at("runs") == runs,
          "the follow-up answer created work or lost the context");

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
    check(delayedReply.at("status") == "done" && delayedReply.at("actions").empty(),
          "The late choice was left pending or applied an action");
    check(t3.replies.size() == 1 && t3.replies[0].environmentId == "linked env" &&
          t3.replies[0].target.threadId == "thread-triage" && t3.replies[0].target.runId == firstWait["run"]["runId"].get<std::string>() &&
          t3.replies[0].questionId == "scope-question" && t3.replies[0].answers == Json{{"scope",reply["text"]}},
          "T3 did not receive exactly one matching answer for the original question");
    const auto& waits = afterAnswer.at("waits");
    check(waits.size() == 1 && waits[0]["id"] == secondWaitTask && waits[0]["wait"]["messageId"] == otherQuestion &&
          waits[0]["wait"]["reason"] == "user_question" && store.tasks().size() == 3,
          "The freeform or stale answer changed another wait or created a task");

    const auto choice = browser.post("/api/messages",{{"botId","triage"},{"clientMessageId","answer-second"},
        {"text","利用者向けのみ"},{"replyToMessageId",otherQuestion}},201);
    tick(runtime,2);
    const auto afterChoice = browser.get("/api/view?bot=triage");
    const auto choiceReply = conversationReply(afterChoice,choice.at("messageId"));
    check(choiceReply.at("actions") == Json::array({{{"tool","answer_task_question"},{"effect","answered"},
          {"taskId",secondWaitTask}}}) && afterChoice.at("waits").empty() && t3.replies.size() == 2 &&
          t3.replies.back().target.threadId == "thread-next-scope" &&
          t3.replies.back().answers == Json{{"scope","利用者向けのみ"}},
          "The exact choice was replaced or reached a different Session");
}

void botMediatedConversation(const std::filesystem::path& path, std::string_view note, bool authenticated = false) {
    constexpr auto request = "出荷前に確認してください。\n\n署名付きビルドの確認手順を教えてください。";
    constexpr auto greeting = "こんにちは。";
    constexpr auto greetingReply = "こんにちは。出荷の準備についてお手伝いします。";
    constexpr auto review = "前のバージョンから更新し、署名が有効であることを確認してください。";
    constexpr auto response = "Reviewer に確認しました。前のバージョンから更新し、署名が有効であることを確認してください。";
    Json original;
    struct ForwardingModel final : Model {
        std::function<Decision(const ModelInput&)> script;
        ModelTurn decide(const ModelInput& input) override { return {{script(input)}}; }
    } model;
    model.script = [&](const ModelInput& input) -> Decision {
        const auto kind = input.event.at("kind");
        if (kind == "conversation_turn") {
            if (input.event.at("text") == greeting) return {"reply",{{"text",greetingReply}},"Greet the user"};
            check(input.event.at("text") == request,"the responsible Bot did not receive the user's original message");
            if (!input.event.at("turn").empty()) return {"reply",{{"text","確認します。"}},"Confirm"};
            return {"start_task",{{"request",input.event.at("text")}},"Ernest owns the request"};
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
    if (authenticated) {
        const Person author{"fixture-message-author",true};
        const auto identities = path.string() + ".identities";
        writeWebIdentities(identities,Json::array({
            {{"authorization","Bearer fixture-author"},{"person",author.id},{"adminRoot",true}},
            {{"authorization","Bearer fixture-reader"},{"person","fixture-shared-reader"}}
        }));
        configureWebIdentities(store,identities,web);
        for (const auto* bot : {"ernest","reviewer"})
            store.policy().setMember(author,bot,"fixture-shared-reader",true);
    }
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; },RuntimeOptions{.peers=peers});
    const auto server = startWeb(store,0,web);
    Browser browser(*server);
    if (authenticated) browser.client.set_default_headers({{"Authorization","Bearer fixture-author"}});
    constexpr auto dm = "/api/peer-conversation?bot=ernest&peer=reviewer";

    const auto hello = browser.post("/api/messages",{{"botId","ernest"},{"clientMessageId","hello"},{"text",greeting}},201);
    runtime.runConversations();
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
    runtime.runConversations();
    check(conversationReply(browser.get("/api/view?bot=ernest"),receipt.at("messageId")).at("actions").at(0).at("tool") == "start_task" &&
          store.tasks("ernest").size() == 1 && store.tasks("reviewer").empty() && browser.get(dm).at("messages").empty(),
          "the user's request bypassed Ernest's decision and contacted Reviewer");

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
    auto expectedQuote = original;
    if (authenticated) expectedQuote["fromViewer"] = true;
    check(forwarded.at("fromBotId") == "ernest" && forwarded.at("toBotId") == "reviewer" &&
          forwarded.value("text",std::string{}) == note && forwarded.at("forwardedMessage") == expectedQuote &&
          answered.at("fromBotId") == "reviewer" && answered.at("toBotId") == "ernest" && answered.at("text") == review &&
          !answered.contains("forwardedMessage") && t3.launches == 0,
          "the exchange did not preserve the forward, its Bot authors, or the reply");
    check(std::ranges::none_of(messages,[](const Json& row) {
        return row.contains("taskId") || row.contains("taskBotId") || row.at("data").contains("evidence");
    }),"the DM exposed task or evidence details");
    if (authenticated) {
        const auto anchored = std::string(dm) + "&anchor=" + std::to_string(messages.front().at("id").get<Id>());
        check(browser.get(anchored).at("messages") == messages,"the author lost attribution in anchored history");
        browser.client.set_default_headers({{"Authorization","Bearer fixture-reader"}});
        auto shared = messages;
        shared.front()["data"]["forwardedMessage"] = original;
        check(browser.get(anchored).at("messages") == shared &&
              browser.get("/api/peer-conversation?bot=reviewer&peer=ernest").at("messages") == shared,
              "a shared viewer saw authorship that belonged to another person");
        check(messages.dump().find("fixture-message-author") == std::string::npos &&
              shared.dump().find("fixture-shared-reader") == std::string::npos,"the DM exposed a person ID");
    }
}
#ifdef FLOCK_TEST_CHATGPT
void fullModeProfiles(const std::filesystem::path& directory) {
    Store store(directory/"state.db");
    app::Bots bots(directory/"bots");
    auto policy = store.policy();
    const Person creator{"creator"};
    policy.setOwner(creator,"release");
    struct ProfileModel final : Model {
        Decision change;
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("turn").empty()) return {{change}};
            return {{{"reply",{{"text","Processed the profile change"}},""}}};
        }
    } model;
    RuntimeOptions options;
    options.applyBotChange = [&](const BotChange& change) { bots.apply(change); };
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](auto) { return std::string{}; },options);
    const auto send = [&](std::string_view operation, std::string_view name) {
        model.change = {"request_bot_change",{{"operation",operation},{"targetBotId","reviewer"},{"name",name}},""};
        store.receive(creator,{"release","","local-env","release"},name,name);
        runtime.runConversations();
    };
    send("create_bot","Review team");
    check(bots.ids() == std::vector<std::string>{"reviewer"} &&
          policy.allows(creator,Action::bot_settings,"reviewer"),
          "Full did not apply a Bot creation with the requesting person's ownership");
    send("rename_bot","Reviewer");
    check(bots.cards().at(0).at("name") == "Reviewer" && store.inbox(creator).empty(),
          "Full did not apply a Bot rename directly");
    policy.changeNeverRule(localOwner,Action::bot_rename,"review*",true);
    send("rename_bot","Other name");
    check(bots.cards().at(0).at("name") == "Reviewer","Never rule did not block the profile write");
}

void chatGPTConversation(const std::filesystem::path& home) {
    const app::Bots bots(home / "bots");
    Store store(home / "conversation.sqlite",[&](std::string_view bot) { return bots.initialMemory(bot); });
    app::ChatGPTModel model(home / "credentials" / "chatgpt");
    FixedT3 t3;
    RuntimeOptions options;
    options.peers = [&](std::string_view bot) { return bots.peers(bot); };
    Runtime runtime(store,model,t3,[&](std::string_view bot) { return bots.soul(bot); },options);
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
    check(store.tasks().empty(),"composer created a Task before the Bot's turn");
    tick(runtime,5);
    check(conversationReply(view(),request).at("actions").at(0).at("tool") == "start_task" && store.tasks().size() == 1,
          "ordinary composer request did not create exactly one Task");
    const auto releaseTask = linkedTask(request);
    check(store.task(releaseTask).at("state") == "running" && t3.launches == 1,
          "the Task did not start one Run");
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed",
        .report = "The signing certificate expired on 18 November.",
        .itemId = "blockers-report", .settled = true
    };
    tick(runtime,3);
    const auto completed = store.task(releaseTask);
    check(completed.at("state") == "completed","the Session report did not become a completed result");
    const auto result = conversationCompletion(view(),releaseTask);
    check(result.at("data").at("run").at("itemId") == "blockers-report","the result lost the report item");
    t3.observations.clear();

    const auto question = send("why-blocked","Why is the release blocked?");
    tick(runtime,3);
    const auto answer = conversationReply(view(),question);
    check(answer.at("route") == "reply" && answer.at("text") == "The release is blocked by the expired signing certificate." &&
          answer.at("actions").empty(),"result answer was not grounded in the saved report");
    const auto missing = send("renewal-date","When will the signing certificate be renewed?");
    tick(runtime,3);
    const auto clarification = conversationReply(view(),missing);
    check(clarification.at("text") == "What is the planned certificate renewal date?",
          "missing evidence did not produce a question for the user");
    check(store.tasks().size() == 1 && t3.launches == 1 && store.task(releaseTask).at("runs") == completed.at("runs"),
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
    check(store.task(consultTask).at("state") == "completed" && store.tasks("verifier").size() == 1 && t3.launches == 1,
          "the originating Task did not resume after the consultation");

    const auto first = send("notes","Prepare release notes.");
    const auto second = send("checksums","Check artifact checksums in the backup environment.");
    tick(runtime,5);
    const auto firstTask = store.task(linkedTask(first));
    const auto secondTask = store.task(linkedTask(second));
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
    const auto& suggestions = confirm.at("suggestions");
    check(confirm.at("text") == "どの作業を停止しますか？" && suggestions.size() == 2 &&
          std::ranges::all_of(active,[&](const Json& run) {
              return std::ranges::find(suggestions,run.at("request")) != suggestions.end();
          }),"ambiguous stop did not ask the user to choose between both running tasks");
    check(afterStop.at("activeSessions") == active && store.tasks().size() == taskCount && t3.launches == 3 &&
          store.task(firstTask.at("id")).at("runs") == firstTask.at("runs") &&
          store.task(secondTask.at("id")).at("runs") == secondTask.at("runs") && t3.interruptions == 0,
          "ambiguous stop changed a Run or created work");
}
#endif

void secretCollisionKeepsBotRoutes(const std::filesystem::path& path) {
    Store store(path);
    auto cb = callbacks();
    cb.bots = [] {
        return Json::array({{{"id","release"},{"name","Release"}},
                            {{"id","staging-triage"},{"name","Staging"},{"soul","Check staging"}}});
    };
    cb.authenticate = [](std::string_view auth) {
        if (auth == "Bearer fixture-owner") return Person{"owner",true};
        if (auth == "Bearer fixture-alice") return Person{"alice"};
        return Person{};
    };
    cb.updateBot = [](std::string_view id, const Json& profile, const std::optional<AvatarImage>&) {
        return Json{{"id",id},{"name",profile.at("name")}};
    };
    const Person owner{"owner",true};
    auto access = store.policy();
    access.setMember(owner,"staging-triage","alice",true);
    const auto request = access.requestSecret("release",{"fixture-token"});
    const auto send = [&](std::string_view bot, std::string_view clientId, std::string_view text) {
        return store.receive(owner,{std::string(bot),"unused","local-env","fixture"},clientId,text)
            .at("messageId").get<Id>();
    };
    const auto oldMessage = send("staging-triage","old","Original handoff for staging");
    const auto otherBotMessage = send("release","other","Unrelated handoff");
    for (int i = 0; i < 40; ++i) send("staging-triage","filler-" + std::to_string(i),"Later update");
    auto server = startWeb(store,0,cb);
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization","Bearer fixture-owner"}});
    browser.post("/api/access/secret",{{"botId","release"},{"requestId",request},{"value","staging"}});
    const auto saved = browser.post("/api/bots/profile",{{"botId","staging-triage"},{"name","Triage"}});
    check(saved.at("id")=="staging-triage","profile route changed the Bot ID");

    browser.client.set_default_headers({{"Authorization","Bearer fixture-alice"}});
    const auto bots = browser.get("/api/bots");
    check(bots.size()==1 && bots.at(0).at("id")=="staging-triage","secret redaction hid or renamed an authorized Bot");
    const auto view = browser.get("/api/view?bot=staging-triage").at("bot");
    check(view.at("id")=="staging-triage" && view.at("soul")=="Check [redacted]",
          "Bot view must preserve its ID and redact display text");
    const std::string history = "/api/conversation?bot=staging-triage";
    const auto latest = browser.get(history);
    const auto older = browser.get(history + "&before=" + latest.at("olderBefore").dump());
    const auto exact = browser.get(history + "&before=" + std::to_string(oldMessage + 1));
    check(latest.at("events").front().at("id") > oldMessage && older.at("events").front().at("id") == oldMessage,
          "An older match must be reachable through the same Bot conversation projection");
    check(exact.at("events").size() == 1 && exact.at("events").back().at("id") == oldMessage &&
          exact.at("events").back().at("data").at("text") == "Original handoff for [redacted]",
          "The older conversation projection must redact its text and omit other Bots' messages");
    const auto foreign = browser.get(history + "&before=" + std::to_string(otherBotMessage + 1));
    check(foreign.at("events").back().at("id") != otherBotMessage,
          "An exact source lookup must stay in the selected Bot");
    Browser::checked(browser.client.Get("/api/conversation?bot=release&before=" +
        std::to_string(otherBotMessage + 1)),403);
    Browser::checked(browser.client.Get("/api/view?bot=release"),403);
}

void approveModelCallBeforeTask(const std::filesystem::path& path) {
    Store webStore(path);
    auto store = std::make_unique<Store>(path);
    FixedModel model;
    FixedT3 t3;
    const auto start = [&] {
        return std::make_unique<Runtime>(*store,model,t3,[](auto) { return std::string{}; });
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
    model.comparisonResult = conversationCompletion(browser.get("/api/view?bot=release"),task).at("id");
    const auto receipt = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","compound"},
        {"text",compoundExplanation}},201);
    runtime->runConversations();
    const auto waiting = browser.get("/api/view?bot=release");
    const auto wait = waiting.at("pausedConversations").at(0);
    const auto reply = conversationReply(waiting,receipt.at("messageId"));
    check(wait.at("route") == "model_budget" && wait.at("messageId") == reply.at("messageId") &&
          waiting.at("presence") == "waiting" && model.turns == 2 && model.explained == 0,
          "the third call reached the fixture or the pause was not shown");
    runtime.reset();
    store = std::make_unique<Store>(path);
    runtime = start();
    tick(*runtime,3);
    check(model.turns == 2 && model.explained == 0,"the paused turn resumed without approval");

    const auto independent = browser.post("/api/messages",{{"botId","release"},{"clientMessageId","independent"},
        {"text","Who are you?"}},201);
    runtime->runConversations();
    check(conversationReply(browser.get("/api/view?bot=release"),independent.at("messageId")).at("text") == "I'm the release Bot." &&
          model.turns == 3,"a paused turn blocked an independent message to the same Bot");

    Json approval = {{"botId","triage"},{"itemKind","conversation_model_budget"},{"itemId",wait.at("replyToMessageId")},
        {"waitId",wait.at("messageId")},{"taskId",0},{"targetRevision",0},{"targetGeneration",wait.at("messageId")},
        {"decision","approve"}};
    browser.post("/api/inbox/decide",approval,409);
    const auto cursor = browser.get("/api/view?bot=release").at("historyCursor").dump();
    approval["botId"] = "release";
    browser.post("/api/inbox/decide",approval);
    browser.post("/api/inbox/decide",approval,409);
    const auto approved = browser.get("/api/view?bot=release&changesAfter=" + cursor);
    const auto& events = approved.at("events");
    const auto input = std::find_if(events.begin(),events.end(),[&](const Json& event) {
        return event.at("id") == receipt.at("messageId");
    });
    check(approved.at("changedMessageIds") == Json::array({receipt.at("messageId"),wait.at("messageId")}) &&
          browser.get("/api/view?bot=triage&changesAfter=" + cursor).at("changedMessageIds").empty() &&
          input != events.end() && input->at("data").at("status") == "received" &&
          conversationReply(approved,receipt.at("messageId")).at("status") == "done" &&
          approved.at("pausedConversations").empty(),
          "Approval did not invalidate both changed events for the owning Bot before resuming the turn");
    runtime->runConversations();
    const auto resumed = browser.get("/api/view?bot=release");
    const auto& answer = resumed.at("events").back().at("data");
    check(resumed.at("pausedConversations").empty() && answer.at("route") == "reply" &&
          answer.at("replyToMessageId") == receipt.at("messageId") &&
          model.turns == 4 && model.explained == 1 && model.summarized == 1 &&
          store->tasks().size() == 1 && t3.launches == 1,
          "approval did not resume the unfinished turn from its saved steps");
}

struct AuthorityModel final : Model {
    std::vector<std::string> inputs;
    std::function<void()> demoteDuringCall;
    bool leakedCredential = false;

    ModelTurn decide(const ModelInput& input) override {
        if (input.event.at("kind") == "intake")
            return {{{"delegate",{{"instruction","Check the release"},{"doneWhen","Report the result"}},"Start the task"}}};
        const auto request = input.event.at("text").get<std::string>();
        inputs.push_back(request);
        leakedCredential = leakedCredential || (input.event.dump() + input.task.dump() + input.history.dump() +
            input.config.dump()).find("Bearer fixture-") != std::string::npos;
        const auto first = input.event.at("turn").empty();
        if (request == "Paused retained root" && first)
            return {{{"find_tasks",{{"query","release"}},"Find release tasks"}}};
        if (request == "Demoted during model call") demoteDuringCall();
        const auto call = first ? Decision{"start_task",{{"request",request}},"Start requested work"}
                                : Decision{"reply",{{"text","Authorized conversation reply"}},"Reply to the user"};
        return {{call}};
    }
};

void queuedWebAuthority(const std::filesystem::path& path) {
    constexpr auto demoted = "Bearer fixture-demoted";
    constexpr auto removed = "Bearer fixture-removed";
    constexpr auto retained = "Bearer fixture-retained";
    constexpr auto granted = "Bearer fixture-granted";
    constexpr auto alternate = "Bearer fixture-alternate";
    constexpr auto view = "/api/view?bot=release";
    const auto identities = path.string() + ".identities";
    const auto identity = [](const char* credential, std::string_view person, bool root) {
        return Json{{"authorization",credential},{"person",person},{"adminRoot",root}};
    };
    const auto setup = [&](Store& store, const Json& accounts) {
        writeWebIdentities(identities,accounts);
        auto web = callbacks();
        web.intake = [original = web.intake](std::string_view bot, std::string_view request) {
            auto intake = original(bot,request);
            if (request.starts_with("Paused")) intake.config["modelCallLimit"] = 1;
            return intake;
        };
        configureWebIdentities(store,identities,web);
        return startWeb(store,0,std::move(web));
    };
    const auto signIn = [](Browser& browser, const char* credential) {
        browser.client.set_default_headers({{"Authorization",credential}});
    };
    const auto send = [&](Browser& browser, const char* credential, std::string_view request) -> Id {
        signIn(browser,credential);
        return browser.post("/api/messages",{{"botId","release"},{"clientMessageId",request},{"text",request}},201)
            .at("messageId");
    };
    AuthorityModel model;
    FixedT3 t3;
    Id demotedQueue, removedQueue, retainedQueue, grantedQueue, demotedPause, retainedPause, mixedQueue, trustedQueue, taskFromDemotedTurn;
    Json pauses;
    {
        Store store(path);
        auto server = setup(store,Json::array({identity(demoted,localOwner.id,true),identity(removed,"removed",true),
            identity(retained,"retained",true),identity(granted,"granted",true),identity(alternate,"mixed",true)}));
        Browser browser(*server);
        store.policy().setMember(localOwner,"release","granted",true);
        store.policy().setMember(localOwner,"release","removed",true);
        demotedPause = send(browser,demoted,"Paused demoted root");
        retainedPause = send(browser,retained,"Paused retained root");
        Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
        runtime.runConversations();
        pauses = browser.get(view).at("pausedConversations");
        const auto tasks = store.tasks();
        check(pauses.size() == 2 && tasks.size() == 1,"Fixture did not pause two turns after creating one task");
        taskFromDemotedTurn = tasks.at(0).at("id");

        demotedQueue = send(browser,demoted,"Queued demoted root");
        removedQueue = send(browser,removed,"Queued removed root");
        retainedQueue = send(browser,retained,"Queued retained root");
        grantedQueue = send(browser,granted,"Queued demoted root with decide grant");
        mixedQueue = send(browser,alternate,"Queued mixed credential roles");
        trustedQueue = store.receive(localOwner,{"release","","local-env","release"},"local","Trusted local owner")
            .at("messageId");
    }
    {
        Store store(path);
        auto server = setup(store,Json::array({identity(demoted,localOwner.id,false),identity(retained,"retained",true),
            identity(granted,"granted",false),identity(alternate,"mixed",false),identity("Bearer fixture-other","mixed",true)}));
        Browser browser(*server);
        signIn(browser,retained);
        for (const auto& pause : pauses)
            browser.post("/api/inbox/decide",{{"botId","release"},{"itemKind","conversation_model_budget"},
                {"itemId",pause.at("replyToMessageId")},{"waitId",pause.at("messageId")},{"taskId",0},
                {"targetRevision",0},{"targetGeneration",pause.at("messageId")},{"decision","approve"}});
        model.inputs.clear();
        Runtime runtime(store,model,t3,[](auto) { return std::string{}; });
        runtime.runConversations();
        const auto page = browser.get(view);
        const auto latestRoute = [&page](Id messageId) {
            for (const auto& event : page.at("events") | std::views::reverse)
                if (event.at("kind") == "conversation_reply" && event.at("data").at("replyToMessageId") == messageId)
                    return event.at("data").at("route");
            return Json(nullptr);
        };
        for (auto id : {demotedQueue,removedQueue,mixedQueue,demotedPause})
            check(latestRoute(id) == "access","Queued or resumed message retained unavailable Web authority");
        for (auto id : {retainedQueue,grantedQueue,trustedQueue,retainedPause})
            check(latestRoute(id) == "reply","A currently authorized queued or resumed message was denied");
        const std::vector<std::string> expected{"Paused retained root","Queued retained root","Queued retained root",
            "Queued demoted root with decide grant","Queued demoted root with decide grant","Trusted local owner","Trusted local owner"};
        check(model.inputs == expected && store.tasks().size() == 4 && page.at("pausedConversations").empty(),
              "Access changes delivered model context or ran work for a revoked actor");
        check(!model.leakedCredential && page.dump().find("Bearer fixture-") == std::string::npos,
              "Web credentials entered a model request or conversation output");

        const auto racing = send(browser,retained,"Demoted during model call");
        model.demoteDuringCall = [&] {
            store.setWebIdentityResolver([](std::string_view person) -> std::optional<bool> {
                return person == "retained" ? std::optional{false} : std::nullopt;
            });
        };
        runtime.runConversations();
        const auto denied = conversationReply(browser.get(view),racing);
        check(model.inputs.back() == "Demoted during model call" && denied.at("route") == "access" &&
              denied.at("actions").empty() && store.tasks().size() == 4,"Revoked authority survived the pre-effect check");
        tick(runtime,3);
        check(t3.launches == 1 && store.task(taskFromDemotedTurn).at("state") == "running",
              "A denied resume consumed the unused model call shared by its existing task");
    }
}

void memoryAutomationEndpoints(const std::filesystem::path& path) {
    Store store(path,[](auto) { return "Initial convention"; });
    auto cb = callbacks();
    cb.clock = [] { return Seconds{2'000'000'000}; };
    cb.authenticate = [](std::string_view credential) {
        if (credential == "Bearer owner") return Person{"owner",true};
        if (credential == "Bearer reader") return Person{"reader",false};
        return Person{};
    };
    store.policy().setMember({"owner",true},"release","reader",true);
    auto server = startWeb(store,0,cb);
    Browser browser(*server);
    Browser::checked(browser.client.Get("/api/memory?bot=release"),401);
    browser.client.set_default_headers({{"Authorization","Bearer owner"}});
    check(browser.get("/api/memory?bot=release").at("content") == "Initial convention","Web memory seed missing");
    const auto proposal = browser.post("/api/memory/propose",{{"botId","release"},{"baseVersion",0},
        {"content","New convention"},{"rationale","Use staging"}}).at("proposalId");
    const auto revision = browser.post("/api/memory/apply",{{"botId","release"},{"baseVersion",0},
        {"proposalId",proposal}}).at("revisionId").get<Id>();
    browser.post("/api/memory/apply",{{"botId","release"},{"baseVersion",0},{"content","Stale"}},409);
    const auto detail = browser.get("/api/memory/history?bot=release&revisionId=" + std::to_string(revision));
    check(detail.at("previousContent") == "Initial convention" && detail.at("newContent") == "New convention",
          "Web revision did not expose the diff");
    browser.post("/api/memory/revert",{{"botId","release"},{"baseVersion",1},{"revisionId",revision}});
    check(browser.get("/api/memory?bot=release").at("content") == "Initial convention","Web revert failed");
    const auto pending = browser.post("/api/memory/propose",{{"botId","release"},{"baseVersion",2},
        {"content","Pending convention"},{"rationale","Original proposal"}}).at("proposalId");
    for (int i = 0; i < 20; ++i) {
        const auto newer = browser.post("/api/memory/propose",{{"botId","release"},{"baseVersion",2},
            {"content","Newer convention"},{"rationale","Subsequent proposal"}}).at("proposalId");
        browser.post("/api/memory/reject",{{"botId","release"},{"proposalId",newer}});
    }
    const auto firstPage = browser.get("/api/memory/history?bot=release");
    const auto older = browser.get("/api/memory/history?bot=release&proposalsBefore=" +
        std::to_string(firstPage.at("proposalsBefore").get<Id>())).at("proposals");
    check(firstPage.at("proposals").size() == 20 && older.at(0).at("id") == pending &&
          older.at(0).at("status") == "pending","Newer proposals hid a pending proposal");

    const auto reminder = browser.post("/api/schedules",{{"botId","release"},{"kind","reminder"},
        {"request","Check staging"},{"timezone","Asia/Tokyo"},{"fireAt",2'000'000'010}},201).at("id").get<Id>();
    browser.post("/api/schedules",{{"botId","release"},{"kind","reminder"},{"request","Check staging"},
        {"timezone","Asia/Tokyo"},{"fireAt",2'000'000'000}},409);
    const auto cancelPath = "/api/schedules?bot=release&id=" + std::to_string(reminder);
    Browser::checked(browser.client.Delete(cancelPath),403);
    check(Browser::checked(browser.client.Delete(cancelPath,browser.headers()),200).at("status") == "canceled",
          "Web schedule cancellation failed");
    const auto trigger = browser.post("/api/schedules",{{"botId","release"},{"kind","trigger"},{"source","deploy"},
        {"eventFilter",{{"branch","main"}}},{"request","Check the deployment"}},201).at("id");
    check(browser.get("/api/schedules?bot=release").at("automations").size() == 2,"Web automation list incomplete");

    const auto webhook = [&](std::string id, std::string branch) {
        return Json{{"externalEventId",id},{"event",{{"branch",branch}}}}.dump();
    };
    Browser::checked(browser.client.Post("/api/webhooks/deploy",webhook("event-1","main"),"application/json"),403);
    const httplib::Headers headers{{"X-Flock-Action","1"}};
    const auto send = [&](const std::string& payload) {
        return Browser::checked(browser.client.Post("/api/webhooks/deploy",headers,payload,"application/json"),202);
    };
    check(send(webhook("skip","test")).at("tasks").empty(),"Webhook filter did not match exactly");
    browser.client.set_default_headers({{"Authorization","Bearer reader"}});
    store.policy().setMember(localOwner,"release","reader",false);
    Browser::checked(browser.client.Post("/api/webhooks/deploy",headers,webhook("event-1","main"),"application/json"),403);
    store.policy().setMember(localOwner,"release","reader",true);
    browser.post("/api/memory/apply",{{"botId","release"},{"baseVersion",2},{"content","Unauthorized"}},403);
    browser.client.set_default_headers({{"Authorization","Bearer owner"}});
    const auto accepted = send(webhook("event-1","main")), replayed = send(webhook("event-1","main"));
    check(accepted.at("duplicate") == false && replayed.at("duplicate") == true &&
          accepted.at("tasks").size() == 1 && replayed.at("tasks") == accepted.at("tasks") && store.tasks().size() == 1,
          "Webhook redelivery enqueued another task");
    store.fireDueAutomations(cb.clock()+60);
    check(store.tasks().size() == 1,"Canceled reminder fired after its due time");
    const auto request = browser.get("/api/conversation?bot=release").at("events").back().at("data");
    check(request.at("trigger").at("source") == "deploy" && request.at("trigger").at("triggerId") == trigger &&
          !request.at("trigger").contains("externalEventId"),
          "Web conversation lost webhook provenance");
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
#ifdef FLOCK_TEST_CHATGPT
        fullModeProfiles(directory.path / "full-mode-profiles");
#endif
        inspectors(directory.path / "inspectors.db");
        imageAttachmentCountInConversation(directory.path / "image-attachment-count.sqlite");
        suggestionsAndEnvironmentsInView(directory.path / "suggestions.sqlite");
        conversationEntry(directory.path / "conversation.sqlite");
        botMediatedConversation(directory.path / "bot-mediated-with-note.sqlite","確認手順の妥当性を確認してください。",true);
        botMediatedConversation(directory.path / "bot-mediated-without-note.sqlite",{});
        secretCollisionKeepsBotRoutes(directory.path / "secret-collision.db");
        approveModelCallBeforeTask(directory.path / "model-budget.sqlite");
        queuedWebAuthority(directory.path / "queued-web-authority.sqlite");
        memoryAutomationEndpoints(directory.path / "automation.sqlite");
        std::cout << "web conversation behavior passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
