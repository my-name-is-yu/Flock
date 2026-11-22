#include "http_fixture.hpp"

#include <atomic>
#include <fstream>
#include <iostream>

using namespace flockv0;
using namespace flockv0::http_test;

namespace {
const ThreadTarget target{"local-env","release","human-session"};
const ThreadTarget otherTarget{"local-env","release","other-session"};
constexpr auto aliceCredential = "Bearer flock-fixture-alice";
constexpr auto bobCredential = "Bearer flock-fixture-bob";
constexpr auto endpoint = "/mcp/flock";
Json invocation(const ThreadTarget& destination, std::string_view requestId, bool coordinate = false) {
    Json args{{"threadId",destination.threadId},{"botId","release"},{"requestId",requestId},
              {"text","Review the existing checks"}};
    if (coordinate) {
        args["mode"] = "coordinate";
        args["goal"] = "Coordinate remaining checks";
    }
    return args;
}

class Session final : public ThreadSession {
public:
    std::vector<std::pair<ThreadTarget,std::string>> sends;

    std::optional<ThreadProof> proveInvocation(std::string_view environment, std::string_view thread,
                                               const Json&, std::chrono::steady_clock::time_point) override {
        for (auto* destination : {&target,&otherTarget})
            if (destination->environmentId == environment && destination->threadId == thread)
                return ThreadProof{*destination,0,"fixture-auth","human-run","tool-call"};
        return {};
    }
    std::optional<ThreadPage> read(const ThreadTarget& requested, std::optional<Id> after) override {
        if (requested != target && requested != otherTarget) return {};
        return ThreadPage{requested,after.value_or(0),"fixture-auth","human-run",{}};
    }
    std::optional<ThreadPage> dispatchHistory(const ThreadTarget& requested, Id after) override {
        if (requested != target && requested != otherTarget) return {};
        return ThreadPage{requested,after,"fixture-auth","human-run",{}};
    }
    ThreadSend send(const ThreadTarget& requested, std::string_view auth, std::string_view,
                    std::string_view text) override {
        check(auth == "fixture-auth","delivery did not reuse the bound authorization");
        sends.emplace_back(requested,text);
        return {ThreadSend::Accepted,ThreadReceipt{"ordinary-input","next-run","queued"}};
    }
    std::optional<std::string> inputCommit(const ThreadTarget&) override { return {}; }
};

class ConsultationModel final : public Model {
public:
    unsigned calls = 0;
    ModelTurn decide(const ModelInput& input) override {
        ++calls;
        check(input.task.at("botId") == "release" && input.event.at("kind") == "session_consultation" &&
              input.tools == std::vector<std::string>{"reply"},"consultation received the wrong scope");
        return {{{"reply",{{"text","Keep the existing checks"}},"One consultation reply"}}};
    }
};

void installIdentities(const std::filesystem::path& file, Store& db, WebCallbacks& cb) {
    const auto identities = Json::array({{{"person","alice"},{"authorization",aliceCredential},{"adminRoot",true}},
                                          {{"person","bob"},{"authorization",bobCredential}}});
    std::ofstream(file) << identities;
    std::filesystem::permissions(file,std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    configureWebIdentities(db,file,cb);
}

class Mcp {
public:
    explicit Mcp(const WebServer& server) : client("127.0.0.1",static_cast<int>(server.port())) {}

    void use(std::string_view token) {
        client.set_default_headers({{"Authorization","Bearer " + std::string(token)}});
    }

    Json request(std::string_view method, const Json& params, httplib::Headers extra = {}) {
        const auto id = ++index;
        extra.emplace("Accept","application/json, text/event-stream");
        const auto data = Json{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}};
        const auto result = Browser::checked(client.Post(endpoint,extra,data.dump(),"application/json"),200);
        check(result.at("jsonrpc") == "2.0" && result.at("id") == id,"wrong MCP response envelope");
        return result;
    }

    Json call(const Json& args, httplib::Headers extra = {}) {
        return request("tools/call",{{"name","invoke_flock_bot"},{"arguments",args}},std::move(extra));
    }

    httplib::Client client;

private:
    Id index = 0;
};

void sessionHandoffs(const std::filesystem::path& directory) {
    Store db(directory/"handoffs.sqlite3");
    Session session;
    ConsultationModel model;
    auto cb = callbacks();
    std::atomic_bool removed = false;
    const auto catalog = cb.bots;
    cb.bots = [&] {
        auto cards = catalog();
        if (removed) cards.erase(cards.begin());
        return cards;
    };
    installIdentities(directory/"identities.json",db,cb);
    cb.sessionHandoffs = std::make_shared<SessionHandoff>(db,session,cb.configuredPerson);
    const auto handoffs = cb.sessionHandoffs;
    const auto intake = cb.intake;

    const Person alice{"alice",true};
    db.access().personGrant(alice,"release","bob",Role::decide,GrantChange::grant);
    db.access().personGrant(alice,"triage","bob",Role::view,GrantChange::grant);
    T3Connections connections(db);
    const auto consult = connections.connect(target.environmentId,"bob",T3ConnectionMode::Consult,4173);

    auto server = startWeb(db,0,std::move(cb));
    Browser browser(*server);
    Mcp mcp(*server);
    constexpr auto ping = R"({"jsonrpc":"2.0","method":"ping","id":1})";
    const auto authFailure = [&](httplib::Headers headers) {
        const auto response = Browser::checked(mcp.client.Post(endpoint,headers,ping,"application/json"),401);
        check(response.at("jsonrpc") == "2.0" && response.contains("error"),"missing JSON-RPC auth error");
    };
    authFailure({});
    authFailure({{"Authorization",aliceCredential}});
    authFailure({{"Authorization","Bearer flock-t3-" + std::string(64,'a')}});
    authFailure({{"Authorization","Bearer " + consult},{"Authorization","Bearer " + consult}});
    Browser::checked(browser.client.Get("/api/t3-connections"),401);

    mcp.use(consult);
    const auto rows = mcp.request("tools/call",{{"name","list_flock_bots"}}).at("result").at("structuredContent").at("bots");
    check(rows == Json::array({{{"id","release"},{"name","Release"},{"role","custom"},{"skill","flock-release"}}}),
          "catalog showed a view-only Bot or exposed private metadata");
    const auto forbidden = mcp.call(invocation(target,"coord-forbidden",true)).at("result");
    check(forbidden.at("isError") == true && forbidden.dump().find("consult mode") != std::string::npos,
          "consult mode allowed coordination or hid the reason");
    auto viewOnly = invocation(target,"role-forbidden");
    viewOnly["botId"] = "triage";
    check(mcp.call(viewOnly).at("result").at("isError") == true,"view role allowed invocation");

    const auto token = connections.connect(target.environmentId,alice.id,T3ConnectionMode::Full,4173);
    authFailure({{"Authorization","Bearer " + consult}});
    mcp.use(token);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    check(browser.get("/api/t3-connections") ==
          Json::array({{{"environmentId",target.environmentId},{"mode","full"}}}),
          "connection view returned an unexpected projection");
    Browser::checked(mcp.client.Post(endpoint,{{"Origin",browser.origin}},ping,"application/json"),403);
    Browser::checked(mcp.client.Post(endpoint,{{"Sec-Fetch-Site","same-origin"}},ping,"application/json"),403);
    const auto init = mcp.request("initialize",{{"protocolVersion","2025-06-18"},
        {"capabilities",Json::object()},{"clientInfo",{{"name","fixture"},{"version","1"}}}});
    check(init.at("result").at("protocolVersion") == "2025-06-18","the MCP server did not negotiate with the client");
    const auto initialized = mcp.client.Post(endpoint,R"({"jsonrpc":"2.0","method":"notifications/initialized"})","application/json");
    check(initialized && initialized->status == 202 && initialized->body.empty(),"wrong initialized notification response");
    const auto gt = mcp.client.Get(endpoint);
    check(gt && gt->status == 405,"stateless MCP GET did not report method availability");
    const auto tools = mcp.request("tools/list",Json::object()).at("result").at("tools");
    check(tools.size() == 2 && tools.at(0).at("name") == "list_flock_bots" &&
          tools.at(0).at("annotations").at("readOnlyHint") == true &&
          tools.at(1).at("name") == "invoke_flock_bot" &&
          tools.at(1).at("inputSchema").at("additionalProperties") == false,"Flock exposes an unexpected MCP tool");

    const auto args = invocation(target,"caller-request");
    auto unknown = args;
    unknown["botId"] = "unconfigured-bot";
    check(mcp.call(unknown).at("result").at("isError") == true,"unknown Bot accepted");
    removed = true;
    check(mcp.call(args).at("result").at("isError") == true,"removed Bot accepted");
    removed = false;

    const auto ack = mcp.call(args).at("result");
    const auto accepted = ack.at("structuredContent");
    check(!accepted.at("requestId").get<std::string>().empty() && ack.dump().find(token) == std::string::npos &&
          session.sends.empty() && model.calls == 0,"MCP intake ran the Bot synchronously or disclosed a credential");
    check(mcp.call(args).at("result").at("structuredContent") == accepted,"identical MCP retry created new work");
    auto wrong = args;
    wrong["text"] = "Change the original request";
    check(mcp.call(wrong).at("result").at("isError") == true,"same request accepted a different payload");
    const auto other = mcp.call(invocation(otherTarget,"other-request")).at("result").at("structuredContent");
    const auto channel = accepted.at("channelId").get<Id>();
    const auto otherChannel = other.at("channelId").get<Id>();
    check(channel != otherChannel,"different return destinations shared a request");
    const auto path = "/api/session-handoffs/" + std::to_string(channel);
    check(browser.get("/api/session-handoffs").size() == 2,"accepted requests did not become visible");
    auto view = browser.get(path);
    check(view.at("target") == Json{{"environmentId",target.environmentId},{"projectId",target.projectId},
                                     {"threadId",target.threadId}} &&
          view.at("deliveries").empty() && view.dump().find(token) == std::string::npos,
          "view exposed a credential or changed the destination");
    browser.client.set_default_headers({{"Authorization",bobCredential}});
    check(browser.get("/api/session-handoffs").empty(),"another person's handoffs appeared in the list");
    Browser::checked(browser.client.Get(path),403);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});

    const Runtime::SoulLoader soul = [](std::string_view) { return "Check releases carefully"; };
    handoffs->consult(alice,channel,model,intake,soul);
    handoffs->consult(alice,otherChannel,model,intake,soul);
    view = browser.get(path);
    const auto& delivery = view.at("deliveries").at(0);
    check(session.sends.size() == 2 && session.sends[0].first == target && session.sends[1].first == otherTarget &&
          session.sends[0].second.find("Keep the existing checks") != std::string::npos &&
          session.sends[0].second.find("release") != std::string::npos && model.calls == 2 && db.tasks().empty(),
          "consultation misrouted, lost attribution, or used the root as Task work");
    check(delivery.at("state") == "queued" && delivery.at("receipt").at("runId") == "next-run" &&
          delivery.at("observedRunId").is_null(),"queue acceptance appeared as provider observation");

    const auto coord = mcp.call(invocation(target,"coordinate",true)).at("result").at("structuredContent");
    const auto coordPath = "/api/session-handoffs/" + std::to_string(coord.at("channelId").get<Id>());
    const auto active = browser.get(coordPath);
    check(active.at("mode") == "coordinate" && active.at("goal") == "Coordinate remaining checks" &&
          active.at("allowDelegation") == true,
          "coordination changed the authorized goal");
    const auto paused = browser.post(coordPath + "/pause",Json::object());
    check(paused.at("state") == "paused" && paused.at("frozen") == true,"local pause did not block admission");
    browser.client.set_default_headers({{"Authorization",bobCredential}});
    browser.post(coordPath + "/resume",Json::object(),403);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    const auto resumed = browser.post(coordPath + "/resume",Json::object());
    check(resumed.at("frozen") == false && resumed.at("state") != "paused","the owner could not resume a paused coordination");
    const auto stopped = browser.post(coordPath + "/freeze",Json::object());
    check(stopped.at("state") == "stopped" && stopped.at("frozen") == true &&
          stopped.at("generation").get<Id>() > active.at("generation").get<Id>() &&
          !stopped.contains("sealed") && browser.get(coordPath) == stopped,
          "local stop was not durable or showed remote seal state");
    browser.post(coordPath + "/resume",Json::object(),403);
    check(connections.disconnect(target.environmentId),"registered connection was not removed");
    authFailure({});
    check(browser.get("/api/t3-connections").empty(),"disconnected environment was still listed");
}
}

int main() {
    try {
        TestDirectory fixture;
        sessionHandoffs(fixture.path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
