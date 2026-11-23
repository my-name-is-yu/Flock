#include "http_fixture.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"
#include "flock/t3_mcp.hpp"

#include <algorithm>
#include <future>
#include <iostream>

using namespace flockv0;
using namespace flockv0::http_test;

namespace {
constexpr auto endpoint = "/mcp/flock";
constexpr auto aliceCredential = "Bearer flock-fixture-alice";
const Person alice{"alice",true}, bob{"bob"};
const Json sessionOrigin{{"environmentId","env-1"},{"threadId","thread"},{"title","Staging follow-up"}};

class Mcp {
public:
    Mcp(const WebServer& server, std::string_view token) : client("127.0.0.1",static_cast<int>(server.port())) {
        if (!token.empty()) client.set_default_headers({{"Authorization","Bearer " + std::string(token)}});
    }

    Json request(std::string_view method, const Json& params, int status = 200) {
        const auto id = ++index;
        const Json data{{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}};
        auto reply = Browser::checked(client.Post(endpoint,{{"Accept","application/json, text/event-stream"}},
                                                   data.dump(),"application/json"),status);
        if (status == 200)
            check(reply.at("jsonrpc") == "2.0" && reply.at("id") == id,"Wrong MCP response envelope");
        return reply;
    }

    Json call(std::string_view tool, const Json& args = Json::object()) {
        auto response = request("tools/call",{{"name",tool},{"arguments",args}});
        check(response.contains("result"),std::string(tool) + " returned " + response.dump());
        return response.at("result");
    }

private:
    httplib::Client client;
    Id index = 0;
};

class AskModel final : public Model {
public:
    int asks = 0, wakes = 0;
    Json origin, send, read;
    std::vector<std::string> sessionTools, webTools;

    ModelTurn decide(const ModelInput& input) override {
        const auto& steps = input.event.at("turn");
        if (input.event.contains("sessionWake")) {
            if (steps.empty()) {
                ++wakes;
                auto args = input.event.at("sessionOrigin");
                args["operation"] = "counts";
                return {{{"flock_session_events",args,"Read the Session after waking"}}};
            }
            read = steps.back().at("result");
        } else if (steps.empty()) {
            if (input.event.contains("sessionOrigin")) {
                ++asks;
                origin = input.event.at("sessionOrigin");
                sessionTools = input.tools;
            } else webTools = input.tools;
            auto args = input.event.value("sessionOrigin",sessionOrigin);
            args["text"] = input.event.at("text") == "Review the existing checks"
                ? "For the person: Which checks should I review?" : "I'll review the smoke checks.";
            return {{{"message_session",args,"Reply in the Session"}}};
        } else send = steps.back().at("result");
        return {{{"reply",{{"text","Checked."}},"Finish this turn"}}};
    }
};

void stopDuringSendPreflight() {
    struct CheckedT3 : FixedT3 {
        std::function<void()> duringRead;
        unsigned sends = 0;

        std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) override {
            if (duringRead) duringRead();
            SessionMetadata metadata;
            metadata.projectId = "origin-project";
            return metadata;
        }
        Delivery send(const Command&) override {
            ++sends;
            return {Delivery::Kind::Accepted,{"thread","queued-run",{}},{},false,"queued"};
        }
    } t3;
    TestDirectory dir;
    Store store(dir.path/"flock.sqlite3");
    const auto token = T3Connections(store).connect("env-1",localOwner.id,4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    const auto server = startWeb(store,0,cb);
    Mcp mcp(*server,token);
    const Json ask{{"bot","release"},{"text","Review the smoke checks"},{"threadId","thread"},{"requestId","ask-race"}};
    check(mcp.call("flock_ask",ask).contains("structuredContent"),"The preflight race ask was rejected");

    bool stopped = false;
    t3.duringRead = [&] {
        const auto result = mcp.call("flock_stop",{{"bot","release"},{"threadId","thread"}});
        stopped = result.at("structuredContent").value("stopped",false);
    };
    AskModel model;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    runtime.runConversations();
    check(model.asks == 1 && stopped && t3.sends == 0,"A stop during Session access verification let the send through");
}

void sessionAsks(std::string_view fixtureUrl, const std::filesystem::path& credentials) {
    httplib::Client fixture(fixtureUrl.data());
    const auto configure = [&](const Json& settings) {
        Browser::checked(fixture.Post("/fixture",settings.dump(),"application/json"),200);
    };
    const auto sends = [&] {
        return Browser::checked(fixture.Get("/fixture/effects"),200).value("t3_thread_send",Json::array());
    };
    loginT3(credentials,"env-1",std::string(fixtureUrl) + "/mcp",[&](const auto& url) {
        configure({{"authorization",url}});
    });
    configure({{"scenario","running"},{"launched","Prior Session request"},{"sessionWakeFixture",true},
               {"botRunStatus","queued"},{"sessionAskFixture",true}});
    auto t3 = makeT3Mcp(credentials);

    TestDirectory dir;
    const auto database = dir.path/"flock.sqlite3";
    const auto makeStore = [&] {
        auto store = std::make_unique<Store>(database);
        store->setWebIdentityResolver([](std::string_view id) -> std::optional<bool> {
            if (id == alice.id) return true;
            if (id == bob.id) return false;
            return {};
        });
        return store;
    };
    auto db = makeStore();
    db->policy().setMember(alice,"release",bob.id,true);
    const auto token = T3Connections(*db).connect("env-1",bob.id,4173);

    auto cb = callbacks();
    cb.intake = [](std::string_view bot, std::string_view text) -> Intake {
        if (bot == "broken") throw std::runtime_error(privateValue);
        return {std::string(bot),std::string(text),"bot-default","bot-project",{{"botDisplayName","Release Bot"}}};
    };
    cb.sessionT3 = t3.get();
    cb.authenticate = [](std::string_view authorization) {
        return authorization == aliceCredential ? alice : Person{};
    };
    cb.resolveIdentity = [](std::string_view id) -> std::optional<bool> {
        if (id == alice.id) return true;
        if (id == bob.id) return false;
        return {};
    };
    auto server = startWeb(*db,0,cb);
    Mcp mcp(*server,token);
    check(Mcp(*server,"").request("tools/list",Json::object(),401).contains("error"),"MCP accepted an absent bearer");
    mcp.request("initialize",{{"protocolVersion","2025-06-18"}});
    const auto tools = mcp.request("tools/list",Json::object()).at("result").at("tools");
    check(tools.size() == 3 && tools[0].at("name") == "flock_bots" &&
          tools[1].at("name") == "flock_ask" && tools[2].at("name") == "flock_stop","Wrong Flock MCP tools");
    const auto askSchema = tools[1].at("inputSchema");
    check(askSchema.at("properties").size() == 4 && !askSchema.at("properties").contains("mode") &&
          askSchema.at("additionalProperties") == false,"flock_ask still accepts a mode");
    const auto bots = mcp.call("flock_bots").at("structuredContent").at("bots");
    check(bots.size() == 1 && bots.front().at("id") == "release","Bot catalog bypassed membership");

    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    for (const auto* route : {"answer","work-answer","pause","resume","freeze"})
        browser.post(std::string("/api/session-handoffs/1/") + route,Json::object(),404);
    for (const auto* page : {"/api/session-handoffs","/api/session-handoffs/1","/session-handoff.html","/session-handoff.js"})
        check(browser.client.Get(page)->status == 404,"A retired handoff route still exists");

    const Json ask{{"bot","release"},{"text","Review the existing checks"},{"threadId","thread"},{"requestId","ask-1"}};
    auto unknown = ask;
    unknown["bot"] = "triage";
    check(mcp.call("flock_ask",unknown).value("isError",false),"Ask bypassed Bot membership");
    unknown = ask;
    unknown["threadId"] = "missing";
    check(mcp.call("flock_ask",unknown).value("isError",false),"Ask accepted a Session the person cannot read");
    auto obsolete = ask;
    obsolete["mode"] = "consult";
    check(mcp.request("tools/call",{{"name","flock_ask"},{"arguments",obsolete}}).contains("error"),
          "Ask accepted the retired mode argument");

    auto parallel = std::async(std::launch::async,[&] { return Mcp(*server,token).call("flock_ask",ask); });
    const auto accepted = mcp.call("flock_ask",ask);
    check(parallel.get() == accepted,"Concurrent retries returned different ask receipts");
    check(accepted.contains("structuredContent"),"Ask failed: " + accepted.dump());
    const auto anchor = accepted.at("structuredContent").at("messageId").get<Id>();
    auto changed = ask;
    changed["text"] = "Review different checks";
    check(mcp.call("flock_ask",changed).value("isError",false),"Changed text with the same requestId was accepted");
    changed = ask;
    changed["threadId"] = "other-thread";
    check(mcp.call("flock_ask",changed).value("isError",false),"Changed threadId with the same requestId was accepted");
    check(db->conversation("release").size() == 1,"Ask retries created multiple conversation messages");

    AskModel model;
    const auto soul = [](std::string_view) { return std::string{}; };
    auto runtime = std::make_unique<Runtime>(*db,model,*t3,soul);
    db->policy().setMode(alice,"supervised");
    runtime->runConversations();
    check(model.asks == 1 && model.origin == sessionOrigin,"Ask did not start an ordinary Bot turn with the Session origin");
    const auto approval = db->inbox(bob).at(0);
    check(sends().empty() && approval.at("status") == "pending" && approval.at("conversationEventId") == anchor,
          "Supervised ask did not wait for the asking person's approval");
    db->decideInbox(bob,{approval.at("botId"),InboxKind::policy_approval,approval.at("itemId"),approval.at("waitId"),
                        approval.at("taskId"),approval.at("targetRevision"),approval.at("targetGeneration")},"allow_once");
    runtime->runConversations();
    const auto wire = sends();
    check(wire.size() == 1 && wire.front() == Json{{"mode","queue"},{"threadId","thread"},
          {"message","Release Bot: For the person: Which checks should I review?"},
          {"clientRequestId","flock-turn-" + std::to_string(anchor) + "-1"}},
          "The Bot's reply did not use the named, idempotent Session queue path");

    db->policy().setMode(alice,"full");
    configure({{"differentWaitRun",true},{"personFinished",true}});
    runtime->runConversations();
    check(model.wakes == 0,"The person's Run caused the Bot to wake");
    configure({{"differentWaitRun",false},{"botRunStatus","failed"}});
    runtime->runConversations();
    check(model.wakes == 0,"A timed-out result consumed the Bot's wake");
    configure({{"botRunStatus","completed"}});
    runtime->runConversations();
    runtime->runConversations();
    check(model.wakes == 1 && model.read.at("ok") == true && sends() == wire,
          "The assigned Run did not wake the Bot once with access to the origin Session");

    auto followUp = ask;
    followUp["requestId"] = "ask-2";
    followUp["text"] = "Review the smoke checks";
    check(!mcp.call("flock_ask",followUp).value("isError",false),"The person's forwarded reply was rejected");
    configure({{"botRunStatus","queued"}});
    runtime->runConversations();
    check(model.asks == 2 && sends().size() == 2,"A fresh requestId did not continue the participation");

    auto pending = followUp;
    pending["requestId"] = "ask-3";
    mcp.call("flock_ask",pending).at("structuredContent");
    const Json stop{{"bot","release"},{"threadId","thread"}};
    const auto stopped = mcp.call("flock_stop",stop);
    check(stopped.at("structuredContent").at("stopped") == true && mcp.call("flock_stop",stop) == stopped,
          "Stop did not end the participation idempotently");
    check(mcp.call("flock_ask",ask) == accepted,"Stop changed the receipt for an already accepted request");
    pending["requestId"] = "ask-4";
    check(mcp.call("flock_ask",pending).value("isError",false),"A fresh ask reopened a stopped participation");

    runtime.reset();
    server.reset();
    db = makeStore();
    runtime = std::make_unique<Runtime>(*db,model,*t3,soul);
    configure({{"botRunStatus","completed"}});
    runtime->runConversations();
    check(model.asks == 2 && model.wakes == 1 && sends().size() == 2,
          "A pending ask or wake survived stop and restart");
    db->policy().setMember(alice,"release",bob.id,false);
    T3Connections(*db).connect("env-1",alice.id,4173);
    db->receiveWeb(alice,cb.intake("release",""),"web-after-stop","Send another update");
    runtime->runConversations();
    check(model.webTools == model.sessionTools,"A Session ask did not get the Web Bot's tools");
    check(sends().size() == 2 && model.send.at("error") == "This Bot's participation in the Session has stopped",
          "The Web send path bypassed the stop");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3,"Expected fixture URL and credentials directory");
        sessionAsks(argv[1],argv[2]);
        stopDuringSendPreflight();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
