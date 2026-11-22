#include "flock/root_delivery.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <openssl/evp.h>
#include <tuple>

namespace flockv0 {
namespace {

const auto stateNames = std::array<std::string_view, 7>{
    "prepared", "accepted", "providerPending", "applied", "failed", "rejected", "unknown"
};

bool finalState(RootDeliveryState state) {
    return state == RootDeliveryState::Applied || state == RootDeliveryState::Failed ||
           state == RootDeliveryState::Rejected;
}

bool supported(const RootGateCapabilities& capabilities, std::string_view environment) {
    return capabilities.environmentId == environment && !capabilities.serverVersion.empty() &&
           capabilities.protocolVersion == 1 && capabilities.exactTarget && capabilities.grantSeal &&
           capabilities.receiptLookup && capabilities.senderAttribution;
}

std::string fingerprint(std::string_view text) {
    std::array<unsigned char, 32> digest;
    unsigned size = 0;
    if (EVP_Digest(text.data(), text.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size()) throw Conflict("Could not fingerprint the root payload");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

bool validCredential(std::string_view credential) {
    return credential.size() == 43 && credential.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == credential.npos;
}

struct Binding {
    RootGrantRef ref;
    std::string sourceMessageId, botId, consultCommandId;
    std::optional<CoordinationRequest> coordination;
};

Binding bindingFor(const Participation& participation) {
    if (const auto* consult = std::get_if<ConsultPermit>(&participation)) {
        const auto& invite = consult->channel.origin.invitation;
        return {{invite.target, invite.requestId, 0}, invite.sourceMessageId, invite.botId,
                consult->commandId, {}};
    }
    const auto& activation = std::get<CoordinationActivation>(participation);
    if (!activation.acknowledgment) throw Conflict("Coordination is still opening");
    const auto& invite = activation.channel.origin.invitation;
    return {{invite.target, activation.binding.activationId, activation.acknowledgment->epoch},
            invite.sourceMessageId, invite.botId, {}, activation.binding.request};
}

void authorizeCommand(const Binding& binding, std::string_view bot, std::string_view command) {
    if (binding.botId != bot || command.empty() || command.size() > 1024 ||
        (!binding.consultCommandId.empty() && command != binding.consultCommandId)) {
        throw Conflict("Root command is outside the Session permission");
    }
}

bool matches(const Binding& binding, const RootGrant& grant) {
    return grant.ref == binding.ref && grant.sourceMessageId == binding.sourceMessageId &&
           grant.botId == binding.botId && grant.coordination == binding.coordination;
}

struct RemoteBinding {
    std::string serverVersion;
    bool sealed, frozen;
};

RemoteBinding remoteBinding(sqlite3* db, Id channel, const Binding& binding) {
    Statement row(db, R"SQL(
SELECT server_version, sealed, frozen FROM root_gate_channels
WHERE channel_id=? AND grant_id=? AND epoch=?
)SQL", {channel, binding.ref.grantId, binding.ref.epoch});
    if (!row.next()) throw Conflict("Session has no verified T3 grant");
    return {row.string(0), row.number(1) != 0, row.number(2) != 0};
}

struct DeliveryRecord {
    RootDeliveryView view;
    std::string text, fingerprint;
    bool frozen;
};

RootGrantView grantView(sqlite3* db, Id channel) {
    Statement row(db, R"SQL(
SELECT sealed,frozen,generation,residual_commands,seal_marker,release_acknowledged
FROM root_gate_channels WHERE channel_id=?
)SQL", {channel});
    if (!row.next()) throw Conflict("Session has no verified T3 grant");
    const auto marker = row.string(4);
    return {false, row.number(0) != 0, row.number(1) != 0, row.number(2),
            row.json(3).get<std::vector<std::string>>(),
            marker.empty() ? std::nullopt : std::optional(marker), row.number(5) != 0};
}

DeliveryRecord loadDelivery(sqlite3* db, Id channel, const Binding& binding,
                            std::string_view commandId) {
    const auto remote = remoteBinding(db, channel, binding);
    Statement row(db, R"SQL(
SELECT text,payload_sha256,evidence_ref,state,receipt_state,native_turn_id,native_item_id
FROM root_deliveries WHERE channel_id=? AND command_id=?
)SQL", {channel, commandId});
    if (!row.next()) throw Conflict("Unknown root command");
    RootDeliveryView view{
        {binding.ref, std::string(commandId)}, binding.botId, row.string(2), remote.serverVersion,
        static_cast<RootDeliveryState>(row.number(3)), {}, remote.sealed
    };
    if (!row.string(4).empty()) {
        view.receipt = RootReceipt{
            view.ref, binding.botId, static_cast<RootDeliveryState>(row.number(4)),
            row.string(5), row.string(6)
        };
    }
    return {std::move(view), row.string(0), row.string(1), remote.frozen};
}

bool matches(const RootDeliveryView& view, const RootReceipt& receipt) {
    if (receipt.ref != view.ref || receipt.botId != view.botId) return false;
    switch (receipt.observation) {
    case RootDeliveryState::Accepted:
    case RootDeliveryState::ProviderPending:
    case RootDeliveryState::Unknown:
    case RootDeliveryState::Failed:
        return receipt.nativeTurnId.empty() && receipt.nativeItemId.empty();
    case RootDeliveryState::Applied:
        return !receipt.nativeTurnId.empty() && !receipt.nativeItemId.empty();
    default:
        return false;
    }
}

}

std::string_view rootDeliveryStateName(RootDeliveryState state) {
    return stateNames.at(static_cast<std::size_t>(state));
}

SecretRef participationCredential(Id channel) {
    return {"root-gate-channel-" + std::to_string(channel)};
}

void Store::Impl::initializeRootDeliverySchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS root_gate_channels(
 channel_id INTEGER PRIMARY KEY REFERENCES return_channels(id),
 grant_id TEXT NOT NULL,
 epoch INTEGER NOT NULL,
 protocol_version INTEGER NOT NULL,
 server_version TEXT NOT NULL,
 sealed INTEGER NOT NULL DEFAULT 0,
 generation INTEGER NOT NULL DEFAULT 1,
 residual_commands TEXT NOT NULL DEFAULT '[]',
 seal_marker TEXT NOT NULL DEFAULT '',
 release_acknowledged INTEGER NOT NULL DEFAULT 0,
 frozen INTEGER NOT NULL DEFAULT 0)
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS root_deliveries(
 channel_id INTEGER NOT NULL REFERENCES root_gate_channels(channel_id),
 command_id TEXT NOT NULL,
 text TEXT NOT NULL,
 payload_sha256 TEXT NOT NULL,
 evidence_ref TEXT NOT NULL,
 state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 6),
 send_attempts INTEGER NOT NULL DEFAULT 0,
 receipt_state INTEGER,
 native_turn_id TEXT NOT NULL DEFAULT '',
 native_item_id TEXT NOT NULL DEFAULT '',
 PRIMARY KEY(channel_id,command_id))
)SQL");
}

void RootDelivery::bind(const Person& person, Id channel, std::string_view credential) {
    if (!validCredential(credential)) throw Conflict("Invalid exact-run credential");
    auto& db = *store_.p_;
    const auto binding = [&] {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db, Transaction::Mode::read);
        auto result = bindingFor(db.authorizeParticipation(person, channel));
        tx.commit();
        return result;
    }();
    const auto caps = gate_.capabilities(binding.ref.target.environmentId);
    if (!caps || !supported(*caps, binding.ref.target.environmentId))
        throw Conflict("T3 exact-run capability could not be verified");
    const auto grant = gate_.readGrant(binding.ref, credential);
    if (!grant || !matches(binding, *grant)) throw Conflict("T3 grant could not be verified");

    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto current = bindingFor(db.authorizeParticipation(person, channel));
    if (!matches(current, *grant)) throw Conflict("Session permission changed during binding");
    auto row = db.query(R"SQL(
SELECT grant_id,epoch,server_version FROM root_gate_channels WHERE channel_id=?
)SQL", {channel});
    if (row.next()) {
        if (row.string(0) != current.ref.grantId || row.number(1) != current.ref.epoch ||
            row.string(2) != caps->serverVersion || db.secrets.read(participationCredential(channel)) != credential)
            throw Conflict("A different T3 grant is already bound");
        if (grant->sealed) db.exec("UPDATE root_gate_channels SET sealed=1 WHERE channel_id=?", {channel});
        tx.commit();
        return;
    }
    if (db.query("SELECT 1 FROM session_handoffs WHERE channel_id=?", {channel}).next()) {
        if (db.secrets.read(participationCredential(channel)) != credential)
            throw Conflict("A different invitation credential is recorded");
    } else db.secrets.insert(participationCredential(channel), credential);
    db.exec(R"SQL(
INSERT INTO root_gate_channels(channel_id,grant_id,epoch,protocol_version,server_version,sealed)
VALUES(?,?,?,?,?,?)
)SQL", {channel, current.ref.grantId, current.ref.epoch, caps->protocolVersion,
         caps->serverVersion, static_cast<int>(grant->sealed)});
    tx.commit();
}

std::optional<std::string> RootDelivery::inputCommit(const Person& person, Id channel) {
    auto& db = *store_.p_;
    std::string credential, expectedVersion;
    RootGrantRef ref;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db, Transaction::Mode::read);
        const auto binding = bindingFor(db.authorizeParticipation(person, channel));
        if (!binding.coordination || !binding.coordination->allowDelegation) return {};
        const auto remote = remoteBinding(db.db, channel, binding);
        if (remote.sealed || remote.frozen) return {};
        ref = binding.ref;
        expectedVersion = remote.serverVersion;
        credential = db.secrets.read(participationCredential(channel));
        tx.commit();
    }
    const auto caps = gate_.capabilities(ref.target.environmentId);
    if (!caps || !supported(*caps,ref.target.environmentId) || !caps->strictPinnedWorktree ||
        caps->serverVersion != expectedVersion) return {};
    return gate_.inputCommit(ref, credential);
}

RootGrantView RootDelivery::observeGrant(const Person& person, Id channel) {
    auto& db = *store_.p_;
    const auto [binding, version, credential] = [&] {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db, Transaction::Mode::read);
        auto binding = bindingFor(db.authorizeParticipation(person, channel));
        auto remote = remoteBinding(db.db, channel, binding);
        auto credential = db.secrets.read(participationCredential(channel));
        tx.commit();
        return std::tuple{std::move(binding), std::move(remote.serverVersion), std::move(credential)};
    }();
    const auto caps = gate_.capabilities(binding.ref.target.environmentId);
    const auto remote = caps && supported(*caps, binding.ref.target.environmentId) && caps->serverVersion == version
        ? gate_.sealStatus(binding.ref, credential) : std::nullopt;

    auto snapshot = [&] {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        const auto current = bindingFor(db.authorizeParticipation(person, channel));
        if (current.ref != binding.ref) throw Conflict("Session permission has changed");
        auto snapshot = grantView(db.db, channel);
        if (remote && matches(current, remote->grant) && (!snapshot.sealed || remote->grant.sealed) &&
            remote->sealMarker.has_value() == remote->grant.sealed &&
            (!snapshot.sealMarker || (snapshot.sealMarker == remote->sealMarker &&
                                      snapshot.residualCommandIds == remote->residualCommandIds))) {
            if (remote->grant.sealed) {
                db.sharedRedactor().requirePublic({*remote->sealMarker, remote->residualCommandIds});
                SecretRedactor({credential}).requirePublic({*remote->sealMarker, remote->residualCommandIds});
                db.exec(R"SQL(
UPDATE root_gate_channels SET sealed=1,generation=generation+(NOT frozen),frozen=1,
 residual_commands=?,seal_marker=?,release_acknowledged=MAX(release_acknowledged,?)
 WHERE channel_id=?
)SQL", {remote->residualCommandIds, *remote->sealMarker, static_cast<int>(remote->releaseAcknowledged), channel});
                snapshot = grantView(db.db, channel);
            }
            snapshot.verified = true;
        }
        tx.commit();
        return snapshot;
    }();
    if (snapshot.verified && snapshot.sealed && snapshot.frozen && snapshot.sealMarker &&
        !snapshot.releaseAcknowledged && binding.coordination) {
        const auto acknowledged = gate_.confirmFreeze(binding.ref, *snapshot.sealMarker,
                                                      snapshot.generation, credential);
        if (acknowledged && acknowledged->releaseAcknowledged && acknowledged->grant.sealed &&
            matches(binding, acknowledged->grant) && acknowledged->sealMarker == snapshot.sealMarker &&
            acknowledged->residualCommandIds == snapshot.residualCommandIds) {
            std::lock_guard lock(db.mutex);
            db.exec(R"SQL(
UPDATE root_gate_channels SET release_acknowledged=1
WHERE channel_id=? AND sealed AND frozen AND seal_marker=? AND generation=?
)SQL", {channel, *snapshot.sealMarker, snapshot.generation});
            snapshot = grantView(db.db, channel);
            snapshot.verified = true;
        }
    }
    return snapshot;
}

RootGrantView RootDelivery::freeze(const Person& person, Id channel) {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    db.authorizeParticipation(person, channel);
    db.exec("UPDATE root_gate_channels SET frozen=1,generation=generation+1 WHERE channel_id=? AND NOT frozen",
            {channel});
    auto result = grantView(db.db, channel);
    tx.commit();
    return result;
}

RootDeliveryView RootDelivery::prepare(const Person& person, std::string_view bot, Id channel,
                                       std::string_view commandId, std::string_view text,
                                       std::string_view evidenceRef) {
    if (text.empty() || text.size() > maxSessionPromptBytes || evidenceRef.size() > 1024)
        throw Conflict("Invalid root payload or evidence reference");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto binding = bindingFor(db.authorizeParticipation(person, channel));
    authorizeCommand(binding, bot, commandId);
    const auto remote = remoteBinding(db.db, channel, binding);
    const auto credential = db.secrets.read(participationCredential(channel));
    SecretRedactor({credential}).requirePublic({text, commandId, evidenceRef});
    db.sharedRedactor().requirePublic({text, commandId, evidenceRef});
    const auto hash = fingerprint(text);
    if (db.query("SELECT 1 FROM root_deliveries WHERE channel_id=? AND command_id=?", {channel, commandId}).next()) {
        const auto record = loadDelivery(db.db, channel, binding, commandId);
        if (record.fingerprint != hash || record.text != text || record.view.evidenceRef != evidenceRef)
            throw Conflict("The root command payload has changed");
        tx.commit();
        return record.view;
    }
    if (remote.sealed || remote.frozen) throw Conflict("Session participation has been revoked");
    db.exec(R"SQL(
INSERT INTO root_deliveries(channel_id,command_id,text,payload_sha256,evidence_ref,state)
VALUES(?,?,?,?,?,?)
)SQL", {channel, commandId, text, hash, evidenceRef, static_cast<int>(RootDeliveryState::Prepared)});
    auto result = loadDelivery(db.db, channel, binding, commandId).view;
    tx.commit();
    return result;
}

RootDeliveryView RootDelivery::view(const Person& person, std::string_view bot, Id channel,
                                    std::string_view commandId) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db, Transaction::Mode::read);
    const auto binding = bindingFor(db.authorizeParticipation(person, channel));
    authorizeCommand(binding, bot, commandId);
    auto result = loadDelivery(db.db, channel, binding, commandId).view;
    tx.commit();
    return result;
}

RootDeliveryView RootDelivery::advance(const Person& person, std::string_view bot, Id channel,
                                       std::string_view commandId, bool allowSend) {
    auto& db = *store_.p_;
    const auto [binding, record, credential] = [&] {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db, Transaction::Mode::read);
        auto binding = bindingFor(db.authorizeParticipation(person, channel));
        authorizeCommand(binding, bot, commandId);
        auto record = loadDelivery(db.db, channel, binding, commandId);
        auto credential = db.secrets.read(participationCredential(channel));
        tx.commit();
        return std::tuple{std::move(binding), std::move(record), std::move(credential)};
    }();
    if (record.view.state == RootDeliveryState::Applied || record.view.state == RootDeliveryState::Failed)
        return record.view;

    auto save = [&](RootDeliveryState state, const std::optional<RootReceipt>& receipt = {}) {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        auto existing = loadDelivery(db.db, channel, binding, commandId);
        if (!finalState(existing.view.state) || (existing.view.state == RootDeliveryState::Rejected && receipt)) {
            db.exec("UPDATE root_deliveries SET state=? WHERE channel_id=? AND command_id=?",
                    {static_cast<int>(state), channel, commandId});
            if (receipt) db.exec(R"SQL(
UPDATE root_deliveries SET receipt_state=?,native_turn_id=?,native_item_id=? WHERE channel_id=? AND command_id=?
)SQL", {static_cast<int>(receipt->observation), receipt->nativeTurnId, receipt->nativeItemId, channel, commandId});
        }
        auto result = loadDelivery(db.db, channel, binding, commandId).view;
        tx.commit();
        return result;
    };
    auto consume = [&](const RootGateReply& reply) {
        if (reply.kind == RootGateReply::Kind::Found && reply.receipt && matches(record.view, *reply.receipt)) {
            db.sharedRedactor().requirePublic({reply.receipt->nativeTurnId, reply.receipt->nativeItemId});
            SecretRedactor({credential}).requirePublic({reply.receipt->nativeTurnId, reply.receipt->nativeItemId});
            return save(reply.receipt->observation, reply.receipt);
        }
        return save(RootDeliveryState::Unknown);
    };

    const auto caps = gate_.capabilities(binding.ref.target.environmentId);
    if (!caps || !supported(*caps, binding.ref.target.environmentId) ||
        caps->serverVersion != record.view.serverVersion) return save(RootDeliveryState::Unknown);
    const auto grant = gate_.readGrant(binding.ref, credential);
    if (!grant || !matches(binding, *grant)) return save(RootDeliveryState::Unknown);
    if (grant->sealed) {
        std::lock_guard lock(db.mutex);
        db.exec("UPDATE root_gate_channels SET sealed=1 WHERE channel_id=?", {channel});
    }
    const auto receipt = gate_.readReceipt(record.view.ref, credential);
    if (receipt.kind != RootGateReply::Kind::AuthoritativelyAbsent) return consume(receipt);

    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        const auto current = bindingFor(db.authorizeParticipation(person, channel));
        authorizeCommand(current, bot, commandId);
        auto latest = loadDelivery(db.db, channel, current, commandId);
        if (latest.view.sealed && !latest.view.receipt && !finalState(latest.view.state)) {
            db.exec("UPDATE root_deliveries SET state=? WHERE channel_id=? AND command_id=?",
                    {static_cast<int>(RootDeliveryState::Rejected), channel, commandId});
            latest.view.state = RootDeliveryState::Rejected;
        }
        if (finalState(latest.view.state) || !allowSend || latest.frozen || latest.view.sealed ||
            latest.view.receipt.has_value()) {
            tx.commit();
            return latest.view;
        }
        db.sharedRedactor().requirePublic({latest.text});
        db.exec("UPDATE root_deliveries SET send_attempts=send_attempts+1 WHERE channel_id=? AND command_id=?",
                {channel, commandId});
        tx.commit();
    }
    const auto sent = gate_.send({record.view.ref, record.text}, credential);
    if (sent.kind != RootGateReply::Kind::Rejected) return consume(sent);
    const auto accepted = gate_.readReceipt(record.view.ref, credential);
    if (accepted.kind == RootGateReply::Kind::AuthoritativelyAbsent) return save(RootDeliveryState::Rejected);
    return consume(accepted);
}

RootDeliveryView RootDelivery::deliver(const Person& person, std::string_view bot, Id channel,
                                       std::string_view commandId) {
    return advance(person, bot, channel, commandId, true);
}

RootDeliveryView RootDelivery::reconcile(const Person& person, std::string_view bot, Id channel,
                                         std::string_view commandId) {
    return advance(person, bot, channel, commandId, false);
}

}
