#include "flock/participation.hpp"

#include <barrier>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <unistd.h>

using namespace flockv0;

namespace {
const Person alice{"alice"}, bob{"bob"};
const ExactRunTarget target{"desktop","project","session","run"};
const auto readScope = Scope::sessionRead(target.environmentId,target.threadId);
const auto replyScope = Scope::consultReply(target.environmentId,target.threadId);
const auto coordinateScope = Scope::sessionCoordinate(target.environmentId,target.threadId);
const auto delegateScope = Scope::workDelegate(target.environmentId,target.threadId);

void check(bool result, std::string_view message) {
    if (!result) throw std::runtime_error(std::string(message));
}

template<class Action> void denied(Action action, std::string_view message) {
    try { action(); }
    catch (const Conflict&) { return; }
    throw std::runtime_error(std::string(message));
}

InvitationRef invite(std::string request, std::string bot = "planner") {
    return {target,std::move(request),"source-message",std::move(bot)};
}

struct Authority : ParticipationAuthority {
    explicit Authority(InvitationRef ref) : verified(VerifiedInvitation{std::move(ref),alice.id,{},{}}) {}

    std::optional<VerifiedInvitation> verified;
    std::optional<CoordinationGrant> observed;
    std::optional<CoordinationBinding> lookup;
    std::function<void()> onGrant;

    std::optional<VerifiedInvitation> invitation(const InvitationRef&) override { return verified; }
    std::optional<CoordinationGrant> grant(const CoordinationBinding& binding) override {
        lookup = binding;
        if (onGrant) onGrant();
        return observed;
    }
};

void personAccess(Store& db, const InvitationRef& ref) {
    auto access = db.access();
    access.personGrant(localOwner,ref.botId,alice.id,Role::decide,GrantChange::grant);
    access.personScope(localOwner,alice.id,Scope::sessionRead(ref.target.environmentId,ref.target.threadId),GrantChange::grant);
}

void botAccess(Store& db, const InvitationRef& ref, const Scope& scope, GrantChange change = GrantChange::grant) {
    db.access().resourceGrant(localOwner,ref.botId,scope,change);
}

void authorization(const std::filesystem::path& path) {
    Store db(path);
    ParticipationStore store(db);
    const auto ref = invite("origin-checks");
    Authority authority(ref);
    auto access = db.access();
    access.personScope(localOwner,alice.id,readScope,GrantChange::grant);
    access.personGrant(localOwner,ref.botId,alice.id,Role::view,GrantChange::grant);
    botAccess(db,ref,replyScope);
    denied([&] { store.open(alice,ref,authority); },"view-only person created a consultation");

    personAccess(db,ref);
    denied([&] { store.open(bob,ref,authority); },"wrong person opened the invitation");
    denied([&] { store.open(localOwner,ref,authority); },"local owner bypassed the authenticated T3 identity");
    for (auto member : {&ExactRunTarget::environmentId,&ExactRunTarget::projectId,
                        &ExactRunTarget::threadId,&ExactRunTarget::runId}) {
        auto wrong = ref;
        wrong.target.*member = "different";
        denied([&] { store.open(alice,wrong,authority); },"another target resolved to the original invitation");
    }
    for (auto member : {&InvitationRef::requestId,&InvitationRef::sourceMessageId,&InvitationRef::botId}) {
        auto wrong = ref;
        wrong.*member = "different";
        denied([&] { store.open(alice,wrong,authority); },"a different request, message, or Bot was accepted");
    }
    authority.verified.reset();
    denied([&] { store.open(alice,ref,authority); },"an unverified invitation was accepted");
    authority.verified = VerifiedInvitation{ref,alice.id,{},{}};

    access.personScope(localOwner,alice.id,readScope,GrantChange::revoke);
    denied([&] { store.open(alice,ref,authority); },"person without Session read opened an invitation");
    personAccess(db,ref);
    botAccess(db,ref,replyScope,GrantChange::revoke);
    botAccess(db,ref,readScope);
    denied([&] { store.open(alice,ref,authority); },"Bot Session read escalated to a consultation reply");
    botAccess(db,ref,readScope,GrantChange::revoke);
    botAccess(db,ref,replyScope);

    const auto permit = std::get<ConsultPermit>(store.open(alice,ref,authority));
    check(!access.permits(ref.botId,readScope,true) && !access.permits(ref.botId,coordinateScope,true) &&
          !access.permits(ref.botId,delegateScope,true),"consultation exposed unrelated Bot permissions");
    denied([&] { store.consult(bob,ref.botId,permit.channel.id); },"wrong person retrieved the consultation");
    denied([&] { store.consult(alice,"specialist",permit.channel.id); },"another Bot retrieved the consultation");
    denied([&] { store.coordination(alice,permit.channel.id); },"consultation became coordination");
    denied([&] { store.observeGrant(alice,permit.channel.id,authority); },"consultation attempted grant observation");

    Store admin(path);
    botAccess(admin,ref,replyScope,GrantChange::revoke);
    denied([&] { store.consult(alice,ref.botId,permit.channel.id); },"revoked Bot retrieved a persisted permit");
    denied([&] { store.open(alice,ref,authority); },"revoked Bot reopened a persisted permit");
}

void contextAndRestart(const std::filesystem::path& path) {
    constexpr auto secret = "participation-fixture-protected-value";
    const auto ref = invite("first-consult");
    Authority authority(ref);
    authority.verified->context = {{target,"shared-note",std::string(8190,'x') + "日本語 " + secret,false},
                                   {target,"shared-status",std::string("Credential ") + secret,false}};
    Id channel = 0;
    Id existingTask = 0;
    std::string command;
    {
        Store db(path);
        ParticipationStore store(db);
        personAccess(db,ref);
        botAccess(db,ref,replyScope);
        db.protectGitCredential(secret);
        existingTask = db.submit({"planner","Ordinary Bot task",target.environmentId,target.projectId});

        auto& context = authority.verified->context;
        context.at(0).target.runId = "unshared-run";
        denied([&] { store.open(alice,ref,authority); },"context crossed the exact Run boundary");
        context.at(0).target = target;
        context.push_back({target,"extra-note","one",false});
        context.push_back({target,"excess-note","two",false});
        denied([&] { store.open(alice,ref,authority); },"an oversized context list was stored");
        context.resize(2);

        const auto permit = std::get<ConsultPermit>(store.open(alice,ref,authority));
        channel = permit.channel.id;
        command = permit.commandId;
        check(channel > 0 && !command.empty(),"consultation has no stable identity");
        check(permit.channel.origin == ImmutableOrigin{ref,alice.id},"return route lost exact provenance");
        const auto& shared = permit.channel.context;
        check(shared.size() == 2 && shared[0].target == target && shared[0].itemId == "shared-note" &&
              shared[0].truncated && shared[0].text == std::string(8190,'x') && shared[1].itemId == "shared-status" &&
              shared[1].text == "Credential [redacted]","stored context is unbounded, invalid, unredacted, or unattributed");
        for (auto suffix : {"","-wal"}) {
            std::ifstream file(path.string() + suffix,std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(file)),{});
            check(bytes.find(secret) == std::string::npos,"raw credential reached SQLite");
        }
    }

    Store reopened(path);
    reopened.protectGitCredential(secret);
    ParticipationStore store(reopened);
    const auto replayed = std::get<ConsultPermit>(store.open(alice,ref,authority));
    check(replayed.channel.id == channel && replayed.commandId == command &&
          store.consult(alice,ref.botId,channel).commandId == command,"restart created another reply identity");

    authority.verified->context.back().text = "Changed evidence";
    denied([&] { store.open(alice,ref,authority); },"reusing a request replaced immutable context");
    authority.verified->ref.sourceMessageId = "another-source";
    denied([&] { store.open(alice,authority.verified->ref,authority); },"reusing a request moved its return channel");

    auto other = invite("second-consult");
    other.target.runId = "another-run";
    other.sourceMessageId = "another-source";
    Authority second(other);
    const auto different = std::get<ConsultPermit>(store.open(alice,other,second));
    check(different.channel.id != channel && different.commandId != command &&
          store.consult(alice,ref.botId,channel).channel.origin.invitation == ref &&
          store.consult(alice,ref.botId,different.channel.id).channel.origin.invitation == other,
          "consultations shared reply identity or delivered to another origin");
    check(reopened.tasks().size() == 1 && reopened.task(existingTask).at("activeRun").is_null(),
          "participation created a Bot task or took ownership of a human Run");
}

void activationAndRestart(const std::filesystem::path& path) {
    const auto ref = invite("coordination");
    Authority authority(ref);
    authority.verified->coordination = CoordinationRequest{7,"Coordinate the remaining checks",true};
    Id channel = 0;
    std::optional<CoordinationBinding> binding;
    {
        Store db(path);
        ParticipationStore store(db);
        personAccess(db,ref);
        botAccess(db,ref,replyScope);
        denied([&] { store.open(alice,ref,authority); },"reply-once authority escalated to coordination");
        botAccess(db,ref,coordinateScope);
        denied([&] { store.open(alice,ref,authority); },"coordination granted delegation without permission");
        botAccess(db,ref,delegateScope);
        const auto activation = std::get<CoordinationActivation>(store.open(alice,ref,authority));
        channel = activation.channel.id;
        binding = activation.binding;
        check(!activation.acknowledgment && !binding->activationId.empty() && binding->request == *authority.verified->coordination,
              "activation was unbound or acknowledged before the remote grant existed");
        denied([&] { store.consult(alice,ref.botId,channel); },"coordination activation yielded a consultation permit");
    }

    Store reopened(path);
    ParticipationStore store(reopened);
    const auto opening = std::get<CoordinationActivation>(store.open(alice,ref,authority));
    check(opening.channel.id == channel && opening.binding == binding && !opening.acknowledgment,
          "opening activation did not survive restart");
    const auto observed = store.observeGrant(alice,channel,authority);
    check(!observed.acknowledgment && authority.lookup == binding,"retry lost the pending activation identity");

    Store admin(path);
    authority.onGrant = [&] { botAccess(admin,ref,delegateScope,GrantChange::revoke); };
    denied([&] { store.observeGrant(alice,channel,authority); },"access changed during grant lookup but returned the activation");
    authority.onGrant = {};
    botAccess(admin,ref,delegateScope);

    authority.observed = CoordinationGrant{*binding,7};
    denied([&] { store.observeGrant(alice,channel,authority); },"stale epoch activated coordination");
    authority.observed = CoordinationGrant{*binding,8};
    authority.observed->binding.origin.invitation.target.runId = "wrong-run";
    denied([&] { store.observeGrant(alice,channel,authority); },"another Run's grant activated coordination");
    authority.observed = CoordinationGrant{*binding,8};
    authority.observed->binding.request.allowDelegation = false;
    denied([&] { store.observeGrant(alice,channel,authority); },"another scope activated coordination");
    check(!store.coordination(alice,channel).acknowledgment,"a rejected acknowledgment changed persisted state");

    authority.observed = CoordinationGrant{*binding,8};
    check(store.observeGrant(alice,channel,authority).acknowledgment == authority.observed &&
          store.observeGrant(alice,channel,authority).acknowledgment == authority.observed,
          "matching remote acknowledgment failed to persist idempotently");
    check(ParticipationStore(admin).coordination(alice,channel).acknowledgment == authority.observed,
          "remote acknowledgment was lost after reopening the database");
    authority.observed->epoch = 9;
    denied([&] { store.observeGrant(alice,channel,authority); },"another epoch replaced a persisted acknowledgment");
    botAccess(admin,ref,delegateScope,GrantChange::revoke);
    denied([&] { store.coordination(alice,channel); },"revoked specialist scope remained available");
    denied([&] { store.observeGrant(alice,channel,authority); },"revoked specialist scope still allowed grant observation");
}

void concurrentInvitations(const std::filesystem::path& path) {
    Store left(path), right(path);
    const auto consult = invite("concurrent-consult");
    personAccess(left,consult);
    botAccess(left,consult,replyScope);
    auto openConsult = [&](Store& db, const InvitationRef& ref, std::barrier<>& start) {
        Authority authority(ref);
        start.arrive_and_wait();
        return std::get<ConsultPermit>(ParticipationStore(db).open(alice,ref,authority));
    };
    std::barrier consultStart(2);
    auto consultRetry = std::async(std::launch::async,openConsult,std::ref(right),std::cref(consult),std::ref(consultStart));
    const auto first = openConsult(left,consult,consultStart);
    const auto second = consultRetry.get();
    check(first.channel.id == second.channel.id && first.commandId == second.commandId,
          "simultaneous retry created duplicate reply authority");

    const auto one = invite("coordinator-one"), two = invite("coordinator-two","specialist");
    personAccess(left,two);
    botAccess(left,one,coordinateScope);
    botAccess(left,two,coordinateScope);
    std::barrier coordinationStart(2);
    auto openCoordination = [&](Store& db, const InvitationRef& ref) {
        Authority authority(ref);
        authority.verified->coordination = CoordinationRequest{0,"Check the work",false};
        coordinationStart.arrive_and_wait();
        try {
            return std::holds_alternative<CoordinationActivation>(ParticipationStore(db).open(alice,ref,authority));
        } catch (const Conflict&) { return false; }
    };
    auto other = std::async(std::launch::async,openCoordination,std::ref(right),std::cref(two));
    const auto won = openCoordination(left,one);
    check(won != other.get(),"competing invitations did not reserve exactly one coordination slot");
}
}

int main() {
    char directory[] = "/tmp/flock-participation-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    const std::filesystem::path root(directory);
    try {
        authorization(root/"authorization.db");
        contextAndRestart(root/"consultations.db");
        activationAndRestart(root/"coordination.db");
        concurrentInvitations(root/"concurrent.db");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
}
