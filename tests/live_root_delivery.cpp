#include "flock/root_delivery.hpp"
#include "flock/session_handoff.hpp"
#include "flock/t3_mcp.hpp"

#include <array>
#include <iostream>

using namespace flockv0;

namespace {

struct CheckFailure : std::runtime_error { using std::runtime_error::runtime_error; };

void check(bool condition, const char* message) {
    if (!condition) throw CheckFailure(message);
}

const Person fixturePerson{"live-root-fixture-person"};
constexpr std::string_view text = "For this integration test, acknowledge FLOCK-ROOT-DELIVERY-CHECK.";
constexpr std::string_view evidence = "live-root-delivery-check";

// An already activated grant can isolate RootDelivery from invitation setup.
class FixtureAuthority final : public ParticipationAuthority {
public:
    FixtureAuthority(RootGate& gate, std::string_view credential) : gate_(gate), credential_(credential) {}

    std::optional<VerifiedInvitation> invitation(const InvitationRef& ref) override {
        const RootGrantRef expected{ref.target, ref.requestId, 0};
        const auto grant = gate_.readGrant(expected, credential_);
        if (!grant || grant->ref != expected || grant->botId != ref.botId ||
            grant->sourceMessageId != ref.sourceMessageId || grant->coordination || grant->sealed) return {};
        return VerifiedInvitation{ref, fixturePerson.id, {}, {}};
    }

    std::optional<CoordinationGrant> grant(const CoordinationBinding&) override { return {}; }

private:
    RootGate& gate_;
    std::string_view credential_;
};

Json readInput() {
    std::array<char, 65537> bytes{};
    std::cin.read(bytes.data(), bytes.size());
    const auto size = std::cin.gcount();
    check(size > 0 && size < static_cast<std::streamsize>(bytes.size()), "Expected bounded JSON on stdin");
    return Json::parse(bytes.data(), bytes.data() + size);
}

bool sameReceipt(const RootReceipt& left, const RootReceipt& right) {
    return left.ref == right.ref && left.botId == right.botId && left.observation == right.observation &&
           left.nativeTurnId == right.nativeTurnId && left.nativeItemId == right.nativeItemId;
}

Json appliedResult(const RootDeliveryView& result, const InvitationRef& invitation) {
    check(result.state == RootDeliveryState::Applied && result.receipt.has_value(),
          "The exact root input has no authoritative Applied receipt");
    const auto& receipt = *result.receipt;
    check(receipt.ref == result.ref && receipt.ref.grant.target == invitation.target &&
          receipt.ref.grant.grantId == invitation.requestId && receipt.botId == invitation.botId &&
          receipt.observation == RootDeliveryState::Applied && !receipt.nativeTurnId.empty() &&
          !receipt.nativeItemId.empty(), "The receipt does not identify exact provider input");
    return {{"state", rootDeliveryStateName(result.state)}, {"commandId", result.ref.commandId},
            {"runId", invitation.target.runId}, {"botId", receipt.botId}, {"sealed", result.sealed},
            {"serverVersion", result.serverVersion}, {"nativeTurnId", receipt.nativeTurnId},
            {"nativeItemId", receipt.nativeItemId}};
}

}

int main() {
    const char* step = "reading fixture input";
    try {
        const auto input = readInput();
        const auto operation = input.at("operation").get<std::string>();
        const auto origin = input.at("t3Origin").get<std::string>();
        constexpr std::string_view loopback = "http://127.0.0.1:";
        const auto port = origin.starts_with(loopback) ? std::string_view(origin).substr(loopback.size()) : "";
        check(!port.empty() && port.find_first_not_of("0123456789") == port.npos,
              "The smoke fixture needs a loopback T3 origin");
        const auto& target = input.at("target");
        const InvitationRef invitation{
            {target.at("environmentId"), target.at("projectId"), target.at("threadId"), target.at("runId")},
            input.at("grantId"), input.at("sourceMessageId"), input.at("botId")
        };
        const auto& credential = input.at("credential").get_ref<const std::string&>();
        const auto stateDirectory = std::filesystem::path(input.at("fixtureStoreDir").get<std::string>());
        check(std::filesystem::is_directory(stateDirectory), "The private Flock fixture directory is missing");
        auto gate = makeT3RootGate([&](std::string_view environment) -> std::optional<std::string> {
            if (environment != invitation.target.environmentId) return {};
            return origin;
        });
        step = "opening Flock fixture storage";
        Store store(stateDirectory / "root-delivery.sqlite3");
        ParticipationStore participation(store);
        RootDelivery delivery(store, *gate);
        const auto personRef = input.value("personRef", "");
        SessionHandoff handoff(store, *gate, [&](std::string_view environment, std::string_view t3Person,
                                                const Person& person) {
            return environment == invitation.target.environmentId && t3Person == personRef &&
                   person.id == fixturePerson.id;
        });
        if (operation == "prepare") {
            step = "verifying the live grant and preparing the local envelope";
            const auto& [environment, project, thread, run] = invitation.target;
            auto access = store.access();
            access.personGrant(localOwner, invitation.botId, fixturePerson.id, Role::decide, GrantChange::grant);
            access.personScope(localOwner, fixturePerson.id, Scope::sessionRead(environment, thread), GrantChange::grant);
            access.resourceGrant(localOwner, invitation.botId, Scope::consultReply(environment, thread), GrantChange::grant);
            Id channel;
            if (personRef.empty()) {
                FixtureAuthority authority(*gate, credential);
                channel = std::get<ConsultPermit>(participation.open(fixturePerson, invitation, authority)).channel.id;
                delivery.bind(fixturePerson, channel, credential);
            } else {
                step = "accepting the authenticated T3 invitation";
                const RootInvitationRef ref{invitation.target, invitation.requestId};
                const auto accepted = handoff.accept(fixturePerson, ref, credential);
                check(!accepted.opening && accepted.origin.invitation == invitation,
                      "Session handoff did not activate its exact invitation");
                const auto replay = handoff.accept(fixturePerson, ref, credential);
                check(!replay.opening && replay.channel == accepted.channel,
                      "Invitation replay changed the channel or returned to Opening");
                channel = accepted.channel;
            }
            const auto permit = participation.consult(fixturePerson, invitation.botId, channel);
            const auto prepared = delivery.prepare(fixturePerson, invitation.botId, permit.channel.id,
                                                   permit.commandId, text, evidence);
            check(prepared.state == RootDeliveryState::Prepared, "Root preparation did not leave an unsent envelope");
            std::cout << Json{{"state", rootDeliveryStateName(prepared.state)}, {"channelId", permit.channel.id},
                              {"commandId", permit.commandId}, {"serverVersion", prepared.serverVersion}}.dump() << '\n';
            return 0;
        }

        const auto channel = input.at("channelId").get<Id>();
        const auto command = input.at("commandId").get<std::string>();
        step = "restoring the same local permit";
        const auto permit = participation.consult(fixturePerson, invitation.botId, channel);
        check(permit.channel.origin.invitation == invitation && permit.commandId == command,
              "Flock restart changed the consultation origin or command ID");
        if (operation == "deliver") {
            step = "reconciling the unsent envelope";
            const auto initial = delivery.reconcile(fixturePerson, invitation.botId, channel, command);
            check(initial.state == RootDeliveryState::Prepared && !initial.receipt,
                  "Read-only reconciliation changed the unsent envelope");
            step = "delivering to the original provider turn";
            auto applied = delivery.deliver(fixturePerson, invitation.botId, channel, command);
            if (applied.state == RootDeliveryState::ProviderPending || applied.state == RootDeliveryState::Unknown)
                applied = delivery.reconcile(fixturePerson, invitation.botId, channel, command);
            const auto result = appliedResult(applied, invitation);
            const auto replay = delivery.deliver(fixturePerson, invitation.botId, channel, command);
            check(replay.receipt && sameReceipt(*applied.receipt, *replay.receipt),
                  "Flock duplicate delivery changed the receipt");
            std::cout << result.dump() << '\n';
            return 0;
        }
        if (operation == "recover") {
            step = "reading the sealed grant and receipt after restart";
            if (personRef.empty()) {
                delivery.bind(fixturePerson, channel, credential);
            } else {
                const auto resumed = handoff.resume(fixturePerson, channel);
                check(resumed.sealed && resumed.frozen &&
                      resumed.residualCommandIds == std::vector<std::string>{command},
                      "The handoff did not record both barriers and their residual input");
            }
            const auto recovered = delivery.reconcile(fixturePerson, invitation.botId, channel, command);
            const auto result = appliedResult(recovered, invitation);
            check(recovered.sealed, "Flock failed to observe the T3 seal");
            const auto remote = gate->readReceipt(recovered.ref, credential);
            check(remote.kind == RootGateReply::Kind::Found && remote.receipt &&
                  sameReceipt(*recovered.receipt, *remote.receipt), "Sealed T3 receipt differs from Flock's persisted receipt");
            std::cout << result.dump() << '\n';
            return 0;
        }
        throw CheckFailure("Use prepare, deliver or recover");
    } catch (const CheckFailure& failure) {
        std::cerr << failure.what() << '\n';
    } catch (const std::exception&) {
        std::cerr << "Live RootDelivery fixture failed while " << step << '\n';
    }
    return 1;
}
