#include "flock/root_delivery.hpp"
#include "internal.hpp"

#include <iostream>

using namespace flockv0;
using t3detail::http;
using t3detail::parse;

namespace {
const ExactRunTarget target{"env-1","project","human-thread","human-run"};
const RootGrantRef coordination{target,"grant",4};
const std::string credential(43,'a');
constexpr std::string_view guidance = "Stay in this Run.\n確認してください。";

void check(bool pass, std::string_view message) {
    if (!pass) throw std::runtime_error(std::string(message));
}

RootCommand command(std::string_view id) { return {{coordination,std::string(id)},std::string(guidance)}; }

void configure(const std::string& origin, const Json& data) {
    check(http(origin + "/fixture","POST",{{"Content-Type","application/json"}},data.dump()).status == 200,
          "fixture settings rejected");
}

Json effects(const std::string& origin) {
    return parse(http(origin + "/fixture/effects","GET").body);
}

void compatibility(const std::string& origin, const std::filesystem::path& credentials, RootGate& gate) {
    const auto expected = RootGateCapabilities{"env-1","0.1.11-fixture",1,true,true,true,true};
    check(gate.capabilities("env-1") == expected,"exact-run capability contract was not recognized");
    check(!gate.capabilities("different-environment"),"another environment supplied the capability");
    auto stored = makeT3RootGate(credentials);
    check(!stored->capabilities("env-1"),"an unconfigured stored environment was contacted");
    loginT3(credentials,"env-1",origin + "/mcp",[&](const auto& url) { configure(origin,{{"authorization",url}}); });
    check(stored->capabilities("env-1") == expected,"trusted stored MCP connection could not resolve exact-run origin");

    auto otherRoute = makeT3RootGate([&](std::string_view) { return origin + "/another-path"; });
    check(!otherRoute->capabilities("env-1"),"untrusted destination path was used for the exact-run operation");
    for (const auto& patch : {Json{{"protocolVersion",2}},Json{{"environmentId","other"}},
                               Json{{"senderAttribution",false}},Json{{"grantSeal",1}}}) {
        configure(origin,{{"exactCapabilities",patch}});
        check(!gate.capabilities("env-1") && gate.send(command("must-not-send"),credential).kind == RootGateReply::Kind::Unknown,
              "command sent without compatible capability for the configured environment");
    }
    configure(origin,{{"exactCapabilities",Json::object()}});
    check(!effects(origin).contains("exact/commands/send"),"credential reached the send route before negotiation");
}

void grants(const std::string& origin, RootGate& gate) {
    const auto grant = gate.readGrant(coordination,credential);
    check(grant && grant->ref == coordination && grant->sourceMessageId == "original-person-message" &&
          grant->botId == "planner" && grant->coordination == CoordinationRequest{3,"Check the remaining work",true} &&
          !grant->sealed,"opened grant lost its binding or scope");
    const RootGrantRef consult{target,"consult",0};
    const auto consulted = gate.readGrant(consult,credential);
    check(consulted && consulted->ref == consult && !consulted->coordination,"consult response acquired coordination");
    check(!gate.readGrant({target,"wrong-project",4},credential),"another project's grant satisfied the lookup");
    const auto unsealed = gate.sealStatus(coordination,credential);
    check(unsealed && !unsealed->grant.sealed && !unsealed->sealMarker && unsealed->residualCommandIds.empty(),
          "unsealed grant reported residual commands or seal evidence");
    configure(origin,{{"exactSealed",true}});
    const auto sealed = gate.readGrant(coordination,credential);
    check(sealed && sealed->sealed,"remote seal was lost");
    const auto evidence = gate.sealStatus(coordination,credential);
    check(evidence && evidence->grant.ref == coordination && evidence->sealMarker == "seal-proof" &&
          evidence->residualCommandIds == std::vector<std::string>{"admitted-command"} && !evidence->releaseAcknowledged,
          "remote seal evidence and release acknowledgment were conflated");
    const auto released = gate.confirmFreeze(coordination,"seal-proof",2,credential);
    check(released && released->releaseAcknowledged && released->residualCommandIds == evidence->residualCommandIds,
          "freeze generation confirmation did not complete release");
    check(!gate.confirmFreeze({target,"wrong-confirmation",4},"seal-proof",2,credential),"another seal marker completed release");
    configure(origin,{{"exactSealed",false}});
}

void invitations(RootGate& gate) {
    const RootInvitationRef coordinate{target,"invite-coordinate"}, consult{target,"invite-consult"};
    const auto invitation = gate.readInvitation(coordinate,credential);
    const CoordinationRequest scope{3,"Check the remaining work",true};
    check(invitation && invitation->ref == InvitationRef{target,coordinate.requestId,"original-person-message","planner"} &&
          invitation->personRef == "authenticated-t3-person" && invitation->coordination == scope &&
          invitation->context == std::vector<SharedSessionContext>{{target,"shared-event","Shared evidence",true}},
          "invitation lost its authenticated origin, scope, or bounded context");
    for (const auto* request : {"wrong-request","wrong-context","oversized-context","duplicate-context","excessive-context"})
        check(!gate.readInvitation({target,request},credential),"a malformed invitation response was used");

    const auto activated = gate.activateInvitation(coordinate,"durable-activation",credential);
    check(activated && activated->ref == RootGrantRef{target,"durable-activation",4} &&
          activated->coordination == scope && !activated->sealed,"activation did not preserve its durable ID and epoch");
    const auto consulted = gate.readInvitation(consult,credential);
    const auto consultGrant = gate.activateInvitation(consult,"consult-attempt",credential);
    check(consulted && !consulted->coordination && consultGrant && !consultGrant->coordination &&
          consultGrant->ref == RootGrantRef{target,consult.requestId,0},"consult activation acquired coordination scope");
    for (const auto* request : {"wrong-activation","wrong-activation-epoch"})
        check(!gate.activateInvitation({target,request},"durable-activation",credential),"another activation satisfied the request");
}

void progressEvents(const std::string& origin, RootGate& gate) {
    const auto page = gate.events(coordination,0,credential);
    check(page && page->target == target && page->cursor == 9 && page->events.size() == 2 &&
          page->events[0].id == "progress-event" && page->events[0].position == 4 &&
          page->events[0].kind == "progress" && page->events[1].kind == "decisionNeeded" &&
          page->events[1].status == "pending" && page->events[1].summary == "The Run is waiting for a decision.",
          "progress batch lost its Run, event fields, or sequence gaps");
    const auto tail = gate.events(coordination,4,credential);
    const auto empty = gate.events(coordination,9,credential);
    check(tail && tail->cursor == 9 && tail->events.size() == 1 && tail->events[0].id == "decision-event" &&
          empty && empty->cursor == 9 && empty->events.empty(),"progress pagination changed the saved cursor");
    for (const auto* scenario : {"wrong-target","duplicate-id","out-of-order","cursor-gap","cursor-rewind",
                                 "summary-too-long","credential-in-summary","wrong-kind","wrong-decision-status","too-many-events"}) {
        configure(origin,{{"exactEvents",scenario}});
        check(!gate.events(coordination,0,credential),std::string("unsafe progress page was accepted: ") + scenario);
    }
    configure(origin,{{"exactEvents",nullptr}});
    const auto before = effects(origin);
    check(!gate.events({target,"consult",0},0,credential) && !gate.events(coordination,-1,credential) &&
          effects(origin) == before,"consult or invalid local cursor reached the progress route");
}

void inputCommit(const std::string& origin, RootGate& gate) {
    check(!gate.inputCommit(coordination,credential) && !effects(origin).contains("exact/grants/input-commit"),
          "an older exact-run server received a pinned work request");
    configure(origin,{{"exactCapabilities",{{"strictPinnedWorktree",1}}}});
    check(!gate.capabilities(target.environmentId) && !gate.inputCommit(coordination,credential),
          "non-boolean strict worktree capability was accepted");
    configure(origin,{{"exactCapabilities",{{"strictPinnedWorktree",true}}}});
    const auto capabilities = gate.capabilities(target.environmentId);
    check(capabilities && capabilities->strictPinnedWorktree && gate.inputCommit(coordination,credential) == std::string(40,'b'),
          "strict worktree capability or exact root input was lost");
    configure(origin,{{"exactInputCommit","sha256"}});
    check(gate.inputCommit(coordination,credential) == std::string(64,'c'),"full SHA-256 Git input was refused");
    for (const auto* scenario : {"missing","wrong-target","abbreviated","upper","non-git","credential","unavailable"}) {
        configure(origin,{{"exactInputCommit",scenario}});
        check(!gate.inputCommit(coordination,credential),std::string("unverified root input was accepted: ") + scenario);
    }
    const auto before = effects(origin);
    check(!gate.inputCommit({target,"consult",0},credential) && effects(origin) == before,
          "consultation was permitted to read the root input commit");
    configure(origin,{{"exactCapabilities",Json::object()}});
}

void delivery(const std::string& origin, RootGate& gate) {
    using Kind = RootGateReply::Kind;
    const auto sent = gate.send(command("applied"),credential);
    check(sent.kind == Kind::Found && sent.receipt && sent.receipt->ref == command("applied").ref &&
          sent.receipt->botId == "planner" && sent.receipt->observation == RootDeliveryState::Applied &&
          sent.receipt->nativeTurnId == "native-turn" && sent.receipt->nativeItemId == "native-item",
          "proof was not bound to the original Run and command");
    for (const auto& [name,state] : std::initializer_list<std::pair<std::string_view,RootDeliveryState>>{
             {"pending",RootDeliveryState::ProviderPending},{"failed",RootDeliveryState::Failed},{"unknown",RootDeliveryState::Unknown}}) {
        const auto reply = gate.send(command(name),credential);
        check(reply.kind == Kind::Found && reply.receipt && reply.receipt->observation == state &&
              reply.receipt->nativeTurnId.empty() && reply.receipt->nativeItemId.empty(),
              "receipt without provider proof was treated as applied");
    }
    for (const auto* name : {"wrong-run","wrong-epoch","wrong-command","invalid-bot","missing-proof","redirect",
                            "unexpected-error","unavailable","disconnect","wrong-content-type"}) {
        const auto reply = gate.send(command(name),credential);
        check(reply.kind == Kind::Unknown && !reply.receipt && reply.reason.find("fixture-private-detail") == reply.reason.npos,
              std::string("ambiguous reply was accepted or exposed response data: ") + name);
    }
    for (const auto* name : {"conflict","stale"}) {
        const auto reply = gate.send(command(name),credential);
        check(reply.kind == Kind::Rejected && !reply.receipt && reply.reason == name,"definitive rejection lost its reason");
    }
    const auto before = effects(origin);
    check(gate.send(command("unsafe-token"),"bad\r\nvalue").kind == Kind::Rejected &&
          effects(origin) == before,"invalid local credential reached the network");
    check(!before.contains("exact/redirected") && !before.contains("t3_thread_send"),
          "exact-run transport followed a redirect or used generic steering");

    const auto found = gate.readReceipt(command("applied").ref,credential);
    check(found.kind == Kind::Found && found.receipt && found.receipt->ref == sent.receipt->ref &&
          found.receipt->nativeItemId == sent.receipt->nativeItemId,"receipt lookup lost the accepted evidence");
    check(gate.readReceipt(command("absent").ref,credential).kind == Kind::AuthoritativelyAbsent,
          "authoritative absence was lost");
    for (const auto* name : {"expired","contradictory","not-found"})
        check(gate.readReceipt(command(name).ref,credential).kind == Kind::Unknown,"lookup inferred absence from ambiguous evidence");
    check(effects(origin).at("exact/commands/send") == before.at("exact/commands/send"),"receipt lookup performed a send");
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 3,"expected fixture origin and credentials directory");
        const std::string origin = argv[1];
        auto gate = makeT3RootGate([&](std::string_view) { return origin; });
        compatibility(origin,argv[2],*gate);
        grants(origin,*gate);
        invitations(*gate);
        progressEvents(origin,*gate);
        inputCommit(origin,*gate);
        delivery(origin,*gate);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
