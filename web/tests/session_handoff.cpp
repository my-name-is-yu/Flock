#include "http_fixture.hpp"

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
constexpr auto pairingRoute = "/api/session-handoffs/pair";

Json pairingInput(const ThreadTarget& destination, bool coordinate = false) {
    Json inputs{{"target",{{"environmentId",destination.environmentId},{"projectId",destination.projectId},
                          {"threadId",destination.threadId}}},
                {"botId","release"},{"mode",coordinate ? "coordinate" : "consult"}};
    if (coordinate) {
        inputs["goal"] = "Coordinate remaining checks";
        inputs["allowDelegation"] = false;
    }
    return inputs;
}

class Session final : public ThreadSession {
public:
    std::vector<std::pair<ThreadTarget,std::string>> sends;

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

    Json request(std::string_view method, const Json& params, httplib::Headers extra = {}) {
        const auto id = ++index;
        extra.emplace("Accept","application/json, text/event-stream");
        const auto data = Json{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}};
        const auto result = Browser::checked(client.Post(endpoint,extra,data.dump(),"application/json"),200);
        check(result.at("jsonrpc") == "2.0" && result.at("id") == id,"wrong MCP response envelope");
        return result;
    }

    Json call(const Json& args, httplib::Headers extra = {}) {
        return request("tools/call",{{"name","request_flock_bot"},{"arguments",args}},std::move(extra));
    }

    httplib::Client client;

private:
    Id index = 0;
};

void anonymous(const std::filesystem::path& path) {
    Store db(path);
    auto server = startWeb(db,0,callbacks());
    Browser(*server).post(pairingRoute,pairingInput(target),401);
}

void sessionHandoffs(const std::filesystem::path& directory) {
    Store db(directory/"handoffs.sqlite3");
    Session session;
    ConsultationModel model;
    auto cb = callbacks();
    installIdentities(directory/"identities.json",db,cb);
    cb.sessionHandoffs = std::make_shared<SessionHandoff>(db,session,cb.configuredPerson);
    const auto handoffs = cb.sessionHandoffs;
    const auto intake = cb.intake;
    cb.intake = [intake](std::string_view bot, std::string_view request) {
        if (bot == "broken") throw Conflict(privateValue);
        return intake(bot,request);
    };

    const Person alice{"alice",true};
    const auto grant = [&](const Scope& scope) {
        db.access().resourceGrant(alice,"release",scope,GrantChange::grant);
    };
    grant(Scope::consultReply(target.environmentId,target.threadId));
    grant(Scope::consultReply(otherTarget.environmentId,otherTarget.threadId));
    grant(Scope::sessionCoordinate(target.environmentId,target.threadId));
    grant(Scope::sessionRead(target.environmentId,target.threadId));
    db.access().resourceGrant(alice,"broken",Scope::consultReply(target.environmentId,target.threadId),GrantChange::grant);

    auto server = startWeb(db,0,std::move(cb));
    Browser browser(*server);
    Mcp mcp(*server);
    browser.post(pairingRoute,pairingInput(target),401);
    browser.client.set_default_headers({{"Authorization",bobCredential}});
    browser.post(pairingRoute,pairingInput(target),403);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    auto unknownBot = pairingInput(target);
    unknownBot["botId"] = "unconfigured-bot";
    browser.post(pairingRoute,unknownBot,403);
    auto profileOnlyBot = pairingInput(target);
    profileOnlyBot["botId"] = "broken";
    browser.post(pairingRoute,profileOnlyBot,403);
    auto oldTarget = pairingInput(target);
    oldTarget.at("target")["runId"] = "human-run";
    browser.post(pairingRoute,oldTarget,400);
    auto wrongTarget = pairingInput(target);
    wrongTarget.at("target")["projectId"] = "different-project";
    browser.post(pairingRoute,wrongTarget,403);
    browser.post("/api/session-handoffs/pair?code=forbidden",pairingInput(target),400);
    Browser::checked(browser.client.Post(pairingRoute,{{"Origin","https://different.invalid"},{"X-Flock-Action","1"}},
        pairingInput(target).dump(),"application/json"),403);

    const auto init = mcp.request("initialize",{{"protocolVersion","2025-06-18"},
        {"capabilities",Json::object()},{"clientInfo",{{"name","fixture"},{"version","1"}}}});
    check(init.at("result").at("protocolVersion") == "2025-06-18","the MCP server did not negotiate with the client");
    const auto initialized = mcp.client.Post(endpoint,R"({"jsonrpc":"2.0","method":"notifications/initialized"})","application/json");
    check(initialized && initialized->status == 202 && initialized->body.empty(),"wrong initialized notification response");
    const auto gt = mcp.client.Get(endpoint);
    check(gt && gt->status == 405,"stateless MCP GET did not report method availability");
    const auto tools = mcp.request("tools/list",Json::object()).at("result").at("tools");
    check(tools.size() == 1 && tools.at(0).at("name") == "request_flock_bot" &&
          tools.at(0).at("inputSchema").at("additionalProperties") == false,"Flock exposes an unexpected MCP tool");

    const auto ticket = browser.post(pairingRoute,pairingInput(target),201);
    const auto secondTicket = browser.post(pairingRoute,pairingInput(otherTarget),201);
    const auto code = ticket.at("code").get<std::string>();
    check(!code.empty() && ticket.at("expiresAt").get<Seconds>() > unixTime() &&
          ticket.at("code") != secondTicket.at("code"),"pairing did not issue a unique expiring code");
    const Json args{{"pairingCode",code},{"requestId","caller-request"},{"text","Review the existing checks"}};
    const Json request{{"jsonrpc","2.0"},{"id",4},{"method","tools/call"},
                       {"params",{{"name","request_flock_bot"},{"arguments",args}}}};
    Browser::checked(mcp.client.Post(endpoint,{{"Origin",browser.origin}},
        request.dump(),"application/json"),403);
    auto wrong = args;
    wrong["threadId"] = otherTarget.threadId;
    check(mcp.call(wrong).at("error").at("code") == -32602,"the tool accepted a caller-supplied destination");
    wrong = args;
    wrong["pairingCode"] = "wrong-code";
    check(mcp.call(wrong,{{"Authorization",aliceCredential}}).at("result").at("isError") == true,
          "a Web credential replaced the pairing code");

    const auto ack = mcp.call(args).at("result");
    const auto paired = ack.at("structuredContent");
    check(!paired.at("requestId").get<std::string>().empty() && ack.dump().find(code) == std::string::npos &&
          ack.dump().find(target.threadId) == std::string::npos && session.sends.empty() && model.calls == 0,
          "MCP intake ran the Bot synchronously or disclosed pairing material");
    check(mcp.call(args).at("result").at("structuredContent") == paired,"identical MCP retry created new work");
    wrong = args;
    wrong["text"] = "Change the original request";
    check(mcp.call(wrong).at("result").at("isError") == true,"same request accepted a different payload");
    wrong = args;
    wrong["requestId"] = "another-request";
    check(mcp.call(wrong).at("result").at("isError") == true,"pairing code authorized a second request");

    auto otherArgs = args;
    otherArgs["pairingCode"] = secondTicket.at("code");
    const auto other = mcp.call(otherArgs).at("result").at("structuredContent");
    const auto channel = paired.at("channelId").get<Id>();
    const auto otherChannel = other.at("channelId").get<Id>();
    check(channel != otherChannel,"different return destinations shared a request");
    const auto path = "/api/session-handoffs/" + std::to_string(channel);
    check(browser.get("/api/session-handoffs").size() == 2,"paired requests did not become visible");
    auto view = browser.get(path);
    check(view.at("target") == pairingInput(target).at("target") && view.at("deliveries").empty() &&
          view.dump().find(code) == std::string::npos,"view exposed a code or changed the destination");
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

    const auto coordTicket = browser.post(pairingRoute,pairingInput(target,true),201);
    const auto coord = mcp.call({{"pairingCode",coordTicket.at("code")},{"requestId","coordinate"},
                                  {"text","Please start coordinating"}}).at("result").at("structuredContent");
    const auto coordPath = "/api/session-handoffs/" + std::to_string(coord.at("channelId").get<Id>());
    const auto active = browser.get(coordPath);
    check(active.at("mode") == "coordinate" && active.at("goal") == "Coordinate remaining checks",
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
}
}

int main() {
    try {
        TestDirectory fixture;
        anonymous(fixture.path/"anonymous.sqlite3");
        sessionHandoffs(fixture.path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
