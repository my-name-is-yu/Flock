#pragma once

#include "flock/participation.hpp"

namespace flockv0 {

struct RootGateCapabilities {
    std::string environmentId, serverVersion;
    Id protocolVersion = 0;
    bool exactTarget = false, grantSeal = false, receiptLookup = false, senderAttribution = false;
    bool strictPinnedWorktree = false;
    bool operator==(const RootGateCapabilities&) const = default;
};

struct RootGrantRef {
    ExactRunTarget target;
    std::string grantId;
    Id epoch = 0;
    bool operator==(const RootGrantRef&) const = default;
};

struct RootGrant {
    RootGrantRef ref;
    std::string sourceMessageId, botId;
    std::optional<CoordinationRequest> coordination;
    bool sealed = false;
};

struct RootSealStatus {
    RootGrant grant;
    std::vector<std::string> residualCommandIds;
    std::optional<std::string> sealMarker;
    bool releaseAcknowledged = false;
};

struct RootGrantView {
    bool verified = false, sealed = false, frozen = false;
    Id generation = 1;
    std::vector<std::string> residualCommandIds;
    std::optional<std::string> sealMarker;
    bool releaseAcknowledged = false;
};

struct RootCommandRef {
    RootGrantRef grant;
    std::string commandId;
    bool operator==(const RootCommandRef&) const = default;
};

struct RootCommand {
    RootCommandRef ref;
    std::string text;
};

enum class RootDeliveryState { Prepared, Accepted, ProviderPending, Applied, Failed, Rejected, Unknown };
std::string_view rootDeliveryStateName(RootDeliveryState);

struct RootReceipt {
    RootCommandRef ref;
    std::string botId;
    RootDeliveryState observation = RootDeliveryState::Unknown;
    std::string nativeTurnId, nativeItemId;
};

struct RootGateReply {
    enum class Kind { Found, AuthoritativelyAbsent, Rejected, Unknown } kind = Kind::Unknown;
    std::optional<RootReceipt> receipt;
    std::string reason;
};

struct RootInvitationRef {
    ExactRunTarget target;
    std::string requestId;
    bool operator==(const RootInvitationRef&) const = default;
};

struct RootInvitation {
    InvitationRef ref;
    std::string personRef;
    std::vector<SharedSessionContext> context;
    std::optional<CoordinationRequest> coordination;
};

struct RootProgressEvent {
    std::string id;
    Id position;
    std::string kind, status, summary;
};

struct RootProgressPage {
    ExactRunTarget target;
    Id cursor;
    std::vector<RootProgressEvent> events;
};

class RootGate {
public:
    virtual ~RootGate() = default;
    virtual std::optional<RootGateCapabilities> capabilities(std::string_view environmentId) = 0;
    virtual std::optional<RootGrant> readGrant(const RootGrantRef&, std::string_view credential) = 0;
    virtual RootGateReply readReceipt(const RootCommandRef&, std::string_view credential) = 0;
    virtual RootGateReply send(const RootCommand&, std::string_view credential) = 0;
    virtual std::optional<RootInvitation> readInvitation(const RootInvitationRef&, std::string_view) { return {}; }
    virtual std::optional<RootGrant> activateInvitation(const RootInvitationRef&, std::string_view, std::string_view) { return {}; }
    virtual std::optional<RootSealStatus> sealStatus(const RootGrantRef&, std::string_view) { return {}; }
    virtual std::optional<RootSealStatus> confirmFreeze(const RootGrantRef&, std::string_view, Id,
                                                       std::string_view) { return {}; }
    virtual std::optional<RootProgressPage> events(const RootGrantRef&, Id, std::string_view) { return {}; }
    virtual std::optional<std::string> inputCommit(const RootGrantRef&, std::string_view) { return {}; }
};

struct RootDeliveryView {
    RootCommandRef ref;
    std::string botId, evidenceRef, serverVersion;
    RootDeliveryState state = RootDeliveryState::Prepared;
    std::optional<RootReceipt> receipt;
    bool sealed = false;
};

class RootDelivery {
public:
    RootDelivery(Store& store, RootGate& gate) : store_(store), gate_(gate) {}

    void bind(const Person&, Id channel, std::string_view credential);
    RootGrantView observeGrant(const Person&, Id channel);
    std::optional<std::string> inputCommit(const Person&, Id channel);
    RootGrantView freeze(const Person&, Id channel);
    RootDeliveryView prepare(const Person&, std::string_view bot, Id channel, std::string_view commandId,
                             std::string_view text, std::string_view evidenceRef);
    RootDeliveryView deliver(const Person&, std::string_view bot, Id channel, std::string_view commandId);
    RootDeliveryView reconcile(const Person&, std::string_view bot, Id channel, std::string_view commandId);
    RootDeliveryView view(const Person&, std::string_view bot, Id channel, std::string_view commandId) const;

private:
    RootDeliveryView advance(const Person&, std::string_view bot, Id channel, std::string_view commandId,
                             bool allowSend);
    Store& store_;
    RootGate& gate_;
};

}
