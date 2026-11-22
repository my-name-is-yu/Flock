#pragma once

#include "flock/access.hpp"

#include <variant>

namespace flockv0 {

struct ExactRunTarget {
    std::string environmentId, projectId, threadId, runId;
    bool operator==(const ExactRunTarget&) const = default;
};

struct InvitationRef {
    ExactRunTarget target;
    std::string requestId, sourceMessageId, botId;
    bool operator==(const InvitationRef&) const = default;
};

struct SharedSessionContext {
    ExactRunTarget target;
    std::string itemId, text;
    bool truncated = false;
    bool operator==(const SharedSessionContext&) const = default;
};

struct CoordinationRequest {
    Id expectedEpoch;
    std::string goal;
    bool allowDelegation = false;
    bool operator==(const CoordinationRequest&) const = default;
};

struct VerifiedInvitation {
    InvitationRef ref;
    std::string personId;
    std::vector<SharedSessionContext> context;
    std::optional<CoordinationRequest> coordination;
};

struct ImmutableOrigin {
    InvitationRef invitation;
    std::string personId;
    bool operator==(const ImmutableOrigin&) const = default;
};

// Provenance and a bounded view of what the person shared.
struct ReturnChannel {
    Id id;
    ImmutableOrigin origin;
    std::vector<SharedSessionContext> context;
};

struct ConsultPermit {
    ReturnChannel channel;
    // Every attempt to deliver this request's reply must use this command ID.
    std::string commandId;
};

struct CoordinationBinding {
    std::string activationId;
    ImmutableOrigin origin;
    CoordinationRequest request;
    bool operator==(const CoordinationBinding&) const = default;
};

struct CoordinationGrant {
    CoordinationBinding binding;
    Id epoch;
    bool operator==(const CoordinationGrant&) const = default;
};

struct CoordinationActivation {
    ReturnChannel channel;
    CoordinationBinding binding;
    // Opening until a matching acknowledgment has been persisted. The observed
    // grant is historical; live admission and seal checks belong to T3.
    std::optional<CoordinationGrant> acknowledgment;
};

using Participation = std::variant<ConsultPermit, CoordinationActivation>;

// Implemented by the trusted T3 ingress. Resolve the authenticated invitation
// and active grant from T3, including the mapped Flock person and Bot identity.
class ParticipationAuthority {
public:
    virtual ~ParticipationAuthority() = default;
    virtual std::optional<VerifiedInvitation> invitation(const InvitationRef&) = 0;
    virtual std::optional<CoordinationGrant> grant(const CoordinationBinding&) = 0;
};

class ParticipationStore {
public:
    explicit ParticipationStore(Store& store) : store_(store) {}

    Participation open(const Person& authenticatedPerson, const InvitationRef&, ParticipationAuthority&);
    ConsultPermit consult(const Person& authenticatedPerson, std::string_view authenticatedBot, Id channel) const;
    CoordinationActivation coordination(const Person& authenticatedPerson, Id channel) const;
    CoordinationActivation observeGrant(const Person& authenticatedPerson, Id channel, ParticipationAuthority&);

private:
    Store& store_;
};

}
