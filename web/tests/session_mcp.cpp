#include "flock/conversation_intake.hpp"
#include "flock/task_intake.hpp"
#include "flock/task_list.hpp"
#include "flock/web_conversation_intake.hpp"
#include "http_fixture.hpp"
#include "flock/decision_errors.hpp"
#include "flock/identity.hpp"
#include "flock/tasks.hpp"
#include "flock/conversations.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/command.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/run_ref.hpp"
#include "flock/runtime.hpp"
#include "flock/session_ref.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_command.hpp"
#include "flock/t3_session.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace flockv0;
using namespace flockv0::http_test;

namespace {
constexpr auto endpoint = "/mcp/flock";
constexpr auto aliceCredential = "Bearer flock-fixture-alice";
const Person alice{"alice", true}, bob{"bob"};
const Json sessionOrigin{{"environmentId", "env-1"}, {"threadId", "thread"}, {"title", "Staging follow-up"}};

PeerBot peer(std::string_view bot) {
    return {std::string(bot), {std::string(bot), "", "env-1", "bot-project", Json::object()}};
}

void listenerRebind() {
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    T3Connections connections(store);
    constexpr unsigned oldPort = 4173;
    const auto oldToken = connections.connect("env-1", alice.id, oldPort);
    connections.connect("env-2", alice.id, oldPort);

    unsigned replacementPort = 0;
    try {
        startWeb(store, 0, callbacks(), [&](unsigned port) {
            replacementPort = port;
            connections.rebindPort(port, [](std::string_view) { throw Conflict("Provider registration failed"); });
        });
        check(false, "A listener started with a failed provider registration");
    } catch (const Conflict&) {
        check(replacementPort && connections.port() == oldPort && connections.authenticate(oldToken),
              "A failed port change invalidated the original T3 registration");
    }

    std::string replacementToken;
    const auto server = startWeb(store, replacementPort, callbacks(), [&](unsigned port) {
        connections.rebindPort(port, [&](std::string_view token) {
            check(connections.ownsBearer(oldToken), "The old provider registration cannot be verified during replacement");
            replacementToken = token;
        });
    });
    const auto settings = connections.settings(alice.id);
    const auto connection = connections.authenticate(replacementToken);
    check(server->port() == replacementPort && settings && settings->port == replacementPort &&
              settings->environments == std::vector<std::string>{"env-1", "env-2"} && connection && connection->personId == alice.id &&
              !connections.authenticate(oldToken),
          "The port change did not replace the bearer and preserve the connected environments and owner");
}

std::string participationState(const std::filesystem::path& database, const char* bot) {
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sql(raw, sqlite3_close);
    check(opened == SQLITE_OK, "Could not read the Session participation database");
    sqlite3_stmt* statement = nullptr;
    const auto prepared = sqlite3_prepare_v2(
        sql.get(), "SELECT state FROM session_participations WHERE bot_id=? AND environment_id='env-1' AND thread_id='thread'", -1,
        &statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> row(statement, sqlite3_finalize);
    check(prepared == SQLITE_OK && sqlite3_bind_text(row.get(), 1, bot, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
              sqlite3_step(row.get()) == SQLITE_ROW,
          "Could not find the Session participation");
    return reinterpret_cast<const char*>(sqlite3_column_text(row.get(), 0));
}

class Mcp {
public:
    Mcp(const WebServer& server, std::string_view token) : client("127.0.0.1", static_cast<int>(server.port())) {
        if (!token.empty()) client.set_default_headers({{"Authorization", "Bearer " + std::string(token)}});
    }

    Json request(std::string_view method, const Json& params, int status = 200) {
        const auto id = ++index;
        const Json data{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
        auto reply = Browser::checked(
            client.Post(endpoint, {{"Accept", "application/json, text/event-stream"}}, data.dump(), "application/json"), status);
        if (status == 200) check(reply.at("jsonrpc") == "2.0" && reply.at("id") == id, "Wrong MCP response envelope");
        return reply;
    }

    Json call(std::string_view tool, const Json& args = Json::object()) {
        auto response = request("tools/call", {{"name", tool}, {"arguments", args}});
        check(response.contains("result"), std::string(tool) + " returned " + response.dump());
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
                return {{{"flock_session_events", args, "Read the Session after waking"}}};
            }
            read = steps.back().at("result");
        } else if (steps.empty()) {
            if (input.event.contains("sessionOrigin")) {
                ++asks;
                origin = input.event.at("sessionOrigin");
                sessionTools = input.tools;
            } else
                webTools = input.tools;
            auto args = input.event.value("sessionOrigin", sessionOrigin);
            args["text"] = input.event.at("text") == "Review the existing checks" ? "For the person: Which checks should I review?"
                                                                                  : "I'll review the smoke checks.";
            return {{{"message_session", args, "Reply in the Session"}}};
        } else
            send = steps.back().at("result");
        return {{{"reply", {{"text", "Checked."}}, "Finish this turn"}}};
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
            return {Delivery::Kind::Accepted, {"thread", "queued-run", {}}, {}, false, "queued"};
        }
    } t3;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto token = T3Connections(store).connect("env-1", localOwner.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    const Json ask{{"bot", "release"}, {"text", "Review the smoke checks"}, {"threadId", "thread"}, {"requestId", "ask-race"}};
    check(mcp.call("flock_ask", ask).contains("structuredContent"), "The preflight race ask was rejected");

    bool stopped = false;
    t3.duringRead = [&] {
        const auto result = mcp.call("flock_stop", {{"bot", "release"}, {"threadId", "thread"}});
        stopped = result.at("structuredContent").value("stopped", false);
    };
    AskModel model;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    runtime.runConversations();
    check(model.asks == 1 && stopped && t3.sends == 0, "A stop during Session access verification let the send through");
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
            return {Delivery::Kind::Accepted, {"thread", "in-flight-run", {}}, {}, false, "queued"};
        }
        QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget& target, std::string_view run) override {
            check(target == SessionTarget{"env-1", "thread"}, "Cancelled the wrong Session");
            cancelled.emplace_back(run);
            return QueueCancelOutcome::Cancelled;
        }
    } t3;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto token = T3Connections(store).connect("env-1", localOwner.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    const Json ask{{"bot", "release"}, {"text", "Send an update"}, {"threadId", "thread"}, {"requestId", "wire-race"}};
    check(mcp.call("flock_ask", ask).contains("structuredContent"), "The dispatch race ask was rejected");

    AskModel model;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    std::promise<void> release, stopping;
    t3.release = release.get_future().share();
    auto entered = t3.entered.get_future();
    auto dispatch = std::async(std::launch::async, [&] { runtime.runConversations(); });
    const auto dispatched = entered.wait_for(std::chrono::seconds(2));
    auto stop = std::async(std::launch::async, [&] {
        stopping.set_value();
        return Mcp(*server, token).call("flock_stop", {{"bot", "release"}, {"threadId", "thread"}});
    });
    stopping.get_future().wait();
    const auto stoppedBeforeReceipt = stop.wait_for(std::chrono::milliseconds(100));
    release.set_value();
    dispatch.get();
    const auto result = stop.get().at("structuredContent");
    check(dispatched == std::future_status::ready && stoppedBeforeReceipt == std::future_status::timeout && result.at("cancelled") == 1 &&
              t3.cancelled == std::vector<std::string>{"in-flight-run"},
          "Stop committed before recording and cancelling the in-flight Run");

    ConversationIntake(store).receive(localOwner, cb.intake("release", ""), "after-wire-race", "Send another update");
    runtime.runConversations();
    check(t3.sends == 1 && model.wakes == 0 && model.send.at("error") == "This Bot's participation in the Session has stopped",
          "A send or wake passed the committed stop");
}

void pausedBotStopsSessionMessages() {
    struct SessionT3 : FixedT3 {
        int reads = 0, sends = 0, observations = 0, reconciliations = 0;
        std::optional<Command> unconfirmed;
        bool recoveryVisible = false;
        std::vector<std::string> interrupted;
        std::map<std::string, int> stopChecks;

        std::optional<SessionMetadata> sessionMetadata(const SessionTarget&, bool, SessionReadDeadline) override {
            ++reads;
            return SessionMetadata{.title = "Staging follow-up", .projectId = "origin-project"};
        }
        Delivery send(const Command& command) override {
            check(command.tool == CommandTool::message_session, "Unexpected command for a Session reply");
            const auto run = "bot-reply-" + std::to_string(++sends);
            if (sends == 3) {
                unconfirmed = command;
                return {};
            }
            return {Delivery::Kind::Accepted, {command.target.threadId, run, {}, {}}, {}, false, sends == 1 ? "started" : "queued"};
        }
        Delivery reconcile(const Command& command) override {
            ++reconciliations;
            check(unconfirmed && command.actionId == unconfirmed->actionId && command.tool == unconfirmed->tool &&
                      command.instruction == unconfirmed->instruction && command.environmentId == unconfirmed->environmentId &&
                      command.target.threadId == unconfirmed->target.threadId,
                  "Pause reconciled a different send instead of its saved request");
            if (!recoveryVisible) return {};
            return {Delivery::Kind::Accepted, {command.target.threadId, "bot-reply-3", {}, {}}, {}, false, "queued"};
        }
        std::string sessionMessageWake(const SessionTarget&, const RunRef& run) override {
            ++observations;
            return run.runId == "bot-reply-1" ? "waiting_for_person" : "";
        }
        InterruptOutcome interrupt(const std::string& environment, const Json&, const RunRef& run) override {
            check(environment == "env-1" && run.threadId == "thread", "Interrupted a different Session");
            if (++stopChecks[run.runId] > 1)
                return run.runId == "bot-reply-2" ? InterruptOutcome::TargetGone : InterruptOutcome::AlreadyTerminal;
            interrupted.push_back(run.runId);
            return InterruptOutcome::Requested;
        }
    } t3;
    TestDirectory dir;
    const auto database = dir.path / "flock.sqlite3";
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    AskModel model;
    const auto soul = [](std::string_view) { return std::string{}; };
    auto now = unixTime();
    const RuntimeOptions options{.clock = [&] { return now; }};
    Json ask{{"bot", "release"}, {"text", "Review the existing checks"}, {"threadId", "thread"}, {"requestId", "before-pause"}};
    std::string token;
    {
        Store store(database);
        token = T3Connections(store).connect("env-1", localOwner.id, 4173);
        const auto server = startWeb(store, 0, cb);
        Mcp mcp(*server, token);
        const auto receipt = mcp.call("flock_ask", ask).at("structuredContent");
        Runtime runtime(store, model, t3, soul, options);
        runtime.runConversations();
        runtime.runConversations();
        check(model.wakes == 1 && t3.sends == 1, "The first Bot reply did not reach the waiting state");
        auto next = ask;
        next["requestId"] = "second-reply";
        mcp.call("flock_ask", next).at("structuredContent");
        runtime.runConversations();
        check(t3.sends == 2, "The second Bot reply was not queued");
        next["requestId"] = "receipt-lost";
        mcp.call("flock_ask", next).at("structuredContent");
        runtime.runConversations();
        check(t3.sends == 3 && t3.reconciliations == 0, "The unconfirmed reply was retried before Pause");
        store.setBotPaused(localOwner, "release", true);
        check(mcp.call("flock_ask", ask).at("structuredContent") == receipt, "Pause changed an existing Session ask receipt");
        ask["requestId"] = "while-paused";
    }

    Store store(database);
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    const auto reads = t3.reads, observations = t3.observations;
    const auto refused = mcp.call("flock_ask", ask);
    check(refused.value("isError", false) && refused.at("content").at(0).at("text") == botPausedMessage && t3.reads == reads,
          "A paused Session ask was admitted or checked T3");
    Runtime runtime(store, model, t3, soul, options);
    runtime.runConversations();
    runtime.runConversations();
    check(t3.interrupted == std::vector<std::string>{"bot-reply-1", "bot-reply-2"} && t3.observations == observations && t3.sends == 3 &&
              t3.reconciliations == 1 && model.wakes == 1,
          "Restart did not interrupt both Bot-owned Runs exactly once, or discarded work was revived");
    now += 5;
    runtime.runConversations();
    check(t3.stopChecks == std::map<std::string, int>{{"bot-reply-1", 2}, {"bot-reply-2", 2}},
          "The accepted interruptions were not checked for settlement");
    store.setBotPaused(localOwner, "release", false, [&] { return now; });
    t3.recoveryVisible = true;
    now += 5;
    runtime.runConversations();
    check(t3.interrupted == std::vector<std::string>{"bot-reply-1", "bot-reply-2", "bot-reply-3"} && t3.reconciliations == 3 &&
              t3.sends == 3,
          "Resume forgot an unconfirmed send or resent it");
    now += 5;
    runtime.runConversations();
    check(t3.stopChecks == std::map<std::string, int>{{"bot-reply-1", 2}, {"bot-reply-2", 2}, {"bot-reply-3", 2}} &&
              t3.observations == observations && t3.sends == 3 && model.wakes == 1,
          "Resume revived a wake for a stopped Bot reply or failed to settle the recovered Run");
    check(mcp.call("flock_ask", ask).contains("structuredContent"), "Resume did not admit a fresh Session ask");
    store.setBotPaused(localOwner, "release", true);
    store.deleteBot(localOwner, "release");
    check(store.botDeleted("release"), "An interrupted reply in a deleted Session blocked Bot deletion");
}

void webSendParticipation() {
    struct SessionT3 : FixedT3 {
        unsigned sends = 0;
        std::vector<std::string> cancelled;

        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override {
            if (target.environmentId != "env-1" || (target.threadId != "thread" && target.threadId != "admin-thread")) return {};
            return SessionMetadata{.title = "Staging follow-up", .projectId = "origin-project"};
        }
        Delivery send(const Command& command) override {
            ++sends;
            return {Delivery::Kind::Accepted, {command.target.threadId, "web-send-run", {}}, {}, false, "queued"};
        }
        QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget& target, std::string_view run) override {
            check(target.environmentId == "env-1", "Cancelled the wrong environment");
            cancelled.push_back(target.threadId + "/" + std::string(run));
            return QueueCancelOutcome::Cancelled;
        }
    } t3;
    struct WebSendModel : Model {
        Json send, invite, leave;

        ModelTurn decide(const ModelInput& input) override {
            const auto& steps = input.event.at("turn");
            if (input.event.at("text") == "Post as admin." || input.event.at("text") == "Leave as admin.") {
                const bool leaves = input.event.at("text") == "Leave as admin.";
                if (steps.empty()) {
                    Json args{{"environmentId", "env-1"}, {"threadId", "admin-thread"}};
                    if (!leaves) args["text"] = "Staging is ready.";
                    return {{{leaves ? "leave_session" : "message_session", args, "Act in the admin's Session"}}};
                }
                if (leaves) leave = steps.back().at("result");
                return {{{"reply", {{"text", "Done."}}, "Finish the turn"}}};
            }
            const Json target{{"environmentId", "env-1"}, {"threadId", "thread"}};
            if (!input.event.contains("sessionInvitation")) {
                check(!input.event.contains("sessionOrigin"), "The Web send already had a Session origin");
                if (steps.empty()) {
                    auto args = target;
                    args["text"] = "Staging is ready.";
                    return {{{"message_session", args, "Send an update"}}};
                }
                send = steps.front().at("result");
                if (steps.size() == 1 && send.value("ok", false)) {
                    auto args = target;
                    args.update(Json{{"botId", "triage"}, {"note", "Review the staging checks."}});
                    return {{{"invite_bot", args, "Invite Triage"}}};
                }
                if (steps.size() == 2) invite = steps.back().at("result");
            }
            return {{{"reply", {{"text", "Done."}}, "Finish the turn"}}};
        }
    } model;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto token = T3Connections(store).connect("env-1", alice.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    cb.authenticate = [](std::string_view authorization) { return authorization == aliceCredential ? alice : Person{}; };
    cb.resolveIdentity = [](std::string_view id) -> std::optional<bool> { return id == alice.id ? std::optional{true} : std::nullopt; };
    const auto server = startWeb(store, 0, cb);
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization", aliceCredential}});
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; },
                    {.peers = [](std::string_view) { return std::vector{peer("triage")}; }, .intake = cb.intake});
    const auto send = [&](std::string_view id) {
        browser.post("/api/messages", {{"botId", "release"}, {"clientMessageId", id}, {"text", "Send a staging update."}}, 201);
        runtime.runConversations();
    };
    send("first-web-send");
    check(t3.sends == 1 && model.send.value("ok", false), "The Web send failed: " + model.send.dump());
    check(model.invite.value("ok", false), "The Web Bot's invitation failed: " + model.invite.dump());
    const auto timeline = browser.get("/api/view?bot=triage").at("events");
    const auto invitation = [](const Json& row) { return row.at("data").contains("sessionInvitation"); };
    check(std::ranges::count_if(timeline, invitation) == 1 &&
              std::ranges::find_if(timeline, invitation)->at("id") == model.invite.at("messageId"),
          "The invitee did not receive exactly one invitation: " + timeline.dump());

    const auto stopped = Mcp(*server, token).call("flock_stop", {{"bot", "release"}, {"threadId", "thread"}});
    check(stopped.at("structuredContent").at("cancelled") == 1 && t3.cancelled == std::vector<std::string>{"thread/web-send-run"},
          "Stop did not cancel the Web Bot's queued Run");
    send("web-send-after-stop");
    check(t3.sends == 1 && model.send == Json{{"ok", false}, {"error", "This Bot's participation in the Session has stopped"}},
          "A Web send reopened the stopped participation");
    check(participationState(dir.path / "flock.sqlite3", "release") == "removed", "A Web send changed the removed participation");
    const Person admin{"ops-admin", true};
    WebIdentity(store).setWebIdentityResolver([&](std::string_view id) -> std::optional<bool> {
        if (id == alice.id || id == admin.id) return true;
        return {};
    });
    WebConversationIntake(store).receiveWeb(admin, cb.intake("release", ""), "admin-send", "Post as admin.");
    runtime.runConversations();
    WebConversationIntake(store).receiveWeb(admin, cb.intake("release", ""), "admin-leave", "Leave as admin.");
    runtime.runConversations();
    check(t3.sends == 2 && model.leave.value("ok", false) && model.leave.at("cancelled") == 1 &&
              t3.cancelled.back() == "admin-thread/web-send-run",
          "A Web admin could not leave the Session they posted to");
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
            if (!reachable || target != SessionTarget{"env-1", "thread"}) return {};
            SessionMetadata metadata;
            metadata.projectId = "origin-project";
            metadata.title = "Staging follow-up";
            return metadata;
        }
        Delivery send(const Command& command) override {
            sends.push_back(command);
            return {Delivery::Kind::Accepted, {"thread", "triage-run", {}}, {}, false, "queued"};
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
        std::map<std::string, Json> events, receipts;
        int wakes = 0, invitations = 0;
        bool inviteOnly = false;

        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            const auto bot = input.config.at("botDisplayName").get<std::string>();
            const auto& steps = event.at("turn");
            const auto reply = []() -> ModelTurn { return {{{"reply", {{"text", "Done."}}, "Finish this turn"}}}; };
            if (event.contains("sessionWake")) {
                check(!event.contains("sessionInvitation") && bot == "Triage Bot",
                      "An invitation was reused as a wake, or woke the wrong Bot");
                if (steps.empty()) ++wakes;
                return reply();
            }
            const auto invite = [&](const std::string& recipient) -> ModelTurn {
                return {{{"invite_bot",
                          {{"botId", recipient}, {"environmentId", "env-1"}, {"threadId", thread}, {"note", "Review the staging checks."}},
                          "Invite a Bot to the Session"}}};
            };
            if (event.at("text") == "Leave this Session") {
                check(steps.empty(), "A Bot reached the model again after leaving its Session");
                return {{{"leave_session", {{"environmentId", "env-1"}, {"threadId", "thread"}}, "Leave the Session"}}};
            }
            if (event.at("text") == "Rejoin the Session") return reply();
            if (steps.empty() && event.contains("sessionInvitation")) {
                ++invitations;
                events[bot] = event;
                if (inviteOnly) return reply();
                for (const auto* tool : {"flock_session_events", "message_session", "start_task", "invite_bot"})
                    check(std::ranges::find(input.tools, tool) != input.tools.end(), "The invited Bot lost an ordinary Session tool");
                if (bot == "Triage Bot")
                    return {{{"message_session",
                              {{"environmentId", "env-1"}, {"threadId", "thread"}, {"text", "I checked staging."}},
                              "Reply in the Session"}}};
                return invite(bot == "Settings Bot" ? "specialist" : "release");
            }
            if (steps.empty()) return invite(target);
            receipts[bot] = steps.back().at("result");
            if (bot == "Triage Bot" && steps.size() == 1) return invite("settings");
            return reply();
        }
    } model;

    TestDirectory dir;
    const auto database = dir.path / "flock.sqlite3";
    Store store(database);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    cb.authenticate = [](std::string_view authorization) { return authorization == aliceCredential ? alice : Person{}; };
    cb.resolveIdentity = [](std::string_view id) -> std::optional<bool> {
        if (id == alice.id) return true;
        if (id == bob.id) return false;
        return {};
    };
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        if (bot == "broken") throw std::runtime_error(privateValue);
        const std::map<std::string_view, std::string_view> names{
            {"release", "Release Bot"}, {"triage", "Triage Bot"}, {"settings", "Settings Bot"}, {"specialist", "Specialist Bot"}};
        return {std::string(bot), std::string(request), "env-1", "bot-project", {{"botDisplayName", names.at(bot)}}};
    };
    for (const auto* bot : {"release", "triage", "settings", "specialist", "broken"}) Policy{store}.setMember(alice, bot, bob.id, true);
    const auto token = T3Connections(store).connect("env-1", bob.id, 4173);
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    const auto soul = [](std::string_view) { return std::string{}; };
    const RuntimeOptions options{.peers = [](std::string_view bot) -> std::vector<PeerBot> {
                                     if (bot == "release") return {peer("triage"), peer("broken"), peer("specialist")};
                                     if (bot == "triage") return {peer("settings")};
                                     if (bot == "settings") return {peer("specialist")};
                                     if (bot == "specialist") return {peer("release")};
                                     return {};
                                 },
                                 .intake = cb.intake};
    auto runtime = std::make_unique<Runtime>(store, model, t3, soul, options);
    Policy{store}.setMode(alice, "supervised");
    const auto ask =
        mcp.call("flock_ask",
                 {{"bot", "release"}, {"threadId", "thread"}, {"requestId", "invite-chain"}, {"text", "Invite a Bot to review staging."}});
    check(ask.contains("structuredContent"), "Could not start the invitation chain: " + ask.dump());
    const auto source = ask.at("structuredContent").at("messageId").get<Id>();
    runtime->runConversations();

    const auto first = model.receipts.at("Release Bot");
    const auto invitation = model.events.at("Triage Bot");
    check(first.at("depth") == 1 && first.at("messageId") == invitation.at("messageId") &&
              invitation.at("sessionOrigin") == sessionOrigin &&
              invitation.at("text") == "Invited to Session Staging follow-up by Release Bot, note: Review the staging checks." &&
              invitation.at("sessionInvitation").at("inviterBotId") == "release",
          "The invitation lost its Session origin or description");
    check(t3.sends.empty() && t3.checkedRuns.empty() && model.wakes == 0, "An invitation caused a send without approval or a wake");
    const auto approval = Inbox(store).inbox(bob).at(0);
    check(approval.at("botId") == "triage" && approval.at("conversationEventId") == first.at("messageId"),
          "The invited Bot's send did not ask the Session requester for approval");
    Inbox(store).decideInbox(bob,
                             {approval.at("botId"), InboxKind::policy_approval, approval.at("itemId"), approval.at("waitId"),
                              approval.at("taskId"), approval.at("targetRevision"), approval.at("targetGeneration")},
                             "allow_once");
    runtime->runConversations();
    check(t3.sends.size() == 1 && t3.sends.front().ownerBotId == "triage" && t3.sends.front().target.threadId == "thread" &&
              t3.sends.front().instruction == "Triage Bot: I checked staging.",
          "The invitee did not send under its own name to the origin Session");
    check(model.receipts.at("Triage Bot").at("depth") == 2 && model.receipts.at("Settings Bot").at("depth") == 3 &&
              model.receipts.at("Specialist Bot") == Json{{"ok", false}, {"error", "Invitations can reach a maximum depth of 3"}},
          "The invitation depth limit did not hold across four Bots");

    // Replay a saved step without its result to exercise the durable invitation key after a restart.
    runtime.reset();
    sqlite3* raw = nullptr;
    const auto opened = sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sql(raw, sqlite3_close);
    check(opened == SQLITE_OK, "Could not open the invitation database");
    const auto where = " WHERE message_id=" + std::to_string(source);
    const auto reset = "UPDATE conversation_steps SET result=NULL" + where +
                       " AND step=1; "
                       "DELETE FROM conversation_steps" +
                       where +
                       " AND step>1; "
                       "UPDATE conversation_turns SET status='running',reply_id=NULL" +
                       where;
    check(sqlite3_exec(sql.get(), reset.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK, "Could not replay the invitation step");
    const auto before = Conversations(store).conversation("triage");
    runtime = std::make_unique<Runtime>(store, model, t3, soul, options);
    runtime->runConversations();
    check(model.receipts.at("Release Bot") == first && Conversations(store).conversation("triage") == before && model.invitations == 3 &&
              model.wakes == 0 && t3.checkedRuns == std::vector<std::string>{"triage-run"},
          "A retried invitation duplicated an event or a different Run caused a wake");
    check(std::ranges::count_if(before, [](const Json& row) { return row.at("data").contains("sessionInvitation"); }) == 1,
          "The timeline did not contain exactly one system invitation");

    t3.completedRun = "triage-run";
    runtime->runConversations();
    runtime->runConversations();
    check(model.wakes == 1 && model.invitations == 3 && t3.sends.size() == 1, "The invitee's own send did not wake it once");
    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization", aliceCredential}});
    const auto timeline = browser.get("/api/view?bot=triage").at("events");
    const auto wake = std::ranges::find_if(timeline, [](const Json& row) { return row.at("data").contains("sessionWake"); });
    check(wake != timeline.end() && wake->at("data").at("sessionWake") == Json{{"threadId", "thread"},
                                                                               {"title", "Staging follow-up"},
                                                                               {"reason", "finished"},
                                                                               {"sourceMessageId", first.at("messageId")}},
          "The Web timeline lost the wake's Session, reason, or original request");
    check(std::ranges::any_of(timeline,
                              [&](const Json& row) {
                                  return row.at("kind") == "conversation_reply" && row.at("data").at("replyToMessageId") == wake->at("id");
                              }),
          "The wake's normal Bot reply was missing from the Web timeline");

    const auto reject = [&](const char* request, const char* recipient, std::string_view error) {
        model.target = recipient;
        WebConversationIntake(store).receiveWeb(bob, cb.intake("release", ""), request, "Try another invitation.");
        runtime->runConversations();
        check(model.receipts.at("Release Bot") == Json{{"ok", false}, {"error", error}},
              "Unexpected invitation result: " + model.receipts.at("Release Bot").dump());
    };
    reject("self", "release", "A Bot cannot invite itself");
    reject("forbidden", "unknown", "Bot access is required");
    reject("not-allowlisted", "settings", "The destination Bot is unavailable or not allowed");
    const auto read = Resource::sessionRead("env-1", "thread");
    Policy{store}.changeNeverRule(alice, read.action, read.target, true);
    reject("restricted-title", "triage", "This Session is unavailable through your T3 connection");
    Policy{store}.changeNeverRule(alice, read.action, read.target, false);
    reject("bad-intake", "broken", "The invited Bot is unavailable");
    model.thread = "unrelated";
    reject("unrelated", "triage", "Invite a Bot into a Session you participate in or launched");
    model.thread = "thread";
    t3.reachable = false;
    reject("unreachable", "triage", "This Session is unavailable through your T3 connection");
    t3.reachable = true;
    const auto stop = [&](const char* bot) { return mcp.call("flock_stop", {{"bot", bot}, {"threadId", "thread"}}); };
    check(stop("specialist").at("structuredContent").at("stopped") == true && t3.cancellationAttempts.empty(),
          "Stopping an invitee stopped its inviter");
    reject("stopped-invitee", "specialist", "The person removed this Bot from the Session");
    t3.duringRead = [&] {
        const auto result = stop("release");
        check(result.at("structuredContent").at("alreadyStarted") == 1,
              "Stopping the inviter omitted an invited Bot's Run: " + result.dump());
    };
    reject("stop-race", "triage", "This Bot's participation in the Session has stopped");
    t3.duringRead = {};
    reject("stopped", "triage", "This Bot's participation in the Session has stopped");
    for (const auto* bot : {"release", "triage", "settings", "specialist"}) {
        check(participationState(database, bot) == "removed", "The stop cascade did not remove an invited Bot");
    }
    check(t3.cancellationAttempts == std::vector<std::string>{"triage-run"} &&
              stop("triage").at("structuredContent").at("alreadyStarted") == 1,
          "Repeated stops did not share the cascade's terminal cancellation receipt");
    check(model.invitations == 3 && model.wakes == 1 && t3.sends.size() == 1, "A rejected invitation produced a side effect");

    const auto reask = [&](const char* bot, const char* key, const char* text) {
        return mcp.call("flock_ask", {{"bot", bot}, {"threadId", "thread"}, {"requestId", key}, {"text", text}});
    };
    // Rejoin the inviter first. A former invitee still requires a person's ask.
    model.target = "triage";
    const auto back = reask("release", "restore-inviter", "Resume in this Session");
    check(back.contains("structuredContent") && reask("release", "restore-inviter", "Resume in this Session") == back,
          "Reactivation did not preserve the ask receipt");
    check(participationState(database, "release") == "active" && participationState(database, "triage") == "removed",
          "Reactivating an inviter also reactivated its former invitee");
    runtime->runConversations();
    check(model.receipts.at("Release Bot") == Json{{"ok", false}, {"error", "The person removed this Bot from the Session"}},
          "A reactivated inviter could invite a removed Bot back");

    reask("triage", "restore-triage", "Rejoin the Session").at("structuredContent");
    reask("settings", "restore-settings", "Rejoin the Session").at("structuredContent");
    reask("release", "leave-cascade", "Leave this Session").at("structuredContent");
    runtime->runConversations();
    for (const auto* bot : {"release", "triage", "settings"})
        check(participationState(database, bot) == "left", "Leaving did not cascade to an invited Bot");
    check(participationState(database, "specialist") == "removed", "Leaving weakened a person's earlier removal");

    // Reinvite a Bot that left using a fresh turn with an active inviter.
    model.target = "triage";
    model.inviteOnly = true;
    reask("release", "return-after-leave", "Invite the Bot that left").at("structuredContent");
    runtime->runConversations();
    check(model.receipts.at("Release Bot").value("ok", false) && participationState(database, "triage") == "active" &&
              participationState(database, "settings") == "left",
          "Inviting a Bot back did not reactivate just that Bot");
}

void syntheticTurnsShareModelBudget() {
    struct BudgetT3 : FixedT3 {
        int sends = 0;
        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override {
            if (target != SessionTarget{"env-1", "thread"} && target != SessionTarget{"env-2", "outside"}) return {};
            return SessionMetadata{.title = "Staging follow-up", .projectId = "project"};
        }
        Delivery send(const Command& command) override {
            return {Delivery::Kind::Accepted, {command.target.threadId, "run-" + std::to_string(++sends), {}}, {}, false, "queued"};
        }
        std::string sessionMessageWake(const SessionTarget&, const RunRef&) override { return "finished"; }
    } t3;
    struct BudgetModel : Model {
        int calls = 0, inviteeCalls = 0;
        ModelTurn decide(const ModelInput& input) override {
            ++calls;
            const auto& event = input.event;
            if (event.contains("sessionInvitation")) ++inviteeCalls;
            if (event.at("turn").empty() && !event.contains("sessionInvitation")) {
                if (event.at("text") == "Invite Triage")
                    return {{{"invite_bot",
                              {{"botId", "triage"}, {"environmentId", "env-1"}, {"threadId", "thread"}, {"note", "Review the checks."}},
                              "Invite Triage"}}};
                const bool outside = event.at("text") == "Check the other Session.";
                return {
                    {{"message_session",
                      {{"environmentId", outside ? "env-2" : "env-1"}, {"threadId", outside ? "outside" : "thread"}, {"text", "Checking."}},
                      "Send a follow-up"}}};
            }
            return {{{"reply", {{"text", "Done."}}, "Finish the turn"}}};
        }
    } model;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    T3Connections connector(store);
    const auto token = connector.connect("env-1", localOwner.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        const int limit = bot == "triage" ? 20 : request == "Invite Triage" ? 1 : 2;
        return {std::string(bot), std::string(request), "env-1", "project", {{"modelCallLimit", limit}}};
    };
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; },
                    {.peers = [](std::string_view) { return std::vector{peer("triage")}; }, .intake = cb.intake});
    const auto ask = [&](const char* key, const char* text) {
        mcp.call("flock_ask", {{"bot", "release"}, {"threadId", "thread"}, {"requestId", key}, {"text", text}}).at("structuredContent");
    };
    const auto approve = [&](const Json& item) {
        Inbox(store).decideInbox(localOwner,
                                 {item.at("botId"), InboxKind::conversation_model_budget, item.at("itemId"), item.at("waitId"),
                                  item.at("taskId"), item.at("targetRevision"), item.at("targetGeneration")},
                                 "approve");
    };
    ask("follow-ups", "Keep checking");
    runtime.runConversations();
    runtime.runConversations();
    const auto wakeApproval = Inbox(store).inbox(localOwner, "release").at(0);
    check(model.calls == 2 && t3.sends == 1 && wakeApproval.at("itemKind") == "conversation_model_budget" &&
              wakeApproval.at("allowance") == 2,
          "A synthetic wake got a new two-call allowance");
    approve(wakeApproval);
    for (int i = 0; i < 3; ++i) runtime.runConversations();
    const auto nextWake = Inbox(store).inbox(localOwner, "release").at(0);
    check(model.calls == 4 && t3.sends == 2 && nextWake.at("waitId") != wakeApproval.at("waitId"),
          "Synthetic wakes did not share the renewed root allowance");
    ask("invite", "Invite Triage");
    runtime.runConversations();
    const auto inviteApproval = Inbox(store).inbox(localOwner, "triage").at(0);
    check(model.calls == 5 && model.inviteeCalls == 0 && inviteApproval.at("itemKind") == "conversation_model_budget" &&
              inviteApproval.at("allowance") == 1,
          "The fresh ask did not get one call, or the invitation used the invitee's allowance");
    approve(inviteApproval);
    runtime.runConversations();
    check(model.calls == 6 && model.inviteeCalls == 1, "The invitee's approval did not restore the originating allowance");

    connector.connect("env-2", localOwner.id, 4173);
    ask("cross-environment", "Check the other Session.");
    runtime.runConversations();
    check(t3.sends == 3, "The Bot did not queue a message in the other environment");
    const auto pausedCount = Conversations(store).pausedConversations("release").size();
    connector.disconnect("env-1");
    connector.connect("env-1", localOwner.id, 4173);
    runtime.runConversations();
    check(model.calls == 8 && Conversations(store).pausedConversations("release").size() < pausedCount,
          "Reconnecting the origin revived a wake in a different environment");
}

void slowProbeAndStoppedApproval() {
    struct ProbeT3 : FixedT3 {
        std::promise<void> healthyStarted;
        std::shared_future<void> healthy = healthyStarted.get_future().share();
        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline deadline) override {
            if (target.environmentId == "env-0-stalled") {
                healthy.wait_until(deadline.value());
                throw SessionReadTimeout{};
            }
            if (deadline != SessionReadDeadline{} && std::chrono::steady_clock::now() >= deadline) throw SessionReadTimeout{};
            if (deadline != SessionReadDeadline{}) healthyStarted.set_value();
            return SessionMetadata{.title = "Staging follow-up", .projectId = "project"};
        }
    } t3;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    auto connection = T3Connections(store);
    const auto token = connection.connect("env-0-stalled", localOwner.id, 4173);
    connection.connect("env-1", localOwner.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    Policy{store}.setMode(localOwner, "supervised");
    mcp.call("flock_ask", {{"bot", "release"}, {"threadId", "thread"}, {"requestId", "healthy"}, {"text", "Check staging."}})
        .at("structuredContent");
    AskModel model;
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; });
    runtime.runConversations();
    const auto card = Inbox(store).inbox(localOwner).at(0);
    check(model.asks == 1 && model.origin.at("environmentId") == "env-1" && card.at("status") == "pending",
          "An earlier environment probe starved the healthy environment or skipped Supervised approval");
    mcp.call("flock_stop", {{"bot", "release"}, {"threadId", "thread"}}).at("structuredContent");
    check(Inbox(store).inbox(localOwner).empty(), "The stopped Session ask still had an approval in the inbox");
    bool unavailable = false;
    try {
        Inbox(store).decideInbox(localOwner,
                                 {card.at("botId"), InboxKind::policy_approval, card.at("itemId"), card.at("waitId"), card.at("taskId"),
                                  card.at("targetRevision"), card.at("targetGeneration")},
                                 "allow_once");
    } catch (const DecisionUnavailable&) {
        unavailable = true;
    }
    check(unavailable, "A saved approval still worked after the Session stop");
}

void stopSelectedTaskAndConsultation() {
    struct TaskT3 : FixedT3 {
        std::vector<Command> sends;
        std::vector<std::string> cancellations, interrupts;

        std::pair<std::vector<std::string>, std::optional<Id>> projectSessions(std::string_view, std::string_view, Id,
                                                                               SessionReadDeadline) override {
            return {{"thread"}, {}};
        }
        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override {
            if (target != SessionTarget{"env-1", "thread"}) return {};
            return SessionMetadata{.title = "Selected Session", .projectId = "selected-project"};
        }
        Delivery send(const Command& command) override {
            sends.push_back(command);
            if (command.tool == CommandTool::start_in_session)
                return {Delivery::Kind::Accepted, {"thread", "queued-task", {}}, {}, false, "queued"};
            return FixedT3::send(command);
        }
        Observation observe(const std::string&, const Json&, const RunRef&) override {
            Observation observation;
            observation.kind = Observation::Kind::Running;
            return observation;
        }
        QueueCancelOutcome cancelQueuedSessionMessage(const SessionTarget& target, std::string_view run) override {
            check(target == SessionTarget{"env-1", "thread"}, "Cancelled a different Session");
            cancellations.emplace_back(run);
            return QueueCancelOutcome::Cancelled;
        }
        InterruptOutcome interrupt(const std::string&, const Json&, const RunRef& run) override {
            interrupts.push_back(run.runId);
            return InterruptOutcome::Requested;
        }
    } t3;
    struct TaskModel : Model {
        Id consultant = 0;
        int consultantCalls = 0;
        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            if (event.at("kind") == "conversation_turn") {
                const auto& steps = event.at("turn");
                const auto request = event.at("text");
                if (request == "Work in the selected Session." && steps.empty())
                    return {{{"flock_session_search", Json::object(), "Find the selected Session"}}};
                if (request == "Work in the selected Session." && steps.size() == 1)
                    return {{{"start_task",
                              {{"request", request}, {"sessionRef", steps.back().at("result").at("sessions").at(0).at("sessionRef")}},
                              "Queue work"}}};
                if (steps.empty()) return {{{"start_task", {{"request", request}}, "Start work"}}};
                return {{{"reply", {{"text", "Started."}}, "Finish the conversation turn"}}};
            }
            if (input.task.at("request") == "Consult a peer.")
                return {{{"send_bot_message", {{"botId", "triage"}, {"kind", "consult"}, {"text", "Offer advice."}}, "Consult Triage"}}};
            if (input.task.at("request") == "Offer advice.") {
                consultant = input.task.at("id");
                ++consultantCalls;
                check(std::ranges::find(input.tools, "delegate") == input.tools.end(), "A consultation acquired delegation");
                return {{{"reply_to_bot", {{"kind", "question"}, {"text", "Which environment?"}}, "Ask the sending Bot"}}};
            }
            return {{{"delegate", {{"instruction", "Check staging."}, {"doneWhen", "Staging is checked."}}, "Run the task"}}};
        }
    } model;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    const auto token = T3Connections(store).connect("env-1", localOwner.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        return {std::string(bot), std::string(request), "env-1", "origin-project", Json::object()};
    };
    const auto server = startWeb(store, 0, cb);
    Mcp mcp(*server, token);
    Runtime runtime(store, model, t3, [](std::string_view) { return std::string{}; },
                    {.peers = [](std::string_view) { return std::vector{peer("triage")}; }});
    auto blockingIntake = peer("triage").intake;
    blockingIntake.request = "Hold the peer's project slot.";
    const auto blocker = TaskIntake(store).submit(blockingIntake);
    const auto start = [&](const char* key, const char* text) -> Id {
        mcp.call("flock_ask", {{"bot", "release"}, {"threadId", "thread"}, {"requestId", key}, {"text", text}}).at("structuredContent");
        runtime.runConversations();
        return TaskList(store).tasks("release").front().task.id;
    };
    const auto selected = start("queued-task", "Work in the selected Session.");
    const auto sender = start("consult", "Consult a peer.");
    tick(runtime, 4);
    check(Tasks(store).task(selected).task.activeRun.value().ref.runId == "queued-task" &&
              Tasks(store).task(blocker).task.state == TaskState::running && model.consultant &&
              Tasks(store).task(model.consultant).task.state == TaskState::peer_waiting,
          "The task did not queue, or the consultation did not wait behind the occupied slot");
    const auto stopped = mcp.call("flock_stop", {{"bot", "release"}, {"threadId", "thread"}}).at("structuredContent");
    tick(runtime, 4);
    check(stopped.at("cancelled") == 1 && stopped.at("stoppedChildSessions") == 1 &&
              t3.cancellations == std::vector<std::string>{"queued-task"} && t3.interrupts.empty() &&
              Tasks(store).task(selected).task.state == TaskState::cancelled &&
              Tasks(store).task(sender).task.state == TaskState::cancelled &&
              Tasks(store).task(model.consultant).task.state == TaskState::cancelled && model.consultantCalls == 1 &&
              Tasks(store).task(blocker).task.state == TaskState::running,
          "Stop left queued work or consultation runnable, or touched an unrelated Run");
}

void inviteIntoPeerLaunch() {
    struct PeerT3 : FixedT3 {
        std::optional<SessionMetadata> sessionMetadata(const SessionTarget& target, bool, SessionReadDeadline) override {
            if (target.environmentId != "env-1" || (target.threadId != "thread" && target.threadId != "thread-bot-project")) return {};
            return SessionMetadata{.title = "Peer Session", .projectId = "project"};
        }
    } t3;
    struct PeerModel : Model {
        Json invite;
        int invitations = 0;
        ModelTurn decide(const ModelInput& input) override {
            const auto& event = input.event;
            if (event.at("kind") == "bot_request")
                return {{{"delegate", {{"instruction", "Check staging."}, {"doneWhen", "Staging is checked."}}, "Launch peer work"}}};
            if (event.at("kind") != "conversation_turn")
                return {{{"send_bot_message",
                          {{"botId", "triage"}, {"kind", "work"}, {"text", "Check staging."}, {"doneWhen", "Staging is checked."}},
                          "Ask Triage"}}};
            const auto& steps = event.at("turn");
            if (event.contains("sessionInvitation"))
                ++invitations;
            else if (event.at("text") == "Start peer work." && steps.empty())
                return {{{"start_task", {{"request", "Start peer work."}}, "Start the parent task"}}};
            else if (event.at("text") == "Invite the verifier.") {
                if (steps.empty())
                    return {{{"invite_bot",
                              {{"botId", "settings"},
                               {"environmentId", "env-1"},
                               {"threadId", "thread-bot-project"},
                               {"note", "Verify the staging checks."}},
                              "Invite the verifier"}}};
                invite = steps.back().at("result");
            }
            return {{{"reply", {{"text", "Done."}}, "Finish the turn"}}};
        }
    } model;
    TestDirectory dir;
    Store store(dir.path / "flock.sqlite3");
    T3Connections connector(store);
    const auto token = connector.connect("env-1", localOwner.id, 4173);
    auto cb = callbacks();
    cb.sessionT3 = &t3;
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        return {std::string(bot), std::string(request), "env-1", "origin-project", Json::object()};
    };
    const auto server = startWeb(store, 0, cb);
    Runtime runtime(
        store, model, t3, [](std::string_view) { return std::string{}; },
        {.peers = [](std::string_view bot) { return bot == "release" ? std::vector{peer("triage")} : std::vector{peer("settings")}; },
         .intake = cb.intake});
    Mcp(*server, token)
        .call("flock_ask", {{"bot", "release"}, {"threadId", "thread"}, {"requestId", "peer-launch"}, {"text", "Start peer work."}})
        .at("structuredContent");
    tick(runtime, 5);
    check(t3.launches["bot-project"] == 1, "Triage did not launch its peer work");
    const Person other{"other-admin", true};
    WebIdentity(store).setWebIdentityResolver([&](std::string_view id) -> std::optional<bool> {
        if (id == localOwner.id || id == other.id) return true;
        return {};
    });
    const auto invite = [&](const Person& actor, const char* id) {
        connector.connect("env-1", actor.id, 4173);
        WebConversationIntake(store).receiveWeb(actor, cb.intake("triage", ""), id, "Invite the verifier.");
        runtime.runConversations();
    };
    invite(other, "foreign-peer-launch");
    check(model.invite.value("error", "") == "Invite a Bot into a Session you participate in or launched" && model.invitations == 0,
          "A different requester inherited the peer's Session");
    invite(localOwner, "my-peer-launch");
    check(model.invite.value("ok", false) && model.invitations == 1,
          "The requester could not invite a Bot into the peer's launched Session");
}

void sessionAsks(std::string_view fixtureUrl, const std::filesystem::path& credentials) {
    httplib::Client fixture(fixtureUrl.data());
    const auto configure = [&](const Json& settings) {
        Browser::checked(fixture.Post("/fixture", settings.dump(), "application/json"), 200);
    };
    const auto sends = [&] { return Browser::checked(fixture.Get("/fixture/effects"), 200).value("t3_thread_send", Json::array()); };
    loginT3(credentials, "env-1", std::string(fixtureUrl) + "/mcp", [&](const auto& url) { configure({{"authorization", url}}); });
    configure({{"scenario", "running"},
               {"launched", "Prior Session request"},
               {"sessionWakeFixture", true},
               {"botRunStatus", "queued"},
               {"sessionAskFixture", true}});
    auto t3 = makeT3Mcp(credentials);

    TestDirectory dir;
    const auto database = dir.path / "flock.sqlite3";
    const auto makeStore = [&] {
        auto store = std::make_unique<Store>(database);
        WebIdentity(*store).setWebIdentityResolver([](std::string_view id) -> std::optional<bool> {
            if (id == alice.id) return true;
            if (id == bob.id) return false;
            return {};
        });
        return store;
    };
    auto db = makeStore();
    Policy{*db}.setMember(alice, "release", bob.id, true);
    const auto token = T3Connections(*db).connect("env-1", bob.id, 4173);

    auto cb = callbacks();
    cb.intake = [](std::string_view bot, std::string_view text) -> Intake {
        if (bot == "broken") throw std::runtime_error(privateValue);
        return {std::string(bot), std::string(text), "bot-default", "bot-project", {{"botDisplayName", "Release Bot"}}};
    };
    cb.sessionT3 = t3.get();
    cb.authenticate = [](std::string_view authorization) { return authorization == aliceCredential ? alice : Person{}; };
    cb.resolveIdentity = [](std::string_view id) -> std::optional<bool> {
        if (id == alice.id) return true;
        if (id == bob.id) return false;
        return {};
    };
    auto server = startWeb(*db, 0, cb);
    Mcp mcp(*server, token);
    check(Mcp(*server, "").request("tools/list", Json::object(), 401).contains("error"), "MCP accepted an absent bearer");
    mcp.request("initialize", {{"protocolVersion", "2025-06-18"}});
    const auto tools = mcp.request("tools/list", Json::object()).at("result").at("tools");
    check(tools.size() == 3 && tools[0].at("name") == "flock_bots" && tools[1].at("name") == "flock_ask" &&
              tools[2].at("name") == "flock_stop",
          "Wrong Flock MCP tools");
    const auto askSchema = tools[1].at("inputSchema");
    check(askSchema.at("properties").size() == 4 && !askSchema.at("properties").contains("mode") &&
              askSchema.at("additionalProperties") == false,
          "flock_ask still accepts a mode");
    const auto bots = mcp.call("flock_bots").at("structuredContent").at("bots");
    check(bots.size() == 1 && bots.front().at("id") == "release", "Bot catalog bypassed membership");

    Browser browser(*server);
    browser.client.set_default_headers({{"Authorization", aliceCredential}});
    for (const auto* route : {"answer", "work-answer", "pause", "resume", "freeze"})
        browser.post(std::string("/api/session-handoffs/1/") + route, Json::object(), 404);
    for (const auto* page : {"/api/session-handoffs", "/api/session-handoffs/1", "/session-handoff.html", "/session-handoff.js"})
        check(browser.client.Get(page)->status == 404, "A retired handoff route still exists");

    const Json ask{{"bot", "release"}, {"text", "Review the existing checks"}, {"threadId", "thread"}, {"requestId", "ask-1"}};
    auto unknown = ask;
    unknown["bot"] = "triage";
    check(mcp.call("flock_ask", unknown).value("isError", false), "Ask bypassed Bot membership");
    unknown = ask;
    unknown["threadId"] = "missing";
    check(mcp.call("flock_ask", unknown).value("isError", false), "Ask accepted a Session the person cannot read");
    auto obsolete = ask;
    obsolete["mode"] = "consult";
    check(mcp.request("tools/call", {{"name", "flock_ask"}, {"arguments", obsolete}}).contains("error"),
          "Ask accepted the retired mode argument");

    auto parallel = std::async(std::launch::async, [&] { return Mcp(*server, token).call("flock_ask", ask); });
    const auto accepted = mcp.call("flock_ask", ask);
    check(parallel.get() == accepted, "Concurrent retries returned different ask receipts");
    check(accepted.contains("structuredContent"), "Ask failed: " + accepted.dump());
    const auto anchor = accepted.at("structuredContent").at("messageId").get<Id>();
    auto changed = ask;
    changed["text"] = "Review different checks";
    check(mcp.call("flock_ask", changed).value("isError", false), "Changed text with the same requestId was accepted");
    changed = ask;
    changed["threadId"] = "other-thread";
    check(mcp.call("flock_ask", changed).value("isError", false), "Changed threadId with the same requestId was accepted");
    check(Conversations(*db).conversation("release").size() == 1, "Ask retries created multiple conversation messages");

    AskModel model;
    const auto soul = [](std::string_view) { return std::string{}; };
    auto runtime = std::make_unique<Runtime>(*db, model, *t3, soul);
    Policy{*db}.setMode(alice, "supervised");
    runtime->runConversations();
    check(model.asks == 1 && model.origin == sessionOrigin, "Ask did not start an ordinary Bot turn with the Session origin");
    const auto approval = Inbox(*db).inbox(bob).at(0);
    check(sends().empty() && approval.at("status") == "pending" && approval.at("conversationEventId") == anchor,
          "Supervised ask did not wait for the asking person's approval");
    Inbox(*db).decideInbox(bob,
                           {approval.at("botId"), InboxKind::policy_approval, approval.at("itemId"), approval.at("waitId"),
                            approval.at("taskId"), approval.at("targetRevision"), approval.at("targetGeneration")},
                           "allow_once");
    runtime->runConversations();
    const auto wire = sends();
    check(wire.size() == 1, "The Bot did not send exactly one reply");
    const auto action = wire.front().at("clientRequestId").get<std::string>();
    check(action.starts_with("flock-turn-") &&
              wire.front() == Json{{"mode", "queue"},
                                   {"threadId", "thread"},
                                   {"message", sessionPrompt(action, "Release Bot: For the person: Which checks should I review?")},
                                   {"clientRequestId", action}},
          "The Bot's reply did not use the named, idempotent Session queue path");

    Policy{*db}.setMode(alice, "full");
    configure({{"differentWaitRun", true}, {"personFinished", true}});
    runtime->runConversations();
    check(model.wakes == 0, "The person's Run caused the Bot to wake");
    configure({{"differentWaitRun", false}, {"botRunStatus", "failed"}});
    runtime->runConversations();
    check(model.wakes == 0, "A timed-out result consumed the Bot's wake");
    configure({{"botRunStatus", "completed"}});
    runtime->runConversations();
    runtime->runConversations();
    check(model.wakes == 1 && model.read.at("ok") == true && sends() == wire,
          "The assigned Run did not wake the Bot once with access to the origin Session");

    auto followUp = ask;
    followUp["requestId"] = "ask-2";
    followUp["text"] = "Review the smoke checks";
    check(!mcp.call("flock_ask", followUp).value("isError", false), "The person's forwarded reply was rejected");
    configure({{"botRunStatus", "queued"}});
    runtime->runConversations();
    check(model.asks == 2 && sends().size() == 2, "A fresh requestId did not continue the participation");

    auto pending = followUp;
    pending["requestId"] = "ask-3";
    mcp.call("flock_ask", pending).at("structuredContent");
    const Json stop{{"bot", "release"}, {"threadId", "thread"}};
    const auto stopped = mcp.call("flock_stop", stop);
    check(stopped.at("structuredContent").at("stopped") == true && mcp.call("flock_stop", stop) == stopped,
          "Stop did not end the participation idempotently");
    check(mcp.call("flock_ask", ask) == accepted, "Stop changed the receipt for an already accepted request");
    check(participationState(database, "release") == "removed", "Replaying a previous ask reactivated the removed Bot");
    pending["requestId"] = "ask-4";
    const auto reactivated = mcp.call("flock_ask", pending);
    check(reactivated.contains("structuredContent") && mcp.call("flock_ask", pending) == reactivated &&
              participationState(database, "release") == "active",
          "A fresh ask did not reactivate a removed Bot exactly once");

    runtime.reset();
    server.reset();
    db = makeStore();
    runtime = std::make_unique<Runtime>(*db, model, *t3, soul);
    configure({{"botRunStatus", "queued"}});
    runtime->runConversations();
    check(model.asks == 3 && model.wakes == 1 && sends().size() == 3 && model.send.value("ok", false),
          "The reactivated Bot could not send, or a pending ask or wake survived stop and restart");
}

void sessionStops(std::string_view fixtureUrl, const std::filesystem::path& credentials) {
    httplib::Client fixture(fixtureUrl.data());
    const auto configure = [&](const Json& settings) {
        Browser::checked(fixture.Post("/fixture", settings.dump(), "application/json"), 200);
    };
    const auto effects = [&] { return Browser::checked(fixture.Get("/fixture/effects"), 200); };
    const auto runs = [&] { return Browser::checked(fixture.Get("/fixture/stop-runs"), 200); };
    configure({{"effects", Json::object()},
               {"stopRuns", {{"person-queued", "queued"}}},
               {"personFinished", false},
               {"botRunStatus", "queued"},
               {"stopDropReply", "bot-run-3"}});
    auto t3 = makeT3Mcp(credentials);
    TestDirectory dir;
    const auto database = dir.path / "flock.sqlite3";
    auto db = std::make_unique<Store>(database);
    auto cb = callbacks();
    cb.sessionT3 = t3.get();
    const auto token = T3Connections(*db).connect("env-1", localOwner.id, 4173);
    auto server = startWeb(*db, 0, cb);
    const auto ask = [&](const char* bot, const char* id, std::string_view text) {
        return Mcp(*server, token).call("flock_ask", {{"bot", bot}, {"threadId", "thread"}, {"requestId", id}, {"text", text}});
    };
    const auto stop = [&](const char* bot) { return Mcp(*server, token).call("flock_stop", {{"bot", bot}, {"threadId", "thread"}}); };
    const auto cancel = [](std::string_view run) -> Json { return {{"action", "cancel"}, {"threadId", "thread"}, {"queuedRunId", run}}; };
    struct StopModel final : Model {
        AskModel ask;
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("text") == "Return without sending") return {{{"reply", {{"text", "Returned."}}, "Finish the turn"}}};
            if (input.event.at("text") == "Leave this Session") {
                check(input.event.at("turn").empty(), "A stopped Session-origin turn reached the model again");
                return {{{"leave_session", {{"environmentId", "env-1"}, {"threadId", "thread"}}, "Leave this Session"}}};
            }
            return ask.decide(input);
        }
    } model;
    auto now = unixTime();
    const RuntimeOptions options{.clock = [&] { return now; }};
    const auto soul = [](std::string_view) { return std::string{}; };
    auto runtime = std::make_unique<Runtime>(*db, model, *t3, soul, options);
    for (const auto* id : {"first-send", "started-send", "lost-cancellation"}) {
        check(ask("release", id, "Send an update").contains("structuredContent"), "Stop trial ask was rejected");
        runtime->runConversations();
    }
    check(ask("triage", "other-bot", "Send an update").contains("structuredContent"), "The other Bot's ask was rejected");
    runtime->runConversations();
    auto outstanding = runs();
    check(outstanding.size() == 5 && outstanding.at("bot-run-4") == "queued", "The stop fixture did not queue four Bot Runs");
    outstanding["bot-run-2"] = "running";
    configure({{"stopRuns", outstanding}});

    // Reopen with the queued Runs still outstanding.
    runtime.reset();
    server.reset();
    db = std::make_unique<Store>(database);
    server = startWeb(*db, 0, cb);
    runtime = std::make_unique<Runtime>(*db, model, *t3, soul, options);
    // Simulate a non-Session-origin wake that was already queued for a model turn.
    const auto queuedWake = ConversationIntake(*db).receive(localOwner, cb.intake("release", ""), "queued-before-stop", "Pending wake");
    const auto markWake = "UPDATE messages SET routing=json_set(routing,'$.sessionWake',json_object("
                          "'environmentId','env-1','threadId','thread','reason','finished')) WHERE id=" +
                          std::to_string(queuedWake.at("messageId").get<Id>());
    {
        sqlite3* raw = nullptr;
        const auto opened = sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> sql(raw, sqlite3_close);
        check(opened == SQLITE_OK && sqlite3_exec(sql.get(), markWake.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
              "Could not prepare the queued wake");
    }
    const auto result = stop("release");
    check(result.at("structuredContent") == Json{{"stopped", true},
                                                 {"cancelled", 1},
                                                 {"alreadyStarted", 1},
                                                 {"notCancellable", 0},
                                                 {"pending", 1},
                                                 {"stoppedChildSessions", 0}} &&
              result.at("content").at(0).at("text").get<std::string>().find("1 message had already started") != std::string::npos &&
              stop("release") == result,
          "The stop report did not distinguish started Runs and a lost cancellation reply");
    check(effects().at("t3_queue") == Json::array({cancel("bot-run-1"), cancel("bot-run-2"), cancel("bot-run-3")}) &&
              !effects().contains("t3_thread_interrupt") &&
              runs() == Json{{"bot-run-1", "cancelled"},
                             {"bot-run-2", "running"},
                             {"bot-run-3", "cancelled"},
                             {"bot-run-4", "queued"},
                             {"person-queued", "queued"}},
          "flock_stop cancelled another participant's work or interrupted the running Run");

    runtime.reset();
    server.reset();
    db = std::make_unique<Store>(database);
    server = startWeb(*db, 0, cb);
    now += 10;
    runtime = std::make_unique<Runtime>(*db, model, *t3, soul, options);
    const auto released = stop("release");
    check(released.at("structuredContent") == Json{{"stopped", true},
                                                   {"cancelled", 2},
                                                   {"alreadyStarted", 1},
                                                   {"notCancellable", 0},
                                                   {"pending", 0},
                                                   {"stoppedChildSessions", 0}} &&
              stop("release") == released &&
              effects().at("t3_queue") == Json::array({cancel("bot-run-1"), cancel("bot-run-2"), cancel("bot-run-3"), cancel("bot-run-3")}),
          "Restart did not retry only the unknown cancellation");
    ask("release", "return-after-restart", "Return without sending").at("structuredContent");
    runtime->runConversations();
    runtime->runConversations();
    check(model.ask.wakes == 0, "A wake queued before removal reached the returned Bot after restart");

    check(ask("triage", "leave", "Leave this Session").contains("structuredContent"), "The other Bot stopped participating");
    runtime->runConversations();
    check(participationState(database, "triage") == "left" &&
              Conversations(*db).conversation("triage").back().at("data").at("text").get<std::string>().find(
                  "cancelled 1 queued message") != std::string::npos &&
              effects().at("t3_queue").back() == cancel("bot-run-4") && effects().at("t3_queue").size() == 5 &&
              !effects().contains("t3_thread_interrupt") && runs().at("person-queued") == "queued",
          "leave_session did not perform and report the same stop as flock_stop");
    ConversationIntake(*db).receive(localOwner, cb.intake("triage", ""), "web-after-leave", "Send another update");
    runtime->runConversations();
    auto sharedTools = model.ask.webTools;
    check(std::erase(sharedTools, "review_preference") == 1 && sharedTools == model.ask.sessionTools,
          "A Session ask should receive the Web Bot's tools except automatic memory");
    check(participationState(database, "triage") == "left" && effects().at("t3_thread_send").size() == 4 &&
              model.ask.send.at("error") == "This Bot's participation in the Session has stopped",
          "A send reactivated a Bot that left");
    const auto back = ask("triage", "after-leave", "Send an update");
    check(back.contains("structuredContent") && ask("triage", "after-leave", "Send an update") == back,
          "A person could not ask a Bot back after it left");
    runtime->runConversations();
    check(participationState(database, "triage") == "active" && effects().at("t3_thread_send").size() == 5 &&
              model.ask.send.value("ok", false) && effects().at("t3_queue").size() == 5,
          "The returned Bot could not send, or reactivation repeated a queue cancellation");
}

void stopChildSessions(std::string_view fixtureUrl, const std::filesystem::path& credentials) {
    httplib::Client fixture(fixtureUrl.data());
    Browser::checked(
        fixture.Post(
            "/fixture",
            Json{{"effects", Json::object()}, {"lineageFixture", true}, {"lineageSessions", Json::object()}, {"sessionWakeFixture", false}}
                .dump(),
            "application/json"),
        200);
    const auto interrupts = [&] {
        return Browser::checked(fixture.Get("/fixture/effects"), 200).value("t3_thread_interrupt", Json::array());
    };
    struct ChildModel : Model {
        Id task = 0, peer = 0;
        ModelTurn decide(const ModelInput& input) override {
            if (input.event.at("kind") != "conversation_turn") {
                if (input.task.at("request") == "Ask a peer first.")
                    return {{{"send_bot_message",
                              {{"botId", "triage"}, {"kind", "work"}, {"text", "Check a detail."}, {"doneWhen", "The detail is checked."}},
                              "Ask the peer Bot"}}};
                if (input.task.at("request") == "Check a detail.") {
                    peer = input.task.at("id");
                    if (input.event.at("kind") == "bot_request")
                        return {{{"reply_to_bot", {{"kind", "question"}, {"text", "Which detail?"}}, "Get the detail"}}};
                    return {{{"ask_user", {{"question", "Which detail?"}}, "Identify the detail to check"}}};
                }
                return {{{"delegate",
                          {{"instruction", "Run the child trial."}, {"doneWhen", "Child trial has finished."}},
                          "Launch the child Session"}}};
            }
            if (input.event.at("text") == "Confirm reactivation")
                return {{{"reply", {{"text", "Back in the Session."}}, "Confirm reactivation"}}};
            const auto& steps = input.event.at("turn");
            if (steps.empty()) return {{{"start_task", {{"request", input.event.at("text")}}, "Start a child task"}}};
            task = steps.back().at("result").at("taskId");
            return {{{"reply", {{"text", "Task started."}}, "End the turn"}}};
        }
    } model;
    TestDirectory dir;
    const auto database = dir.path / "flock.sqlite3";
    auto db = std::make_unique<Store>(database);
    auto t3 = makeT3Mcp(credentials);
    auto cb = callbacks();
    cb.sessionT3 = t3.get();
    cb.intake = [](std::string_view bot, std::string_view request) -> Intake {
        return {std::string(bot), std::string(request), "env-1", request == "Ask a peer first." ? "peer-project" : "project",
                Json::object()};
    };
    const RuntimeOptions options{.peers = [](std::string_view bot) -> std::vector<PeerBot> {
        if (bot == "release") return {{"Triage", {"triage", "", "env-1", "peer-project", Json::object()}}};
        return {};
    }};
    Policy{*db}.setMode(localOwner, "full");
    const auto token = T3Connections(*db).connect("env-1", localOwner.id, 4173);
    auto server = startWeb(*db, 0, cb);
    const auto soul = [](std::string_view) { return std::string{}; };
    auto runtime = std::make_unique<Runtime>(*db, model, *t3, soul, options);
    const auto start = [&](const char* bot, const char* thread, const char* key, const char* text = "Start a child Session.") {
        Mcp(*server, token)
            .call("flock_ask", {{"bot", bot}, {"threadId", thread}, {"requestId", key}, {"text", text}})
            .at("structuredContent");
        runtime->runConversations();
        check(model.task != 0, "Bot did not start a task");
        return std::exchange(model.task, 0);
    };
    const auto child = start("release", "lineage-parent", "matching-child");
    tick(*runtime, 3);
    const auto launching = Tasks(*db).task(child);
    check(launching.task.state == TaskState::running && launching.task.activeRun.value().ref.threadId == "lineage-child-1",
          "Child launch did not assign a Run in its own Session: " + taskJson(launching).at("wait").dump());
    const auto beforeLaunch = start("release", "lineage-parent", "peer-before-child", "Ask a peer first.");
    tick(*runtime, 4);
    check(model.peer && Tasks(*db).task(model.peer).task.state == TaskState::peer_waiting,
          "The peer needed by the other child task did not reach its question");
    const auto otherOrigin = start("release", "lineage-unrelated", "unrelated-origin");
    const auto otherBot = start("triage", "lineage-parent", "unrelated-bot");
    const Json stop{{"bot", "release"}, {"threadId", "lineage-parent"}};
    const auto stopped = Mcp(*server, token).call("flock_stop", stop);
    check(stopped.at("structuredContent").at("stoppedChildSessions") == 2 &&
              stopped.at("content").at(0).at("text").get<std::string>().find("2 child Sessions.") != std::string::npos &&
              Mcp(*server, token).call("flock_stop", stop) == stopped,
          "Stop did not report the exact child cancellation once");
    check(Tasks(*db).task(child).task.state == TaskState::cancelling && Tasks(*db).task(otherOrigin).task.state == TaskState::queued &&
              Tasks(*db).task(otherBot).task.state == TaskState::queued && interrupts().empty(),
          "Stop targeted unrelated tasks or failed to durably admit the cancellation");

    // Restart before the cancellation worker ever sees the stopped task.
    runtime.reset();
    server.reset();
    db = std::make_unique<Store>(database);
    server = startWeb(*db, 0, cb);
    runtime = std::make_unique<Runtime>(*db, model, *t3, soul, options);
    tick(*runtime, 3);
    check(Tasks(*db).task(child).task.state == TaskState::cancelled && Tasks(*db).task(beforeLaunch).task.state == TaskState::cancelled &&
              Tasks(*db).task(model.peer).task.state == TaskState::waiting &&
              Tasks(*db).task(otherOrigin).task.state != TaskState::cancelled &&
              Tasks(*db).task(otherOrigin).task.state != TaskState::cancelling &&
              Tasks(*db).task(otherBot).task.state == TaskState::queued &&
              interrupts() == Json::array({{{"threadId", "lineage-child-1"}, {"runId", "root"}}}) &&
              Mcp(*server, token).call("flock_stop", stop) == stopped,
          "Restart did not settle precisely the child's Run, or the origin Session was interrupted");
    Mcp(*server, token)
        .call("flock_ask",
              {{"bot", "release"}, {"threadId", "lineage-parent"}, {"requestId", "return-without-child"}, {"text", "Confirm reactivation"}})
        .at("structuredContent");
    runtime->runConversations();
    const auto stoppedAgain = Mcp(*server, token).call("flock_stop", stop);
    check(stoppedAgain.at("structuredContent").at("stoppedChildSessions") == 0 &&
              Mcp(*server, token).call("flock_stop", stop) == stoppedAgain &&
              interrupts() == Json::array({{{"threadId", "lineage-child-1"}, {"runId", "root"}}}),
          "A later stop counted or repeated the previous participation's child cancellation");
}
}

int main(int argc, char** argv) {
    const char* scenario = "setup";
    try {
        check(argc == 3, "Expected fixture URL and credentials directory");
        scenario = "listener port change";
        listenerRebind();
        scenario = "Session asks";
        sessionAsks(argv[1], argv[2]);
        scenario = "Session stops";
        sessionStops(argv[1], argv[2]);
        scenario = "stop during send preflight";
        stopDuringSendPreflight();
        scenario = "stop during send dispatch";
        stopDuringSendDispatch();
        scenario = "Bot pause during Session participation";
        pausedBotStopsSessionMessages();
        scenario = "Web send participation";
        webSendParticipation();
        scenario = "Session invitations";
        sessionInvitations();
        scenario = "synthetic turn model budget";
        syntheticTurnsShareModelBudget();
        scenario = "slow Session probe and stopped approval";
        slowProbeAndStoppedApproval();
        scenario = "stop selected tasks and consultations";
        stopSelectedTaskAndConsultation();
        scenario = "invite into a peer launch";
        inviteIntoPeerLaunch();
        scenario = "stop child Sessions";
        stopChildSessions(argv[1], argv[2]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << scenario << ": " << error.what() << '\n';
        return 1;
    }
}
