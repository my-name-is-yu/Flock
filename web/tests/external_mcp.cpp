#include "../views.hpp"
#include "http_fixture.hpp"
#include "flock/external_mcp.hpp"
#include "flock/t3_connection.hpp"

#include <algorithm>
#include <iostream>

using namespace flockv0;
using namespace flockv0::http_test;

namespace {
constexpr auto endpoint = "/mcp/flock/external";
constexpr auto submitTool = "flock_submit_task";
constexpr auto statusTool = "flock_get_task";
constexpr auto answerTool = "flock_answer_question";
constexpr auto protocol = "2025-11-25";
constexpr auto releaseLink = "https://app.t3.codes/threads/local-env/thread-release";
constexpr auto peerLink = "https://app.t3.codes/threads/linked%20env/thread-triage";

class Mcp {
public:
    Mcp(const WebServer& server, std::string_view token = {}, std::string path = endpoint)
        : client_("127.0.0.1", static_cast<int>(server.port())), path_(std::move(path)) {
        if (!token.empty()) headers_.emplace("Authorization", "Bearer " + std::string(token));
    }

    void initialize() {
        const auto info = request("initialize", {{"protocolVersion", protocol},
                                                 {"capabilities", Json::object()},
                                                 {"clientInfo", {{"name", "flock-external-test"}, {"version", "1"}}}})
                              .at("result");
        check(info.at("protocolVersion") == protocol, "MCP protocol negotiation failed");
        headers_.emplace("MCP-Protocol-Version", protocol);
        const auto response = client_.send(httpRequest({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}));
        check(response && response->status == 202 && response->body.empty(), "MCP notification returned a response");
    }

    Json request(std::string_view method, const Json& params = Json::object(), int status = 200) {
        const auto id = ++index_;
        const auto response = client_.send(httpRequest({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}));
        const auto data = Browser::checked(response, status);
        if (status == 200) check(data.at("jsonrpc") == "2.0" && data.at("id") == id, "Wrong MCP response envelope");
        return data;
    }

    Json call(std::string_view tool, const Json& arguments) {
        const auto data = request("tools/call", {{"name", tool}, {"arguments", arguments}});
        check(data.contains("result"), std::string(tool) + " returned a JSON-RPC error");
        return data.at("result");
    }

    Json success(std::string_view tool, const Json& arguments) {
        const auto result = call(tool, arguments);
        check(result.contains("structuredContent") && !result.value("isError", false), std::string(tool) + " failed");
        return result.at("structuredContent");
    }

    void denied(std::string_view tool, const Json& arguments, std::string_view scenario) {
        const auto result = call(tool, arguments);
        check(result.value("isError", false) && !result.contains("structuredContent"), scenario);
    }

    void submitWithoutReceipt(const Json& arguments) {
        auto request = httpRequest(
            {{"jsonrpc", "2.0"}, {"id", ++index_}, {"method", "tools/call"}, {"params", {{"name", submitTool}, {"arguments", arguments}}}});
        int status = 0;
        request.response_handler = [&](const httplib::Response& response) {
            status = response.status;
            return false;
        };
        const auto response = client_.send(request);
        check(status == 200 && !response && response.error() == httplib::Error::Canceled,
              "The first submission did not lose its MCP response body");
    }
private:
    httplib::Request httpRequest(const Json& message) const {
        httplib::Request request;
        request.method = "POST";
        request.path = path_;
        request.headers = headers_;
        request.body = message.dump();
        return request;
    }

    httplib::Client client_;
    std::string path_;
    httplib::Headers headers_{{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    Id index_ = 0;
};

struct LinkedT3 : FixedT3 {
    std::map<std::string, std::string, std::less<>> links{{"release", releaseLink}};

    Delivery send(const Command& command) override {
        auto result = FixedT3::send(command);
        if (command.tool == CommandTool::delegate) {
            const auto link = links.find(command.projectId);
            if (link != links.end()) result.run.link = link->second;
        }
        return result;
    }
};

std::string multilingualText(std::size_t bytes) {
    constexpr std::string_view symbol = "📘";
    std::string result;
    for (std::size_t i = 0; i < bytes / symbol.size(); ++i) result += symbol;
    return result;
}

void checkByteLimit(const Json& tools, std::string_view name, std::string_view description) {
    const auto tool = std::ranges::find_if(tools, [&](const Json& value) { return value.at("name") == name; });
    check(tool != tools.end(), "Missing MCP tool schema");
    const auto& text = tool->at("inputSchema").at("properties").at("text");
    check(!text.contains("maxLength") && text.at("description") == description, "MCP schema did not describe the UTF-8 byte limit");
}

void credentialScopes() {
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto externalToken = ExternalMcp(store).connect(localOwner.id, {"release"}).bearerToken;
    const auto sessionToken = T3Connections(store).connect("local-env", localOwner.id, 4173);
    auto cb = callbacks();
    FixedT3 t3;
    cb.sessionT3 = &t3;
    const auto server = startWeb(store, 0, cb);
    constexpr auto sessionEndpoint = "/mcp/flock";
    Mcp(*server, externalToken, sessionEndpoint).request("tools/list", Json::object(), 401);
    Mcp(*server, sessionToken).request("tools/list", Json::object(), 401);
    Mcp session(*server, sessionToken, sessionEndpoint), external(*server, externalToken);
    session.initialize();
    external.initialize();
    const auto sessionTools = session.request("tools/list").at("result").at("tools");
    const auto externalTools = external.request("tools/list").at("result").at("tools");
    check(sessionTools.size() == 3 && externalTools.size() == 3, "MCP route exposed the wrong number of tools");
    checkByteLimit(sessionTools, "flock_ask", "Maximum 8192 UTF-8 bytes.");
    checkByteLimit(externalTools, submitTool, "Maximum 8192 UTF-8 bytes.");
    checkByteLimit(externalTools, answerTool, "Maximum 8000 UTF-8 bytes.");
    check(session.success("flock_bots", Json::object()).contains("bots"), "Session tools did not receive their connection");
    check(session.request("tools/call", {{"name", submitTool}}).at("error").at("message") == "Unknown tool" &&
              external.request("tools/call", {{"name", "flock_bots"}}).at("error").at("message") == "Unknown tool",
          "A valid credential could call the other route's tool");
}

void pausedBotRefusesExternalMessages() {
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto token = ExternalMcp(store).connect(localOwner.id, {"release", "triage"}).bearerToken;
    const auto server = startWeb(store, 0, callbacks());
    Mcp client(*server, token);
    Json ask{{"bot", "release"}, {"text", "Check the release"}, {"requestId", "before-pause"}};
    const auto receipt = client.success(submitTool, ask);
    store.setBotPaused(localOwner, "release", true);
    check(client.success(submitTool, ask) == receipt, "Pause changed an existing external MCP receipt");

    ask["requestId"] = "while-paused";
    const auto expectPaused = [&](std::string_view tool, const Json& arguments) {
        const auto result = client.call(tool, arguments);
        check(result.value("isError", false) && result.at("content").at(0).at("text") == botPausedMessage,
              "An external MCP request did not return the paused response");
    };
    expectPaused(submitTool, ask);
    expectPaused(answerTool,
                 {{"taskId", receipt.at("taskId")}, {"waitId", 1}, {"targetRevision", 0}, {"targetGeneration", 0}, {"text", "Use main"}});
    check(client.success(statusTool, {{"taskId", receipt.at("taskId")}}).at("status") == "cancelled" && store.tasks().size() == 1,
          "Pause did not close the old request or refused requests created work");

    const auto other = client.success(submitTool, {{"bot", "triage"}, {"text", "Check triage"}, {"requestId", "other-bot"}});
    check(other.at("status") == "received", "Pause prevented submitting to another Bot");
    store.setBotPaused(localOwner, "release", false);
    check(client.success(submitTool, ask).at("taskId") != receipt.at("taskId"), "Resume did not admit fresh external work");
}

void roundTrip() {
    TestDirectory dir;
    const auto database = dir.path / "flock.sqlite3";
    const Json ask{{"bot", "release"}, {"text", multilingualText(8192)}, {"requestId", "notes-1"}};
    ExternalMcpCredential caller, other;
    Id taskId;
    {
        Store store(database);
        ExternalMcp connections(store);
        caller = connections.connect(localOwner.id, {"release", "triage"});
        other = connections.connect(localOwner.id, {"release", "triage"});
        check(caller.connectionId != other.connectionId && caller.bearerToken != other.bearerToken,
              "Provisioning did not produce distinct external connections for the same person");
        const auto server = startWeb(store, 0, callbacks());
        Mcp(*server).request("tools/list", Json::object(), 401);
        Mcp first(*server, caller.bearerToken);
        first.initialize();
        const auto tools = first.request("tools/list").at("result").at("tools");
        for (const auto name : {submitTool, statusTool, answerTool}) {
            check(std::ranges::any_of(tools, [&](const Json& tool) { return tool.at("name") == name; }),
                  "An external MCP operation was missing from tools/list");
        }
        auto tooLong = ask;
        tooLong["text"] = ask.at("text").get<std::string>() + "a";
        const auto tooLongError = first.request("tools/call", {{"name", submitTool}, {"arguments", tooLong}}).at("error");
        check(tooLongError.at("code") == -32602 && tooLongError.at("message") == "text: Maximum 8192 UTF-8 bytes." && store.tasks().empty(),
              "The submission byte budget was not enforced or reported");
        first.submitWithoutReceipt(ask);
        const auto tasks = store.tasks();
        check(tasks.size() == 1 && tasks.front().task.request == ask.at("text").get<std::string>(),
              "The lost receipt did not leave exactly one Task");
        taskId = tasks.front().task.id;
    }

    Store store(database);
    const auto server = startWeb(store, 0, callbacks());
    Mcp client(*server, caller.bearerToken), second(*server, other.bearerToken);
    client.initialize();
    second.initialize();
    const auto receipt = client.success(submitTool, ask);
    check(receipt.at("taskId") == taskId && receipt.at("requestId") == ask.at("requestId") && receipt.at("status") == "received",
          "Restart retry did not return the original Task receipt");
    auto changed = ask;
    changed["text"] = "Check a different revision";
    client.denied(submitTool, changed, "Reusing the request ID with different text succeeded");
    changed = ask;
    changed["bot"] = "triage";
    client.denied(submitTool, changed, "Reusing the request ID with another allowed Bot succeeded");
    changed["bot"] = "settings";
    changed["requestId"] = "not-allowed-1";
    client.denied(submitTool, changed, "A connection could submit to a Bot outside its allowlist");
    check(store.tasks().size() == 1, "Retries or denied requests enqueued another Task");

    const Json task{{"taskId", taskId}};
    const auto queued = client.success(statusTool, task);
    check(queued.at("taskId") == taskId && queued.at("bot") == "release" && queued.at("status") == "queued" &&
              queued.at("question").is_null() && queued.at("result").is_null() && queued.at("sessionLink").is_null(),
          "The queued Task projection was incorrect");
    second.denied(statusTool, task, "A different connection read progress for the queued Task");

    FixedModel model;
    LinkedT3 t3;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    tick(runtime, 3);
    const auto running = client.success(statusTool, task);
    check(running.at("status") == "running" && running.at("sessionLink") == releaseLink && t3.launches.at("release") == 1,
          "The external Task did not launch one T3 Session");
    t3.observations["thread-release"] = {.kind = Observation::Kind::Question,
                                         .question = {"scope-question", Json::array({{{"id", "scope"}, {"required", true}}})}};
    tick(runtime, 2);

    const auto waiting = client.success(statusTool, task);
    const auto& question = waiting.at("question");
    check(waiting.at("status") == "waiting" && waiting.at("result").is_null() &&
              question.at("text") == "利用者向けの変更だけを載せますか？" &&
              question.at("choices") == Json::array({"利用者向けのみ", "すべて"}),
          "The pending Session question or its choices were lost");
    const Json answer{{"taskId", taskId},
                      {"waitId", question.at("waitId")},
                      {"targetRevision", question.at("targetRevision")},
                      {"targetGeneration", question.at("targetGeneration")},
                      {"text", multilingualText(8000)}};
    second.denied(statusTool, task, "A different connection read the pending question");
    second.denied(answerTool, answer, "A different connection answered the pending question");
    auto stale = answer;
    stale["waitId"] = answer.at("waitId").get<Id>() + 1;
    client.denied(answerTool, stale, "Answering a mismatched question succeeded");
    stale = answer;
    stale["targetGeneration"] = answer.at("targetGeneration").get<Id>() + 1;
    client.denied(answerTool, stale, "Answering a mismatched generation succeeded");
    auto tooLong = answer;
    tooLong["text"] = answer.at("text").get<std::string>() + "a";
    const auto tooLongError = client.request("tools/call", {{"name", answerTool}, {"arguments", tooLong}}).at("error");
    check(tooLongError.at("code") == -32602 && tooLongError.at("message") == "text: Maximum 8000 UTF-8 bytes.",
          "The answer byte budget was not enforced or reported");
    check(client.success(answerTool, answer).at("accepted") == true, "The originating connection could not answer");
    client.denied(answerTool, answer, "A repeated answer was accepted while the Session was resuming");
    tick(runtime, 2);
    check(t3.replies.size() == 1 && t3.replies.front().target.threadId == "thread-release" &&
              t3.replies.front().target.runId == "run-release-1" && t3.replies.front().questionId == "scope-question" &&
              t3.replies.front().answers.at("scope") == answer.at("text"),
          "The answer was not delivered exactly once to the matching Session question");

    t3.observations["thread-release"] = {
        .kind = Observation::Kind::Finished, .status = "completed", .report = "The checked revision contains the user-facing changes"};
    tick(runtime, 2);
    const auto completed = client.success(statusTool, task);
    check(completed.at("status") == "completed" && completed.at("question").is_null() &&
              completed.at("result").at("text") == "Draft complete" && completed.at("sessionLink") == releaseLink,
          "The final report or safe Session link was lost");
    second.denied(statusTool, task, "A different connection read the final result");
    client.denied(answerTool, answer, "The ended question accepted another answer");
    check(t3.replies.size() == 1 && store.tasks().size() == 1, "One exchange produced duplicate work or answers");

    const auto independent = second.success(submitTool, ask);
    check(independent.at("taskId") != taskId && store.tasks().size() == 2,
          "A request ID on a different connection did not create an independent exchange");
    client.denied(statusTool, {{"taskId", independent.at("taskId")}}, "The first connection read the other exchange");
    ExternalMcp(store).disconnect(caller.connectionId);
    client.request("tools/list", Json::object(), 401);
}

void peerCompletion(const std::optional<std::string>& peerUrl, const Json& expectedLink) {
    struct PeerModel : FixedModel {
        bool runsPeer = false;

        ModelTurn decide(const ModelInput& input) override {
            const auto& kind = input.event.at("kind");
            if (input.task.at("botId") == "triage") {
                if (kind == "bot_request" && runsPeer) {
                    const Decision delegate{
                        "delegate", {{"instruction", "Verify the release"}, {"doneWhen", "Report findings"}}, "Get independent evidence"};
                    return {{delegate}};
                }
                const Decision reply{"reply_to_bot", {{"kind", "result"}, {"text", "Checked by Triage"}}, "Return verification"};
                return {{reply}};
            }
            if (kind == "intake") return FixedModel::decide(input);
            if (kind == "result") {
                const Decision request{"send_bot_message",
                                       {{"botId", "triage"}, {"kind", "work"}, {"text", "Verify the release"}},
                                       "Ask for independent verification"};
                return {{request}};
            }
            const Decision complete{"complete", {{"report", "Release verified by Triage"}}, "Accept the verification"};
            return {{complete}};
        }
    } model;
    model.runsPeer = peerUrl.has_value();
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    constexpr auto requester = "peer-requester";
    store.setWebIdentityResolver([](std::string_view person) -> std::optional<bool> {
        if (person == requester) return false;
        return std::nullopt;
    });
    store.policy().setMember(localOwner, "release", requester, true);
    store.policy().setMember(localOwner, "triage", requester, true);
    const auto token = ExternalMcp(store).connect(requester, {"release"}).bearerToken;
    const auto cb = callbacks();
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    const auto receipt = mcp.success(submitTool, {{"bot", "release"}, {"text", "Verify release"}, {"requestId", "peer-check"}});
    const Json task{{"taskId", receipt.at("taskId")}};
    LinkedT3 t3;
    if (peerUrl) t3.links["triage"] = *peerUrl;
    RuntimeOptions options;
    options.peers = [&](std::string_view bot) {
        if (bot != "release") return std::vector<PeerBot>{};
        return std::vector<PeerBot>{{"Triage", cb.intake("triage", "")}};
    };
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; }, std::move(options));
    tick(runtime, 3);
    check(mcp.success(statusTool, task).at("sessionLink") == releaseLink, "The parent Run was missing while active");
    t3.observations["thread-release"] = {.kind = Observation::Kind::Finished, .status = "completed", .report = "Draft complete"};
    tick(runtime, 1);
    check(mcp.success(statusTool, task).at("sessionLink") == releaseLink, "The parent Run was missing after settlement");
    if (peerUrl) {
        tick(runtime, 5);
        check(t3.launches["triage"] == 1, "The peer did not start a Session");
        t3.observations["thread-triage"] = {
            .kind = Observation::Kind::Finished, .status = "completed", .report = "Verified", .resultRunId = "result-run-triage"};
    }
    tick(runtime, 8);
    const auto status = mcp.success(statusTool, task);
    check(status.at("status") == "completed" && status.at("result").at("text") == "Release verified by Triage" &&
              status.at("sessionLink") == expectedLink,
          "Peer completion did not use the saved result Session and URL");
    if (!expectedLink.is_null()) {
        const auto target = Resource::sessionRead("linked env", "thread-triage").target;
        store.policy().changeNeverRule(localOwner, Action::session_read, target, true);
        const auto revoked = mcp.success(statusTool, task);
        check(revoked.at("result") == status.at("result") && revoked.at("sessionLink").is_null(),
              "A completed task exposed a peer Session link after access was revoked");
        store.policy().changeNeverRule(localOwner, Action::session_read, target, false);
        store.policy().setMember(localOwner, "triage", requester, false);
        const auto hidden = mcp.success(statusTool, task);
        check(hidden.at("result") == status.at("result") && hidden.at("sessionLink").is_null(),
              "A completed task exposed a Session link from a Bot the requester can no longer view");
    }
}

void replannedTaskDropsPriorRun() {
    struct ReplanModel : FixedModel {
        Id taskId = 0;

        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("kind") != "conversation_turn") return FixedModel::decide(input);
            if (!input.event.at("turn").empty()) return {{{"reply", {{"text", "Scope updated"}}, "Confirm the change"}}};
            for (const auto& task : input.task.at("tasks")) {
                if (task.at("taskId") != taskId) continue;
                return {{{"change_task",
                          {{"taskId", taskId},
                           {"generation", task.at("generation")},
                           {"revisionId", task.at("revisionId")},
                           {"instruction", input.event.at("text")},
                           {"doneWhen", "Include internal changes as well"}},
                          "Update the completion criteria"}}};
            }
            throw std::runtime_error("The conversation could not see the external Task");
        }
    } model;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto token = ExternalMcp(store).connect(localOwner.id, {"release"}).bearerToken;
    const auto cb = callbacks();
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    model.taskId = mcp.success(submitTool, {{"bot", "release"}, {"text", "Draft release notes"}, {"requestId", "replan"}}).at("taskId");
    const Json task{{"taskId", model.taskId}};
    LinkedT3 t3;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    tick(runtime, 3);
    check(mcp.success(statusTool, task).at("sessionLink") == releaseLink, "The initial Run was not active");
    store.receive(localOwner, cb.intake("release", ""), "revise-scope", "Include internal changes too");
    runtime.runConversations();
    check(store.task(model.taskId).task.rest.value("replanning", false) && mcp.success(statusTool, task).at("sessionLink").is_null(),
          "The old Run was still linked during replanning");
    t3.observations["thread-release"] = {.kind = Observation::Kind::Finished, .status = "stopped", .report = "Stopped"};
    tick(runtime, 2);
    const auto current = store.task(model.taskId).task;
    check(!current.rest.value("replanning", false) && current.rest.at("lastRun").at("revisionId") != current.currentRevision.value() &&
              mcp.success(statusTool, task).at("sessionLink").is_null(),
          "A settled Run for the old revision was linked");
}

void hostedSessionLinks() {
    constexpr auto hostedUrl = "https://app.t3.codes/threads/build%2Eprod/thread%7Enightly";
    constexpr auto loopbackUrl = "http://localhost:3773/build.prod/loopback~nightly";
    Json hostedSession{{"environmentId", "build.prod"}, {"threadId", "thread~nightly"}, {"link", hostedUrl}};
    check(web::externalSessionLink(hostedSession) == hostedUrl, "An encoded hosted link did not match its Session IDs");
    hostedSession["link"] = "https://app.t3.codes/threads/build.prod/thread~nightly";
    check(web::externalSessionLink(hostedSession).empty(), "A noncanonical hosted link was accepted");

    const Json loopbackSession{{"environmentId", "build.prod"}, {"threadId", "loopback~nightly"}, {"link", loopbackUrl}};
    TaskView task;
    task.task.rest["activeRun"] = nullptr;
    task.runs = Json::array({loopbackSession, hostedSession});
    const std::vector tasks{std::move(task)};
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto view = web::botView(store, {{"id", "release"}}, &tasks);
    const auto& links = view.at("sessionTasks").at(0).at("sessions");
    check(links.at(0).at("link") == hostedUrl && links.at(1).at("link") == loopbackUrl,
          "Web Session links did not use the hosted and loopback encodings");
}
}

int main() {
    try {
        credentialScopes();
        pausedBotRefusesExternalMessages();
        roundTrip();
        hostedSessionLinks();
        peerCompletion(peerLink, peerLink);
        peerCompletion("http://localhost:3773/linked%20env/thread-triage", nullptr);
        peerCompletion("https://app.t3.codes/threads/linked%20env/other-thread", nullptr);
        peerCompletion(std::nullopt, nullptr);
        replannedTaskDropsPriorRun();
        std::cout << "Local external MCP tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "External MCP round trip: " << error.what() << '\n';
        return 1;
    }
}
