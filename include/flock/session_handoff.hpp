#pragma once

#include "flock/root_delivery.hpp"

namespace flockv0 {

// The mapping is configured by the Flock operator. The T3 reference is
// obtained from the authenticated T3 invitation, never from the caller.
using T3PersonBinding = std::function<bool(std::string_view environment,
                                          std::string_view t3PersonRef, const Person&)>;

struct SessionHandoffView {
    Id channel;
    ImmutableOrigin origin;
    bool coordination = false, opening = true, sealed = false, frozen = false;
    std::optional<RootDeliveryView> reply;
    std::optional<CoordinationRequest> permission;
    std::optional<Id> epoch;
    Id generation = 1;
    std::vector<std::string> residualCommandIds;
    std::optional<std::string> sealMarker;
    bool releaseAcknowledged = false;
    Id eventCursor = 0;
    Json latestDecision = nullptr;
    Json pendingQuestion = nullptr;
    Json work = Json::array();
    std::vector<std::string> admittedChildIds;
};

class SessionHandoff {
public:
    using Peers = std::function<std::vector<PeerBot>(std::string_view)>;
    SessionHandoff(Store& store, RootGate& gate, T3PersonBinding binding, Runtime* runtime = nullptr, Peers peers = {})
        : store_(store), gate_(gate), bindsPerson_(std::move(binding)), runtime_(runtime), peers_(std::move(peers)) {}

    SessionHandoffView accept(const Person&, const RootInvitationRef&, std::string_view credential);
    SessionHandoffView resume(const Person&, Id channel);
    SessionHandoffView freeze(const Person&, Id channel);
    SessionHandoffView answer(const Person&, Id channel, Id generation, std::string_view eventId,
                              std::string_view text);
    SessionHandoffView answerWork(const Person&, Id channel, Id generation, std::string_view delegation,
                                  std::string_view stage, Id wait, std::string_view text);
    SessionHandoffView view(const Person&, Id channel) const;
    RootDeliveryView consult(const Person&, Id channel, Model&, const IntakeResolver&,
                             const Runtime::SoulLoader&);
    SessionHandoffView step(const Person&, Id channel, Model&, const IntakeResolver&,
                            const Runtime::SoulLoader&);
    std::vector<std::pair<Id,std::string>> pending(Id after) const;

private:
    Store& store_;
    RootGate& gate_;
    T3PersonBinding bindsPerson_;
    Runtime* runtime_;
    Peers peers_;
};

}
