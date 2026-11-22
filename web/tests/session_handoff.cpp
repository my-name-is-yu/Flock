#include "http_fixture.hpp"

#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>

using namespace flockv0;
using namespace flockv0::http_test;

namespace {
const ExactRunTarget target{"local-env","release","human-session","human-run"};
const RootInvitationRef request{target,"consultation"};
const RootInvitationRef coordinationRequest{target,"coordination"};
const std::string credential(43,'q');
const std::string coordinatorCredential(43,'w');
const CoordinationRequest coordination{0,"Coordinate remaining checks",false};
constexpr auto t3Person = "authenticated-t3-person";
constexpr auto aliceCredential = "Bearer flock-fixture-alice";
constexpr auto bobCredential = "Bearer flock-fixture-bob";

const Json input{{"target",{{"environmentId",target.environmentId},{"projectId",target.projectId},
                            {"threadId",target.threadId},{"runId",target.runId}}},
                 {"requestId",request.requestId},{"credential",credential}};

class Gate final : public RootGate {
public:
    std::atomic<bool> canActivate{false}, applied{false};
    std::atomic<bool> coordinationSealed{false}, canAcknowledgeFreeze{false}, freezeAcknowledged{false};
    std::atomic<unsigned> sends{0}, reads{0};

    std::optional<RootGateCapabilities> capabilities(std::string_view environment) override {
        check(environment == target.environmentId,"wrong capability environment");
        return RootGateCapabilities{target.environmentId,"fixture",1,true,true,true,true};
    }

    std::optional<RootInvitation> readInvitation(const RootInvitationRef& ref, std::string_view token) override {
        ++reads;
        const bool consult = ref == request && token == credential;
        if (!consult && (ref != coordinationRequest || token != coordinatorCredential)) return {};
        return RootInvitation{{target,ref.requestId,"source-message","release"},t3Person,
                              {{target,"selected-message","Shared evidence",false}},
                              consult ? std::nullopt : std::optional(coordination)};
    }

    std::optional<RootGrant> activateInvitation(const RootInvitationRef& ref, std::string_view activation,
                                               std::string_view token) override {
        if (ref == coordinationRequest && token == coordinatorCredential) {
            std::lock_guard lock(mutex);
            if (coordinatorGrantId.empty()) coordinatorGrantId = activation;
            check(activation == coordinatorGrantId,"coordination retry created another activation");
            return coordinatorGrant();
        }
        check(ref == request && activation == request.requestId && token == credential,"activation changed the consult binding");
        if (!canActivate) return {};
        return rootGrant();
    }

    std::optional<RootGrant> readGrant(const RootGrantRef& ref, std::string_view token) override {
        if (token == coordinatorCredential) {
            std::lock_guard lock(mutex);
            auto grant = coordinatorGrant();
            return grant.ref == ref ? std::optional(grant) : std::nullopt;
        }
        if (ref != rootGrant().ref || token != credential) return {};
        return rootGrant();
    }

    std::optional<RootSealStatus> sealStatus(const RootGrantRef& ref, std::string_view token) override {
        const auto grant = readGrant(ref,token);
        if (!grant) return {};
        if (!grant->sealed) return RootSealStatus{*grant,{},{},false};
        return RootSealStatus{*grant,{"accepted-before-seal"},"seal-marker",freezeAcknowledged};
    }

    std::optional<RootProgressPage> events(const RootGrantRef& ref, Id after, std::string_view token) override {
        if (token != coordinatorCredential || !readGrant(ref,token)) return {};
        if (after == 0) return RootProgressPage{target,5,{{"decision-needed",5,"decisionNeeded","pending","A decision is needed"}}};
        return RootProgressPage{target,after,{}};
    }

    std::optional<RootSealStatus> confirmFreeze(const RootGrantRef& ref, std::string_view marker, Id generation,
                                               std::string_view token) override {
        check(token == coordinatorCredential && marker == "seal-marker" && generation == 2,
              "T3 release was acknowledged without the local freeze generation and exact seal");
        if (!canAcknowledgeFreeze) return {};
        freezeAcknowledged = true;
        return sealStatus(ref,token);
    }

    RootGateReply readReceipt(const RootCommandRef& ref, std::string_view token) override {
        check(ref.grant == rootGrant().ref && token == credential,"receipt lookup changed the grant");
        if (!sends) return {RootGateReply::Kind::AuthoritativelyAbsent,{},{}};
        if (!applied) return pending(ref);
        return {RootGateReply::Kind::Found,RootReceipt{ref,"release",RootDeliveryState::Applied,"native-turn","native-item"},{}};
    }

    RootGateReply send(const RootCommand& command, std::string_view token) override {
        check(command.ref.grant == rootGrant().ref && command.text == "Keep the existing checks" &&
              token == credential,"root send changed the Bot's scoped reply or exact Run");
        ++sends;
        return pending(command.ref);
    }

private:
    std::mutex mutex;
    std::string coordinatorGrantId;
    RootGrant coordinatorGrant() const {
        return {{target,coordinatorGrantId,1},"source-message","release",coordination,coordinationSealed};
    }
    static RootGrant rootGrant() { return {{target,request.requestId,0},"source-message","release",{},false}; }
    static RootGateReply pending(const RootCommandRef& ref) {
        return {RootGateReply::Kind::Found,RootReceipt{ref,"release",RootDeliveryState::ProviderPending,{},{}},{}};
    }
};

class ConsultationModel final : public Model {
public:
    std::atomic<unsigned> calls{0};
    std::atomic<unsigned> coordinationCalls{0};

    ModelTurn decide(const ModelInput& input) override {
        if (input.event.at("kind") == "root_coordination") {
            ++coordinationCalls;
            if (input.event.at("answer").is_null()) {
                check(input.event.at("progress").at(0).at("kind") == "decisionNeeded", "wrong event for the coordinator");
                return {{{"handoff_question",{{"text","Which test suite should run?"}},"A person must decide"}}};
            }
            check(input.event.at("answer").at("question") == "Which test suite should run?" &&
                  input.event.at("answer").at("text") == "Integration checks", "the answer changed source question");
            return {{{"wait_for_root",{},"Wait for further progress"}}};
        }
        ++calls;
        check(input.task["botId"] == "release" && input.event["sourceMessageId"] == "source-message" &&
              input.event["sharedContext"].at(0).at("text") == "Shared evidence" &&
              input.tools == std::vector<std::string>{"reply"},"consultation model received the wrong scope or source");
        return {{{"reply",{{"text","Keep the existing checks"}},"One consultation reply"}}};
    }
};

void installIdentities(const std::filesystem::path& file, const Json& identities, Store& db, WebCallbacks& cb) {
    std::ofstream(file) << identities;
    std::filesystem::permissions(file,std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    configureWebIdentities(db,file,cb);
}

void anonymous(const std::filesystem::path& path) {
    Store db(path);
    auto server = startWeb(db,0,callbacks());
    Browser browser(*server);
    browser.post("/api/session-handoffs",input,401);
}

void sessionHandoffs(const std::filesystem::path& directory) {
    Store db(directory/"handoffs.sqlite3");
    Gate gate;
    ConsultationModel model;
    auto cb = callbacks();
    auto identities = Json::array({{{"person","alice"},{"authorization",aliceCredential},{"t3People",{{"local-env",t3Person}}}},
                                  {{"person","bob"},{"authorization",bobCredential},{"t3People",{{"another-env",t3Person}}}}});
    installIdentities(directory/"identities.json",identities,db,cb);
    identities.at(1).at("t3People") = {{"local-env",t3Person}};
    bool rejected = false;
    try { installIdentities(directory/"overlapping.json",identities,db,cb); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"one remote person was mapped to two local actors");
    cb.sessionHandoffs = std::make_shared<SessionHandoff>(db,gate,cb.bindT3Person);
    cb.sessionHandoffModel = &model;
    cb.sessionHandoffSoul = [](std::string_view bot) {
        check(bot == "release","web supplied the wrong Bot profile");
        return "You check releases";
    };

    auto access = db.access();
    for (const auto* person : {"alice","bob"}) {
        access.personGrant(localOwner,"release",person,Role::decide,GrantChange::grant);
        access.personScope(localOwner,person,Scope::sessionRead("local-env","human-session"),GrantChange::grant);
    }
    access.resourceGrant(localOwner,"release",Scope::consultReply("local-env","human-session"),GrantChange::grant);
    access.resourceGrant(localOwner,"release",Scope::sessionCoordinate("local-env","human-session"),GrantChange::grant);
    auto server = startWeb(db,0,std::move(cb));
    Browser browser(*server);
    browser.post("/api/session-handoffs",input,401);
    browser.client.set_default_headers({{"Authorization",bobCredential}});
    check(browser.post("/api/session-handoffs",input,403).at("error") == "handoff_unavailable",
          "another mapped person obtained the invitation");
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    auto forged = input;
    forged["botId"] = "triage";
    browser.post("/api/session-handoffs",forged,400);
    browser.post("/api/session-handoffs?credential=forbidden",input,400);
    auto crossOrigin = browser.headers();
    crossOrigin.erase("Origin");
    crossOrigin.emplace("Origin","https://different.invalid");
    Browser::checked(browser.client.Post("/api/session-handoffs",crossOrigin,input.dump(),"application/json"),403);
    check(gate.reads == 1,"an unauthenticated, malformed, or cross-origin request reached T3");

    const auto opening = browser.post("/api/session-handoffs",input);
    check(opening.at("opening") == true && opening.at("botId") == "release" && opening.at("target") == input.at("target") &&
          opening.at("reply").is_null() && opening.dump().find(credential) == std::string::npos &&
          opening.dump().find(t3Person) == std::string::npos,"opening status changed the origin or exposed credentials");
    const auto path = "/api/session-handoffs/" + std::to_string(opening.at("channelId").get<Id>());
    check(browser.get("/api/session-handoffs").at(0).at("channelId") == opening.at("channelId"),
          "the authenticated person could not reopen their saved handoff");
    browser.client.set_default_headers({{"Authorization",bobCredential}});
    check(browser.get("/api/session-handoffs").empty(),"another person's handoff appeared in the list");
    Browser::checked(browser.client.Get(path),403);
    browser.post(path + "/resume",Json::object(),403);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    browser.post(path + "/consult",Json::object(),403);
    gate.canActivate = true;
    const auto active = browser.post(path + "/resume",Json::object());
    check(active.at("opening") == false && active.at("channelId") == opening.at("channelId") &&
          browser.post("/api/session-handoffs",input).at("channelId") == opening.at("channelId"),
          "resume or invite retry did not keep the same return channel");
    const auto pending = browser.post(path + "/consult",Json::object());
    check(pending.at("state") == rootDeliveryStateName(RootDeliveryState::ProviderPending) &&
          pending.at("provider").size() == 1 && browser.get(path).at("reply") == pending,
          "pending delivery was not visible or presented as provider proof");
    gate.applied = true;
    const auto applied = browser.post(path + "/consult",Json::object());
    check(applied.at("state") == rootDeliveryStateName(RootDeliveryState::Applied) &&
          applied.at("provider").at("nativeItemId") == "native-item" &&
          applied.at("commandId") == pending.at("commandId") && applied.dump().find(credential) == std::string::npos &&
          gate.sends == 1 && model.calls == 1 && db.tasks().empty(),"provider evidence, dedupe, or root ownership changed");

    auto inviteCoordinator = input;
    inviteCoordinator["requestId"] = coordinationRequest.requestId;
    inviteCoordinator["credential"] = coordinatorCredential;
    const auto coordinating = browser.post("/api/session-handoffs",inviteCoordinator);
    const auto coordPath = "/api/session-handoffs/" + std::to_string(coordinating.at("channelId").get<Id>());
    const auto question = browser.post(coordPath + "/step",Json::object());
    check(question.at("eventCursor") == 5 && question.at("pendingQuestion").at("eventId") == "decision-needed",
          "coordination question lost its durable event binding");
    const Json answer{{"generation",question.at("generation")},{"eventId","decision-needed"},{"text","Integration checks"}};
    auto wrongAnswer = answer;
    wrongAnswer["generation"] = 2;
    browser.post(coordPath + "/answer",wrongAnswer,403);
    browser.client.set_default_headers({{"Authorization",bobCredential}});
    browser.post(coordPath + "/answer",answer,403);
    browser.client.set_default_headers({{"Authorization",aliceCredential}});
    check(browser.post(coordPath + "/answer",answer).at("pendingQuestion").is_null(),"answer did not settle the question");
    browser.post(coordPath + "/answer",answer);
    wrongAnswer = answer;
    wrongAnswer["text"] = "Other checks";
    browser.post(coordPath + "/answer",wrongAnswer,403);
    browser.post(coordPath + "/step",Json::object());
    browser.post(coordPath + "/step",Json::object());
    check(model.coordinationCalls == 2,"a repeated event or answer rescheduled a decision");
    const auto frozen = browser.post(coordPath + "/freeze",Json::object());
    check(frozen.at("mode") == "coordinate" && frozen.at("frozen") == true && frozen.at("sealed") == false &&
          frozen.at("generation") == 2 && frozen.at("released") == false,
          "local freeze alone was represented as released or remotely sealed");
    browser.post(coordPath + "/answer",answer,403);
    gate.coordinationSealed = true;
    const auto sealing = browser.post(coordPath + "/resume",Json::object());
    check(sealing.at("sealed") == true && sealing.at("frozen") == true && sealing.at("released") == false &&
          sealing.at("residualCommandIds") == Json::array({"accepted-before-seal"}) && sealing.at("sealMarker") == "seal-marker",
          "remote seal hid residuals or was represented as final release before acknowledgment");
    gate.canAcknowledgeFreeze = true;
    const auto released = browser.post(coordPath + "/resume",Json::object());
    check(released.at("released") == true && released.at("releaseAcknowledged") == true &&
          released.at("generation") == 2 && browser.get(coordPath) == released && db.tasks().empty(),
          "acknowledged two-barrier take-back was not durable and independently visible");
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
