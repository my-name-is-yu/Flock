#include "flock/root_delivery.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

using namespace flockv0;
namespace {

const ExactRunTarget target{"environment", "project", "thread", "run-A"};
const std::string token(43, 'x');
const Person person{"participant"};

void check(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

template <typename Action> void rejects(Action action) {
    try { action(); }
    catch (const Conflict&) { return; }
    throw std::runtime_error("Expected a denied root operation");
}

struct Authority final : ParticipationAuthority {
    std::optional<VerifiedInvitation> invitation(const InvitationRef& request) override {
        return VerifiedInvitation{request, person.id, {}, {}};
    }
    std::optional<CoordinationGrant> grant(const CoordinationBinding&) override { return {}; }
};

struct Gate final : RootGate {
    RootGrant grant{{target, "request", 0}, "source-message", "reviewer", {}, false};
    std::optional<RootReceipt> accepted;
    bool unavailable = false;
    bool matchingReceipt = true;
    unsigned sends = 0;

    std::optional<RootGateCapabilities> capabilities(std::string_view environment) override {
        return RootGateCapabilities{std::string(environment), "0.1.11+fixture", 1, true, true, true, true};
    }
    std::optional<RootGrant> readGrant(const RootGrantRef& ref, std::string_view credential) override {
        if (unavailable || ref != grant.ref || credential != token) return {};
        return grant;
    }
    RootGateReply readReceipt(const RootCommandRef& ref, std::string_view credential) override {
        check(ref.grant == grant.ref && credential == token, "receipt lookup escaped its grant");
        if (!accepted) return {RootGateReply::Kind::AuthoritativelyAbsent, {}, {}};
        return {RootGateReply::Kind::Found, accepted, {}};
    }
    RootGateReply send(const RootCommand& command, std::string_view credential) override {
        check(command.ref.grant == grant.ref && credential == token && !grant.sealed,
              "root send escaped its active grant");
        ++sends;
        accepted = RootReceipt{command.ref, grant.botId, RootDeliveryState::ProviderPending, {}, {}};
        if (!matchingReceipt) accepted->ref.grant.target.runId = "run-B";
        return {};
    }
};

ConsultPermit authorize(Store& store) {
    const InvitationRef ref{target, "request", "source-message", "reviewer"};
    auto access = store.access();
    access.personGrant(localOwner, ref.botId, person.id, Role::decide, GrantChange::grant);
    access.personScope(localOwner, person.id, Scope::sessionRead(target.environmentId, target.threadId), GrantChange::grant);
    access.resourceGrant(localOwner, ref.botId, Scope::consultReply(target.environmentId, target.threadId), GrantChange::grant);
    Authority authority;
    return std::get<ConsultPermit>(ParticipationStore(store).open(person, ref, authority));
}

void restartAndSeal(const std::filesystem::path& path) {
    Gate gate;
    Id channel = 0;
    std::string command;
    {
        Store store(path);
        const auto permit = authorize(store);
        channel = permit.channel.id;
        command = permit.commandId;
        RootDelivery delivery(store, gate);
        rejects([&] { delivery.bind(person, channel, std::string(43, 'y')); });
        delivery.bind(person, channel, token);
        rejects([&] { delivery.prepare(person, "other", channel, command, "reply", "evidence"); });
        rejects([&] { delivery.prepare(person, "reviewer", channel, "alternate", "reply", "evidence"); });
        rejects([&] { delivery.prepare(person, "reviewer", channel, command, token, "evidence"); });
        check(delivery.prepare(person, "reviewer", channel, command, "reply", "evidence").state ==
              RootDeliveryState::Prepared, "did not prepare exact envelope");
        rejects([&] { delivery.prepare(person, "reviewer", channel, command, "changed", "evidence"); });
        check(gate.sends == 0, "prepare performed an external effect");
    }
    {
        Store reopened(path);
        RootDelivery delivery(reopened, gate);
        check(delivery.reconcile(person, "reviewer", channel, command).state ==
              RootDeliveryState::Prepared && gate.sends == 0, "receipt read replayed a mutation");
        gate.unavailable = true;
        check(delivery.deliver(person, "reviewer", channel, command).state ==
              RootDeliveryState::Unknown && gate.sends == 0, "grant unavailability did not fail closed");
        gate.unavailable = false;
        check(delivery.deliver(person, "reviewer", channel, command).state ==
              RootDeliveryState::Unknown && gate.sends == 1, "lost reply was treated as confirmed");
    }
    {
        gate.grant.sealed = true;
        Store reopened(path);
        RootDelivery delivery(reopened, gate);
        const auto pending = delivery.deliver(person, "reviewer", channel, command);
        check(pending.sealed && pending.receipt && pending.state == RootDeliveryState::ProviderPending &&
              gate.sends == 1, "sealed root mutated or lost accepted pending input");
        gate.accepted->observation = RootDeliveryState::Applied;
        gate.accepted->nativeTurnId = "exact-turn";
        gate.accepted->nativeItemId = "provider-item";
        const auto applied = delivery.reconcile(person, "reviewer", channel, command);
        check(applied.state == RootDeliveryState::Applied && applied.receipt &&
              applied.receipt->nativeTurnId == "exact-turn" && applied.receipt->ref.grant.target == target &&
              gate.sends == 1, "same-turn evidence was not durably recovered after seal");
    }
    std::ifstream raw(path, std::ios::binary);
    const std::string database((std::istreambuf_iterator<char>(raw)), {});
    check(database.find(token) == std::string::npos, "root credential reached SQLite");
}

void rejectMisdirection(const std::filesystem::path& path) {
    Store store(path);
    const auto permit = authorize(store);
    Gate gate;
    gate.matchingReceipt = false;
    RootDelivery delivery(store, gate);
    delivery.bind(person, permit.channel.id, token);
    delivery.prepare(person, "reviewer", permit.channel.id, permit.commandId, "reply", "evidence");
    delivery.deliver(person, "reviewer", permit.channel.id, permit.commandId);
    const auto result = delivery.reconcile(person, "reviewer", permit.channel.id, permit.commandId);
    check(result.state == RootDeliveryState::Unknown && !result.receipt && gate.sends == 1,
          "wrong Run became proof or prompted new send");
}

}

int main() {
    char temporary[] = "/tmp/flock-root-delivery-XXXXXX";
    if (!mkdtemp(temporary)) return 1;
    const std::filesystem::path path(temporary);
    try {
        restartAndSeal(path/"restart.sqlite");
        rejectMisdirection(path/"mismatch.sqlite");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
    std::filesystem::remove_all(path);
}
