#include "http_fixture.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"
#include "flock/t3_mcp.hpp"

#include <sqlite3.h>

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

void stopDuringSendDispatch() {
    struct InFlightT3 : FixedT3 {
        std::promise<void> entered;
        std::shared_future<void> release;
        std::vector<std::string> cancelled;
        unsigned sends = 0;

        std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) override {
            SessionMetadata metadata;
            metadata.projectId = "origin-project";
            return metadata;
        }
        Delivery send(const Command&) override {
            ++sends;
            entered.set_value();
            release.wait();
            return {Delivery::Kind::Accepted,{"thread","in-flight-run",{}},{},false,"queued"};
        }
        QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget& target, std::string_view run) override {
            check(target == SessionTarget{"env-1","thread"},"Cancelled the wrong Session");
            cancelled.emplace_back(run);
            return QueueCancelOutcome::Cancelled;
        }
    } t3;
    TestDirectory dir;
    Store store(dir.path/"flock.sqlite3");
    const auto token = T3Connections(store).connect("env-1",localOwner.id,4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    const auto server = startWeb(store,0,cb);
    Mcp mcp(*server,token);
    const Json ask{{"bot","release"},{"text","Send an update"},{"threadId","thread"},{"requestId","wire-race"}};
    check(mcp.call("flock_ask",ask).contains("structuredContent"),"The dispatch race ask was rejected");

    AskModel model;
    Runtime runtime(store,model,t3,[](std::string_view) { return std::string{}; });
    std::promise<void> release, stopping;
    t3.release = release.get_future().share();
    auto entered = t3.entered.get_future();
    auto dispatch = std::async(std::launch::async,[&] { runtime.runConversations(); });
    const auto dispatched = entered.wait_for(std::chrono::seconds(2));
    auto stop = std::async(std::launch::async,[&] {
        stopping.set_value();
        return Mcp(*server,token).call("flock_stop",{{"bot","release"},{"threadId","thread"}});
    });
    stopping.get_future().wait();
    const auto stoppedBeforeReceipt = stop.wait_for(std::chrono::milliseconds(100));
    release.set_value();
    dispatch.get();
    const auto result = stop.get().at("structuredContent");
    check(dispatched == std::future_status::ready && stoppedBeforeReceipt == std::future_status::timeout &&
          result.at("cancelled") == 1 && t3.cancelled == std::vector<std::string>{"in-flight-run"},
          "Stop committed before recording and cancelling the in-flight Run");

    store.receive(localOwner,cb.intake("release",""),"after-wire-race","Send another update");
    runtime.runConversations();
    check(t3.sends == 1 && model.wakes == 0 && model.send.at("error") == "This Bot's participation in the Session has stopped",
          "A send or wake passed the committed stop");
}

void sessionInvitations() {
    struct InviteT3 : FixedT3 {
        std::vector<Command> sends;
        std::vector<std::string> checkedRuns;
        bool reachable = true;
        std::string completedRun = "someone-elses-run";
        std::function<void()> duringRead;
        std::vector<std::string> cancellationAttempts;

        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override {
            if (duringRead) duringRead();
            if (!reachable || target != SessionTarget{"env-1","thread"}) return {};
            SessionMetadata metadata;
            metadata.projectId = "origin-project";
            metadata.title = "Staging follow-up";
            return metadata;
        }
        Delivery send(const Command& command) override {
            sends.push_back(command);
            return {Delivery::Kind::Accepted,{"thread","triage-run",{}},{},false,"queued"};
        }
        std::string sessionMessageWake(const SessionTarget&, const RunRef& run) override {
            checkedRuns.push_back(run.runId);
            return run.runId == completedRun ? "finished" : "";
        }
        QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget&, std::string_view run) override {
            cancellationAttempts.emplace_back(run);
            return QueueCancelOutcome::AlreadyStarted;
        }
    } t3;
    struct InviteModel : Model {
        std::string target = "triage";
        std::string thread = "thread";
        std::map<std::string,Json> events, receipts;
        int wakes = 0, invitations = 0;

        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            const auto bot = input.config.at("botDisplayName").get<std::string>();
            const auto& steps = event.at("turn");
            const auto reply = []() -> ModelTurn {
                return {{{"reply",{{"text","Done."}},"Finish this turn"}}};
            };
            if (event.contains("sessionWake")) {
                check(!event.contains("sessionInvitation") && bot == "Triage Bot",
                      "An invitation was reused as a wake, or woke the wrong Bot");
                if (steps.empty()) ++wakes;
                return reply();
            }
            const auto invite = [&](const std::string& recipient) -> ModelTurn {
                return {{{"invite_bot",{{"botId",recipient},{"environmentId","env-1"},{"threadId",thread},
                                        {"note","Review the staging checks."}},"Invite a Bot to the Session"}}};
            };
            if (steps.empty() && event.contains("sessionInvitation")) {
                ++invitations;
                events[bot] = event;
                for (const auto* tool : {"flock_session_events","message_session","start_task","invite_bot"})
                    check(std::ranges::find(input.tools,tool) != input.tools.end(),"The invited Bot lost an ordinary Session tool");
                if (bot == "Triage Bot")
                    return {{{"message_session",{{"environmentId","env-1"},{"threadId","thread"},
                                                  {"text","I checked staging."}},"Reply in the Session"}}};
                return invite(bot == "Settings Bot" ? "specialist" : "release");
            }
            if (steps.empty()) return invite(target);
            receipts[bot] = steps.back().at("result");
            if (bot == "Triage Bot" && steps.size() == 1) return invite("settings");
            return reply();
        }
    } model;

    TestDirectory dir;
    const auto database = dir.path/"flock.sqlite3";
    Store store(database);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    cb.authenticate = [](std::string_view authorization) {
        return authorization == aliceCredential ? alice : Person{};
    };
    cb.resolveIdentity = [](std::string_view id) -> std::optional<bool> {
        if (id == alice.id) return true;
        if (id == bob.id) return false;
        return {};
    };
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        if (bot == "broken") throw std::runtime_error(privateValue);
        const std::map<std::string_view,std::string_view> names{{"release","Release Bot"},{"triage","Triage Bot"},
            {"settings","Settings Bot"},{"specialist","Specialist Bot"}};
        return {std::string(bot),std::string(request),"env-1","bot-project",{{"botDisplayName",names.at(bot)}}};
    };
    for (const auto* bot : {"release","triage","settings","specialist","broken"})
        store.policy().setMember(alice,bot,bob.id,true);
    const auto token = T3Connections(store).connect("env-1",bob.id,4173);
    const auto server = startWeb(store,0,cb);
    Mcp mcp(*server,token);
    const auto soul = [](std::string_view) { return std::string{}; };
    const RuntimeOptions options{.intake=cb.intake};
    auto runtime = std::make_unique<Runtime>(store,model,t3,soul,options);
    store.policy().setMode(alice,"supervised");
    const auto ask = mcp.call("flock_ask",{{"bot","release"},{"threadId","thread"},{"requestId","invite-chain"},
                                         {"text","Invite a Bot to review staging."}});
    check(ask.contains("structuredContent"),"Could not start the invitation chain: " + ask.dump());
    const auto source = ask.at("structuredContent").at("messageId").get<Id>();
    runtime->runConversations();

    const auto first = model.receipts.at("Release Bot");
    const auto invitation = model.events.at("Triage Bot");
    check(first.at("depth") == 1 && first.at("messageId") == invitation.at("messageId") &&
          invitation.at("sessionOrigin") == sessionOrigin &&
          invitation.at("text") == "Invited to Session Staging follow-up by Release Bot, note: Review the staging checks." &&
          invitation.at("sessionInvitation").at("inviterBotId") == "release","The invitation lost its Session origin or description");
    check(t3.sends.empty() && t3.checkedRuns.empty() && model.wakes == 0,"An invitation caused a send without approval or a wake");
    const auto approval = store.inbox(bob).at(0);
    check(approval.at("botId") == "triage" && approval.at("conversationEventId") == first.at("messageId"),
          "The invited Bot's send did not ask the Session requester for approval");
    store.decideInbox(bob,{approval.at("botId"),InboxKind::policy_approval,approval.at("itemId"),approval.at("waitId"),
                           approval.at("taskId"),approval.at("targetRevision"),approval.at("targetGeneration")},"allow_once");
    runtime->runConversations();
    check(t3.sends.size() == 1 && t3.sends.front().ownerBotId == "triage" &&
          t3.sends.front().target.threadId == "thread" && t3.sends.front().instruction == "Triage Bot: I checked staging.",
          "The invitee did not send under its own name to the origin Session");
    check(model.receipts.at("Triage Bot").at("depth") == 2 && model.receipts.at("Settings Bot").at("depth") == 3 &&
          model.receipts.at("Specialist Bot") == Json{{"ok",false},{"error","Invitations can reach a maximum depth of 3"}},
          "The invitation depth limit did not hold across four Bots");

    // Replay a saved step without its result to exercise the durable invitation key after a restart.
    runtime.reset();
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(database.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> sql(raw,sqlite3_close);
    check(opened == SQLITE_OK,"Could not open the invitation database");
    const auto where = " WHERE message_id=" + std::to_string(source);
    const auto reset = "UPDATE conversation_steps SET result=NULL" + where + " AND step=1; "
        "DELETE FROM conversation_steps" + where + " AND step>1; "
        "UPDATE conversation_turns SET status='running',reply_id=NULL" + where;
    check(sqlite3_exec(sql.get(),reset.c_str(),nullptr,nullptr,nullptr) == SQLITE_OK,"Could not replay the invitation step");
    const auto before = store.conversation("triage");
    runtime = std::make_unique<Runtime>(store,model,t3,soul,options);
    runtime->runConversations();
    check(model.receipts.at("Release Bot") == first && store.conversation("triage") == before &&
          model.invitations == 3 && model.wakes == 0 && t3.checkedRuns == std::vector<std::string>{"triage-run"},
          "A retried invitation duplicated an event or a different Run caused a wake");
    check(std::ranges::count_if(before,[](const Json& row) { return row.at("data").contains("sessionInvitation"); }) == 1,
          "The timeline did not contain exactly one system invitation");

    t3.completedRun = "triage-run";
    runtime->runConversations();
    runtime->runConversations();
    check(model.wakes == 1 && model.invitations == 3 && t3.sends.size() == 1,"The invitee's own send did not wake it once");

    const auto reject = [&](const char* request, const char* recipient, std::string_view error) {
        model.target = recipient;
        store.receiveWeb(bob,cb.intake("release",""),request,"Try another invitation.");
        runtime->runConversations();
        check(model.receipts.at("Release Bot") == Json{{"ok",false},{"error",error}},
              "Unexpected invitation result: " + model.receipts.at("Release Bot").dump());
    };
    reject("self","release","A Bot cannot invite itself");
    reject("forbidden","unknown","Bot access is required");
    reject("bad-intake","broken","The invited Bot is unavailable");
    model.thread = "unrelated";
    reject("unrelated","triage","Invite a Bot into a Session you participate in or launched");
    model.thread = "thread";
    t3.reachable = false;
    reject("unreachable","triage","This Session is unavailable through your T3 connection");
    t3.reachable = true;
    const auto stop = [&](const char* bot) {
        return mcp.call("flock_stop",{{"bot",bot},{"threadId","thread"}});
    };
    check(stop("specialist").at("structuredContent").at("stopped") == true && t3.cancellationAttempts.empty(),
          "Stopping an invitee stopped its inviter");
    reject("stopped-invitee","specialist","The invited Bot's participation in the Session has stopped");
    t3.duringRead = [&] {
        const auto result = stop("release");
        check(result.at("structuredContent").at("alreadyStarted") == 1,
              "Stopping the inviter omitted an invited Bot's Run: " + result.dump());
    };
    reject("stop-race","triage","This Bot's participation in the Session has stopped");
    t3.duringRead = {};
    reject("stopped","triage","This Bot's participation in the Session has stopped");
    for (const auto* bot : {"release","triage","settings","specialist"}) {
        const auto result = mcp.call("flock_ask",{{"bot",bot},{"threadId","thread"},{"requestId",std::string("after-stop-")+bot},
                                                 {"text","Check whether this participation is still active."}});
        check(result.value("isError",false),"The cascade left an invited Bot active: " + result.dump());
    }
    check(t3.cancellationAttempts == std::vector<std::string>{"triage-run"} &&
          stop("triage").at("structuredContent").at("alreadyStarted") == 1,
          "Repeated stops did not share the cascade's terminal cancellation receipt");
    check(model.invitations == 3 && model.wakes == 1 && t3.sends.size() == 1,"A rejected invitation produced a side effect");
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

void sessionStops(std::string_view fixtureUrl, const std::filesystem::path& credentials) {
    httplib::Client fixture(fixtureUrl.data());
    const auto configure = [&](const Json& settings) {
        Browser::checked(fixture.Post("/fixture",settings.dump(),"application/json"),200);
    };
    const auto effects = [&] { return Browser::checked(fixture.Get("/fixture/effects"),200); };
    const auto runs = [&] { return Browser::checked(fixture.Get("/fixture/stop-runs"),200); };
    configure({{"effects",Json::object()},{"stopRuns",{{"person-queued","queued"}}},{"personFinished",false},
               {"botRunStatus","queued"},{"stopDropReply","bot-run-3"}});
    auto t3 = makeT3Mcp(credentials);
    TestDirectory dir;
    const auto database = dir.path/"flock.sqlite3";
    auto db = std::make_unique<Store>(database);
    auto cb = callbacks();
    cb.sessionT3 = t3.get();
    const auto token = T3Connections(*db).connect("env-1",localOwner.id,4173);
    auto server = startWeb(*db,0,cb);
    const auto ask = [&](const char* bot, const char* id, std::string_view text) {
        return Mcp(*server,token).call("flock_ask",{{"bot",bot},{"threadId","thread"},{"requestId",id},{"text",text}});
    };
    const auto stop = [&](const char* bot) {
        return Mcp(*server,token).call("flock_stop",{{"bot",bot},{"threadId","thread"}});
    };
    const auto cancel = [](std::string_view run) -> Json {
        return {{"action","cancel"},{"threadId","thread"},{"queuedRunId",run}};
    };
    struct StopModel final : Model {
        AskModel ask;
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("text") == "Leave this Session") {
                check(input.event.at("turn").empty(),"A stopped Session-origin turn reached the model again");
                return {{{"leave_session",{{"environmentId","env-1"},{"threadId","thread"}},"Leave this Session"}}};
            }
            return ask.decide(input);
        }
    } model;
    auto now = unixTime();
    const RuntimeOptions options{.clock=[&] { return now; }};
    const auto soul = [](std::string_view) { return std::string{}; };
    auto runtime = std::make_unique<Runtime>(*db,model,*t3,soul,options);
    for (const auto* id : {"first-send","started-send","lost-cancellation"}) {
        check(ask("release",id,"Send an update").contains("structuredContent"),"Stop trial ask was rejected");
        runtime->runConversations();
    }
    check(ask("triage","other-bot","Send an update").contains("structuredContent"),"The other Bot's ask was rejected");
    runtime->runConversations();
    auto outstanding = runs();
    check(outstanding.size() == 5 && outstanding.at("bot-run-4") == "queued","The stop fixture did not queue four Bot Runs");
    outstanding["bot-run-2"] = "running";
    configure({{"stopRuns",outstanding}});

    // Reopen saved send receipts from a database created before the stop outbox existed.
    runtime.reset();
    server.reset();
    db.reset();
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(database.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> oldState(raw,sqlite3_close);
    check(opened == SQLITE_OK && sqlite3_exec(oldState.get(),"DROP TABLE session_queue_cancellations",
                                             nullptr,nullptr,nullptr) == SQLITE_OK,"Could not prepare the pre-stop schema");
    db = std::make_unique<Store>(database);
    server = startWeb(*db,0,cb);
    runtime = std::make_unique<Runtime>(*db,model,*t3,soul,options);
    const auto result = stop("release");
    check(result.at("structuredContent") == Json{{"stopped",true},{"cancelled",1},{"alreadyStarted",1},
                                                {"notCancellable",0},{"pending",1}} &&
          result.at("content").at(0).at("text").get<std::string>().find("1 message had already started") != std::string::npos &&
          stop("release") == result,"The stop report did not distinguish started Runs and a lost cancellation reply");
    check(effects().at("t3_queue") == Json::array({cancel("bot-run-1"),cancel("bot-run-2"),cancel("bot-run-3")}) &&
          !effects().contains("t3_thread_interrupt") &&
          runs() == Json{{"bot-run-1","cancelled"},{"bot-run-2","running"},{"bot-run-3","cancelled"},
                         {"bot-run-4","queued"},{"person-queued","queued"}},
          "flock_stop cancelled another participant's work or interrupted the running Run");

    runtime.reset();
    server.reset();
    db = std::make_unique<Store>(database);
    server = startWeb(*db,0,cb);
    now += 10;
    runtime = std::make_unique<Runtime>(*db,model,*t3,soul,options);
    runtime->runConversations();
    runtime->runConversations();
    const auto released = stop("release");
    check(released.at("structuredContent") == Json{{"stopped",true},{"cancelled",2},{"alreadyStarted",1},
                                                  {"notCancellable",0},{"pending",0}} &&
          stop("release") == released && model.ask.wakes == 0 &&
          effects().at("t3_queue") == Json::array({cancel("bot-run-1"),cancel("bot-run-2"),cancel("bot-run-3"),cancel("bot-run-3")}),
          "Restart did not retry only the unknown cancellation or woke the stopped Bot");

    check(ask("triage","leave","Leave this Session").contains("structuredContent"),"The other Bot stopped participating");
    runtime->runConversations();
    const auto left = stop("triage");
    check(left.at("structuredContent").at("cancelled") == 1 &&
          db->conversation("triage").back().at("data").at("text") == left.at("content").at(0).at("text") &&
          effects().at("t3_queue").back() == cancel("bot-run-4") && effects().at("t3_queue").size() == 5 &&
          !effects().contains("t3_thread_interrupt") && runs().at("person-queued") == "queued",
          "leave_session did not perform and report the same stop as flock_stop");
    check(ask("triage","after-leave","Send an update").value("isError",false),"An ask reopened a Bot that left");
}
}

int main(int argc, char** argv) {
    const char* scenario = "setup";
    try {
        check(argc == 3,"Expected fixture URL and credentials directory");
        scenario = "Session asks";
        sessionAsks(argv[1],argv[2]);
        scenario = "Session stops";
        sessionStops(argv[1],argv[2]);
        scenario = "stop during send preflight";
        stopDuringSendPreflight();
        scenario = "stop during send dispatch";
        stopDuringSendDispatch();
        scenario = "Session invitations";
        sessionInvitations();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << scenario << ": " << error.what() << '\n';
        return 1;
    }
}
