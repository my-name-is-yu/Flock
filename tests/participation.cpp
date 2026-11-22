#include "flock/session_handoff.hpp"
#include "flock/skill_export.hpp"
#include <fstream>

#include <filesystem>
#include <iostream>
#include <map>
#include <unistd.h>

using namespace flockv0;

namespace {
const Person owner{"owner",true}, visitor{"visitor"};
const ThreadTarget first{"desktop","project","first"}, second{"desktop","project","second"};
const auto resolve = [](std::string_view person) -> std::optional<Person> {
    return person == owner.id ? std::optional(owner) : person == visitor.id ? std::optional(visitor) : std::nullopt;
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
    std::function<void()> beforeProof;
    std::optional<ThreadProof> proveInvocation(std::string_view environment, std::string_view thread,
                                               const Json&, std::chrono::steady_clock::time_point) override {
        if (beforeProof) beforeProof();
        for (auto* target : {&first,&second})
            if (target->environmentId == environment && target->threadId == thread)
                return ThreadProof{*target,static_cast<Id>(history.at(target->threadId).size()),auth,"run","tool"};
        return {};
    }
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

T3Connection connect(Store& store, const Person& person = owner, T3ConnectionMode mode = T3ConnectionMode::Full) {
    T3Connections connections(store);
    return *connections.authenticate(connections.connect(first.environmentId,person.id,mode,4173));
}

HandoffAck invoke(SessionHandoff& handoff, const T3Connection& connection, const ThreadTarget& target,
                  std::string_view requestId, std::string_view text, std::string_view goal = {}) {
    Json arguments{{"threadId",target.threadId},{"botId","planner"},{"requestId",requestId},{"text",text}};
    if (!goal.empty()) {
        arguments["mode"] = "coordinate";
        arguments["goal"] = goal;
    }
    return handoff.invoke(connection,arguments,[](std::string_view bot) { return bot == "planner"; },{});
}

void invocationAndRecovery(const std::filesystem::path& path) {
    Sessions sessions;
    Id lost;
    {
        Store store(path);
        SessionHandoff handoff(store,sessions,resolve);
        const auto visitorConnection = connect(store,visitor);
        store.access().personGrant(owner,"planner",visitor.id,Role::view,GrantChange::grant);
        denied([&] { invoke(handoff,visitorConnection,first,"request","Run a check"); });
        store.access().personGrant(owner,"planner",visitor.id,Role::decide,GrantChange::grant);
        sessions.beforeProof = [&] {
            store.access().personGrant(owner,"planner",visitor.id,Role::decide,GrantChange::revoke);
        };
        denied([&] { invoke(handoff,visitorConnection,first,"request","Recheck my access"); });
        sessions.beforeProof = {};
        const auto consult = connect(store,owner,T3ConnectionMode::Consult);
        denied([&] { invoke(handoff,consult,first,"request","Run a check","Coordinate"); });
        const auto connection = connect(store);
        denied([&] { invoke(handoff,consult,first,"request","Old token"); });
        denied([&] { invoke(handoff,connection,{"desktop","project","missing"},"request","Run a check"); });

        const auto a = invoke(handoff,connection,first,"client-request","Check first.");
        const auto retry = invoke(handoff,connection,first,"client-request","Check first.");
        const auto b = invoke(handoff,connection,second,"other-request","Check second.");
        check(a.channel == retry.channel && a.requestId == retry.requestId && b.channel != a.channel,
              "invocation retry or routing changed the return channel");
        denied([&] { invoke(handoff,connection,first,"client-request","Changed request."); });
        denied([&] { invoke(handoff,connection,second,"client-request","Check first."); });
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
    const auto connection = connect(store);
    constexpr auto goal = "Run two checks and report";
    const auto channel = invoke(handoff,connection,first,"coordinate-once","Start the checks.",goal).channel;
    denied([&] { invoke(handoff,connection,first,"another","Start over.",goal); });
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

    const auto stopped = invoke(handoff,connection,first,"stop-test","Check again.",goal).channel;
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
    const auto routing = intakeWithLimit(30);
    const auto connection = connect(store);
    const auto channel = invoke(handoff,connection,first,"coordinate","Begin.","Observe progress only").channel;
    Bot bot;
    bot.decision = {"root_guidance"};
    bot.beforeDecision = [&] {
        sessions.event(first,"user_message","user","web","","New priority from the person.");
    };
    auto changed = handoff.step(owner,channel,bot,routing,soul);
    check(sessions.sent.empty() && changed.deliveries.front().state == RootDeliveryState::Rejected,
          "coordinator sent guidance from before a newer human message");
    bot.beforeDecision = {};
    bot.decision.assign(23,"wait_for_root");
    handoff.step(owner,channel,bot,routing,soul);
    check(bot.seen.back().event.at("progress").dump().find("New priority") != std::string::npos,
          "coordinator did not re-evaluate newer human input");
    for (int i = 0; i < 22; ++i) {
        sessions.event(first,"assistant_message","agent","provider","","Progress update.");
        handoff.step(owner,channel,bot,routing,soul);
    }
    const auto stopped = handoff.step(owner,channel,bot,routing,soul);
    check(stopped.state == "budget_exhausted" && bot.seen.size() == 24,
          "the cumulative planning budget did not stop further actions");

    const auto request = invoke(handoff,connection,first,"request","Reply once.").channel;
    T3Connections(store).disconnect(first.environmentId);
    denied([&] { handoff.consult(owner,request,bot,routing,soul); });
    check(handoff.freeze(owner,request).state == "stopped" && sessions.sent.empty(),
          "revoking the connection removed the owner's ability to stop the request");
}

void dispatchHistoryCoverage(const std::filesystem::path& path) {
    const auto run = [&](std::string name, const std::function<void(Sessions&)>& populate,
                         bool available, RootDeliveryState expected, bool sent) {
        Store store(path/(name+".sqlite3"));
        Sessions sessions;
        sessions.dispatchUnavailable = !available;
        SessionHandoff handoff(store,sessions,resolve);
        const auto connection = connect(store);
        const auto channel = invoke(handoff,connection,first,name,"Recheck before dispatch.","Check progress").channel;
        populate(sessions);
        Bot bot;
        bot.decision = {"root_guidance"};
        const auto view = handoff.step(owner,channel,bot,intake,soul);
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
    const auto connection = connect(store);
    SessionHandoff handoff(store,sessions,resolve);
    const std::string secret="late-secret-value-007";
    const auto channel = invoke(handoff,connection,first,"opaque-request-id","Request "+secret,"Goal "+secret).channel;
    Bot bot;
    bot.response=[&](const ModelInput&) {
        ModelTurn turn;
        turn.calls.push_back({"handoff_question",{{"text","Question "+secret}},""});
        return turn;
    };
    handoff.step(owner,channel,bot,intake,soul);
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
        const auto connection = connect(store);
        SessionHandoff handoff(store,sessions,resolve);
        channel = invoke(handoff,connection,first,"bounded-model-attempts","Try this request.",
                         coordination ? "Bounded decision attempts" : "").channel;
        for (std::size_t call=1;call<=2;++call) {
            bool failed = false;
            try {
                if (coordination) handoff.step(owner,channel,bot,routing,soul);
                else handoff.consult(owner,channel,bot,routing,soul);
            } catch (const std::exception&) { failed = true; }
            check(failed && bot.seen.size()==call && bot.seen.back().config.at("modelCallLimit")==2 &&
                  handoff.view(owner,channel).state=="active",
                  "an invalid or throwing decision did not consume one configured attempt");
        }
    }
    Store restarted(path);
    SessionHandoff handoff(restarted,sessions,resolve);
    if (coordination) handoff.step(owner,channel,bot,routing,soul);
    else handoff.consult(owner,channel,bot,routing,soul);
    check(handoff.view(owner,channel).state=="budget_exhausted" && bot.seen.size()==2,
          "the exhausted session budget was not persisted across restart");
    if (coordination) handoff.step(owner,channel,bot,routing,soul);
    else handoff.consult(owner,channel,bot,routing,soul);
    check(bot.seen.size()==2,"an exhausted handoff called the model again");
}

void originMatching() {
    const Json args{{"threadId","first"},{"botId","planner"},{"requestId","sample"},{"text","Review"}};
    const auto fixture = [&](std::string_view tool, const Json& input) {
        return Json{{"type","dynamic_tool"},{"visibility","local"},{"sourceThreadId","first"},
                    {"runId","active"},{"textTruncated",false},
                    {"text",Json{{"toolName",tool},{"input",input}}.dump()}};
    };
    const auto match = [&](const Json& item) { return matchesThreadInvocation(item,"first","active",args); };
    check(match(fixture("flock.invoke_flock_bot",args)), "Codex running call did not match");
    check(match(fixture("mcp__flock__invoke_flock_bot",args)), "Claude running call did not match");
    check(match(fixture("invoke_flock_bot",args.dump())), "JSON-string arguments did not match");
    check(!match(fixture("invoke_flock_bot",Json::array())), "array arguments matched");
    check(!match(fixture("invoke_flock_bot","{bad")), "invalid JSON arguments matched");
    auto changed = args;
    changed["mode"] = "consult";
    check(!match(fixture("invoke_flock_bot",changed)), "different argument presence matched");
    check(!match(fixture("service.other_tool",args)), "different tool matched");
    const auto item = fixture("flock.invoke_flock_bot",args);
    check(!matchesThreadInvocation(item,"first","",args), "finished run matched");
    for (const auto& diff : {Json{{"visibility","inherited"}},Json{{"visibility","synthetic"}},
                            Json{{"sourceThreadId","second"}},Json{{"runId","old"}},
                            Json{{"textTruncated",true}},Json{{"text",28}},Json{{"text","{"}}}) {
        auto wrong = item;
        wrong.update(diff);
        check(!match(wrong), "invalid activity matched");
    }
}

void skillExport(const std::filesystem::path& root) {
    const auto bots = Json::array({
        {{"id","A___B."},{"name","設計レビュー"},{"role","reviewer"},{"private","secret settings"}},
        {{"id","a.b"},{"name","Other"}},
        {{"id",std::string(95,'X')},{"name","Long ID"}}
    });
    const auto names = flockSkillNames(bots);
    check(names.at("A___B.").starts_with("flock-a-b-") && names.at("a.b") != names.at("A___B.") &&
          names.at(std::string(95,'X')).size() == 64,"invalid skill slugs or unresolved collision");
    std::filesystem::create_directories(root / "flock-foreign");
    std::ofstream(root / "flock-foreign" / "SKILL.md") << "foreign";
    exportFlockSkills(root,bots);
    const auto read = [](const auto& path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in),{});
    };
    const auto file = root / names.at("A___B.") / "SKILL.md";
    const auto firstSkill = read(file);
    check(firstSkill.find("設計レビュー") != firstSkill.npos && firstSkill.find("reviewer") != firstSkill.npos &&
          firstSkill.find("A___B.") != firstSkill.npos && firstSkill.find("secret settings") == firstSkill.npos,
          "skill description or stable Bot ID was missing, or config leaked");
    auto renamed = bots;
    renamed[0]["name"] = "新しい名前";
    exportFlockSkills(root,renamed);
    check(read(file).find("新しい名前") != std::string::npos,"rename did not update the skill");
    exportFlockSkills(root,Json::array());
    check(!std::filesystem::exists(file) && std::filesystem::exists(root / "flock" / "SKILL.md") &&
          read(root / "flock-foreign" / "SKILL.md") == "foreign","cleanup changed an unowned skill");
}

}

int main() {
    char name[] = "/tmp/flock-participation.XXXXXX";
    const auto* dir = mkdtemp(name);
    if (!dir) return 1;
    const std::filesystem::path path(dir);
    try {
        originMatching();
        skillExport(path / "skills");
        invocationAndRecovery(path / "invocation.sqlite3");
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
