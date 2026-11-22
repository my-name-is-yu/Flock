#include "flock/session_handoff.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <unistd.h>

using namespace flockv0;

namespace {
const Person owner{"owner",true}, visitor{"visitor"};
const ThreadTarget first{"desktop","project","first"}, second{"desktop","project","second"};
const auto resolve = [](std::string_view person) -> std::optional<Person> {
    return person == owner.id ? std::optional(owner) : std::nullopt;
};
const auto intake = [](std::string_view bot,std::string_view) { return Intake{std::string(bot),"","desktop","project"}; };
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
    bool uncertain = false;
    std::optional<ThreadPage> read(const ThreadTarget& target, std::optional<Id> after) override {
        if (target != first && target != second) return {};
        const auto& all = history.at(target.threadId);
        std::vector<ThreadEvent> events;
        for (const auto& event : all) if (!after || event.position > *after) events.push_back(event);
        return ThreadPage{target,static_cast<Id>(all.size()),auth,"",events};
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
               std::string message, std::string text) {
        auto& rows = history[target.threadId];
        const auto position = static_cast<Id>(rows.size() + 1);
        rows.push_back({target.threadId + ":" + std::to_string(position),std::move(message),"new-run",
                        std::move(kind),"completed",std::move(by),std::move(text),position,std::move(source)});
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
    ModelTurn decide(const ModelInput& input) override {
        seen.push_back(input);
        if (beforeDecision) beforeDecision();
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
        SessionHandoff handoff(store,sessions,resolve);
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
        handoff.consult(owner,a.channel,bot,intake,soul);
        sessions.uncertain = false;
        handoff.consult(owner,b.channel,bot,intake,soul);
        check(sessions.sent.size() == 2 && sessions.sent[0].target == first && sessions.sent[1].target == second,
              "consultations lost their immutable return Thread");
        check(handoff.view(owner,a.channel).deliveries.front().state == RootDeliveryState::Unknown,
              "ambiguous send was presented as accepted");
        check(handoff.view(owner,b.channel).deliveries.front().state == RootDeliveryState::Queued,
              "accepted queue was presented as observed");
        lost = a.channel;
    }
    Store restarted(path);
    SessionHandoff handoff(restarted,sessions,resolve);
    handoff.recover();
    Bot bot;
    handoff.consult(owner,lost,bot,intake,soul);
    check(bot.seen.empty() && sessions.sent.size() == 2,"restart blindly repeated an uncertain reply");
    sessions.consumed(sessions.sent.front());
    handoff.consult(owner,lost,bot,intake,soul);
    const auto reconciled = handoff.view(owner,lost);
    check(reconciled.state == "completed" && reconciled.deliveries.front().state == RootDeliveryState::Observed &&
          reconciled.deliveries.front().observedRunId == "new-run","history did not reconcile delivery in a later Run");
}

void coordinate(const std::filesystem::path& path) {
    Store store(path);
    Sessions sessions;
    SessionHandoff handoff(store,sessions,resolve);
    access(store,first);
    const CoordinationRequest goal{"Run two checks and report",false};
    const auto ticket = handoff.pair(owner,first,"planner",goal);
    const auto channel = handoff.intake(ticket.code,"coordinate-once","Start the checks.").channel;
    const auto duplicate = handoff.pair(owner,first,"planner",goal);
    denied([&] { handoff.intake(duplicate.code,"another","Start over."); });
    Bot bot;
    bot.decision = {"root_guidance","root_guidance","handoff_question","complete_handoff"};
    handoff.step(owner,channel,bot,intake,soul);
    check(sessions.sent.size() == 1,"first autonomous input was not sent");
    const auto firstSend = sessions.sent.front();
    sessions.event(first,"user_message","agent","mcp",firstSend.id,firstSend.text);
    handoff.step(owner,channel,bot,intake,soul);
    check(bot.seen.size() == 1 && sessions.sent.size() == 1,"own input triggered another decision");
    sessions.event(first,"assistant_message","agent","provider","","Finished step one.");
    handoff.step(owner,channel,bot,intake,soul);
    check(sessions.sent.size() == 2,"agent progress did not trigger step two");
    sessions.consumed(sessions.sent.back());
    auto question = handoff.step(owner,channel,bot,intake,soul);
    check(!question.pendingQuestion.is_null(),"human question was not saved");
    handoff.step(owner,channel,bot,intake,soul);
    check(bot.seen.size() == 3,"coordination continued through a human decision");
    handoff.answer(owner,channel,question.generation,question.pendingQuestion.at("eventId").get<std::string>(),"Finish.");
    const auto done = handoff.step(owner,channel,bot,intake,soul);
    check(done.state == "completed" && sessions.sent.size() == 2,"completion created another input");

    const auto fresh = handoff.pair(owner,first,"planner",goal);
    const auto stopped = handoff.intake(fresh.code,"stop-test","Check again.").channel;
    sessions.beforeSend = [&] { handoff.pause(owner,stopped); };
    bot.decision = {"root_guidance","root_guidance"};
    const auto paused = handoff.step(owner,stopped,bot,intake,soul);
    check(paused.state == "paused" && paused.residualCommandIds.size() == 1,"pause lost an admitted residual");
    denied([&]{ handoff.resume(owner,stopped); });
    sessions.consumed(sessions.sent.back());
    const auto resumed = handoff.resume(owner,stopped);
    check(resumed.state == "active" && resumed.generation == paused.generation,"resume changed the wrong generation");
    handoff.freeze(owner,stopped);
    const auto halted = handoff.step(owner,stopped,bot,intake,soul);
    check(halted.state == "stopped" && sessions.sent.size() == 3,"stop admitted a new effect");
}
void concurrentHumanAndBudget(const std::filesystem::path& path) {
    Store store(path);
    Sessions sessions;
    SessionHandoff handoff(store,sessions,resolve);
    access(store,first);
    const auto pairing = handoff.pair(owner,first,"planner",CoordinationRequest{"Observe progress only",false});
    const auto channel = handoff.intake(pairing.code,"request","Begin.").channel;
    Bot bot;
    bot.decision = {"root_guidance"};
    bot.beforeDecision = [&] {
        sessions.event(first,"user_message","user","web","","New priority from the person.");
    };
    auto changed = handoff.step(owner,channel,bot,intake,soul);
    check(sessions.sent.empty() && changed.deliveries.front().state == RootDeliveryState::Rejected,
          "coordinator sent guidance from before a newer human message");
    bot.beforeDecision = {};
    bot.decision.assign(23,"wait_for_root");
    handoff.step(owner,channel,bot,intake,soul);
    check(bot.seen.back().event.at("progress").dump().find("New priority") != std::string::npos,
          "coordinator did not re-evaluate newer human input");
    for (int i = 0; i < 22; ++i) {
        sessions.event(first,"assistant_message","agent","provider","","Progress update.");
        handoff.step(owner,channel,bot,intake,soul);
    }
    const auto stopped = handoff.step(owner,channel,bot,intake,soul);
    check(stopped.state == "budget_exhausted" && bot.seen.size() == 24,
          "the cumulative planning budget did not stop further actions");

    const auto consult = handoff.pair(owner,first,"planner",{});
    const auto request = handoff.intake(consult.code,"request","Reply once.").channel;
    store.access().resourceGrant(owner,"planner",Scope::consultReply(first.environmentId,first.threadId),GrantChange::revoke);
    denied([&] { handoff.consult(owner,request,bot,intake,soul); });
    check(handoff.freeze(owner,request).state == "stopped" && sessions.sent.empty(),
          "revoking the Bot's scope removed the owner's ability to stop the request");
}

}

int main() {
    char name[] = "/tmp/flock-participation.XXXXXX";
    const auto* dir = mkdtemp(name);
    if (!dir) return 1;
    const std::filesystem::path path(dir);
    try {
        pairingAndRecovery(path / "pairing.sqlite3");
        coordinate(path / "coordination.sqlite3");
        concurrentHumanAndBudget(path / "human-policy.sqlite3");
        std::filesystem::remove_all(path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
