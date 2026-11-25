#include "flock/policy.hpp"
#include "flock/channels.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"

#include <algorithm>

using namespace flockv0;

namespace {
void check(bool pass, const char* reason) {
    if (!pass) throw std::runtime_error(reason);
}

struct SearchModel final : Model {
    Decision next;
    Json result;

    ModelTurn decide(const ModelInput& input) override {
        const auto& steps = input.event.at("turn");
        if (steps.empty()) return {{next}};
        result = steps.back().at("result");
        return {{{"reply", {{"text", "Checked."}}, "Respond with the evidence"}}};
    }
};

struct SearchConversation {
    Store store;
    SearchModel model;
    Runtime runtime;
    int sequence = 0;

    SearchConversation(T3& t3, const std::filesystem::path& database)
        : store(database), runtime(store, model, t3, [](std::string_view) { return std::string{}; }) {}

    void blockSession(std::string_view thread, bool blocked = false) {
        store.policy().changeNeverRule(localOwner, Action::session_read, Resource::sessionRead("env-1", thread).target, blocked);
    }

    Json call(std::string tool, Json args, std::string bot = "release") {
        const Intake intake{std::move(bot), "Investigate staging", "env-1", "project", Json::object()};
        model.result = nullptr;
        model.next = {std::move(tool), std::move(args), "Inspect the requested investigation"};
        store.receive(localOwner, intake, std::to_string(++sequence), "Check the investigation.");
        runtime.runConversations();
        return model.result;
    }

    Json search(Json args, std::string bot = "release") { return call("flock_session_search", std::move(args), std::move(bot)); }
};
}

void deletedSessionSearch(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture) {
    SearchConversation conversation(t3, database);
    const auto call = [&](std::string tool, Json args) { return conversation.call(std::move(tool), std::move(args)); };
    const auto found = call("flock_session_search", {{"query", "Former investigation"}});
    check(found.at("ok") == true && found.at("sessions").size() == 1, "could not reference the Session before deletion");
    const Json counts{{"operation", "counts"}, {"sessionRef", found.at("sessions").at(0).at("sessionRef")}};
    check(call("flock_session_events", counts).at("ok") == true, "could not read the Session before deletion");

    const auto threadError = [&](const char* code, const char* message) {
        fixture({{"threadReadErrors", {{"deleted", {{"code", code}, {"message", message}}}}}});
    };
    threadError("capability_denied", "Read scope required.");
    const auto denied = call("flock_session_events", counts);
    check(denied.at("ok") == false, "T3 read denial was ignored");

    threadError("invalid_request", "The thread was not found.");
    const auto surviving = call("flock_session_search", Json::object());
    check(surviving.at("ok") == true && surviving.at("sessions").size() == 1 &&
              surviving.at("sessions").at(0).at("title") == "Staging investigation",
          "deleted Session blocked a readable search result");
    check(call("flock_session_events", counts).dump() == denied.dump(), "deleted and inaccessible Sessions differed");

    threadError("invalid_request", "The view was not supported.");
    check(call("flock_session_search", Json::object()).at("ok") == false, "unrelated invalid request was suppressed");
    fixture({{"threadReadErrors", Json::object()}});
}

void boundedSessionSearch(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture,
                          const std::function<Json(std::string_view)>& effects) {
    SearchConversation conversation(t3, database);
    fixture(
        {{"scenario", "sparse-session-search"},
         {"missingSearchThreads", {"candidate-1063"}},
         {"searchTitles",
          {{"candidate-1064", "Staging failure A"}, {"candidate-1128", "Staging failure B"}, {"candidate-1129", "Staging failure C"}}}});

    const Json query{{"query", "staging failure"}};
    const auto before = effects("t3_thread_read").size();
    const auto first = conversation.search(query);
    const auto reads = effects("t3_thread_read");
    check(first.at("ok") == true && first.at("sessions").empty() && first.at("nextCursor").is_string() && first.at("truncated") == false,
          "a sparse empty page did not return continuation");
    check(reads.size() == before + 64 && reads.back().at("threadId") == "candidate-1063", "a search page read beyond its candidate limit");
    check(first.dump().find("candidate-") == std::string::npos, "the opaque scan cursor exposed a thread ID");
    auto resume = query;
    resume["cursor"] = first.at("nextCursor");
    auto wrongFilter = resume;
    wrongFilter["status"] = "failed";
    check(conversation.search(wrongFilter).at("ok") == false && conversation.search(resume, "other-bot").at("ok") == false,
          "scan cursor crossed filters or Bots");

    const auto second = conversation.search(resume);
    check(second.at("sessions").size() == 1 && second.at("sessions").at(0).at("title") == "Staging failure A" &&
              second.at("nextCursor").is_string() && second.at("nextCursor") != first.at("nextCursor"),
          "continuing an empty page skipped a match or failed to advance");
    check(effects("t3_thread_read").at(reads.size()).at("threadId") == "candidate-1064",
          "continuation did not resume immediately after the last scanned candidate");
    resume["cursor"] = second.at("nextCursor");
    conversation.blockSession("candidate-1128", true);
    const auto last = conversation.search(resume);
    check(last.at("sessions").size() == 1 && last.at("sessions").at(0).at("title") == "Staging failure C" &&
              last.at("nextCursor").is_null(),
          "continuation included a blocked Session or missed the end of the search");
    conversation.blockSession("candidate-1128");
    const auto restored = conversation.search(resume);
    check(restored.at("sessions").size() == 2 && restored.at("sessions").at(0).at("title") == "Staging failure B" &&
              restored.at("sessions").at(1).at("title") == "Staging failure C" && restored.at("nextCursor").is_null(),
          "continuation did not preserve order or reflect the current Session filter");
}

void sessionSearchDeadline(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture) {
    SearchConversation conversation(t3, database);
    fixture(
        {{"slowSearchThread", "deadline-2"}, {"searchTitles", {{"deadline-2", "Staging failure A"}, {"deadline-3", "Staging failure B"}}}});
    const Json query{{"query", "staging failure"}};
    const auto start = std::chrono::steady_clock::now();
    const auto partial = conversation.search(query);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(elapsed < std::chrono::milliseconds(3100), "slow T3 calls outlasted the shared search deadline");
    check(partial.at("ok") == true && partial.at("sessions").empty() && partial.at("nextCursor").is_string(),
          "a slow candidate discarded scan progress");

    auto resume = query;
    resume["cursor"] = partial.at("nextCursor");
    const auto deferredStart = std::chrono::steady_clock::now();
    const auto deferred = conversation.search(resume);
    check(std::chrono::steady_clock::now() - deferredStart < std::chrono::milliseconds(3100),
          "a candidate that supplied no evidence was read again before the next model call");
    check(deferred.at("ok") == true && deferred.at("truncated") == true && deferred.at("sessions").empty() &&
              deferred.at("nextCursor").is_string(),
          "a repeatedly slow candidate did not return an incomplete continuation");
    auto pastSlow = query;
    pastSlow["cursor"] = deferred.at("nextCursor");
    const auto later = conversation.search(pastSlow);
    check(later.at("sessions").size() == 1 && later.at("sessions").at(0).at("title") == "Staging failure B" &&
              later.at("nextCursor").is_null(),
          "a persistently slow candidate blocked later matches");
    fixture({{"slowSearchThread", nullptr}});
    const auto resumed = conversation.search(resume);
    check(resumed.at("sessions").size() == 2 && resumed.at("nextCursor").is_null() &&
              resumed.at("sessions").at(0).at("title") == "Staging failure A" &&
              resumed.at("sessions").at(1).at("title") == "Staging failure B",
          "the timed-out candidate was skipped or reordered on retry");

    auto matchedPage = query;
    matchedPage["limit"] = 1;
    const auto firstMatch = conversation.search(matchedPage);
    check(firstMatch.at("sessions").size() == 1 && firstMatch.at("nextCursor").is_string(), "could not create a matched-result cursor");
    matchedPage["cursor"] = firstMatch.at("nextCursor");
    fixture({{"searchReadDelays", {{"deadline-2", 1.15}, {"deadline-3", 0.9}}}});
    const auto afterSlowAnchor = conversation.search(matchedPage);
    check(afterSlowAnchor.at("ok") == true && afterSlowAnchor.at("sessions").size() == 1 &&
              afterSlowAnchor.at("sessions").at(0).at("title") == "Staging failure B" && afterSlowAnchor.at("nextCursor").is_null() &&
              afterSlowAnchor.at("truncated") == false,
          "a slow but readable anchor exhausted the next candidate's budget");

    fixture({{"searchReadDelays", Json::object()}, {"missingSearchThreads", {"deadline-2"}}});
    check(conversation.search(matchedPage).at("ok") == false, "matched-result cursor accepted an unreadable anchor");
    fixture({{"scenario", "session-read"}, {"missingSearchThreads", Json::array()}});
}

void sessionMessageWake(T3& t3, const std::filesystem::path& database, const std::function<void(Json)>& fixture,
                        const std::function<Json(std::string_view)>& effects) {
    struct ModelFixture final : Model {
        Decision next;
        Json result;
        std::vector<Json> wakes, reads;
        ModelTurn decide(const ModelInput& input) override {
            const auto& steps = input.event.at("turn");
            if (const auto wake = input.event.find("sessionWake"); wake != input.event.end()) {
                if (steps.empty()) {
                    wakes.push_back(*wake);
                    return {{{"flock_session_events",
                              {{"operation", "counts"}, {"environmentId", wake->at("environmentId")}, {"threadId", wake->at("threadId")}},
                              "Read the Session after waking"}}};
                }
                reads.push_back(steps.back().at("result"));
            } else if (steps.empty()) {
                check(std::ranges::find(input.tools, next.tool) != input.tools.end(), "Session messaging was not offered");
                return {{next}};
            } else
                result = steps.back().at("result");
            return {{{"reply", {{"text", "Checked."}}, "Respond after checking"}}};
        }
    } model;
    fixture({{"scenario", "running"}, {"sessionWakeFixture", true}, {"botRunStatus", "queued"}});
    auto store = std::make_unique<Store>(database);
    auto runtime = std::make_unique<Runtime>(*store, model, t3, [](std::string_view) { return std::string{}; });
    const auto configureIdentity = [&] {
        store->setWebIdentityResolver([](std::string_view person) -> std::optional<bool> {
            if (person == localOwner.id) return true;
            return {};
        });
    };
    configureIdentity();
    const auto restart = [&] {
        runtime.reset();
        store = std::make_unique<Store>(database);
        configureIdentity();
        runtime = std::make_unique<Runtime>(*store, model, t3, [](std::string_view) { return std::string{}; });
    };
    T3Connections(*store).connect("env-1", localOwner.id, 3210);
    const Intake intake{"release", "Inspect staging", "env-1", "project", {{"botDisplayName", "Ernest"}}};
    const auto receive = [&](std::string_view id) { return store->receiveWeb(localOwner, intake, id, "Send the follow-up."); };
    model.next = {"flock_session_search", Json::object(), "Find the Session"};
    receive("find");
    runtime->runConversations();
    check(model.result.is_object(), "Web conversation did not call Session search");
    const auto session = model.result.at("sessions").at(0);
    const auto thread = session.at("threadId").get<std::string>();
    check(thread == "thread" && session.at("environmentId") == "env-1", "Search did not return the Session identifiers");
    model.next = {"message_session",
                  {{"environmentId", session.at("environmentId")}, {"threadId", thread}, {"text", "Check the fixture output"}},
                  "Queue the follow-up"};
    const auto resource = Resource::sessionRead("env-1", thread).target;
    const auto sends = [&] { return effects("t3_thread_send").size(); };
    const auto before = sends();
    store->policy().changeNeverRule(localOwner, Action::t3_send, resource, true);
    receive("never");
    runtime->runConversations();
    check(sends() == before && model.result.at("ok") == false, "Never permitted a Session send");
    store->policy().changeNeverRule(localOwner, Action::t3_send, resource, false);
    store->policy().setMode(localOwner, "supervised");
    const auto origin = receive("supervised");
    runtime->runConversations();
    const auto item = store->inbox(localOwner).at(0);
    check(sends() == before && item.at("status") == "pending", "Supervised sent before approval");
    restart();
    const InboxTarget approval{item.at("botId"),  InboxKind::policy_approval, item.at("itemId"),          item.at("waitId"),
                               item.at("taskId"), item.at("targetRevision"),  item.at("targetGeneration")};
    store->decideInbox(localOwner, approval, "allow_once");
    runtime->runConversations();
    if (sends() != before + 1 || model.result.value("delivery", "") != "queued")
        throw std::runtime_error("Approved Session send did not queue: " + model.result.dump());
    const auto wire = effects("t3_thread_send").back();
    const auto requestId = wire.at("clientRequestId").get<std::string>();
    check(wire.at("mode") == "queue" && wire.at("threadId") == thread && requestId.starts_with("flock-turn-") &&
              wire.at("message") == sessionPrompt(requestId, "Ernest: Check the fixture output"),
          "Wrong Session send arguments");
    Command reconcile;
    reconcile.tool = "message_session";
    reconcile.environmentId = "env-1";
    reconcile.target.threadId = thread;
    reconcile.instruction = "Ernest: Check the fixture output";
    reconcile.actionId = requestId;
    const auto checkRecoveredSend = [&](std::string_view delivery) {
        const auto recovered = t3.reconcile(reconcile);
        check(recovered.kind == Delivery::Kind::Accepted && recovered.run.threadId == thread &&
                  recovered.run.runId == "bot-run-" + std::to_string(sends()) && sends() == before + 1 && recovered.delivery == delivery,
              "Session send reconciliation missed the exact Run or resent work");
    };
    fixture({{"reconcileSessionSend", true}});
    checkRecoveredSend("queued");
    fixture({{"botRunStatus", "running"}});
    checkRecoveredSend("started");
    fixture({{"directStopReadError", "run_not_found"}});
    check(t3.reconcile(reconcile).kind == Delivery::Kind::Unknown, "A missing Run incorrectly cleared a Session send with lost receipt");
    fixture({{"directStopReadError", "thread_not_found"}});
    check(t3.reconcile(reconcile).kind == Delivery::Kind::NotAccepted, "A confirmed deleted Session did not settle an unconfirmed send");
    fixture({{"directStopReadError", nullptr}});
    fixture({{"reconcileSessionSend", false}, {"botRunStatus", "queued"}});
    check(receive("supervised").at("messageId") == origin.at("messageId"), "Duplicate intake created another turn");
    fixture({{"personFinished", true}});
    runtime->runConversations();
    check(model.wakes.empty() && sends() == before + 1, "The person's completion woke the Bot or resent the step");
    const RunRef directRun{thread, "bot-run-" + std::to_string(sends()), {}, {}};
    const auto previousInterrupts = effects("t3_thread_interrupt").size();
    fixture({{"botRunStatus", "completed"}, {"botRunDelegatesPending", true}});
    check(t3.interruptSessionMessage("env-1", directRun) == InterruptOutcome::AlreadyTerminal &&
              effects("t3_thread_interrupt").size() == previousInterrupts,
          "A completed reply stop depended on the shared Session's delegated work");
    fixture({{"botRunDelegatesPending", false}, {"botRunTimedOut", true}});
    check(t3.interruptSessionMessage("env-1", directRun) == InterruptOutcome::NoActiveRun,
          "A completed reply stop settled while its restart continuation was pending");
    fixture({{"botRunTimedOut", false}, {"botRunStatus", "running"}, {"botRunContinuation", "continued-reply"}});
    check(t3.interruptSessionMessage("env-1", directRun) == InterruptOutcome::Requested &&
              effects("t3_thread_interrupt").size() == previousInterrupts + 1 &&
              effects("t3_thread_interrupt").back() == Json{{"threadId", thread}, {"runId", "continued-reply"}} &&
              t3.interruptSessionMessage("env-1", directRun) == InterruptOutcome::AlreadyTerminal,
          "Pause did not interrupt and settle the exact restart continuation");
    fixture({{"botRunContinuation", ""}});
    fixture({{"botRunStatus", "queued"}, {"differentWaitRun", true}});
    bool stopRejected = false;
    try {
        t3.interruptSessionMessage("env-1", directRun);
    } catch (const std::exception&) {
        stopRejected = true;
    }
    check(stopRejected, "A different Run's wait response settled a Session message interruption");
    runtime->runConversations();
    check(model.wakes.empty(), "A different Run's wait response woke the Bot");
    fixture({{"differentWaitRun", false}, {"directStopWaitError", "thread_not_found"}});
    const auto requireStopRetry = [&] {
        bool retry = false;
        try {
            t3.interruptSessionMessage("env-1", directRun);
        } catch (const std::exception&) {
            retry = true;
        }
        check(retry, "An unconfirmed missing Session incorrectly settled interruption");
    };
    requireStopRetry();
    fixture({{"directStopReadError", "capability_denied"}});
    requireStopRetry();
    fixture({{"directStopReadError", nullptr}, {"directStopWaitError", "run_not_found"}});
    requireStopRetry();
    fixture({{"directStopReadError", "thread_not_found"}});
    check(t3.interruptSessionMessage("env-1", directRun) == InterruptOutcome::TargetGone,
          "A missing Run in a confirmed deleted Session did not finish interruption");
    fixture({{"directStopWaitError", "invalid_request"},
             {"directStopReadError", "invalid_request"},
             {"directStopWaitErrorMessage", "The thread was not found."},
             {"directStopReadErrorMessage", "The thread was not found."}});
    check(t3.interruptSessionMessage("env-1", directRun) == InterruptOutcome::TargetGone,
          "A deleted Session reported by older T3 did not finish interruption");
    fixture({{"directStopWaitError", nullptr}, {"directStopReadError", nullptr}});
    fixture({{"differentWaitRun", false}, {"transientWakeFailure", true}});
    runtime->runConversations();
    check(model.wakes.empty(), "A transient failure consumed the Run wait");
    fixture({{"transientWakeFailure", false}, {"botRunStatus", "completed"}, {"botRunTimedOut", true}});
    runtime->runConversations();
    check(model.wakes.empty(), "A timed-out terminal response consumed the Run wait");
    restart();
    fixture({{"botRunTimedOut", false}, {"botRunContinuation", "continued-send"}});
    runtime->runConversations();
    check(model.wakes.size() == 1 && model.wakes.back().at("reason") == "finished" && model.reads.back().at("ok") == true,
          "Restart lost the Bot Run wake or its Session history");
    const auto timeline = store->conversation(intake.botId);
    check(std::ranges::any_of(timeline,
                              [&](const Json& event) {
                                  const auto wake = event.at("data").value("sessionWake", Json::object());
                                  return wake.value("reason", "") == "finished" && wake.value("threadId", "") == thread;
                              }),
          "The conversation timeline omitted the Session wake event");
    restart();
    runtime->runConversations();
    check(model.wakes.size() == 1, "Restart repeated an already delivered wake");

    store->policy().setMode(localOwner, "full");
    fixture({{"botRunStatus", "queued"}, {"botRunContinuation", ""}});
    receive("full");
    runtime->runConversations();
    check(sends() == before + 2 && model.result.at("ok") == true, "Full did not send");
    fixture({{"botRunStatus", "running"}, {"botQuestion", true}, {"botRunContinuation", "continued-with-question"}});
    runtime->runConversations();
    fixture({{"botRunStatus", "completed"}, {"botQuestion", false}, {"botRunContinuation", ""}});
    runtime->runConversations();
    check(model.wakes.size() == 2 && model.wakes.back().at("reason") == "waiting_for_person",
          "A pending user question did not wake exactly once");
    fixture({{"botRunStatus", "queued"}});
    receive("revoke");
    runtime->runConversations();
    check(sends() == before + 3, "The revocation test did not queue a send");
    T3Connections(*store).disconnect("env-1");
    restart();
    T3Connections(*store).connect("env-1", localOwner.id, 3210);
    fixture({{"botRunStatus", "completed"}});
    runtime->runConversations();
    check(model.wakes.size() == 2, "Reconnecting revived a wait retired after access loss");
    struct SlackTransport final : ChannelTransport {
        ChannelDelivery send(std::string_view, const ChannelOrigin&, std::string_view, const Json&) override {
            return {ChannelDelivery::State::accepted, "fixture-post"};
        }
        ChannelDelivery reconcile(std::string_view, const ChannelOrigin&) override { return {}; }
    };
    store->policy().setOwner(localOwner, intake.botId);
    store->connectChannel({ChannelKind::slack, channelCapabilities(ChannelKind::slack),
                           [](auto token, auto workspace) { return token == "fixture" && workspace == "test"; },
                           [&](auto, auto) -> std::optional<ChannelSender> { return ChannelSender{intake.botId, localOwner.id}; },
                           std::make_shared<SlackTransport>()});
    const auto slack = [&](std::string_view id) {
        store->receiveChannel(ChannelKind::slack, "fixture",
                              {{"workspaceId", "test"},
                               {"threadRef", "slack-thread"},
                               {"externalMessageId", id},
                               {"senderId", "sender"},
                               {"text", "Check staging."}},
                              [&](auto, auto) { return intake; });
        runtime->runConversations();
    };
    const auto send = model.next;
    model.next = {"flock_session_search", Json::object(), "Find unrelated Sessions"};
    slack("catalog");
    check(model.result.at("sessions").empty(), "The linked personal Bot exposed its T3 catalog to Slack");
    model.next = {
        "flock_session_events", {{"environmentId", "env-1"}, {"threadId", thread}, {"operation", "counts"}}, "Read an unrelated Session"};
    slack("history");
    check(model.result.at("ok") == false, "A channel turn read a Session outside the Slack thread");
    model.next = send;
    store->policy().setMode(localOwner, "supervised");
    fixture({{"botRunStatus", "queued"}});
    store->receiveChannel(ChannelKind::slack, "fixture",
                          {{"workspaceId", "test"},
                           {"threadRef", "slack-thread"},
                           {"externalMessageId", "slack-ask"},
                           {"senderId", "sender"},
                           {"text", "Send an update to T3."}},
                          [&](auto, auto) { return intake; });
    runtime->runConversations();
    const auto items = store->inbox(localOwner);
    const auto card = std::ranges::find_if(
        items, [](const Json& item) { return item.value("itemKind", "") == "policy_approval" && item.value("status", "") == "pending"; });
    check(sends() == before + 3 && card != items.end(), "The Slack send did not wait for policy approval");
    store->decideInbox(localOwner,
                       {card->at("botId"), InboxKind::policy_approval, card->at("itemId"), card->at("waitId"), card->at("taskId"),
                        card->at("targetRevision"), card->at("targetGeneration")},
                       "allow_once");
    runtime->runConversations();
    check(sends() == before + 4, "The approved Slack send did not queue");
    fixture({{"botRunStatus", "completed"}});
    runtime->runConversations();
    check(model.wakes.size() == 3, "The Slack send did not wake the Bot");
    check(model.wakes.back().at("title") == "" && model.reads.back().at("ok") == false,
          "A Slack send or wake gave the thread access to unrelated Session evidence");
    const auto channelTimeline = store->conversation(intake.botId);
    check(std::ranges::any_of(channelTimeline,
                              [](const Json& event) {
                                  const auto& data = event.at("data");
                                  return data.contains("sessionWake") &&
                                         data.value("channel", Json::object()).value("threadRef", "") == "slack-thread";
                              }),
          "The wake did not preserve its Slack scope");
    fixture({{"sessionWakeFixture", false}});
}
