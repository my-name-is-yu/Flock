#include "flock/t3_connection.hpp"
#include "../../tests/support.hpp"
#include "flock/bot_lifecycle.hpp"
#include "flock/conversation_intake.hpp"
#include "flock/web_conversation_intake.hpp"
#include "flock/task_history.hpp"
#include "flock/task_intake.hpp"
#include "flock/task_list.hpp"
#include "flock/identity.hpp"
#include "flock/secret_ref.hpp"
#include "flock/tasks.hpp"
#include "flock/automations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/conversations.hpp"
#include "flock/web.hpp"

#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"
#include "../views.hpp"
#include "flock/bot_change.hpp"
#include "flock/command.hpp"
#include "flock/decision.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/bot_access.hpp"
#include "flock/policy.hpp"
#include "flock/policy_rules.hpp"
#include "flock/secret_requests.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/runtime_options.hpp"
#include "flock/store.hpp"
#include "flock/store_options.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>

#include <httplib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <ranges>
#include <sqlite3.h>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>

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

using flockv0::test::check;
using flockv0::test::TemporaryDirectory;

std::string statusText(const ModelInput& input) {
    std::string text;
    for (const auto& task : input.task.at("tasks")) {
        if (!text.empty()) text += '\n';
        text += "- " + task.at("request").get<std::string>() + " ／ " + (task.at("state") == "completed" ? "完了" : "作業中");
    }
    return text;
}

Question scopeQuestion() {
    return {"scope-question",
            {{.id = "scope",
              .prompt = "利用者向けの変更だけを載せますか？",
              .options = {{"利用者向けのみ", "利用者向けのみ"}, {"すべて", "すべて"}}}}};
}

struct FixedModel : Model {
    unsigned turns = 0, explained = 0, summarized = 0;
    Id comparisonResult = 0;
    std::size_t summarySourceSize = 0;

    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        check(input.event["kind"] == "conversation_turn", "a task without a conversation called the model");
        return converse(input);
    }

    Decision converse(const ModelInput& input) {
        ++turns;
        const auto& event = input.event;
        check(event.dump().find(privateValue) == std::string::npos && input.task.dump().find(privateValue) == std::string::npos &&
                  input.history.dump().find(privateValue) == std::string::npos,
              "Connection settings entered model-visible context");
        const auto text = event.at("text").get<std::string>();
        const auto& steps = event.at("turn");
        const auto reply = [](std::string message, Json suggestions = nullptr) {
            Json args = {{"text", std::move(message)}};
            if (!suggestions.is_null()) args["suggestions"] = std::move(suggestions);
            return Decision{"reply", std::move(args), "Reply to the user"};
        };
        if (text == "2案目を詳しく" || text == "2案目を要約して" || text == compoundExplanation) {
            if (steps.empty()) return {"read_result", {{"messageId", comparisonResult}}, "Read the comparison"};
            const auto& source = steps[0].at("result").at("text").get_ref<const std::string&>();
            check(source == proposedOptions, "The comparison was not readable in full");
            if (text == compoundExplanation && steps.size() == 1)
                return {"read_result", {{"messageId", comparisonResult}}, "Read it again for the summary"};
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
        if (text == "markdown fixture") return reply("**完了**：[結果を見る](https://example.com/result)");
        if (text == "nested URL fixture") return reply("[Result](https://example.test/?q=%252525252541)");
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
            return reply("どちらの実行先で準備しますか？", choices);
        }
        if (text == "backup-env") {
            if (steps.empty())
                return {"start_task", {{"request", "Prepare the notes"}, {"environment", "backup-env"}}, "Use the chosen destination"};
            return reply("backup で準備を始めました。");
        }
        const auto& anchor = event.at("replyTo");
        check(anchor.is_object() && anchor.at("kind") == "task_question", "Unexpected message in HTTP fixture: " + text);
        if (!steps.empty())
            return reply(steps.back().at("result").value("ok", false) ? "回答を伝えました。" : "その質問は終了しています。");
        if (!anchor.at("current").get<bool>()) return reply("その質問は終了しています。");
        const auto answer = text == "利用者向けのみ" ? "すべて" : text;
        return {"answer_task_question",
                {{"taskId", anchor.at("taskId")},
                 {"waitId", anchor.at("waitId")},
                 {"text", answer},
                 {"revisionId", anchor.at("revisionId")},
                 {"generation", anchor.at("generation")}},
                "Answer the question"};
    }
};

struct FixedT3 : T3 {
    std::map<std::string, Observation, std::less<>> observations;
    std::map<std::string, unsigned> projectLaunches;
    std::vector<Command> replies;
    unsigned launches = 0, interruptions = 0;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    std::vector<T3Project> projects(std::string_view environment) override {
        if (environment == "backup-env") return {{"backup-release", "Backup release", "release"}};
        if (environment == "mini-env") return {{"mini-project", "Mini project", {}}};
        return {{"release", "Local release", {}}};
    }
    Delivery send(const Command& command) override {
        if (command.tool == CommandTool::delegate) {
            ++launches;
            if (command.projectId == "settings") return {Delivery::Kind::Settings, {}, privateValue};
            const auto& project = command.projectId;
            const auto sequence = ++projectLaunches[project];
            auto thread = "thread-" + project;
            if (sequence > 1) thread += "-" + std::to_string(sequence);
            const auto link = project == "triage" ? "http://localhost:3773/linked%20env/" + thread : "javascript:unexpected";
            return {Delivery::Kind::Accepted, {std::move(thread), "run-" + std::to_string(launches), link}, {}};
        }
        check(command.tool == CommandTool::answer_session_question, "unexpected command in fixture");
        replies.push_back(command);
        observations.erase(command.target.threadId);
        return {Delivery::Kind::Accepted, {}, {}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override { return observations[run.threadId]; }
    InterruptOutcome interrupt(const std::string&, const Json&, const RunRef&) override {
        ++interruptions;
        return InterruptOutcome::Requested;
    }
};

WebCallbacks callbacks(const std::atomic_bool* intakeDisabled = nullptr) {
    return {[] {
                return Json::array({
                    {{"id", "release"},
                     {"name", "Release"},
                     {"soul", "利用者向けのリリースノートをまとめる。"},
                     {"cron", Json::array()},
                     {"avatar", {{"example", "mug"}, {"color", "#982fff"}, {"secret", privateValue}}},
                     {"token", privateValue}},
                    {{"id", "triage"}, {"name", "Triage"}, {"soul", "報告された問題を整理する。"}, {"cron", Json::array()}},
                    {{"id", "settings"}, {"name", "Settings"}, {"soul", "設定エラーの確認"}},
                    {{"id", "broken"}, {"name", "Broken"}},
                });
            },
            [intakeDisabled](std::string_view bot, std::string_view request) -> Intake {
                if (bot == "broken" || (intakeDisabled && intakeDisabled->load())) throw std::runtime_error(privateValue);
                return {std::string(bot),
                        std::string(request),
                        bot == "triage" ? "linked env" : "local-env",
                        std::string(bot),
                        {{"token", privateValue}}};
            }};
}

class Browser {
public:
    explicit Browser(const WebServer& server)
        : client("127.0.0.1", static_cast<int>(server.port())), origin("http://127.0.0.1:" + std::to_string(server.port())) {}

    Json get(const std::string& path) { return checked(client.Get(path), 200); }
    Json post(const std::string& path, const Json& data, int status = 200) {
        return checked(client.Post(path, headers(), data.dump(), "application/json"), status);
    }
    httplib::Headers headers() const { return {{"Origin", origin}, {"Sec-Fetch-Site", "same-origin"}, {"X-Flock-Action", "1"}}; }
    static Json checked(const httplib::Result& response, int status) {
        if (!response) throw std::runtime_error("HTTP request failed: " + httplib::to_string(response.error()));
        if (response->status != status)
            throw std::runtime_error("Unexpected HTTP " + std::to_string(response->status) + " from " + response->body.substr(0, 240));
        check(response->body.find(privateValue) == std::string::npos, "private connection value entered HTTP response");
        return Json::parse(response->body);
    }
    httplib::Client client;
    std::string origin;
};

void tick(Runtime& runtime, int count) {
    for (int i = 0; i < count; ++i) runtime.tick();
}

void botPauseEndpoints(const std::filesystem::path& path) {
    Store store(path);
    const Person owner{"owner", false};
    BotAccess{store}.setOwner(owner, "release");
    BotAccess{store}.setMember(owner, "release", "reader", true);
    auto settings = callbacks();
    settings.authenticate = [](std::string_view credential) {
        if (credential == "Bearer owner") return Person{"owner", false};
        if (credential == "Bearer reader") return Person{"reader", false};
        if (credential == "Bearer root") return Person{"root", true};
        return Person{};
    };
    settings.resolveIdentity = [](std::string_view person) -> std::optional<bool> { return person == "root"; };
    const auto server = startWeb(store, 0, std::move(settings));
    Browser browser(*server);
    const auto use = [&](std::string name) { browser.client.set_default_headers({{"Authorization", "Bearer " + name}}); };
    const Json pause{{"botId", "release"}, {"paused", true}};
    use("reader");
    browser.post("/api/bots/pause", pause, 403);
    use("owner");
    browser.post("/api/bots/pause", {{"botId", "release"}, {"paused", "yes"}}, 400);
    check(browser.post("/api/bots/pause", pause).at("paused") == true, "the owner could not pause their Bot");
    use("reader");
    check(browser.get("/api/bots").at(0).at("paused") == true && browser.get("/api/view?bot=release").at("bot").at("paused") == true,
          "pause was missing from the Bot list or conversation view");
    const Json message{{"botId", "release"}, {"clientMessageId", "while-paused"}, {"text", "Prepare the release"}};
    const auto denied = browser.post("/api/messages", message, 409);
    check(denied.at("error") == "bot_paused" && denied.at("message") == botPausedMessage, "the Web composer received no clear pause error");
    auto resume = pause;
    resume["paused"] = false;
    browser.post("/api/bots/pause", resume, 403);
    use("root");
    check(browser.post("/api/bots/pause", resume).at("paused") == false, "an installation root could not resume the Bot");
    use("reader");
    browser.post("/api/messages", message, 201);
}

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
        check(result.is_null(), "message received more than one Bot reply");
        result = event.at("data");
    }
    check(!result.is_null(), "message received no Bot reply");
    return result;
}

Json conversationCompletion(const Json& page, Id taskId) {
    const auto& events = page.at("events");
    const auto completion =
        std::ranges::find_if(events, [&](const Json& event) { return event.at("kind") == "completed" && event.at("taskId") == taskId; });
    check(completion != events.end(), "the conversation omitted the completion");
    return *completion;
}

void writeWebIdentities(const std::filesystem::path& path, const Json& accounts) {
    std::ofstream(path) << accounts;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

void conversationSummaries(const std::filesystem::path& path) {
    struct InspectorModel : Model {
        const std::string credential = "credential-protected-after-inspector-write";
        Id boundary = 0;
        ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
        Decision choose(const ModelInput& input) {
            if (input.event.at("turn").empty() && input.event.at("text") == "Summarize")
                return {"compact_conversation",
                        {{"endMessageId", boundary}, {"summary", "Preserve the release notes " + credential}},
                        "Summarize earlier work"};
            if (input.event.at("turn").empty() && input.event.at("text") == "Release")
                return {"start_task", {{"request", "Prepare a release " + credential}}, "Prepare the release"};
            return {"reply", {{"text", "Recorded"}}, "Acknowledge the request"};
        }
    } model;
    Store store(path);
    FixedT3 t3;
    Runtime runtime(store, model, t3, [](auto) { return std::string{}; });
    const auto routing = callbacks().intake("release", "");
    auto server = startWeb(store, 0, callbacks());
    Browser browser(*server);
    model.boundary = ConversationIntake(store).receive({"fixture", true}, routing, "a", "Original release instructions").at("messageId");
    ConversationIntake(store).receive({"fixture", true}, routing, "b", "Summarize");
    const auto release = ConversationIntake(store).receive({"fixture", true}, routing, "c", "Release").at("messageId").get<Id>();
    runtime.runConversations();
    SecretRequests secrets{store};
    secrets.submitSecret({"fixture", true}, "release", secrets.requestSecret("release", {"inspector-token"}), model.credential);
    const auto summaries = browser.get("/api/compactions?bot=release");
    const auto view = browser.get("/api/view?bot=release");
    check(summaries.size() == 1 && summaries[0]["endMessageId"] == model.boundary &&
              summaries[0]["summary"] == "Preserve the release notes [redacted]" && view.at("compactions") == summaries,
          "summary or its covered range missing from the inspector");
    const auto& tasks = view.at("activeTasks");
    check(tasks.size() == 1 && tasks.front().at("request") == "Prepare a release [redacted]",
          "Task was missing or unredacted in the Bot view");
    check(browser.get("/api/compactions?bot=triage").empty(), "Summaries mixed Bot histories");
    const auto audit = browser.get("/api/conversation?bot=release");
    check(audit.dump().find("Original release instructions") != std::string::npos,
          "summary removed the original message from the HTTP audit history");
    const auto reply = conversationReply(audit, release);
    check(reply.at("status") == "working" &&
              reply.at("actions") == Json::array({{{"tool", "start_task"}, {"taskId", tasks.front().at("id")}, {"effect", "started"}}}),
          "Task start was missing from the conversation reply");
}

void imageAttachmentCountInConversation(const std::filesystem::path& path) {
    Store store(path);
    auto config = callbacks();
    config.authenticate = [](std::string_view credential) {
        if (credential == "Bearer owner") return Person{"owner", true};
        if (credential == "Bearer reader") return Person{"reader", false};
        return Person{};
    };
    config.resolveIdentity = [](std::string_view person) -> std::optional<bool> {
        if (person == "owner") return true;
        if (person == "reader") return false;
        return std::nullopt;
    };
    BotAccess{store}.setMember({"owner", true}, "release", "reader", true);
    const auto server = startWeb(store, 0, std::move(config));
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization", "Bearer owner"}});
    const Json image = {{"name", "private-screenshot.png"},
                        {"mimeType", "image/png"},
                        {"dataBase64", "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"}};
    const Json audio = {{"name", "private-audio.mp3"}, {"mimeType", "audio/mpeg"}, {"dataBase64", "SUQzAA=="}};
    const auto submit = [&](std::string_view key, const Json& attachments, std::string_view text) {
        return browser
            .post("/api/messages",
                  {{"botId", "release"}, {"clientMessageId", key}, {"text", std::string(text)}, {"attachments", attachments}}, 201)
            .at("messageId")
            .get<Id>();
    };
    const auto single = submit("a", Json::array({image, audio}), "Look at this");
    const auto multiple = submit("b", Json::array({image, image}), "Look at this");
    const auto textOnly = submit("c", Json::array(), "Look at this");
    const std::string markdownText =
        "**From user**\n\n- [safe link](https://example.com/user)\n- [unsafe link](javascript:alert(1))\n\n<script>alert(1)</script>";
    const auto markdownMessage = submit("markdown-user", Json::array(), markdownText);
    auto noncanonicalAudio = audio;
    noncanonicalAudio["dataBase64"] = "SUQzAB==";
    browser.post("/api/messages",
                 {{"botId", "release"},
                  {"clientMessageId", "invalid-padding"},
                  {"text", "Look at this"},
                  {"attachments", Json::array({noncanonicalAudio})}},
                 409);
    const auto assertCounts = [](const Json& events, const std::map<Id, std::pair<int, int>>& expected) {
        auto remaining = expected;
        for (const auto& event : events) {
            const auto match = remaining.find(event.at("id").get<Id>());
            if (match == remaining.end()) continue;
            const auto& data = event.at("data");
            const auto [files, images] = match->second;
            check(data.at("attachmentCount") == files && data.at("imageAttachmentCount") == images,
                  "wrong saved file or image count in the conversation");
            check(data.at("textHtml").is_string(), "user message was not projected as safe Markdown HTML");
            check(!data.contains("attachments") && !data.contains("dataBase64") &&
                      data.dump().find("private-screenshot") == std::string::npos,
                  "Web conversation exposed private attachment details");
            remaining.erase(match);
        }
        check(remaining.empty(), "messages were missing from the conversation");
    };
    constexpr auto conversation = "/api/conversation?bot=release";
    browser.client.set_default_headers({});
    Browser::checked(browser.client.Get(conversation), 401);
    browser.client.set_default_headers({{"Authorization", "Bearer reader"}});
    const auto view = browser.get("/api/view?bot=release");
    assertCounts(view.at("events"), {{single, {2, 1}}, {multiple, {2, 2}}, {textOnly, {0, 0}}, {markdownMessage, {0, 0}}});
    bool renderedUserMarkdown = false;
    for (const auto& event : view.at("events")) {
        if (event["kind"] != "conversation_message" || event["id"] != markdownMessage) continue;
        const auto& data = event.at("data");
        const auto html = data.at("textHtml").get<std::string>();
        renderedUserMarkdown =
            data.at("text") == markdownText && html.find("<strong>From user</strong>") != std::string::npos &&
            html.find("<a href=\"https://example.com/user\">safe link</a>") != std::string::npos &&
            html.find("unsafe link (javascript:alert(1))") != std::string::npos && html.find("href=\"javascript:") == std::string::npos &&
            html.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos && html.find("<script>") == std::string::npos;
    }
    check(renderedUserMarkdown, "Web user-message projection changed canonical text or rendered unsafe Markdown");
    assertCounts(browser.get(std::string(conversation) + "&before=" + std::to_string(multiple)).at("events"), {{single, {2, 1}}});
    Browser::checked(browser.client.Get("/api/conversation?bot=triage"), 403);
}

void suggestionsAndEnvironmentsInView(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    T3Connections(store).connect("backup-env", localOwner.id, 48123);
    const auto server = startWeb(store, 0, callbacks());
    Browser browser(*server);
    const auto send = [&](std::string_view key, std::string_view message, Id replyTo) {
        Json input = {{"botId", "release"}, {"clientMessageId", key}, {"text", message}};
        if (replyTo) input["replyToMessageId"] = replyTo;
        return browser.post("/api/messages", input, 201);
    };
    const auto question = send("request-destination", "Prepare the notes in local-env or backup", 0);
    runtime.runConversations();
    const auto page = browser.get("/api/view?bot=release");
    const auto asked = conversationReply(page, question.at("messageId"));
    check(asked.at("suggestions") == Json::array({"local-env", "backup-env"}) && page.at("waits").empty() &&
              TaskList(store).tasks().empty(),
          "the Bot's question was not an ordinary message with its suggested replies");
    const auto receipt = send("choose-backup", "backup-env", asked.at("messageId"));
    tick(runtime, 4);
    const auto resolved = browser.get("/api/view?bot=release");
    const auto tasks = TaskList(store).tasks("release");
    check(tasks.size() == 1 && conversationReply(resolved, receipt.at("messageId")).at("actions").at(0).at("tool") == "start_task",
          "the chosen destination did not create a Task");
    const auto& task = tasks.front();
    check(task.task.projectId == "backup-release" && !task.runs.empty() && task.runs.back().run.environmentId == "backup-env",
          "the chosen destination did not receive the new Run");
}

void parallelVerificationInView(const std::filesystem::path& path) {
    const std::string commit(40, 'a');
    struct VerifyModel : Model {
        const std::string& commit;
        explicit VerifyModel(const std::string& revision) : commit(revision) {}
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("kind") != "conversation_turn")
                return {{{"delegate", {{"instruction", "Run the checks"}}, "Verify both worktrees"}}};
            if (input.event.at("turn").empty())
                return {{{"start_task",
                          {{"request", "Verify the release"},
                           {"doneWhen", "Run the checks"},
                           {"environments", {"local-env", "mini-env"}},
                           {"commit", commit}},
                          "Use the two requested environments"}}};
            return {{{"reply", {{"text", "I saved the request."}}, "Acknowledge the request"}}};
        }
    } model(commit);
    struct VerifyT3 : FixedT3 {
        const std::string& commit;
        bool localFinished = false, miniFinished = false, miniReceipt = false;
        explicit VerifyT3(const std::string& revision) : commit(revision) {}
        Delivery send(const Command& command) override { return reconcile(command); }
        Delivery reconcile(const Command& command) override {
            if (command.environmentId == "mini-env" && !miniReceipt) return {Delivery::Kind::Unknown, {}, {}};
            return {Delivery::Kind::Accepted, {"verification-thread", "verification-run", "javascript:unexpected"}, {}};
        }
        Observation observe(const std::string& environment, const Json&, const RunRef&) override {
            if (environment == "mini-env" && !miniFinished) return {.kind = Observation::Kind::Unavailable};
            if (environment == "local-env" && !localFinished) return {};
            return {.kind = Observation::Kind::Finished,
                    .status = environment == "mini-env" ? "failed" : "completed",
                    .report = "Checks returned from " + environment,
                    .checkedCommit = commit};
        }
    } t3(commit);
    Store store(path);
    Seconds now = unixTime();
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; }, RuntimeOptions{[&] { return now; }});
    T3Connections(store).connect("mini-env", localOwner.id, 48123);
    auto server = startWeb(store, 0, callbacks());
    Browser browser(*server);
    const auto receipt = browser.post(
        "/api/messages", {{"botId", "release"}, {"clientMessageId", "verify-both"}, {"text", "Verify the release on both machines."}}, 201);
    tick(runtime, 3);
    const auto activePage = browser.get("/api/view?bot=release");
    const auto& activeRuns = activePage.at("parallelTasks").at(0).at("runs");
    check(std::ranges::any_of(activeRuns,
                              [](const Json& run) {
                                  return run.at("environmentId") == "local-env" && run.at("state") == "running" &&
                                         run.at("name") == "local-env" && run.contains("threadId");
                              }),
          "a running destination was missing its Session link, name or state");
    t3.localFinished = true;
    runtime.tick();
    now += 48 * 3600;
    runtime.tick();
    check(browser.get("/api/view?bot=release").at("waits").empty() && Inbox(store).inbox(localOwner, "release").empty(),
          "A pending destination launch prompted for intervention after a long wait");
    t3.miniReceipt = true;
    now += 6;
    tick(runtime, 2);
    const auto running = browser.get("/api/view?bot=release");
    const auto task = running.at("parallelTasks").at(0);
    const auto& runs = task.at("runs");
    check(runs.at(0).at("state") == "completed" && runs.at(0).at("commit") == commit && runs.at(1).at("state") == "confirmation_pending" &&
              runs.at(1).at("link") == "https://app.t3.codes/threads/mini-env/verification-thread",
          "the active request needs its successful result and the disconnected Session");
    check(std::ranges::any_of(
              running.at("events"),
              [&](const Json& event) { return event.at("id") == receipt.at("messageId") && event.at("taskId") == task.at("id"); }),
          "the verification status is not associated with the initial request");
    t3.miniFinished = true;
    runtime.tick();
    const auto settled = browser.get("/api/view?bot=release");
    check(settled.at("parallelTasks").empty(), "settled verification is still active");
    unsigned reports = 0;
    for (const auto& event : settled.at("events")) {
        if (event.at("kind") != "report" || event.at("taskId") != task.at("id")) continue;
        ++reports;
        const auto& result = event.at("data");
        const auto& run = result.at("run");
        const bool mini = result.at("destination") == "mini-env";
        check(result.contains("textHtml") == !result.value("report", std::string{}).empty(),
              "a written report was not rendered as Markdown, or an absent one was");
        check(result.at("status") == (mini ? "failed" : "completed") && result.at("commit") == commit &&
                  run.at("link") ==
                      std::string("https://app.t3.codes/threads/") + (mini ? "mini-env/" : "local-env/") + "verification-thread",
              "the terminal report lost its destination, SHA or Session link");
    }
    check(reports == 2 && conversationCompletion(settled, task.at("id")).at("data").at("runOutcome") == "partial_failure",
          "the conversation lost one of the results or their aggregate outcome");
}

void conversationEntry(const std::filesystem::path& path) {
    Store store(path);
    FixedModel model;
    FixedT3 t3;
    std::atomic_bool disableIntake = false;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    const auto web = callbacks(&disableIntake);
    auto server = startWeb(store, 0, web);
    Browser browser(*server);
    const auto taskId = TaskIntake(store).submit(web.intake("release", comparisonRequest));
    tick(runtime, 3);
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed", .report = proposedOptions, .itemId = "comparison-report"};
    tick(runtime, 2);
    check(Tasks(store).task(taskId).task.state == TaskState::completed, "the comparison did not complete");
    const auto result = conversationCompletion(browser.get("/api/view?bot=release"), taskId);
    const auto& resultData = result.at("data");
    check(resultData.at("text") == proposedOptions, "the conversation lost the completion report");
    check(resultData.at("textHtml").get<std::string>().starts_with("<ol>"), "the completion report was not rendered as Markdown");
    check(!resultData.contains("reason") && !resultData.contains("doneWhen"), "the completed result exposed internal decision fields");
    model.comparisonResult = result.at("id");
    const auto& reportRun = resultData.at("run");
    check(reportRun["threadId"] == "thread-release" && reportRun["runId"] == "run-1" && reportRun["itemId"] == "comparison-report",
          "the conversation did not link the Run and report item that produced the result");

    const Json message = {{"botId", "release"}, {"clientMessageId", "options-more"}, {"text", "2案目を詳しく"}};
    const auto receipt = browser.post("/api/messages", message, 201);
    const auto messageId = receipt.at("messageId").get<Id>();
    const auto cursor = browser.get("/api/view?bot=release").at("historyCursor").get<Id>();
    const auto since = "&changesAfter=" + std::to_string(cursor);
    check(receipt["status"] == "received" && messageId > 0, "conversation receipt was missing");
    check(model.turns == 0 && TaskList(store).tasks().size() == 1, "HTTP intake ran the model before returning its receipt");
    disableIntake = true;
    check(browser.post("/api/messages", message, 201) == receipt, "retry did not recover its receipt with changed Bot settings");
    auto changed = message;
    changed["text"] = "3案目を詳しく";
    browser.post("/api/messages", changed, 409);
    changed = message;
    changed["replyToMessageId"] = result.at("id");
    browser.post("/api/messages", changed, 409);
    browser.post("/api/messages", {{"botId", "release"}, {"text", "追加確認"}}, 400);

    tick(runtime, 3);
    const auto page = browser.get("/api/view?bot=release" + since);
    const auto triage = browser.get("/api/view?bot=triage" + since);
    check(page.at("changedMessageIds") == Json::array({messageId}) && triage.at("changedMessageIds").empty(),
          "a processed message was not invalidated exclusively for its own Bot");
    check(triage.at("events").empty() && triage.at("sessionTasks").empty(), "another Bot could see the comparison");
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
    const auto response = conversationReply(page, messageId);
    const auto responseText = response.at("text").get<std::string>();
    check(response["route"] == "reply" && responseText.find("差分を先に集計する") != std::string::npos &&
              responseText.find("金額が変わった更新だけを人が確かめる") != std::string::npos && response["actions"].empty(),
          "answer did not explain the second option without changing work");
    check(questions == 1 && answers == 1 && TaskList(store).tasks().size() == 1 && t3.launches == 1,
          "read-only conversation or retry created extra messages, Tasks or Runs");
    const auto& sessionTasks = page.at("sessionTasks");
    check(sessionTasks.size() == 1 && sessionTasks.at(0).at("id") == taskId && sessionTasks.at(0).at("sessions").size() == 1 &&
              sessionTasks.at(0).at("sessions").at(0).at("runId") == reportRun.at("runId") && !sessionTasks.at(0).contains("config"),
          "the Session list omitted the comparison Run or exposed config");
    const auto runs = Tasks(store).task(taskId).runs;

    const auto markdownReceipt =
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", "markdown-reply"}, {"text", "markdown fixture"}}, 201);
    tick(runtime, 3);
    const auto markdownPage = browser.get("/api/view?bot=release");
    const auto markdownReply = conversationReply(markdownPage, markdownReceipt.at("messageId"));
    check(markdownReply.at("text") == "**完了**：[結果を見る](https://example.com/result)" &&
              markdownReply.at("textHtml") == "<p><strong>完了</strong>：<a href=\"https://example.com/result\">結果を見る</a></p>",
          "Web projection changed canonical reply text or omitted safe rendered HTML");
    bool userMessageProjectionPreservedSource = false;
    for (const auto& event : markdownPage.at("events"))
        if (event["kind"] == "conversation_message" && event["data"]["messageId"] == markdownReceipt.at("messageId"))
            userMessageProjectionPreservedSource =
                event["data"]["text"] == "markdown fixture" && event["data"]["textHtml"] == "<p>markdown fixture</p>";
    check(userMessageProjectionPreservedSource, "Web user-message projection changed canonical text or omitted rendered HTML");

    const auto nestedUrlReceipt =
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", "nested-url-reply"}, {"text", "nested URL fixture"}}, 201);
    tick(runtime, 3);
    const auto nestedUrlReply = conversationReply(browser.get("/api/view?bot=release"), nestedUrlReceipt.at("messageId"));
    check(nestedUrlReply.at("text") == "[Result](https://example.test/?q=%252525252541)" &&
              nestedUrlReply.at("textHtml") == "<p>[redacted]</p>",
          "Web rendered a URL that exceeded the decoding limit in a reply without protected values");

    const auto summary =
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", "options-summary"}, {"text", "2案目を要約して"}}, 201);
    tick(runtime, 3);
    const auto summarized = conversationReply(browser.get("/api/view?bot=release"), summary.at("messageId"));
    const auto summaryText = summarized.at("text").get<std::string>();
    check(summarized.at("route") == "reply" && summaryText.find("ID で明細の差分を集計") != std::string::npos &&
              summaryText.size() < model.summarySourceSize,
          "The summary did not condense the selected option");
    check(model.explained == 1 && model.summarized == 1 && TaskList(store).tasks().size() == 1 && t3.launches == 1 &&
              Tasks(store).task(taskId).runs == runs,
          "Two read-only answers added work or missed the model calls");

    const auto estimate =
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", "estimate"}, {"text", missingBudgetQuestion}}, 201);
    runtime.runConversations();
    const auto estimatePage = browser.get("/api/view?bot=release");
    const auto estimateReply = conversationReply(estimatePage, estimate.at("messageId"));
    check(estimateReply.at("text") == "見積もりに必要な明細の件数を教えてください。" && estimatePage.at("waits").empty() &&
              TaskList(store).tasks().size() == 1,
          "the Bot's question left pending state or started work");
    const auto detail =
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", "estimate-detail"}, {"text", budgetDetail}}, 201);
    runtime.runConversations();
    const auto estimateAnswer = conversationReply(browser.get("/api/view?bot=release"), detail.at("messageId"));
    check(estimateAnswer.at("text").get<std::string>().find("100件") != std::string::npos && TaskList(store).tasks().size() == 1 &&
              t3.launches == 1 && Tasks(store).task(taskId).runs == runs,
          "the follow-up answer created work or lost the context");

    const Json status = {{"botId", "release"}, {"clientMessageId", "with-invalid-settings"}, {"text", "進捗どうなってる"}};
    const auto statusReceipt = browser.post("/api/messages", status, 201);
    tick(runtime, 3);
    const auto statusReply = conversationReply(browser.get("/api/view?bot=release"), statusReceipt.at("messageId"));
    const auto statusReplyText = statusReply.at("text").get<std::string>();
    check(statusReplyText.find(comparisonRequest) != std::string::npos && statusReplyText.find("完了") != std::string::npos &&
              TaskList(store).tasks().size() == 1 && t3.launches == 1,
          "progress inquiry with changed settings failed to report completion without creating work");
    disableIntake = false;
    const auto firstWaitTask = TaskIntake(store).submit(web.intake("triage", "掲載する範囲を決める"));
    const auto secondWaitTask = TaskIntake(store).submit({"triage", "次回の掲載範囲を決める", "linked env", "next-scope"});
    tick(runtime, 3);
    t3.observations["thread-triage"] = {.kind = Observation::Kind::Question, .question = scopeQuestion()};
    t3.observations["thread-next-scope"] = t3.observations.at("thread-triage");
    tick(runtime, 2);
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
    check(question > 0 && otherQuestion > 0 && question != otherQuestion, "pending questions did not have distinct reply anchors");
    check(!firstWait.contains("decisionReason") && !secondWait.contains("decisionReason"),
          "the pending waits exposed the model's rationale");
    const auto& events = waitPage.at("events");
    const auto questionEvent =
        std::ranges::find_if(events, [&](const Json& event) { return event.at("kind") == "waiting" && event.at("id") == question; });
    check(questionEvent != events.end() && !questionEvent->at("data").contains("decisionReason"),
          "the conversation omitted the question or exposed the model's rationale");
    const Json reply = {{"botId", "triage"},
                        {"clientMessageId", "answer-first"},
                        {"text", "Status\n\nApprove\n\nseparately: keep this answer together"},
                        {"replyToMessageId", question}};
    const auto answerReceipt = browser.post("/api/messages", reply, 201);
    tick(runtime, 2);
    check(browser.post("/api/messages", reply, 201) == answerReceipt, "exact answer retry lost its receipt");
    auto delayed = reply;
    delayed["clientMessageId"] = "delayed-answer-first";
    delayed["text"] = "すべて";
    const auto delayedReceipt = browser.post("/api/messages", delayed, 201);
    tick(runtime, 2);
    const auto afterAnswer = browser.get("/api/view?bot=triage");
    const auto delayedReply = conversationReply(afterAnswer, delayedReceipt.at("messageId"));
    check(delayedReply.at("status") == "done" && delayedReply.at("actions").empty(),
          "The late choice was left pending or applied an action");
    check(t3.replies.size() == 1 && t3.replies[0].environmentId == "linked env" && t3.replies[0].target.threadId == "thread-triage" &&
              t3.replies[0].target.runId == firstWait["run"]["runId"].get<std::string>() && t3.replies[0].questionId == "scope-question" &&
              t3.replies[0].answers == Json{{"scope", reply["text"]}},
          "T3 did not receive exactly one matching answer for the original question");
    const auto& waits = afterAnswer.at("waits");
    check(waits.size() == 1 && waits[0]["id"] == secondWaitTask && waits[0]["wait"]["messageId"] == otherQuestion &&
              waits[0]["wait"]["reason"] == "user_question" && TaskList(store).tasks().size() == 3,
          "The freeform or stale answer changed another wait or created a task");

    const auto choice = browser.post(
        "/api/messages",
        {{"botId", "triage"}, {"clientMessageId", "answer-second"}, {"text", "利用者向けのみ"}, {"replyToMessageId", otherQuestion}}, 201);
    tick(runtime, 2);
    const auto afterChoice = browser.get("/api/view?bot=triage");
    const auto choiceReply = conversationReply(afterChoice, choice.at("messageId"));
    check(choiceReply.at("actions") ==
                  Json::array({{{"tool", "answer_task_question"}, {"effect", "answered"}, {"taskId", secondWaitTask}}}) &&
              afterChoice.at("waits").empty() && t3.replies.size() == 2 && t3.replies.back().target.threadId == "thread-next-scope" &&
              t3.replies.back().answers == Json{{"scope", "すべて"}} && choiceReply.at("text") == "回答を伝えました。",
          "The model answer did not reach the intended Session or its reply was lost");
}

void conversationMessagesAnotherBot(const std::filesystem::path& path) {
    struct ScriptedModel final : Model {
        std::function<Decision(const ModelInput&)> script;
        ModelTurn decide(const ModelInput& input) override { return {{script(input)}}; }
    } model;
    model.script = [&](const ModelInput& input) -> Decision {
        check(input.event.at("kind") == "conversation_turn", "Bot messaging started a task decision");
        const auto& steps = input.event.at("turn");
        const auto from = input.event.contains("botMessage") ? input.event.at("botMessage").at("fromBotId").get<std::string>() : "";
        const auto silent = [&]() -> Decision {
            check(std::ranges::find(input.tools, "no_reply") != input.tools.end(), "The Bot could not finish silently");
            return {"no_reply", Json::object(), {}};
        };
        if (from.empty()) {
            if (steps.empty()) return {"message_bot", {{"botId", "reviewer"}, {"text", "Can we ship today?"}}, "Ask Reviewer"};
            if (steps.size() == 1) return {"message_bot", {{"botId", "verifier"}, {"text", "Did the checks pass?"}}, "Ask Verifier"};
            return silent();
        }
        if (from == "ernest") {
            check(input.event.at("botMessage").at("fromName") == "ernest", "The receiving Bot lost the original sender");
            for (const auto* tool : {"request_bot_change", "update_bot_setup", "write_memory", "create_schedule", "request_secret_input"})
                check(std::ranges::find(input.tools, tool) == input.tools.end(), "another Bot's message was given the person's authority");
            if (!steps.empty()) return silent();
            const auto answer = input.event.at("text") == "Can we ship today?" ? "Yes, ship it." : "All checks passed.";
            return {"message_bot", {{"botId", "ernest"}, {"text", answer}}, "Answer Ernest"};
        }
        check((from == "reviewer" || from == "verifier") && input.event.at("botMessage").at("depth") == 2,
              "A Bot answer did not reach Ernest");
        check(std::ranges::any_of(input.history,
                                  [](const Json& entry) {
                                      return entry.contains("actions") && entry.at("actions").size() == 2 &&
                                             entry.at("actions").at(0).at("text") == "Can we ship today?" &&
                                             entry.at("actions").at(1).at("text") == "Did the checks pass?";
                                  }),
              "Ernest's history lost the consultation after finishing silently");
        if (!std::ranges::any_of(input.history, [&](const Json& entry) {
                return entry.contains("botMessage") && entry.at("botMessage").at("fromBotId") != from;
            }))
            return silent();
        return {"reply", {{"text", "Reviewer says we can ship. Verifier says the checks passed."}}, "Relay both answers"};
    };

    Store store(path);
    FixedT3 t3;
    const auto peers = [](std::string_view bot) -> std::vector<PeerBot> {
        if (bot == "ernest")
            return {{"Reviewer", {"reviewer", "", "local-env", "ernest"}}, {"Verifier", {"verifier", "", "local-env", "ernest"}}};
        if (bot == "reviewer" || bot == "verifier") return {{"Ernest", {"ernest", "", "local-env", std::string(bot)}}};
        return {};
    };
    auto web = callbacks();
    web.bots = [] {
        return Json::array({{{"id", "ernest"}, {"name", "Ernest"}},
                            {{"id", "reviewer"}, {"name", "Reviewer"}},
                            {{"id", "verifier"}, {"name", "Verifier"}}});
    };
    const IntakeResolver intake = [](std::string_view bot, std::string_view request) {
        return Intake{std::string(bot), std::string(request), "local-env", ""};
    };
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; }, RuntimeOptions{.peers = peers, .intake = intake});
    const auto server = startWeb(store, 0, web);
    Browser browser(*server);

    const Json consultation{{"botId", "ernest"}, {"clientMessageId", "ship"}, {"text", "Ask Reviewer and Verifier"}};
    const auto admitted = browser.post("/api/messages", consultation, 201);
    for (int i = 0; i < 6; ++i) runtime.runConversations();
    const auto reviewer = browser.get("/api/view?bot=reviewer").at("events");
    const auto inbound = std::ranges::find_if(
        reviewer, [](const Json& event) { return event.at("kind") == "conversation_message" && event.at("data").contains("botMessage"); });
    check(inbound != reviewer.end() && inbound->at("data").at("botMessage").at("fromBotId") == "ernest" &&
              inbound->at("data").at("text") == "Can we ship today?",
          "Reviewer's conversation did not show who sent the Bot message");
    const auto ernest = browser.get("/api/view?bot=ernest");
    check(std::ranges::count_if(ernest.at("events"), [](const Json& event) { return event.at("kind") == "conversation_reply"; }) == 1 &&
              hasEvent(ernest.at("events"), "conversation_reply", "Reviewer says we can ship. Verifier says the checks passed.") &&
              std::ranges::none_of(reviewer, [](const Json& event) { return event.at("kind") == "conversation_reply"; }),
          "The consultation did not produce exactly one combined reply");
    check(browser.post("/api/messages", consultation, 201) == admitted, "Retrying the silent turn changed its receipt");
    check(TaskList(store).tasks().empty() && t3.launches == 0, "Bot messaging created a task or used T3");
}

void silentBotSendKeepsTranscriptNavigation(const std::filesystem::path& path) {
    struct ScriptedModel final : Model {
        std::function<Decision(const ModelInput&)> script;
        ModelTurn decide(const ModelInput& input) override { return {{script(input)}}; }
    } model;
    model.script = [](const ModelInput& input) -> Decision {
        check(input.event.at("kind") == "conversation_turn", "Bot messaging started a task decision");
        if (input.event.contains("botMessage")) {
            check(std::ranges::find(input.tools, "no_reply") != input.tools.end(), "A Bot message could not finish silently");
            return {"no_reply", Json::object(), "Keep the handoff silent"};
        }
        if (input.event.at("turn").empty()) {
            check(std::ranges::find(input.tools, "message_bot") != input.tools.end(), "The sender could not message its peer");
            return {"message_bot", {{"botId", "reviewer"}, {"text", "Check the release."}}, "Send to Reviewer"};
        }
        check(std::ranges::find(input.tools, "no_reply") != input.tools.end(), "The sender could not finish silently after the send");
        return {"no_reply", Json::object(), "Keep the handoff silent"};
    };

    Store store(path);
    FixedT3 t3;
    const auto peers = [](std::string_view bot) -> std::vector<PeerBot> {
        if (bot == "ernest") return {{"Reviewer", {"reviewer", "", "local-env", "ernest"}}};
        if (bot == "reviewer") return {{"Ernest", {"ernest", "", "local-env", "reviewer"}}};
        return {};
    };
    auto web = callbacks();
    web.bots = [] { return Json::array({{{"id", "ernest"}, {"name", "Ernest"}}, {{"id", "reviewer"}, {"name", "Reviewer"}}}); };
    const IntakeResolver intake = [](std::string_view bot, std::string_view request) {
        return Intake{std::string(bot), std::string(request), "local-env", ""};
    };
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; }, RuntimeOptions{.peers = peers, .intake = intake});
    const auto server = startWeb(store, 0, web);
    Browser browser(*server);

    browser.post("/api/messages", {{"botId", "ernest"}, {"clientMessageId", "silent-handoff"}, {"text", "Ask Reviewer to check"}}, 201);
    for (int i = 0; i < 8; ++i) runtime.runConversations();

    const auto sender = browser.get("/api/view?bot=ernest");
    const auto recipient = browser.get("/api/view?bot=reviewer");
    const auto handoff =
        std::ranges::find_if(sender.at("events"), [](const Json& event) { return event.at("kind") == "conversation_handoff"; });
    check(handoff != sender.at("events").end() && handoff->at("data").at("peerBotId") == "reviewer" &&
              handoff->at("data").at("messageId").is_number_integer() && !handoff->at("data").contains("text"),
          "A silent Bot send did not preserve a text-free link to its transcript");
    check(std::ranges::any_of(recipient.at("events"),
                              [&](const Json& event) {
                                  return event.at("kind") == "conversation_message" &&
                                         event.at("id") == handoff->at("data").at("messageId") &&
                                         event.at("data").at("botMessage").at("fromBotId") == "ernest";
                              }),
          "The transcript link did not point to the delivered Bot message");
    check(std::ranges::none_of(sender.at("events"), [](const Json& event) { return event.at("kind") == "conversation_reply"; }) &&
              std::ranges::none_of(recipient.at("events"), [](const Json& event) { return event.at("kind") == "conversation_reply"; }),
          "A silent handoff created a chat reply");
    const Person reader{"handoff-reader"};
    BotAccess{store}.setMember(localOwner, "ernest", reader.id, true);
    check(!Policy{store}.allows(reader, Action::bot_view, "reviewer"), "The synthetic reader unexpectedly has peer access");
    const auto restricted = flockv0::web::conversation(store, "ernest", 0, &reader);
    const auto restrictedHandoff =
        std::ranges::find_if(restricted.at("events"), [](const Json& event) { return event.at("kind") == "conversation_handoff"; });
    check(restrictedHandoff != restricted.at("events").end() && !restrictedHandoff->at("data").contains("peerBotId") &&
              !restrictedHandoff->at("data").contains("messageId"),
          "The sender-only projection exposed peer transcript metadata");
    check(TaskList(store).tasks().empty() && t3.launches == 0, "Bot messaging created a task or used T3");
}

void botStartedTaskKeepsItsBotOrigin(const std::filesystem::path& path, std::string status, std::string report, bool failHandoff = false) {
    struct ScriptedModel final : Model {
        std::function<Decision(const ModelInput&)> script;
        ModelTurn decide(const ModelInput& input) override { return {{script(input)}}; }
    } model;
    const bool canFinishSilently = status == "completed" && !report.empty();
    const auto* replyText = status == "failed" ? "The release check failed." : "The release check has no readable result.";
    bool woke = false;
    bool handoffAttempted = false;
    model.script = [&](const ModelInput& input) -> Decision {
        if (input.event.contains("sessionWake")) {
            woke = true;
            check(input.event.contains("task") && input.event.contains("botMessage"), "the task wake lost the Bot that asked for the work");
            for (const auto* tool : {"request_bot_change", "update_bot_setup", "write_memory", "create_schedule", "request_secret_input"})
                check(std::ranges::find(input.tools, tool) == input.tools.end(), "a Bot-started task woke with the person's authority");
            const bool silentAvailable = std::ranges::find(input.tools, "no_reply") != input.tools.end();
            if (failHandoff && handoffAttempted) {
                check(!silentAvailable, "A failed Bot handoff still allowed the task wake to finish silently");
                return {"reply", {{"text", "The report could not be delivered to the requesting Bot."}}, "Report the failed handoff"};
            }
            check(silentAvailable == canFinishSilently,
                  "The silent tool was unavailable for a readable success or allowed for other results");
            if (failHandoff) {
                handoffAttempted = true;
                return {"message_bot", {{"botId", "ernest"}, {"text", "The task completed."}}, "Return the result"};
            }
            if (!canFinishSilently) return {"reply", {{"text", replyText}}, {}};
            return {"no_reply", Json::object(), {}};
        }
        if (!input.event.at("turn").empty()) return {"reply", {{"text", "Done."}}, "Tell the person"};
        if (input.event.contains("botMessage")) return {"start_task", {{"request", "Check the release"}}, "Do the work"};
        return {"message_bot", {{"botId", "reviewer"}, {"text", "Check the release"}}, "Ask Reviewer"};
    };
    Store store(path);
    FixedT3 t3;
    const auto peers = [](std::string_view bot) -> std::vector<PeerBot> {
        if (bot == "ernest") return {{"Reviewer", {"reviewer", "", "local-env", "ernest"}}};
        return {};
    };
    auto web = callbacks();
    web.bots = [] { return Json::array({{{"id", "ernest"}, {"name", "Ernest"}}, {{"id", "reviewer"}, {"name", "Reviewer"}}}); };
    const IntakeResolver intake = [](std::string_view bot, std::string_view request) {
        return Intake{std::string(bot), std::string(request), "local-env", ""};
    };
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; }, RuntimeOptions{.peers = peers, .intake = intake});
    const auto server = startWeb(store, 0, web);
    Browser browser(*server);
    for (const auto* reserved : {"bot-message:1:0", "task-wake:1:1"})
        browser.post("/api/messages", {{"botId", "ernest"}, {"clientMessageId", reserved}, {"text", "Hello"}}, 409);
    browser.post("/api/messages", {{"botId", "ernest"}, {"clientMessageId", "work"}, {"text", "Ask Reviewer to check"}}, 201);
    for (int i = 0; i < 12 && !woke; ++i) {
        runtime.runConversations();
        runtime.tick();
        if (t3.launches == 1)
            t3.observations["thread-release"] = {
                .kind = Observation::Kind::Finished, .status = status, .report = report, .itemId = "report"};
    }
    check(woke, "the Bot-started task never woke its conversation");
    const auto work = TaskList(store).tasks("reviewer");
    const auto page = browser.get("/api/view?bot=reviewer");
    if (failHandoff) {
        check(work.size() == 1 && work.front().task.state == TaskState::completed &&
                  conversationCompletion(page, work.front().task.id).at("data").at("text") == report &&
                  hasEvent(page.at("events"), "conversation_reply", "The report could not be delivered to the requesting Bot."),
              "The completed report or failed Bot handoff was not surfaced");
        return;
    }
    if (!canFinishSilently) {
        check(work.size() == 1 && work.front().task.state == TaskState::waiting &&
                  hasEvent(page.at("events"), "conversation_reply", replyText),
              "The failed or unreadable task result was silently closed");
        return;
    }
    check(work.size() == 1 && work.front().task.state == TaskState::completed &&
              conversationCompletion(page, work.front().task.id).at("data").at("text") == report &&
              std::ranges::count_if(page.at("events"), [](const Json& event) { return event.at("kind") == "conversation_reply"; }) == 1,
          "The silent wake did not retain the finished task report without posting a reply");
}

void botMessagesStopAtTheExchangeLimit(const std::filesystem::path& path) {
    struct ScriptedModel final : Model {
        std::function<Decision(const ModelInput&)> script;
        ModelTurn decide(const ModelInput& input) override { return {{script(input)}}; }
    } model;
    Json lastResult;
    int sends = 0;
    model.script = [&](const ModelInput& input) -> Decision {
        const auto& turn = input.event.at("turn");
        if (!turn.empty()) {
            lastResult = turn.back();
            return {"reply", {{"text", "Done."}}, "Tell the person"};
        }
        ++sends;
        return {"message_bot", {{"botId", input.task.at("bots").at(0).at("botId")}, {"text", "Thanks!"}}, "Keep talking"};
    };
    Store store(path);
    FixedT3 t3;
    const auto peers = [](std::string_view bot) -> std::vector<PeerBot> {
        if (bot == "ernest") return {{"Reviewer", {"reviewer", "", "local-env", "ernest"}}};
        if (bot == "reviewer") return {{"Ernest", {"ernest", "", "local-env", "reviewer"}}};
        return {};
    };
    auto web = callbacks();
    web.bots = [] { return Json::array({{{"id", "ernest"}, {"name", "Ernest"}}, {{"id", "reviewer"}, {"name", "Reviewer"}}}); };
    const IntakeResolver intake = [](std::string_view bot, std::string_view request) {
        return Intake{std::string(bot), std::string(request), "local-env", ""};
    };
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; }, RuntimeOptions{.peers = peers, .intake = intake});
    const auto server = startWeb(store, 0, web);
    Browser browser(*server);
    browser.post("/api/messages", {{"botId", "ernest"}, {"clientMessageId", "chat"}, {"text", "Say thanks to Reviewer"}}, 201);
    for (int i = 0; i < 20; ++i) runtime.runConversations();
    check(sends == 7 && lastResult.dump().find("at most 6 times") != std::string::npos,
          "two Bots kept messaging each other past the exchange limit");
}

void conversationBotSetup(const std::filesystem::path& directory) {
    app::Bots bots(directory / "bots");
    const auto report = [](auto, auto) {};
    const auto catalog = Json::array({{{"slug", "planner-next"}, {"reasoningEfforts", Json::array({"high", "xhigh"})}}});
    Store store(directory / "state.db",
                {.saveBotSetup = [&](Store& target, std::string_view bot, const BotSetupEdit& edit,
                                     const Json& choices) { bots.saveSetup(&target, bot, edit, unixTime(), report, choices); },
                 .botSetupCatalog = [&] { return catalog; }});
    const Person creator{"creator"};
    BotAccess{store}.setOwner(creator, "release");
    struct ProfileModel final : Model {
        Decision change;
        Json replyConfig;
        bool changedRoleSeen = false;
        unsigned conversationCalls = 0;
        bool pairReply = false;
        ModelTurn decide(const ModelInput& input) override {
            check(input.event.at("kind") == "conversation_turn", "a task asked the model for a decision");
            ++conversationCalls;
            if (input.event.at("turn").empty()) {
                if (pairReply) return {{{"reply", {{"text", "I updated my setup"}}, ""}, change}};
                return {{change}};
            }
            replyConfig = input.config;
            changedRoleSeen = input.notes.soul == "Run make check before reporting.";
            return {{{"reply", {{"text", "Processed the setup request"}}, ""}}};
        }
    } model;
    FixedT3 t3;
    RuntimeOptions options;
    options.applyBotChange = [&](const BotChange& change) { bots.apply(change); };
    options.intake = [&](std::string_view id, std::string_view text) { return bots.get(id).intake(text); };
    Runtime runtime(store, model, t3, [&](auto id) { return id == "release" ? std::string{} : bots.soul(id); }, options);
    model.change = {"request_bot_change", {{"operation", "create_bot"}, {"targetBotId", "reviewer"}, {"name", "Review team"}}, ""};
    ConversationIntake(store).receive(creator, {"release", "", "local-env", "release"}, "create", "Create a reviewer");
    runtime.runConversations();
    check(bots.ids() == std::vector<std::string>{"reviewer"} && Policy{store}.allows(creator, Action::bot_settings, "reviewer"),
          "Full did not apply Bot creation with the requesting person's ownership");
    bots.updateRunSettings(store, "reviewer", {{"model", "planner"}, {"environmentId", "env"}, {"projectId", "project"}}, unixTime(),
                           report);

    auto cb = callbacks();
    cb.bots = [&] { return bots.cards(); };
    cb.intake = options.intake;
    cb.updateBot = [&](const Person&, std::string_view bot, const Json& profile, const std::optional<AvatarImage>&) {
        return bots.saveSetup(&store, bot, {profile, {}}, unixTime(), report, catalog);
    };
    const auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    const auto send = [&](std::string_view key, Json patch) {
        model.change = {"update_bot_setup", std::move(patch), ""};
        browser.post("/api/messages", {{"botId", "reviewer"}, {"clientMessageId", key}, {"text", "Change how you work"}}, 201);
        runtime.runConversations();
    };
    const auto bot = [&] { return browser.get("/api/view?bot=reviewer").at("bot"); };
    const auto updates = [&] {
        Json notes = Json::array();
        const auto events = browser.get("/api/conversation?bot=reviewer").at("events");
        for (const auto& event : events)
            if (event.at("kind") == "bot_setup_updated") notes.push_back(event);
        return notes;
    };
    const auto undo = [&](const Json& note, int status = 200) {
        return browser.post("/api/bots/setup/undo", {{"botId", "reviewer"}, {"eventId", note.at("id")}}, status);
    };
    const Json patch{{"name", "Release Scout"},
                     {"soul", "Run make check before reporting."},
                     {"model", "planner-next"},
                     {"reasoningEffort", "high"},
                     {"environmentId", "next-env"},
                     {"projectId", "next-project"},
                     {"sessionModel", {{"model", "session-model"}, {"providerInstanceId", "provider"}}},
                     {"workspaceStrategy", {{"type", "worktree"}, {"baseRef", "origin/main"}}},
                     {"fixPullRequests", false},
                     {"avatar", {{"kind", "icon"}, {"icon", "compass"}}}};
    send("setup-all", patch);
    auto notes = updates();
    const auto updated = bot();
    check(notes.size() == 1 && notes.at(0).at("data").at("after").size() == patch.size() && updated.at("name") == "Release Scout" &&
              updated.at("sessionModel") == patch.at("sessionModel") && updated.at("workspaceStrategy") == patch.at("workspaceStrategy") &&
              updated.at("fixPullRequests") == false && updated.at("setupChangedBy").at("soul") == "bot" &&
              updated.at("setupChangedBy").size() == patch.size() && Inbox(store).inbox(localOwner).empty(),
          "Full did not update all settings with one attributed and reversible note");
    check(model.changedRoleSeen && model.replyConfig.at("model") == "planner-next" && model.replyConfig.at("reasoningEffort") == "high",
          "The next decision did not use the saved Role, model and effort");
    const auto cursor = browser.get("/api/view?bot=reviewer").at("historyCursor").get<Id>();
    undo(notes.at(0));
    check(browser.get("/api/view?bot=reviewer&changesAfter=" + std::to_string(cursor)).at("changedMessageIds") ==
              Json::array({notes.at(0).at("id")}),
          "Undo did not invalidate the original note for other tabs");
    check(bot().at("name") == "Review team" && bot().at("soul") == "" && bot().at("model") == "planner" &&
              !bot().contains("sessionModel") && updates().at(0).at("data").at("undone"),
          "Undo did not restore the previous settings and mark the note");

    const auto beforeRename = model.conversationCalls;
    model.pairReply = true;
    send("new-name", {{"name", "Reviewer"}});
    model.pairReply = false;
    check(model.conversationCalls == beforeRename + 1, "A setup-only turn made an additional model call");
    const auto renamed = updates().back();
    browser.post("/api/bots/profile", {{"botId", "reviewer"}, {"name", "Human override"}});
    browser.post("/api/bots/profile", {{"botId", "reviewer"}, {"name", "Reviewer"}});
    check(undo(renamed, 409).at("error") == "setup_changed" && bot().at("setupChangedBy").at("name") == "person",
          "Undo ignored an intervening edit to the same field");
    send("role-only", {{"soul", "Check the latest release."}});
    const auto roleChange = updates().back();
    browser.post("/api/bots/profile", {{"botId", "reviewer"}, {"name", "New display name"}});
    undo(roleChange);
    check(bot().at("name") == "New display name" && bot().at("soul") == "", "Undo overwrote or blocked an unrelated edit");

    browser.post("/api/bots/profile", {{"botId", "reviewer"}, {"model", "planner-next"}, {"reasoningEffort", "xhigh"}});
    send("clear-effort", {{"reasoningEffort", ""}});
    const auto clearedEffort = updates().back();
    browser.post("/api/bots/profile", {{"botId", "reviewer"}, {"model", "planner"}});
    check(undo(clearedEffort, 409).at("error") == "setup_changed" && !bot().contains("reasoningEffort"),
          "Undo restored effort for a different model");

    const auto noticeCount = [&] {
        return std::ranges::count_if(Conversations(store).conversation("reviewer", 0, 200),
                                     [](const Json& event) { return event.at("kind") == "bot_setup_updated"; });
    };
    const auto count = noticeCount();
    for (const auto* key : {"targetBotId", "runtimeMode", "members", "sharing", "channels", "secrets", "mode", "rules"}) {
        send(key, {{"name", "Invalid"}, {key, "restricted"}});
    }
    send("session-escalation", {{"sessionModel", {{"model", "t3"}, {"providerInstanceId", "x"}, {"runtimeMode", "full-access"}}}});
    send("large-role", {{"soul", std::string(8193, 'a')}, {"name", "Invalid"}});
    send("bad-session", {{"workspaceStrategy", {{"type", "worktree"}}}, {"name", "Invalid"}});
    send("bad-routing", {{"projectId", "has space"}, {"name", "Invalid"}});
    send("no-model", {{"model", ""}, {"name", "Invalid"}});
    PolicyRules{store}.changeNeverRule(localOwner, Action::bot_settings, "review*", true);
    send("denied", {{"name", "Invalid"}});
    PolicyRules{store}.changeNeverRule(localOwner, Action::bot_settings, "review*", false);
    check(noticeCount() == count && bot().at("name") == "New display name",
          "A forbidden or invalid setup update wrote some settings or emitted a notice");
}

constexpr auto blockers = "Summarize the release blockers.";
constexpr auto signingReport = "The signing certificate expired on 18 November.";
constexpr auto consultation = "Ask Verifier to confirm the release criteria.";
constexpr auto consultationReply = "Publish only after the certificate is renewed.";
constexpr auto whyBlocked = "Why is the release blocked?";
constexpr auto renewal = "When will the signing certificate be renewed?";
constexpr auto notes = "Prepare release notes.";
constexpr auto checksums = "Check artifact checksums in the backup environment.";

Json chatGPTToolCall(std::string_view name, Json arguments, std::string_view reason = {}) {
    if (!reason.empty()) arguments["reason"] = reason;
    return {{"type", "function_call"}, {"namespace", "flock"}, {"status", "completed"}, {"name", name}, {"arguments", arguments.dump()}};
}

Json chatGPTConversationTool(const Json& task, const Json& event) {
    if (event.contains("sessionWake")) return chatGPTToolCall("reply", {{"text", "The work finished."}});
    const auto text = event.at("text").get<std::string>();
    const auto& steps = event.at("turn");
    if (event.contains("botMessage") && steps.empty()) {
        const auto from = event.at("botMessage").at("fromBotId").get<std::string>();
        if (from == "release") {
            check(text == "What are the release criteria?", "Verifier did not receive the release Bot's question");
            return chatGPTToolCall("message_bot", {{"botId", "release"}, {"text", consultationReply}});
        }
        check(from == "verifier" && text == consultationReply, "the release Bot did not receive Verifier's answer");
        return chatGPTToolCall("reply", {{"text", std::string("Verifier: ") + consultationReply}});
    }
    if (!steps.empty()) {
        const auto& last = steps.back();
        const auto& result = last.at("result");
        check(result.at("ok") == true, "ChatGPT tool did not succeed: " + result.dump());
        if (last.at("tool") == "read_result") {
            check(result.at("text") == signingReport, "read_result did not return the saved report");
            if (text == whyBlocked)
                return chatGPTToolCall("reply", {{"text", "The release is blocked by the expired signing certificate."}});
            check(text == renewal, "unexpected follow-up after reading the report");
            return chatGPTToolCall("reply", {{"text", "What is the planned certificate renewal date?"}});
        }
        return chatGPTToolCall("reply", {{"text", "I queued your request."}});
    }
    if (text == consultation) {
        check(std::ranges::any_of(task.at("bots"), [](const Json& bot) { return bot.at("botId") == "verifier"; }),
              "the Verifier Bot was missing from the conversation context");
        return chatGPTToolCall("message_bot", {{"botId", "verifier"}, {"text", "What are the release criteria?"}});
    }
    if (text == blockers || text == notes) return chatGPTToolCall("start_task", {{"request", text}});
    if (text == checksums) {
        check(std::ranges::any_of(task.at("environments"), [](const Json& env) { return env.at("name") == "backup-env"; }),
              "the backup destination was missing from the production request");
        return chatGPTToolCall("start_task", {{"request", text}, {"environment", "backup-env"}});
    }
    if (text == whyBlocked || text == renewal) {
        const auto& tasks = task.at("tasks");
        const auto source = std::ranges::find_if(tasks, [](const Json& item) { return item.at("request") == blockers; });
        check(source != tasks.end() && source->at("latestResult").at("excerpt") == signingReport,
              "the saved report was missing from the conversation context");
        return chatGPTToolCall("read_result", {{"messageId", source->at("latestResult").at("messageId")}});
    }
    check(text == "それ止めて", "unexpected composer message: " + text);
    std::vector<std::string> active;
    for (const auto& item : task.at("tasks"))
        if (item.at("state") == "running") active.push_back(item.at("request"));
    auto sorted = active;
    std::ranges::sort(sorted);
    check(sorted == std::vector<std::string>{checksums, notes}, "expected both running tasks in the stop request context");
    return chatGPTToolCall("reply", {{"text", "どの作業を停止しますか？"}, {"suggestions", active}});
}

Json chatGPTOutput(const Json& task, const Json& event) {
    check(event.at("kind") == "conversation_turn", "a conversation task asked the task model: " + event.at("kind").dump());
    return chatGPTConversationTool(task, event);
}

app::chatgpt::HttpResponse chatGPTResponse(const app::chatgpt::HttpRequest& http) {
    check(http.url == "https://api.openai.com/v1/responses" && http.authorization == "Bearer fixture-access" &&
              http.contentType == "application/json" && http.accept == "text/event-stream",
          "unexpected ChatGPT request URL or headers");
    const auto request = Json::parse(http.body);
    check(request.at("model") == "fixture-model" && request.at("stream") == true,
          "ChatGPT request did not select the model and enable streaming");
    const auto& group = request.at("tools").at(0);
    check(group.at("name") == "flock", "the Flock tool namespace was missing");
    const auto context = Json::parse(request.at("input").at(0).at("content").at(0).at("text").get<std::string>());
    const auto& event = context.at("event");
    const auto result = chatGPTOutput(context.at("task"), event);
    const auto& tools = group.at("tools");
    const auto tool = std::ranges::find_if(tools, [&](const Json& item) { return item.at("name") == result.at("name"); });
    check(tool != tools.end(), "response tool was missing from the production request: " + result.at("name").dump());
    const auto args = Json::parse(result.at("arguments").get<std::string>());
    const auto& parameters = tool->at("parameters");
    for (const auto& key : parameters.at("required"))
        check(args.contains(key.get<std::string>()), "fixture omitted a required tool argument: " + key.dump());
    const auto& properties = parameters.at("properties");
    for (const auto& [key, value] : args.items()) check(properties.contains(key), "fixture used an unknown tool argument: " + key);
    if (event.at("kind") == "conversation_turn")
        check(request.at("instructions").get_ref<const std::string&>().find("You are a Flock Bot talking with your user") !=
                  std::string::npos,
              "ChatGPT did not receive the conversation instructions");
    if (event.at("kind") == "bot_request")
        check(properties.at("kind")
                      .at("description")
                      .get_ref<const std::string&>()
                      .find("Use result to return work or consultation advice") != std::string::npos,
              "ChatGPT did not receive the consultation reply instructions");

    const Json response = {{"type", "response.completed"}, {"response", {{"output", Json::array({result})}}}};
    const auto stream = "data: " + response.dump() + "\n\n";
    const app::chatgpt::HttpResponse metadata{200, "text/event-stream", {}, {}};
    check(static_cast<bool>(http.onData), "ChatGPT request did not accept response chunks");
    check(http.onData(std::string_view(stream).substr(0, stream.size() - 1), metadata),
          "ChatGPT stopped reading before the SSE event delimiter");
    check(!http.onData("\n", metadata), "ChatGPT kept reading after receiving a completion event");
    return metadata;
}

void chatGPTConversation(const std::filesystem::path& home) {
    const auto credentials = home / "credentials" / "chatgpt";
    std::filesystem::create_directories(credentials);
    check(chmod(credentials.c_str(), 0700) == 0, "could not protect the ChatGPT test directory");
    const auto expires =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch() + std::chrono::hours(1))
            .count();
    const Json profile = {{"subject", "fixture"},
                          {"accessToken", "fixture-access"},
                          {"refreshToken", "fixture-refresh"},
                          {"granted", {"chatgpt.tokens.use.direct"}},
                          {"expiresAt", expires}};
    const auto profilePath = credentials / "profile.json";
    std::ofstream(profilePath) << profile.dump();
    check(chmod(profilePath.c_str(), 0600) == 0, "could not protect the ChatGPT test profile");
    const auto writeBot = [&](std::string_view id, Json config) {
        const auto directory = home / "bots" / id;
        std::filesystem::create_directories(directory);
        config.update(Json{{"modelProvider", "chatgpt"}, {"model", "fixture-model"}, {"environmentId", "local-env"}, {"projectId", id}});
        std::ofstream(directory / "config.json") << config.dump();
    };
    writeBot("release", {{"name", "Release"}});
    writeBot("verifier", {{"name", "Verifier"}});
    const app::Bots bots(home / "bots");
    Store store(home / "conversation.sqlite", {.memorySeed = [&](std::string_view bot) { return bots.initialMemory(bot); }});
    BotAccess(store).setOwner(localOwner, "release");
    BotAccess(store).setOwner(localOwner, "verifier");
    T3Connections(store).connect("backup-env", localOwner.id, 48123);
    std::exception_ptr fixtureError;
    app::ChatGPTModel model(credentials, [&](const app::chatgpt::HttpRequest& request) {
        try {
            return chatGPTResponse(request);
        } catch (...) {
            if (!fixtureError) fixtureError = std::current_exception();
            throw;
        }
    });
    FixedT3 t3;
    RuntimeOptions options;
    options.peers = [&](std::string_view bot) { return bots.peers(store, bot); };
    options.intake = [&](std::string_view bot, std::string_view request) { return bots.get(bot).intake(request); };
    Runtime runtime(store, model, t3, [&](std::string_view bot) { return bots.soul(bot); }, options);
    const auto advance = [&](int steps) {
        tick(runtime, steps);
        if (fixtureError) std::rethrow_exception(fixtureError);
    };
    const auto server = startWeb(store, 0,
                                 {
                                     [&] { return bots.cards(); },
                                     [&](std::string_view bot, std::string_view request) { return bots.get(bot).intake(request); },
                                 });
    Browser browser(*server);
    const auto send = [&](std::string_view key, std::string_view text) -> Id {
        const auto receipt = browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", key}, {"text", text}}, 201);
        check(receipt.at("status") == "received", "composer did not acknowledge the message");
        return receipt.at("messageId");
    };
    const auto view = [&] { return browser.get("/api/view?bot=release"); };
    const auto linkedTask = [&](Id messageId) -> Id {
        const auto page = view();
        const auto& events = page.at("events");
        const auto message = std::ranges::find_if(
            events, [&](const Json& event) { return event.at("kind") == "conversation_message" && event.at("id") == messageId; });
        check(message != events.end() && message->at("taskId").is_number_integer(), "composer message was not linked to its Task");
        return message->at("taskId");
    };

    const auto request = send("blockers", blockers);
    check(TaskList(store).tasks().empty(), "composer created a Task before the Bot's turn");
    advance(5);
    check(conversationReply(view(), request).at("actions").at(0).at("tool") == "start_task" && TaskList(store).tasks().size() == 1,
          "ordinary composer request did not create exactly one Task");
    const auto releaseTask = linkedTask(request);
    check(Tasks(store).task(releaseTask).task.state == TaskState::running && t3.launches == 1, "the Task did not start one Run");
    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed", .report = signingReport, .itemId = "blockers-report"};
    advance(3);
    const auto completed = Tasks(store).task(releaseTask);
    check(completed.task.state == TaskState::completed, "the Session report did not become a completed result");
    const auto result = conversationCompletion(view(), releaseTask);
    check(result.at("data").at("run").at("itemId") == "blockers-report", "the result lost the report item");
    t3.observations.clear();

    const auto question = send("why-blocked", whyBlocked);
    advance(3);
    const auto answer = conversationReply(view(), question);
    check(answer.at("route") == "reply" && answer.at("text") == "The release is blocked by the expired signing certificate." &&
              answer.at("actions").empty(),
          "result answer was not grounded in the saved report");
    const auto missing = send("renewal-date", renewal);
    advance(3);
    const auto clarification = conversationReply(view(), missing);
    check(clarification.at("text") == "What is the planned certificate renewal date?",
          "missing evidence did not produce a question for the user");
    check(TaskList(store).tasks().size() == 1 && t3.launches == 1 && Tasks(store).task(releaseTask).runs == completed.runs,
          "result follow-ups created a Task or changed Runs");

    const auto consult = send("consult-verifier", consultation);
    advance(8);
    const auto asked = std::ranges::any_of(Conversations(store).conversation("verifier"), [](const Json& event) {
        return event.at("kind") == "conversation_message" && event.at("data").contains("botMessage") &&
               event.at("data").at("botMessage").at("fromBotId") == "release" &&
               event.at("data").at("text") == "What are the release criteria?";
    });
    check(asked && hasEvent(view().at("events"), "conversation_reply", std::string("Verifier: ") + consultationReply) &&
              TaskList(store).tasks().size() == 1 && t3.launches == 1,
          "the release Bot did not ask Verifier and relay its answer without starting work");
    (void)consult;

    const auto first = send("notes", notes);
    const auto second = send("checksums", checksums);
    advance(5);
    const auto firstTask = Tasks(store).task(linkedTask(first));
    const auto secondTask = Tasks(store).task(linkedTask(second));
    const auto& firstRuns = firstTask.runs;
    const auto& secondRuns = secondTask.runs;
    check(!firstRuns.empty() && !secondRuns.empty(), "both composer requests must launch a Run");
    check(firstRuns.back().run.environmentId == "local-env" && firstTask.task.projectId == "release" &&
              secondRuns.back().run.environmentId == "backup-env" && secondTask.task.projectId == "backup-release",
          "the composer Runs did not keep each request on its own destination");
    const auto active = view().at("activeTasks");
    check(
        firstTask.task.state == TaskState::running && secondTask.task.state == TaskState::running && active.size() == 2 && t3.launches == 3,
        "ambiguous-stop fixture requires two active Runs: " + std::string(taskStateName(firstTask.task.state)) + ", " +
            std::string(taskStateName(secondTask.task.state)) + ", active=" + active.dump() + ", launches=" + std::to_string(t3.launches));
    const auto taskCount = TaskList(store).tasks().size();
    const auto stop = send("stop", "それ止めて");
    advance(3);
    const auto afterStop = view();
    const auto confirm = conversationReply(afterStop, stop);
    const auto& suggestions = confirm.at("suggestions");
    check(confirm.at("text") == "どの作業を停止しますか？" && suggestions.size() == 2 &&
              std::ranges::all_of(active,
                                  [&](const Json& run) { return std::ranges::find(suggestions, run.at("request")) != suggestions.end(); }),
          "ambiguous stop did not ask the user to choose between both running tasks");
    check(afterStop.at("activeTasks") == active && TaskList(store).tasks().size() == taskCount && t3.launches == 3 &&
              Tasks(store).task(firstTask.task.id).runs == firstTask.runs &&
              Tasks(store).task(secondTask.task.id).runs == secondTask.runs && t3.interruptions == 0,
          "ambiguous stop changed a Run or created work");

    writeBot("release", {{"name", "Release"}, {"conversationEnvironments", Json::object()}});
    const auto rejected =
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", "removed-setting"}, {"text", "Try the invalid Bot"}}, 400);
    check(rejected.at("error") == "removed_bot_setting" &&
              rejected.at("message").get<std::string>().find("conversationEnvironments") != std::string::npos &&
              !WebConversationIntake(store).receipt(localOwner, "release", "removed-setting", "Try the invalid Bot", 0, Json::array()),
          "Web intake accepted a removed Bot setting or omitted its name from the error");
}

void botRolePreviewRedactsBeforeBounding(const std::filesystem::path& path) {
    Store store(path);
    const auto visible = "Role: " + std::string(134, 'a');
    const auto protectedValue = "RolePreviewFixture-" + std::string(40, 'z') + "\ncredential suffix";
    auto cb = callbacks();
    cb.bots = [&] {
        return Json::array({{{"id", "release"}, {"name", "Release"}, {"soul", visible + protectedValue + "\nRole details"}}});
    };
    SecretRequests secrets{store};
    const auto request = secrets.requestSecret("release", {"fixture-role-token"});
    secrets.submitSecret(localOwner, "release", request, protectedValue);
    auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    check(browser.get("/api/bots").at(0).at("about") == visible + "[redacted]",
          "Bot role preview must redact a multiline secret before splitting or truncating");
}

void botRoleSavesRequireCompleteOriginal(const std::filesystem::path& home) {
    const auto botDir = home / "bots" / "release";
    std::filesystem::create_directories(botDir);
    std::ofstream(botDir / "config.json") << R"({"name":"Release"})";
    const auto roleFile = botDir / "SOUL.md";
    const auto writeRole = [&](const std::string& text) { std::ofstream(roleFile, std::ios::binary) << text; };
    const auto readRole = [&] {
        std::ifstream input(roleFile, std::ios::binary);
        check(static_cast<bool>(input), "could not read role fixture");
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    app::Bots bots(home / "bots");
    Store store(home / "flock.db");
    SecretRequests secrets{store};
    const auto request = secrets.requestSecret("release", {"fixture-role-token"});
    secrets.submitSecret(localOwner, "release", request, privateValue);
    auto cb = callbacks();
    cb.bots = [&] { return bots.cards(); };
    cb.updateBot = [&](const Person&, std::string_view bot, const Json& profile, const std::optional<AvatarImage>& image) {
        return bots.updateProfile(bot, profile, image);
    };
    auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    const auto viewRole = [&] { return browser.get("/api/view?bot=release").at("bot").at("soul"); };
    const auto save = [&](const Json& original, std::string_view edit, int status) {
        return browser.post("/api/bots/profile", {{"botId", "release"}, {"soul", edit}, {"originalSoul", original}}, status);
    };
    const auto expectRefused = [&](const Json& original, std::string_view edit, const std::string& unchanged) {
        check(save(original, edit, 409).at("error") == "role_changed" && readRole() == unchanged, "refused role edit changed SOUL.md");
    };

    const auto withSecret = "Review releases using " + std::string(privateValue);
    writeRole(withSecret);
    const auto redacted = viewRole();
    check(redacted == "Review releases using [redacted]", "role settings did not show redacted text");
    expectRefused(redacted, "Review release candidates", withSecret);
    expectRefused(withSecret, "Review release candidates", withSecret);

    const auto prefix = std::string(maxBotRoleBytes, 'a');
    const auto oversized = prefix + "Unseen instructions";
    writeRole(oversized);
    const auto truncated = viewRole();
    check(truncated == prefix, "role settings did not show the bounded preview");
    expectRefused(truncated, "", oversized);
    expectRefused(prefix + "U", "", oversized);

    const std::string original = "Review releases.\r\nCheck the tests.\r\n";
    writeRole(original);
    const auto stale = viewRole();
    check(stale == original, "role settings changed the original line endings");
    const std::string changed = "Review weekly releases.";
    writeRole(changed);
    expectRefused(stale, "Review monthly releases.", changed);

    writeRole(prefix);
    const auto edited = save(viewRole(), "Review monthly releases.", 200);
    check(edited.at("soul") == "Review monthly releases." && readRole() == "Review monthly releases.",
          "editing a complete role did not persist the new text");
    check(save(viewRole(), "", 200).at("soul") == "" && !std::filesystem::exists(roleFile),
          "clearing a complete role did not remove SOUL.md");
}

void secretCollisionKeepsBotRoutes(const std::filesystem::path& path) {
    Store store(path);
    auto cb = callbacks();
    cb.bots = [] {
        return Json::array(
            {{{"id", "release"}, {"name", "Release"}}, {{"id", "staging-triage"}, {"name", "Staging"}, {"soul", "Check staging"}}});
    };
    cb.authenticate = [](std::string_view auth) {
        if (auth == "Bearer fixture-owner") return Person{"owner", true};
        if (auth == "Bearer fixture-alice") return Person{"alice"};
        return Person{};
    };
    cb.updateBot = [](const Person&, std::string_view id, const Json& profile, const std::optional<AvatarImage>&) {
        return Json{{"id", id}, {"name", profile.at("name")}};
    };
    const Person owner{"owner", true};
    BotAccess{store}.setMember(owner, "staging-triage", "alice", true);
    SecretRequests secrets{store};
    const auto request = secrets.requestSecret("release", {"fixture-token"});
    const auto ampRequest = secrets.requestSecret("release", {"renderer-amp"});
    const auto markdownRequest = secrets.requestSecret("release", {"renderer-markdown"});
    const auto semanticRequest = secrets.requestSecret("staging-triage", {"renderer-semantic"});
    const auto formattedRequest = secrets.requestSecret("staging-triage", {"renderer-formatted"});
    const auto adjacentRequest = secrets.requestSecret("staging-triage", {"renderer-adjacent"});
    const auto formRequest = secrets.requestSecret("staging-triage", {"renderer-form"});
    const auto send = [&](std::string_view bot, std::string_view clientId, std::string_view text) {
        return ConversationIntake(store)
            .receive(owner, {std::string(bot), "unused", "local-env", "fixture"}, clientId, text)
            .at("messageId")
            .get<Id>();
    };
    const std::string ampSecret = "Synthetic&Credential-221";
    const std::string markdownSecret = "**Markdown&Credential-221**";
    const std::string ampText = "Amp handoff " + ampSecret + " end";
    const std::string markdownText = "Markdown handoff " + markdownSecret + " end";
    const auto oldMessage = send("staging-triage", "old", "Original handoff for staging");
    const auto otherBotMessage = send("release", "other", "Unrelated handoff");
    const auto historyAmpMessage = send("staging-triage", "history-amp", ampText);
    const auto historyMarkdownMessage = send("staging-triage", "history-markdown", markdownText);
    const std::string rawSemanticText = "A&B";
    const std::string entitySemanticText = "A&amp;B";
    const std::string formattedSemanticText = "A**x**B";
    const std::string adjacentSemanticText = "[A](https://example.com)B";
    const std::string encodedUrlText = "[credential](https://example.com/A%26B)";
    const std::string doubleEncodedUrlText = "[credential](https://example.com/?token=A%2526B)";
    const std::string decodedUrlText = "[credential](https://example.com/?q=A&amp;B)";
    const std::string formUrlText = "[credential](https://example.com/?q=A+B)";
    const std::string hashFormUrlText = "[credential](https://example.com/?q=%23A+B)";
    const std::string safeRichText = "**safe** [docs](https://example.com/path%252Fpart)";
    const auto rawSemanticMessage = send("staging-triage", "semantic-raw", rawSemanticText);
    const auto formattedSemanticMessage = send("staging-triage", "semantic-formatted", formattedSemanticText);
    const auto adjacentSemanticMessage = send("staging-triage", "semantic-adjacent", adjacentSemanticText);
    const auto encodedUrlMessage = send("staging-triage", "semantic-url", encodedUrlText);
    const auto doubleEncodedUrlMessage = send("staging-triage", "semantic-double-encoded-url", doubleEncodedUrlText);
    const auto decodedUrlMessage = send("staging-triage", "semantic-decoded-url", decodedUrlText);
    const auto formUrlMessage = send("staging-triage", "semantic-form-url", formUrlText);
    const auto hashFormUrlMessage = send("staging-triage", "semantic-hash-form-url", hashFormUrlText);
    for (int i = 0; i < 40; ++i) send("staging-triage", "filler-" + std::to_string(i), "Later update");
    const auto viewAmpMessage = send("staging-triage", "view-amp", ampText);
    const auto viewMarkdownMessage = send("staging-triage", "view-markdown", markdownText);
    const auto entitySemanticMessage = send("staging-triage", "semantic-entity", entitySemanticText);
    const auto safeRichMessage = send("staging-triage", "semantic-safe-rich", safeRichText);
    auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization", "Bearer fixture-owner"}});
    browser.post("/api/bot-settings/secret", {{"botId", "release"}, {"requestId", request}, {"value", "staging"}});
    browser.post("/api/bot-settings/secret", {{"botId", "release"}, {"requestId", ampRequest}, {"value", ampSecret}});
    browser.post("/api/bot-settings/secret", {{"botId", "release"}, {"requestId", markdownRequest}, {"value", markdownSecret}});
    browser.post("/api/bot-settings/secret", {{"botId", "staging-triage"}, {"requestId", semanticRequest}, {"value", "A&B"}});
    browser.post("/api/bot-settings/secret", {{"botId", "staging-triage"}, {"requestId", formattedRequest}, {"value", "AxB"}});
    browser.post("/api/bot-settings/secret", {{"botId", "staging-triage"}, {"requestId", adjacentRequest}, {"value", "AB"}});
    browser.post("/api/bot-settings/secret", {{"botId", "staging-triage"}, {"requestId", formRequest}, {"value", "A B"}});
    const auto saved = browser.post("/api/bots/profile", {{"botId", "staging-triage"}, {"name", "Triage"}});
    check(saved.at("id") == "staging-triage", "profile route changed the Bot ID");

    browser.client.set_default_headers({{"Authorization", "Bearer fixture-alice"}});
    const auto bots = browser.get("/api/bots");
    check(bots.size() == 1 && bots.at(0).at("id") == "staging-triage", "secret redaction hid or renamed an authorized Bot");
    const auto viewResponse = browser.get("/api/view?bot=staging-triage");
    const auto view = viewResponse.at("bot");
    check(view.at("id") == "staging-triage" && view.at("soul") == "Check [redacted]",
          "Bot view must preserve its ID and redact display text");
    const auto verifyRenderedMessage = [&](const Json& events, Id id, std::string_view expectedText) {
        const auto event = std::ranges::find_if(events, [&](const Json& row) { return row.at("id").get<Id>() == id; });
        check(event != events.end(), "rendered conversation message was missing from its response page");
        const auto& data = event->at("data");
        const auto html = data.at("textHtml").get<std::string>();
        check(data.at("text") == expectedText && html == "<p>" + std::string(expectedText) + "</p>" &&
                  html.find(ampSecret) == std::string::npos && html.find(markdownSecret) == std::string::npos &&
                  html.find("&amp;") == std::string::npos,
              "conversation response must redact source and rendered HTML before Markdown parsing");
    };
    verifyRenderedMessage(viewResponse.at("events"), viewAmpMessage, "Amp handoff [redacted] end");
    verifyRenderedMessage(viewResponse.at("events"), viewMarkdownMessage, "Markdown handoff [redacted] end");
    const auto verifyProtectedHtml = [&](const Json& events, Id id, std::string_view expectedText) {
        const auto event = std::ranges::find_if(events, [&](const Json& row) { return row.at("id").get<Id>() == id; });
        check(event != events.end(), "protected Markdown message was missing from its response page");
        const auto& data = event->at("data");
        const auto actualText = data.at("text").get<std::string>();
        const auto actualHtml = data.at("textHtml").get<std::string>();
        check(actualText == expectedText && actualHtml == "<p>[redacted]</p>",
              "protected HTML semantics were not replaced for message " + std::to_string(id) + ": " + actualText + " / " + actualHtml);
    };
    const auto verifySafeRichHtml = [&](const Json& events) {
        const auto event = std::ranges::find_if(events, [&](const Json& row) { return row.at("id").get<Id>() == safeRichMessage; });
        check(event != events.end() && event->at("data").at("text") == safeRichText &&
                  event->at("data").at("textHtml") == "<p><strong>safe</strong> <a href=\"https://example.com/path%252Fpart\">docs</a></p>",
              "ordinary safe Markdown lost rich rendering");
    };
    const auto verifyRecentSemanticMessages = [&](const Json& events) {
        verifyProtectedHtml(events, entitySemanticMessage, entitySemanticText);
        verifySafeRichHtml(events);
    };
    const auto verifyOlderSemanticMessages = [&](const Json& events) {
        verifyProtectedHtml(events, rawSemanticMessage, "[redacted]");
        verifyProtectedHtml(events, formattedSemanticMessage, formattedSemanticText);
        verifyProtectedHtml(events, adjacentSemanticMessage, adjacentSemanticText);
        verifyProtectedHtml(events, encodedUrlMessage, encodedUrlText);
        verifyProtectedHtml(events, doubleEncodedUrlMessage, doubleEncodedUrlText);
        verifyProtectedHtml(events, decodedUrlMessage, decodedUrlText);
        verifyProtectedHtml(events, formUrlMessage, formUrlText);
        verifyProtectedHtml(events, hashFormUrlMessage, hashFormUrlText);
    };
    verifyRecentSemanticMessages(viewResponse.at("events"));
    const auto storedAmp = Conversations(store).conversation("staging-triage", viewAmpMessage + 1, 1);
    const auto storedMarkdown = Conversations(store).conversation("staging-triage", viewMarkdownMessage + 1, 1);
    check(storedAmp.size() == 1 && storedAmp.front().at("data").at("text") == ampText && storedMarkdown.size() == 1 &&
              storedMarkdown.front().at("data").at("text") == markdownText,
          "response redaction must preserve canonical journal text");
    const std::string history = "/api/conversation?bot=staging-triage";
    const auto latest = browser.get(history);
    const auto older = browser.get(history + "&before=" + latest.at("olderBefore").dump());
    const auto exact = browser.get(history + "&before=" + std::to_string(oldMessage + 1));
    check(latest.at("events").front().at("id") > oldMessage && older.at("events").front().at("id") == oldMessage,
          "An older match must be reachable through the same Bot conversation projection");
    verifyRecentSemanticMessages(latest.at("events"));
    verifyOlderSemanticMessages(older.at("events"));
    verifyRenderedMessage(older.at("events"), historyAmpMessage, "Amp handoff [redacted] end");
    verifyRenderedMessage(older.at("events"), historyMarkdownMessage, "Markdown handoff [redacted] end");
    check(exact.at("events").size() == 1 && exact.at("events").back().at("id") == oldMessage &&
              exact.at("events").back().at("data").at("text") == "Original handoff for [redacted]",
          "The older conversation projection must redact its text and omit other Bots' messages");
    const auto requireStored = [&](Id id, std::string_view text) {
        const auto stored = Conversations(store).conversation("staging-triage", id + 1, 1);
        check(stored.size() == 1 && stored.front().at("data").at("text") == text, "render-time redaction mutated canonical journal text");
    };
    requireStored(rawSemanticMessage, rawSemanticText);
    requireStored(entitySemanticMessage, entitySemanticText);
    requireStored(formattedSemanticMessage, formattedSemanticText);
    requireStored(adjacentSemanticMessage, adjacentSemanticText);
    requireStored(encodedUrlMessage, encodedUrlText);
    requireStored(doubleEncodedUrlMessage, doubleEncodedUrlText);
    requireStored(decodedUrlMessage, decodedUrlText);
    requireStored(formUrlMessage, formUrlText);
    requireStored(hashFormUrlMessage, hashFormUrlText);
    const auto foreign = browser.get(history + "&before=" + std::to_string(otherBotMessage + 1));
    check(foreign.at("events").back().at("id") != otherBotMessage, "An exact source lookup must stay in the selected Bot");
    Browser::checked(browser.client.Get("/api/conversation?bot=release&before=" + std::to_string(otherBotMessage + 1)), 403);
    Browser::checked(browser.client.Get("/api/view?bot=release"), 403);
}

struct AuthorityModel final : Model {
    std::vector<std::string> inputs;
    std::function<void()> demoteDuringCall;
    bool leakedCredential = false;

    ModelTurn decide(const ModelInput& input) override {
        if (input.event.at("kind") == "intake")
            return {{{"delegate", {{"instruction", "Check the release"}, {"doneWhen", "Report the result"}}, "Start the task"}}};
        const auto request = input.event.at("text").get<std::string>();
        inputs.push_back(request);
        leakedCredential = leakedCredential ||
                           (input.event.dump() + input.task.dump() + input.history.dump() + input.config.dump()).find("Bearer fixture-") !=
                               std::string::npos;
        const auto first = input.event.at("turn").empty();
        if (request == "Demoted during model call") demoteDuringCall();
        const auto call = first ? Decision{"start_task", {{"request", request}}, "Start requested work"}
                                : Decision{"reply", {{"text", "Authorized conversation reply"}}, "Reply to the user"};
        return {{call}};
    }
};

void queuedWebAuthority(const std::filesystem::path& path) {
    constexpr auto demoted = "Bearer fixture-demoted";
    constexpr auto removed = "Bearer fixture-removed";
    constexpr auto retained = "Bearer fixture-retained";
    constexpr auto memberCredential = "Bearer fixture-member";
    constexpr auto alternate = "Bearer fixture-alternate";
    constexpr auto view = "/api/view?bot=release";
    const auto identities = path.string() + ".identities";
    const auto identity = [](const char* credential, std::string_view person, bool root) {
        return Json{{"authorization", credential}, {"person", person}, {"adminRoot", root}};
    };
    const auto setup = [&](Store& store, const Json& accounts) {
        writeWebIdentities(identities, accounts);
        auto web = callbacks();
        configureWebIdentities(store, identities, web);
        return startWeb(store, 0, std::move(web));
    };
    const auto signIn = [](Browser& browser, const char* credential) {
        browser.client.set_default_headers({{"Authorization", credential}});
    };
    const auto send = [&](Browser& browser, const char* credential, std::string_view request) -> Id {
        signIn(browser, credential);
        return browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", request}, {"text", request}}, 201).at("messageId");
    };
    AuthorityModel model;
    FixedT3 t3;
    Id demotedQueue, removedQueue, retainedQueue, memberQueue, priorTask, mixedQueue, trustedQueue, taskFromDemotedTurn;
    {
        Store store(path);
        auto server = setup(store, Json::array({identity(demoted, localOwner.id, true), identity(removed, "removed", true),
                                                identity(retained, "retained", true), identity(memberCredential, "member", true),
                                                identity(alternate, "mixed", true)}));
        Browser browser(*server);
        BotAccess{store}.setMember(localOwner, "release", "member", true);
        BotAccess{store}.setMember(localOwner, "release", "removed", true);
        priorTask = send(browser, demoted, "Work requested before demotion");
        Runtime runtime(store, model, t3, [](auto) { return std::string{}; });
        runtime.runConversations();
        const auto tasks = TaskList(store).tasks();
        check(tasks.size() == 1 && tasks.front().task.sourceMessageId == priorTask, "Fixture did not create the prior task");
        taskFromDemotedTurn = tasks.front().task.id;

        demotedQueue = send(browser, demoted, "Queued demoted root");
        removedQueue = send(browser, removed, "Queued removed root");
        retainedQueue = send(browser, retained, "Queued retained root");
        memberQueue = send(browser, memberCredential, "Queued demoted root with Bot membership");
        mixedQueue = send(browser, alternate, "Queued mixed credential roles");
        trustedQueue = ConversationIntake(store)
                           .receive(localOwner, {"release", "", "local-env", "release"}, "local", "Trusted local owner")
                           .at("messageId");
    }
    {
        Store store(path);
        auto server = setup(store, Json::array({identity(demoted, localOwner.id, false), identity(retained, "retained", true),
                                                identity(memberCredential, "member", false), identity(alternate, "mixed", false),
                                                identity("Bearer fixture-other", "mixed", true)}));
        Browser browser(*server);
        signIn(browser, retained);
        model.inputs.clear();
        Runtime runtime(store, model, t3, [](auto) { return std::string{}; });
        runtime.runConversations();
        const auto page = browser.get(view);
        const auto latestRoute = [&page](Id messageId) {
            for (const auto& event : page.at("events") | std::views::reverse)
                if (event.at("kind") == "conversation_reply" && event.at("data").at("replyToMessageId") == messageId)
                    return event.at("data").at("route");
            return Json(nullptr);
        };
        for (auto id : {demotedQueue, removedQueue, mixedQueue})
            check(latestRoute(id) == "access", "Queued or resumed message retained unavailable Web authority");
        for (auto id : {retainedQueue, memberQueue, trustedQueue})
            check(latestRoute(id) == "reply", "A currently authorized queued or resumed message was denied");
        const std::vector<std::string> expected{"Queued retained root",
                                                "Queued retained root",
                                                "Queued demoted root with Bot membership",
                                                "Queued demoted root with Bot membership",
                                                "Trusted local owner",
                                                "Trusted local owner"};
        check(model.inputs == expected && TaskList(store).tasks().size() == 4,
              "Access changes delivered model context or ran work for a revoked actor");
        check(!model.leakedCredential && page.dump().find("Bearer fixture-") == std::string::npos,
              "Web credentials entered a model request or conversation output");

        const auto racing = send(browser, retained, "Demoted during model call");
        model.demoteDuringCall = [&] {
            WebIdentity(store).setWebIdentityResolver(
                [](std::string_view person) -> std::optional<bool> { return person == "retained" ? std::optional{false} : std::nullopt; });
        };
        runtime.runConversations();
        const auto denied = conversationReply(browser.get(view), racing);
        check(model.inputs.back() == "Demoted during model call" && denied.at("route") == "access" && denied.at("actions").empty() &&
                  TaskList(store).tasks().size() == 4,
              "Revoked authority survived the pre-effect check");
        tick(runtime, 3);
        check(t3.launches == 1 && Tasks(store).task(taskFromDemotedTurn).task.state == TaskState::running,
              "A denied resume consumed the unused model call shared by its existing task");
    }
}

void demoUsesSeparateTasksForImplementationAndVerification(const std::filesystem::path& path) {
    struct CatalogDemo : Model {
        std::unique_ptr<Model> demo = app::makeDemoModel();
        std::size_t definitions = 0;
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("kind") == "conversation_turn" && input.event.at("turn").empty()) {
                auto webInput = input;
                webInput.config.update({{"modelProvider", "chatgpt"}, {"model", "fixture"}});
                definitions = app::responseRequest(webInput).at("tools").at(0).at("tools").size();
            }
            return demo->decide(input);
        }
    } model;
    Store store(path, {.saveBotSetup = [](Store&, std::string_view, const BotSetupEdit&, const Json&) {}});
    const auto t3 = app::makeDemoT3();
    auto web = callbacks();
    web.intake = [](std::string_view bot, std::string_view request) { return Intake{std::string(bot), std::string(request), "demo", ""}; };
    Runtime runtime(store, model, *t3, [](auto) { return std::string{}; }, {.intake = web.intake});
    const auto server = startWeb(store, 0, std::move(web));
    Browser browser(*server);
    const auto receipt = browser.post("/api/messages",
                                      {{"botId", "release"},
                                       {"clientMessageId", "implement-and-verify"},
                                       {"text", "Implement the release summary in Demo project, then verify it in a separate Session."}},
                                      201);
    runtime.runConversations();
    const auto conversation = "/api/conversation?bot=release";
    const auto reply = conversationReply(browser.get(conversation), receipt.at("messageId"));
    const auto& actions = reply.at("actions");
    check(actions.size() == 2 && actions[0].at("tool") == "start_task" && actions[1].at("tool") == "start_task",
          "The Demo did not start two ordinary tasks in one conversation");
    check(model.definitions == 47, "The default Web conversation provider catalog changed");

    const Id implementation = actions[0].at("taskId"), verification = actions[1].at("taskId");
    tick(runtime, 3);
    const auto queued = Tasks(store).task(verification);
    check(Tasks(store).task(implementation).task.state == TaskState::running && queued.task.state == TaskState::queued &&
              queued.blockedBy && queued.blockedBy->id == implementation && queued.runs.empty(),
          "Verification bypassed the active task on the same project");
    for (int i = 0; i < 12; ++i) {
        runtime.tick();
        const auto first = Tasks(store).task(implementation);
        const auto second = Tasks(store).task(verification);
        check(first.task.state == TaskState::completed || second.runs.empty(),
              "The verification Session started before implementation finished");
        if (second.task.state == TaskState::completed) break;
    }
    const auto first = Tasks(store).task(implementation);
    const auto second = Tasks(store).task(verification);
    check(first.task.state == TaskState::completed && second.task.state == TaskState::completed && first.task.projectId == "demo-project" &&
              second.task.projectId == first.task.projectId && first.runs.size() == 1 && second.runs.size() == 1 &&
              first.runs[0].run.ref.threadId != second.runs[0].run.ref.threadId,
          "The two Demo tasks did not complete in separate Sessions on the same project");
    const auto view = browser.get("/api/view?bot=release");
    const auto& sessions = view.at("sessionTasks");
    check(sessions.size() == 2 && sessions[0].at("id") == verification && sessions[1].at("id") == implementation &&
              conversationCompletion(view, implementation).at("id") < conversationCompletion(view, verification).at("id"),
          "Ordinary Session rows or ordered results are missing from the Bot view");
}

void learningNoticeEndpoints(const std::filesystem::path& path) {
    Store store(path, {.memorySeed = [](auto) { return "Initial convention"; }});
    const auto model = app::makeDemoModel();
    const auto t3 = app::makeDemoT3();
    Runtime runtime(store, *model, *t3, [](std::string_view) { return std::string{}; });
    auto server = startWeb(store, 0, callbacks());
    Browser browser(*server);
    const std::string conversation = "/api/conversation?bot=release";
    const auto anchor = [&](const Json& id) {
        return browser.get(conversation + "&before=" + std::to_string(id.get<Id>() + 1)).at("events").back();
    };
    const auto learn = [&](std::string_view clientId) {
        const auto request = browser.post(
            "/api/messages", {{"botId", "release"}, {"clientMessageId", clientId}, {"text", "Show me how to verify this project"}}, 201);
        tick(runtime, 8);
        const auto events = browser.get(conversation).at("events");
        const auto recent = events | std::views::reverse;
        const auto notice = std::ranges::find_if(recent, [](const Json& event) { return event.at("kind") == "memory_learned"; });
        check(notice != recent.end() && notice->at("data").at("canUndo"), "Conversation did not offer Undo on the learned revision");
        const auto requestId = notice->at("data").at("requestEventId");
        const auto requestAnchor = anchor(requestId);
        check(requestId == request.at("messageId") && requestAnchor.at("id") == requestId &&
                  requestAnchor.at("kind") == "conversation_message",
              "The Learned notice did not target the person's message");
        return *notice;
    };
    const auto saved = learn("learning");
    const auto& data = saved.at("data");
    const auto completion = conversationCompletion(browser.get("/api/view?bot=release"), saved.at("taskId"));
    auto session = completion.at("data").at("run");
    session.erase("itemId");
    const auto history = browser.get("/api/memory/history?bot=release").at("revisions");
    const auto records = TaskHistory(store).history(saved.at("taskId"));
    const auto report = std::ranges::find_if(records, [](const Json& event) { return event.at("kind") == "report"; });
    check(report != records.end() && history.size() == 1 && history.front().at("id") == data.at("revisionId") &&
              history.front().at("source") == Json{{"taskId", saved.at("taskId")}, {"runId", session.at("runId")}} &&
              history.front().at("learning") == Json{{"added", data.at("added")},
                                                     {"removed", Json::array()},
                                                     {"run", session},
                                                     {"taskTitle", "Show me how to verify this project"},
                                                     {"requestEventId", data.at("requestEventId")},
                                                     {"reportEventId", report->at("id")}},
          "Memory history did not link the exact learned text to its task and Session");
    const auto reportAnchor = anchor(history.front().at("learning").at("reportEventId"));
    check(reportAnchor.at("kind") == "report" && reportAnchor.at("id") == report->at("id"),
          "Memory history did not target the exact Session report");

    const auto undo = [&](const Json& revision, int expected) {
        browser.post("/api/memory/revert",
                     {{"botId", "release"}, {"baseVersion", revision.at("version")}, {"revisionId", revision.at("revisionId")}}, expected);
    };
    undo(data, 200);
    check(browser.get("/api/memory?bot=release").at("content") == "Initial convention", "Undo did not restore the prior memory exactly");
    const auto newer = learn("learning-again").at("data");
    browser.post("/api/memory/write",
                 {{"botId", "release"}, {"baseVersion", newer.at("version")}, {"content", "A later edit"}, {"summary", "A later edit"}});
    undo(newer, 409);
    check(browser.get("/api/memory?bot=release").at("content") == "A later edit", "Stale Undo reverted a later edit");
}

void memoryAutomationEndpoints(const std::filesystem::path& path) {
    Store store(path, {.memorySeed = [](auto) { return "Initial convention"; }});
    auto cb = callbacks();
    cb.clock = [] { return Seconds{2'000'000'000}; };
    cb.authenticate = [](std::string_view credential) {
        if (credential == "Bearer owner") return Person{"owner", true};
        if (credential == "Bearer reader") return Person{"reader", false};
        return Person{};
    };
    BotAccess{store}.setMember({"owner", true}, "release", "reader", true);
    auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    Browser::checked(browser.client.Get("/api/memory?bot=release"), 401);
    browser.client.set_default_headers({{"Authorization", "Bearer owner"}});
    check(browser.get("/api/memory?bot=release").at("content") == "Initial convention", "Web memory seed missing");
    const auto edit = [&](int version, std::string_view content, int status = 200) {
        return browser.post("/api/memory/write",
                            {{"botId", "release"}, {"baseVersion", version}, {"content", content}, {"summary", "Set convention"}}, status);
    };
    const auto revision = edit(0, "New convention").at("revisionId").get<Id>();
    edit(0, "Stale", 409);
    const auto detail = browser.get("/api/memory/history?bot=release&revisionId=" + std::to_string(revision));
    check(detail.at("previousContent") == "Initial convention" && detail.at("newContent") == "New convention",
          "Web revision did not expose the diff");
    browser.post("/api/memory/revert", {{"botId", "release"}, {"baseVersion", 1}, {"revisionId", revision}});
    check(browser.get("/api/memory?bot=release").at("content") == "Initial convention", "Web revert failed");
    for (int i = 0; i < 20; ++i) edit(i + 2, "Convention " + std::to_string(i));
    const auto firstPage = browser.get("/api/memory/history?bot=release");
    const auto older =
        browser.get("/api/memory/history?bot=release&before=" + std::to_string(firstPage.at("before").get<Id>())).at("revisions");
    check(firstPage.at("revisions").size() == 20 && older.size() == 2 && older.at(0).at("revertedFrom") == revision &&
              older.at(1).at("author") == "person:owner",
          "Memory history did not paginate revisions with their authors and undo links");

    const auto reminder = browser
                              .post("/api/schedules",
                                    {{"botId", "release"},
                                     {"kind", "reminder"},
                                     {"request", "Check staging"},
                                     {"timezone", "Asia/Tokyo"},
                                     {"fireAt", 2'000'000'010}},
                                    201)
                              .at("id")
                              .get<Id>();
    browser.post(
        "/api/schedules",
        {{"botId", "release"}, {"kind", "reminder"}, {"request", "Check staging"}, {"timezone", "Asia/Tokyo"}, {"fireAt", 2'000'000'000}},
        409);
    const auto cancelPath = "/api/schedules?bot=release&id=" + std::to_string(reminder);
    Browser::checked(browser.client.Delete(cancelPath), 403);
    Browser::checked(browser.client.Delete(cancelPath, httplib::Headers{{"X-Flock-Action", "1"}}), 403);
    check(Browser::checked(browser.client.Delete(cancelPath, browser.headers()), 200).at("status") == "canceled",
          "Web schedule cancellation failed");
    const auto trigger = browser
                             .post("/api/schedules",
                                   {{"botId", "release"},
                                    {"kind", "trigger"},
                                    {"source", "deploy"},
                                    {"eventFilter", {{"branch", "main"}}},
                                    {"request", "Check the deployment"}},
                                   201)
                             .at("id");
    check(browser.get("/api/schedules?bot=release").at("automations").size() == 2, "Web automation list incomplete");

    const auto webhook = [&](std::string id, std::string branch) {
        return Json{{"externalEventId", id}, {"event", {{"branch", branch}}}}.dump();
    };
    Browser::checked(browser.client.Post("/api/webhooks/deploy", webhook("event-1", "main"), "application/json"), 403);
    const httplib::Headers headers{{"X-Flock-Action", "1"}};
    const auto send = [&](const std::string& payload) {
        return Browser::checked(browser.client.Post("/api/webhooks/deploy", headers, payload, "application/json"), 202);
    };
    check(send(webhook("skip", "test")).at("tasks").empty(), "Webhook filter did not match exactly");
    browser.client.set_default_headers({{"Authorization", "Bearer reader"}});
    check(browser.get("/api/schedules?bot=release&active=1").at("canManage") == false, "a Bot member was offered schedule controls");
    BotAccess{store}.setMember(localOwner, "release", "reader", false);
    Browser::checked(browser.client.Post("/api/webhooks/deploy", headers, webhook("event-1", "main"), "application/json"), 403);
    BotAccess{store}.setMember(localOwner, "release", "reader", true);
    browser.post("/api/memory/write", {{"botId", "release"}, {"baseVersion", 2}, {"content", "Unauthorized"}}, 403);
    browser.client.set_default_headers({{"Authorization", "Bearer owner"}});
    const auto accepted = send(webhook("event-1", "main")), replayed = send(webhook("event-1", "main"));
    check(accepted.at("duplicate") == false && replayed.at("duplicate") == true && accepted.at("tasks").size() == 1 &&
              replayed.at("tasks") == accepted.at("tasks") && TaskList(store).tasks().size() == 1,
          "Webhook redelivery enqueued another task");
    Automations(store).fireDueAutomations(cb.clock() + 60);
    check(TaskList(store).tasks().size() == 1, "Canceled reminder fired after its due time");
    const auto request = browser.get("/api/conversation?bot=release").at("events").back().at("data");
    check(request.at("trigger").at("source") == "deploy" && request.at("trigger").at("triggerId") == trigger &&
              !request.at("trigger").contains("externalEventId"),
          "Web conversation lost webhook provenance");
}

}

int main(int argc, char** argv) {
    try {
        TemporaryDirectory directory;
        if (argc == 2 && std::string_view(argv[1]) == "--chatgpt") {
            chatGPTConversation(directory.path);
            std::cout << "web ChatGPT conversation behavior passed\n";
            return 0;
        }
        conversationBotSetup(directory.path / "full-mode-profiles");
        botPauseEndpoints(directory.path / "bot-pause.sqlite");
        conversationSummaries(directory.path / "conversation-summaries.db");
        imageAttachmentCountInConversation(directory.path / "image-attachment-count.sqlite");
        suggestionsAndEnvironmentsInView(directory.path / "suggestions.sqlite");
        parallelVerificationInView(directory.path / "parallel-verification.sqlite");
        conversationEntry(directory.path / "conversation.sqlite");
        conversationMessagesAnotherBot(directory.path / "message-bot.sqlite");
        silentBotSendKeepsTranscriptNavigation(directory.path / "silent-handoff.sqlite");
        botStartedTaskKeepsItsBotOrigin(directory.path / "message-bot-task.sqlite", "completed", "Checked.");
        botStartedTaskKeepsItsBotOrigin(directory.path / "message-bot-task-failed.sqlite", "failed", "Could not check.");
        botStartedTaskKeepsItsBotOrigin(directory.path / "message-bot-task-empty.sqlite", "completed", "");
        botStartedTaskKeepsItsBotOrigin(directory.path / "message-bot-task-handoff-failed.sqlite", "completed", "Checked.", true);
        botMessagesStopAtTheExchangeLimit(directory.path / "message-bot-limit.sqlite");
        secretCollisionKeepsBotRoutes(directory.path / "secret-collision.db");
        botRolePreviewRedactsBeforeBounding(directory.path / "role-preview.db");
        botRoleSavesRequireCompleteOriginal(directory.path / "role-edit");
        queuedWebAuthority(directory.path / "queued-web-authority.sqlite");
        demoUsesSeparateTasksForImplementationAndVerification(directory.path / "demo-separate-tasks.db");
        learningNoticeEndpoints(directory.path / "learning.sqlite");
        memoryAutomationEndpoints(directory.path / "automation.sqlite");
        std::cout << "web conversation behavior passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
