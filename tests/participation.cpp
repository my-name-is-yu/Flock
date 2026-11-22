#include "flock/session_handoff.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <sqlite3.h>
#include <unistd.h>

using namespace flockv0;

namespace {
const Person owner{"owner",true}, visitor{"visitor"};
const ThreadTarget first{"desktop","project","first"}, second{"desktop","project","second"};
const auto resolve = [](std::string_view person) -> std::optional<Person> {
    return person == owner.id ? std::optional(owner) : std::nullopt;
};
const auto intake = [](std::string_view bot,std::string_view) { return Intake{std::string(bot),"","desktop","project"}; };
IntakeResolver intakeWithLimit(Id limit) {
    return [limit](std::string_view bot,std::string_view) {
        return Intake{std::string(bot),"","desktop","project",{{"modelCallLimit",limit}}};
    };
}
const auto soul = [](std::string_view) { return std::string("Follow the authorized goal."); };

void check(bool pass, std::string_view message) {
    if (!pass) throw std::runtime_error(std::string(message));
}
template<class Call> void denied(Call call) {
    try { call(); }
    catch (const Conflict&) { return; }
    throw std::runtime_error("Operation should have been denied");
}

class Sessions final : public ThreadSession {
public:
    struct Sent { ThreadTarget target; std::string id, text; };
    std::map<std::string,std::vector<ThreadEvent>> history{{first.threadId,{}},{second.threadId,{}}};
    std::vector<Sent> sent;
    std::function<void()> beforeSend;
    std::string auth = "auth";
    bool uncertain = false, dispatchUnavailable = false;
    std::optional<ThreadPage> read(const ThreadTarget& target, std::optional<Id> after) override {
        if (target != first && target != second) return {};
        const auto& all = history.at(target.threadId);
        std::vector<ThreadEvent> events;
        auto cursor = after.value_or(0);
        for (const auto& event : all) {
            if (after && event.position <= *after) continue;
            if (event.kind == "assistant_message" && event.status != "completed") break;
            cursor = event.position;
            events.push_back(event);
            if (events.size() == 20) break;
        }
        return ThreadPage{target,cursor,auth,"",events};
    }
    std::optional<ThreadPage> dispatchHistory(const ThreadTarget& target, Id after) override {
        if (dispatchUnavailable || (target != first && target != second)) return {};
        const auto& all = history.at(target.threadId);
        std::vector<ThreadEvent> events;
        auto cursor = after;
        for (const auto& event : all) if (event.position > after) {
            cursor = event.position;
            events.push_back(event);
        }
        return ThreadPage{target,cursor,auth,"",events};
    }
    ThreadSend send(const ThreadTarget& target, std::string_view generation, std::string_view id,
                    std::string_view text) override {
        check(generation == auth,"changed authentication entered the send port");
        sent.push_back({target,std::string(id),std::string(text)});
        if (beforeSend) beforeSend();
        return uncertain ? ThreadSend{} : ThreadSend{ThreadSend::Accepted,ThreadReceipt{std::string(id),"new-run","queued"}};
    }
    std::optional<std::string> inputCommit(const ThreadTarget&) override { return std::nullopt; }
    void event(const ThreadTarget& target, std::string kind, std::string by, std::string source,
               std::string message, std::string text, std::string status = "completed") {
        auto& rows = history[target.threadId];
        const auto position = static_cast<Id>(rows.size() + 1);
        rows.push_back({target.threadId + ":" + std::to_string(position),std::move(message),"new-run",
                        std::move(kind),std::move(status),std::move(by),std::move(text),position,std::move(source)});
    }
    void consumed(const Sent& input) {
        event(input.target,"user_message","agent","mcp",input.id,input.text);
        event(input.target,"assistant_message","agent","provider","","Performed the requested step.");
    }
};

class Bot final : public Model {
public:
    std::vector<std::string> decision;
    std::vector<ModelInput> seen;
    std::function<void()> beforeDecision;
    std::function<ModelTurn(const ModelInput&)> response;
    ModelTurn decide(const ModelInput& input) override {
        seen.push_back(input);
        if (beforeDecision) beforeDecision();
        if (response) return response(input);
        if (decision.empty()) throw std::runtime_error("Unexpected second model decision");
        auto next = decision.front();
        decision.erase(decision.begin());
        return {{{next,{{"text","Test output from Flock"}},""}}};
    }
};

void access(Store& store, const ThreadTarget& target) {
    for (const auto& scope : {Scope::consultReply(target.environmentId,target.threadId),
                             Scope::sessionRead(target.environmentId,target.threadId),
                             Scope::sessionCoordinate(target.environmentId,target.threadId)})
        store.access().resourceGrant(owner,"planner",scope,GrantChange::grant);
}

void pairingAndRecovery(const std::filesystem::path& path) {
    Sessions sessions;
    Id lost;
    {
        Store store(path);
        access(store,first);
        access(store,second);
        SessionHandoff handoff(store,sessions,resolve,intake);
        denied([&] { handoff.pair(visitor,first,"planner",{}); });
        denied([&] { handoff.pair(owner,{"desktop","other","first"},"planner",{}); });
        denied([&] { handoff.intake("unpaired","client-request","Run a check"); });

        const auto left = handoff.pair(owner,first,"planner",{});
        const auto right = handoff.pair(owner,second,"planner",{});
        const auto a = handoff.intake(left.code,"client-request","Check first.");
        const auto retry = handoff.intake(left.code,"client-request","Check first.");
        const auto b = handoff.intake(right.code,"client-request","Check second.");
        check(a.channel == retry.channel && a.requestId == retry.requestId && b.channel != a.channel,
              "pairing was duplicated or conversations were combined");
        denied([&] { handoff.intake(left.code,"client-request","Changed request."); });
        denied([&] { handoff.view(visitor,a.channel); });
        denied([&] { handoff.resume(owner,a.channel); });

        Bot bot;
        bot.decision = {"reply","reply"};
        sessions.uncertain = true;
        handoff.consult(owner,a.channel,bot,soul);
        sessions.uncertain = false;
        handoff.consult(owner,b.channel,bot,soul);
        check(sessions.sent.size() == 2 && sessions.sent[0].target == first && sessions.sent[1].target == second,
              "consultations lost their immutable return Thread");
        check(handoff.view(owner,a.channel).deliveries.front().state == RootDeliveryState::Unknown,
              "ambiguous send was presented as accepted");
        check(handoff.view(owner,b.channel).deliveries.front().state == RootDeliveryState::Queued,
              "accepted queue was presented as observed");
        lost = a.channel;
    }
    Store restarted(path);
    SessionHandoff handoff(restarted,sessions,resolve,intake);
    handoff.recover();
    Bot bot;
    handoff.consult(owner,lost,bot,soul);
    check(bot.seen.empty() && sessions.sent.size() == 2,"restart blindly repeated an uncertain reply");
    sessions.consumed(sessions.sent.front());
    handoff.consult(owner,lost,bot,soul);
    const auto reconciled = handoff.view(owner,lost);
    check(reconciled.state == "completed" && reconciled.deliveries.front().state == RootDeliveryState::Observed &&
          reconciled.deliveries.front().observedRunId == "new-run","history did not reconcile delivery in a later Run");
}

void reconcileAfterReceiptWriteContention(const std::filesystem::path& path) {
    Store store(path);
    Sessions sessions;
    SessionHandoff handoff(store,sessions,resolve,intake);
    access(store,first);
    const auto ticket = handoff.pair(owner,first,"planner",{});
    const auto channel = handoff.intake(ticket.code,"receipt-contention","Check the fixture.").channel;
    sqlite3* connection = nullptr;
    const auto opened = sqlite3_open(path.c_str(),&connection);
    const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> writer(connection,sqlite3_close);
    check(opened == SQLITE_OK,"Could not open the fixture writer");
    sessions.beforeSend = [&] {
        sessions.consumed(sessions.sent.back());
        check(sqlite3_exec(writer.get(),"BEGIN IMMEDIATE",nullptr,nullptr,nullptr) == SQLITE_OK,
              "Could not acquire the fixture write lock");
    };
    Bot bot;
    bot.decision = {"reply"};
    bool contended = false;
    try { handoff.consult(owner,channel,bot,soul); }
    catch (const std::runtime_error& error) {
        check(std::string_view(error.what()) == "database is locked","Unexpected receipt write error");
        contended = true;
    }
    check(sqlite3_exec(writer.get(),"ROLLBACK",nullptr,nullptr,nullptr) == SQLITE_OK && contended,
          "The accepted send did not contend at receipt storage");
    handoff.consult(owner,channel,bot,soul);
    const auto view = handoff.view(owner,channel);
    check(view.state == "completed" && view.deliveries.front().state == RootDeliveryState::Observed &&
          sessions.sent.size() == 1 && bot.seen.size() == 1,
          "The live worker did not reconcile the accepted send without replay");
}

void coordinate(const std::filesystem::path& path) {
    Store store(path);
    Sessions sessions;
    SessionHandoff handoff(store,sessions,resolve,intake);
    access(store,first);
    const CoordinationRequest goal{"Run two checks and report",false};
    const auto ticket = handoff.pair(owner,first,"planner",goal);
    const auto channel = handoff.intake(ticket.code,"coordinate-once","Start the checks.").channel;
    const auto duplicate = handoff.pair(owner,first,"planner",goal);
    denied([&] { handoff.intake(duplicate.code,"another","Start over."); });
    Bot bot;
    bot.decision = {"root_guidance","root_guidance","handoff_question","complete_handoff"};
    handoff.step(owner,channel,bot,soul);
    check(sessions.sent.size() == 1,"first autonomous input was not sent");
    const auto firstSend = sessions.sent.front();
    sessions.event(first,"user_message","agent","mcp",firstSend.id,firstSend.text);
    handoff.step(owner,channel,bot,soul);
    check(bot.seen.size() == 1 && sessions.sent.size() == 1,"own input triggered another decision");
    sessions.event(first,"assistant_message","agent","provider","","Finished step one.");
    handoff.step(owner,channel,bot,soul);
    check(sessions.sent.size() == 2,"agent progress did not trigger step two");
    sessions.consumed(sessions.sent.back());
    auto question = handoff.step(owner,channel,bot,soul);
    check(!question.pendingQuestion.is_null(),"human question was not saved");
    handoff.step(owner,channel,bot,soul);
    check(bot.seen.size() == 3,"coordination continued through a human decision");
    handoff.answer(owner,channel,question.generation,question.pendingQuestion.at("eventId").get<std::string>(),"Finish.");
    const auto done = handoff.step(owner,channel,bot,soul);
    check(done.state == "completed" && sessions.sent.size() == 2,"completion created another input");

    const auto fresh = handoff.pair(owner,first,"planner",goal);
    const auto stopped = handoff.intake(fresh.code,"stop-test","Check again.").channel;
    sessions.beforeSend = [&] { handoff.pause(owner,stopped); };
    bot.decision = {"root_guidance","root_guidance"};
    const auto paused = handoff.step(owner,stopped,bot,soul);
    check(paused.state == "paused" && paused.residualCommandIds.size() == 1,"pause lost an admitted residual");
    denied([&]{ handoff.resume(owner,stopped); });
    sessions.consumed(sessions.sent.back());
    const auto resumed = handoff.resume(owner,stopped);
    check(resumed.state == "active" && resumed.generation == paused.generation,"resume changed the wrong generation");
    handoff.freeze(owner,stopped);
    const auto halted = handoff.step(owner,stopped,bot,soul);
    check(halted.state == "stopped" && sessions.sent.size() == 3,"stop admitted a new effect");
}
void concurrentHumanAndBudget(const std::filesystem::path& path) {
    Store store(path);
    Sessions sessions;
    const auto routing = intakeWithLimit(30);
    SessionHandoff handoff(store,sessions,resolve,routing);
    access(store,first);
    const auto pairing = handoff.pair(owner,first,"planner",CoordinationRequest{"Observe progress only",false});
    const auto channel = handoff.intake(pairing.code,"request","Begin.").channel;
    Bot bot;
    bot.decision = {"root_guidance"};
    bot.beforeDecision = [&] {
        sessions.event(first,"user_message","user","web","","New priority from the person.");
    };
    auto changed = handoff.step(owner,channel,bot,soul);
    check(sessions.sent.empty() && changed.deliveries.front().state == RootDeliveryState::Rejected,
          "coordinator sent guidance from before a newer human message");
    bot.beforeDecision = {};
    bot.decision.assign(23,"wait_for_root");
    handoff.step(owner,channel,bot,soul);
    check(bot.seen.back().event.at("progress").dump().find("New priority") != std::string::npos,
          "coordinator did not re-evaluate newer human input");
    for (int i = 0; i < 22; ++i) {
        sessions.event(first,"assistant_message","agent","provider","","Progress update.");
        handoff.step(owner,channel,bot,soul);
    }
    const auto stopped = handoff.step(owner,channel,bot,soul);
    check(stopped.state == "budget_exhausted" && bot.seen.size() == 24,
          "the cumulative planning budget did not stop further actions");

    const auto consult = handoff.pair(owner,first,"planner",{});
    const auto request = handoff.intake(consult.code,"request","Reply once.").channel;
    store.access().resourceGrant(owner,"planner",Scope::consultReply(first.environmentId,first.threadId),GrantChange::revoke);
    denied([&] { handoff.consult(owner,request,bot,soul); });
    check(handoff.freeze(owner,request).state == "stopped" && sessions.sent.empty(),
          "revoking the Bot's scope removed the owner's ability to stop the request");
}

void dispatchHistoryCoverage(const std::filesystem::path& path) {
    const auto run = [&](std::string name, const std::function<void(Sessions&)>& populate,
                         bool available, RootDeliveryState expected, bool sent) {
        Store store(path/(name+".sqlite3"));
        Sessions sessions;
        sessions.dispatchUnavailable = !available;
        SessionHandoff handoff(store,sessions,resolve,intake);
        access(store,first);
        const auto pairing = handoff.pair(owner,first,"planner",CoordinationRequest{"Check progress",false});
        const auto channel = handoff.intake(pairing.code,name,"Recheck before dispatch.").channel;
        populate(sessions);
        Bot bot;
        bot.decision = {"root_guidance"};
        const auto view = handoff.step(owner,channel,bot,soul);
        check(sessions.sent.size()==static_cast<std::size_t>(sent) && !view.deliveries.empty() &&
              view.deliveries.front().state==expected,"dispatch did not honor the complete-history result");
    };
    run("dispatch-later-human",[](Sessions& sessions) {
        for (int i=0;i<24;++i)
            sessions.event(first,"assistant_message","agent","provider","","Progress "+std::to_string(i));
        sessions.event(first,"user_message","user","web","","A newer human priority.");
    },true,RootDeliveryState::Rejected,false);
    run("dispatch-human-behind-stream",[](Sessions& sessions) {
        sessions.event(first,"assistant_message","agent","provider","","Still streaming.","running");
        sessions.event(first,"user_message","user","web","","A human message after the stream.");
    },true,RootDeliveryState::Rejected,false);
    run("dispatch-incomplete",[](Sessions& sessions) {
        sessions.event(first,"assistant_message","agent","provider","","Complete item.");
    },false,RootDeliveryState::Prepared,false);
    run("dispatch-complete",[](Sessions& sessions) {
        for (int i=0;i<24;++i)
            sessions.event(first,"assistant_message","agent","provider","","Completed progress "+std::to_string(i));
    },true,RootDeliveryState::Queued,true);
}

void lateRedactionProjection(const std::filesystem::path& path) {
    Store store(path/"late-redaction.sqlite3");
    Sessions sessions;
    access(store,first);
    SessionHandoff handoff(store,sessions,resolve,intake);
    const std::string secret="late-secret-value-007";
    const auto pairing=handoff.pair(owner,first,"planner",CoordinationRequest{"Goal "+secret,false});
    const auto channel=handoff.intake(pairing.code,"opaque-request-id","Request "+secret).channel;
    Bot bot;
    bot.response=[&](const ModelInput&) {
        ModelTurn turn;
        turn.calls.push_back({"handoff_question",{{"text","Question "+secret}},""});
        return turn;
    };
    handoff.step(owner,channel,bot,soul);
    const auto secretRequest=store.access().requestSecret("planner",{"late-secret"});
    store.access().submitSecret(owner,"planner",secretRequest,secret);
    const auto view=handoff.view(owner,channel);
    const auto recent=handoff.recent(owner);
    check(view.latestDecision.dump().find(secret)==std::string::npos,
          "late-registered secret remained in the persisted decision projection");
    check(view.pendingQuestion.dump().find(secret)==std::string::npos,
          "late-registered secret remained in the pending question projection");
    check(view.permission->goal.find(secret)==std::string::npos,
          "late-registered secret remained in the goal projection");
    const auto opaqueId=view.origin.invitation.requestId;
    check(recent.size()==1 && recent.front().latestDecision.dump().find(secret)==std::string::npos &&
          recent.front().pendingQuestion.dump().find(secret)==std::string::npos &&
          recent.front().permission->goal.find(secret)==std::string::npos &&
          !opaqueId.empty() && view.origin.invitation.requestId==opaqueId &&
          recent.front().origin.invitation.requestId==opaqueId,
          "recent projection exposed a late secret or changed an opaque ID");
}

void modelAttemptBudget(const std::filesystem::path& path, bool coordination, bool throws) {
    Sessions sessions;
    const auto routing = intakeWithLimit(2);
    Bot bot;
    bot.response = [throws](const ModelInput&) {
        if (throws) throw std::runtime_error("scripted model failure");
        ModelTurn turn;
        turn.calls.push_back({"invalid_action",Json::object(),{}});
        return turn;
    };
    Id channel = 0;
    {
        Store store(path);
        access(store,first);
        SessionHandoff handoff(store,sessions,resolve,routing);
        std::optional<CoordinationRequest> permission;
        if (coordination) permission=CoordinationRequest{"Bounded decision attempts",false};
        const auto pairing = handoff.pair(owner,first,"planner",permission);
        channel = handoff.intake(pairing.code,"bounded-model-attempts","Try this request.").channel;
        for (std::size_t call=1;call<=2;++call) {
            bool failed = false;
            try {
                if (coordination) handoff.step(owner,channel,bot,soul);
                else handoff.consult(owner,channel,bot,soul);
            } catch (const std::exception&) { failed = true; }
            check(failed && bot.seen.size()==call && bot.seen.back().config.at("modelCallLimit")==2 &&
                  handoff.view(owner,channel).state=="active",
                  "an invalid or throwing decision did not consume one configured attempt");
        }
    }
    Store restarted(path);
    SessionHandoff handoff(restarted,sessions,resolve,routing);
    if (coordination) handoff.step(owner,channel,bot,soul);
    else handoff.consult(owner,channel,bot,soul);
    check(handoff.view(owner,channel).state=="budget_exhausted" && bot.seen.size()==2,
          "the exhausted session budget was not persisted across restart");
    if (coordination) handoff.step(owner,channel,bot,soul);
    else handoff.consult(owner,channel,bot,soul);
    check(bot.seen.size()==2,"an exhausted handoff called the model again");
}

}

int main() {
    char name[] = "/tmp/flock-participation.XXXXXX";
    const auto* dir = mkdtemp(name);
    if (!dir) return 1;
    const std::filesystem::path path(dir);
    try {
        pairingAndRecovery(path / "pairing.sqlite3");
        reconcileAfterReceiptWriteContention(path / "receipt-contention.sqlite3");
        coordinate(path / "coordination.sqlite3");
        concurrentHumanAndBudget(path / "human-policy.sqlite3");
        dispatchHistoryCoverage(path);
        lateRedactionProjection(path);
        std::size_t attemptCase = 0;
        for (const auto coordination : {false,true})
            for (const auto throws : {false,true})
                modelAttemptBudget(path / ("model-budget-" + std::to_string(attemptCase++) + ".sqlite3"),
                                   coordination,throws);
        std::filesystem::remove_all(path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
