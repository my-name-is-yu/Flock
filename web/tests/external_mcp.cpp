#include "http_fixture.hpp"
#include "flock/external_mcp.hpp"

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

class Mcp {
public:
    Mcp(const WebServer& server, std::string_view token = {})
        : client_("127.0.0.1",static_cast<int>(server.port())) {
        if (!token.empty()) headers_.emplace("Authorization","Bearer " + std::string(token));
    }

    void initialize() {
        const auto info = request("initialize",{{"protocolVersion",protocol},{"capabilities",Json::object()},
            {"clientInfo",{{"name","flock-external-test"},{"version","1"}}}}).at("result");
        check(info.at("protocolVersion") == protocol,"MCP protocol negotiation failed");
        headers_.emplace("MCP-Protocol-Version",protocol);
        const auto response = client_.send(httpRequest({{"jsonrpc","2.0"},{"method","notifications/initialized"}}));
        check(response && response->status == 202 && response->body.empty(),"MCP notification returned a response");
    }

    Json request(std::string_view method, const Json& params = Json::object(), int status = 200) {
        const auto id = ++index_;
        const auto response = client_.send(httpRequest({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}}));
        const auto data = Browser::checked(response,status);
        if (status == 200) check(data.at("jsonrpc") == "2.0" && data.at("id") == id,"Wrong MCP response envelope");
        return data;
    }

    Json call(std::string_view tool, const Json& arguments) {
        const auto data = request("tools/call",{{"name",tool},{"arguments",arguments}});
        check(data.contains("result"),std::string(tool) + " returned a JSON-RPC error");
        return data.at("result");
    }

    Json success(std::string_view tool, const Json& arguments) {
        const auto result = call(tool,arguments);
        check(result.contains("structuredContent") && !result.value("isError",false),std::string(tool) + " failed");
        return result.at("structuredContent");
    }

    void denied(std::string_view tool, const Json& arguments, std::string_view scenario) {
        const auto result = call(tool,arguments);
        check(result.value("isError",false) && !result.contains("structuredContent"),scenario);
    }

    void submitWithoutReceipt(const Json& arguments) {
        auto request = httpRequest({{"jsonrpc","2.0"},{"id",++index_},{"method","tools/call"},
            {"params",{{"name",submitTool},{"arguments",arguments}}}});
        int status = 0;
        request.response_handler = [&](const httplib::Response& response) {
            status = response.status;
            return false;
        };
        // Drop the connection after Flock sends the headers, before the client reads the receipt body.
        const auto response = client_.send(request);
        check(status == 200 && !response && response.error() == httplib::Error::Canceled,
              "The first submission did not lose its MCP response body");
    }

private:
    httplib::Request httpRequest(const Json& message) const {
        httplib::Request request;
        request.method = "POST";
        request.path = endpoint;
        request.headers = headers_;
        request.body = message.dump();
        return request;
    }

    httplib::Client client_;
    httplib::Headers headers_{{"Content-Type","application/json"},{"Accept","application/json, text/event-stream"}};
    Id index_ = 0;
};

void roundTrip() {
    TestDirectory dir;
    const auto database = dir.path/"flock.sqlite3";
    const Json ask{{"bot","release"},{"text","Draft release notes for the checked revision"},{"requestId","notes-1"}};
    ExternalMcpCredential caller, other;
    Id taskId;
    {
        Store store(database);
        ExternalMcp connections(store);
        caller = connections.connect(localOwner.id,{"release","triage"});
        other = connections.connect(localOwner.id,{"release","triage"});
        check(caller.connectionId != other.connectionId && caller.bearerToken != other.bearerToken,
              "Provisioning did not produce distinct external connections for the same person");
        const auto server = startWeb(store,0,callbacks());
        Mcp(*server).request("tools/list",Json::object(),401);
        Mcp first(*server,caller.bearerToken);
        first.initialize();
        const auto tools = first.request("tools/list").at("result").at("tools");
        for (const auto name : {submitTool,statusTool,answerTool}) {
            check(std::ranges::any_of(tools,[&](const Json& tool) { return tool.at("name") == name; }),
                  "An external MCP operation was missing from tools/list");
        }
        first.submitWithoutReceipt(ask);
        const auto tasks = store.tasks();
        check(tasks.size() == 1 && tasks.front().at("request") == ask.at("text"),"The lost receipt did not leave exactly one Task");
        taskId = tasks.front().at("id");
    }

    Store store(database);
    const auto server = startWeb(store,0,callbacks());
    Mcp client(*server,caller.bearerToken), second(*server,other.bearerToken);
    client.initialize();
    second.initialize();
    const auto receipt = client.success(submitTool,ask);
    check(receipt.at("taskId") == taskId && receipt.at("requestId") == ask.at("requestId") &&
          receipt.at("status") == "received","Restart retry did not return the original Task receipt");
    auto changed = ask;
    changed["text"] = "Check a different revision";
    client.denied(submitTool,changed,"Reusing the request ID with different text succeeded");
    changed = ask;
    changed["bot"] = "triage";
    client.denied(submitTool,changed,"Reusing the request ID with another allowed Bot succeeded");
    changed["bot"] = "settings";
    changed["requestId"] = "not-allowed-1";
    client.denied(submitTool,changed,"A connection could submit to a Bot outside its allowlist");
    check(store.tasks().size() == 1,"Retries or denied requests enqueued another Task");

    const Json task{{"taskId",taskId}};
    const auto queued = client.success(statusTool,task);
    check(queued.at("taskId") == taskId && queued.at("bot") == "release" && queued.at("status") == "queued" &&
          queued.at("question").is_null() && queued.at("result").is_null(),"The queued Task projection was incorrect");
    second.denied(statusTool,task,"A different connection read progress for the queued Task");

    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    tick(runtime,3);
    check(client.success(statusTool,task).at("status") == "running" && t3.launches.at("release") == 1,
          "The external Task did not launch one T3 Session");
    t3.observations["thread-release"] = {.kind=Observation::Kind::Question,
        .question={"scope-question",Json::array({{{"id","scope"},{"required",true}}})}};
    tick(runtime,2);

    const auto waiting = client.success(statusTool,task);
    const auto& question = waiting.at("question");
    check(waiting.at("status") == "waiting" && waiting.at("result").is_null() &&
          question.at("text") == "利用者向けの変更だけを載せますか？" &&
          question.at("choices") == Json::array({"利用者向けのみ","すべて"}),
          "The pending Session question or its choices were lost");
    const Json answer{{"taskId",taskId},{"waitId",question.at("waitId")},{"targetRevision",question.at("targetRevision")},
        {"targetGeneration",question.at("targetGeneration")},{"text","利用者向けのみ"}};
    second.denied(statusTool,task,"A different connection read the pending question");
    second.denied(answerTool,answer,"A different connection answered the pending question");
    auto stale = answer;
    stale["waitId"] = answer.at("waitId").get<Id>() + 1;
    client.denied(answerTool,stale,"Answering a mismatched question succeeded");
    stale = answer;
    stale["targetGeneration"] = answer.at("targetGeneration").get<Id>() + 1;
    client.denied(answerTool,stale,"Answering a mismatched generation succeeded");
    check(client.success(answerTool,answer).at("accepted") == true,"The originating connection could not answer");
    client.denied(answerTool,answer,"A repeated answer was accepted while the Session was resuming");
    tick(runtime,2);
    check(t3.replies.size() == 1 && t3.replies.front().target.threadId == "thread-release" &&
          t3.replies.front().target.runId == "run-release-1" && t3.replies.front().questionId == "scope-question" &&
          t3.replies.front().answers.at("scope") == answer.at("text"),
          "The answer was not delivered exactly once to the matching Session question");

    t3.observations["thread-release"] = {.kind=Observation::Kind::Finished,.status="completed",
        .report="The checked revision contains the user-facing changes"};
    tick(runtime,2);
    const auto completed = client.success(statusTool,task);
    check(completed.at("status") == "completed" && completed.at("question").is_null() &&
          completed.at("result").at("text") == "Draft complete" &&
          completed.at("sessionLink") == "https://app.t3.codes/threads/local-env/thread-release",
          "The final report or safe Session link was lost");
    second.denied(statusTool,task,"A different connection read the final result");
    client.denied(answerTool,answer,"The ended question accepted another answer");
    check(t3.replies.size() == 1 && store.tasks().size() == 1,"One exchange produced duplicate work or answers");

    const auto independent = second.success(submitTool,ask);
    check(independent.at("taskId") != taskId && store.tasks().size() == 2,
          "A request ID on a different connection did not create an independent exchange");
    client.denied(statusTool,{{"taskId",independent.at("taskId")}},"The first connection read the other exchange");
}
}

int main() {
    try {
        roundTrip();
        std::cout << "Local MCP round trip passed through restart replay, question, answer, result and connection isolation\n";
    } catch (const std::exception& error) {
        std::cerr << "External MCP round trip: " << error.what() << '\n';
        return 1;
    }
}
