#include "journal_storage.hpp"
#include "journal_peer.hpp"
#include "journal.hpp"
#include "record_decode.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "database.hpp"

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
Json encodePeerResultReference(const PeerResultReference& ref) {
    return {{"environmentId", ref.environmentId},
            {"threadId", ref.threadId},
            {"runId", ref.runId},
            {"itemId", ref.itemId},
            {"link", ref.link},
            {"status", ref.status}};
}

PeerResultReference readPeerResultReference(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "threadId"), row.get<std::string>(data, "runId"),
            row.get<std::string>(data, "itemId"),        row.get<std::string>(data, "link"),     row.get<std::string>(data, "status")};
}

ForwardedPeerMessage readForwardedPeerMessage(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "text"), row.get<Seconds>(data, "time")};
}

BotMessageEntry readBotMessage(const Json& data, const RecordDecoder& row) {
    BotMessageEntry entry{.exchangeId = row.get<std::string>(data, "exchangeId"),
                          .messageId = row.get<std::string>(data, "messageId"),
                          .fromBotId = row.get<std::string>(data, "fromBotId"),
                          .toBotId = row.get<std::string>(data, "toBotId"),
                          .messageType = row.get<std::string>(data, "messageType"),
                          .requestKind = row.get<std::string>(data, "requestKind"),
                          .text = row.get<std::string>(data, "text"),
                          .replyTo = row.optional<std::string>(data, "replyTo"),
                          .status = row.optional<std::string>(data, "status")};
    if (const auto forwarded = row.find(data, "forwardedMessage"); forwarded != data.end())
        entry.forwarded = row.nested("forwardedMessage").decodeClosed(*forwarded, readForwardedPeerMessage);
    if (const auto evidence = row.find(data, "evidence"); evidence != data.end()) {
        auto reader = row.nested("evidence");
        for (const auto& ref : reader.array(*evidence))
            entry.evidence.push_back(reader.nested(std::to_string(entry.evidence.size())).decodeClosed(ref, readPeerResultReference));
    }
    return entry;
}

SessionExcerptEntry readSessionExcerpt(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "threadId"),
            row.get<std::string>(data, "runId"),         row.get<std::string>(data, "itemId"),
            row.get<std::string>(data, "text"),          row.get<std::string>(data, "nextCursor"),
            row.get<std::size_t>(data, "nextOffset"),    row.get<bool>(data, "more"),
            row.get<bool>(data, "itemTruncated")};
}
}

Json encodeForwardedPeerMessage(const ForwardedPeerMessage& message) {
    return {{"text", message.text}, {"time", message.time}};
}

Json encodeBotMessage(const BotMessageEntry& entry) {
    Json data{{"exchangeId", entry.exchangeId},
              {"messageId", entry.messageId},
              {"fromBotId", entry.fromBotId},
              {"toBotId", entry.toBotId},
              {"messageType", entry.messageType},
              {"requestKind", entry.requestKind},
              {"text", entry.text}};
    if (entry.replyTo) data["replyTo"] = *entry.replyTo;
    if (entry.forwarded) data["forwardedMessage"] = encodeForwardedPeerMessage(*entry.forwarded);
    if (entry.status) data["status"] = *entry.status;
    if (!entry.evidence.empty()) {
        auto& evidence = data["evidence"] = Json::array();
        for (const auto& ref : entry.evidence) evidence.push_back(encodePeerResultReference(ref));
    }
    return data;
}

BotMessageEntry decodeBotMessage(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, readBotMessage);
}

Json encodeSessionExcerpt(const SessionExcerptEntry& excerpt) {
    return {{"environmentId", excerpt.environmentId},
            {"threadId", excerpt.threadId},
            {"runId", excerpt.runId},
            {"itemId", excerpt.itemId},
            {"text", excerpt.text},
            {"nextCursor", excerpt.nextCursor},
            {"nextOffset", excerpt.nextOffset},
            {"more", excerpt.more},
            {"itemTruncated", excerpt.itemTruncated}};
}

Json encodePeerEvidence(const PeerEvidenceEntry& entry) {
    auto data = encodeSessionExcerpt(entry.excerpt);
    data["exchangeId"] = entry.exchangeId;
    data["messageId"] = entry.messageId;
    return data;
}

PeerEvidenceEntry decodePeerEvidence(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [](const Json& payload, const RecordDecoder& row) {
        row.find(payload, "usage");
        return PeerEvidenceEntry{row.get<std::string>(payload, "exchangeId"), row.get<std::string>(payload, "messageId"),
                                 readSessionExcerpt(payload, row)};
    });
}

Id JournalStorage::journal(Id task, const PeerEvidenceEntry& entry, const Json& usage) {
    auto data = encodePeerEvidence(entry);
    if (!usage.empty()) data["usage"] = usage;
    return database.writeJournal(task, JournalKind::read_bot_evidence, data,
                                 {.exchangeId = entry.exchangeId, .peerMessageId = entry.messageId});
}

Id JournalStorage::journal(Id task, const GitPrFollowupEntry& entry) {
    return database.writeJournal(task, JournalKind::git_pr_followup_requested,
                                 {{"operationId", entry.operationId}, {"eventId", entry.eventId}, {"messageId", entry.messageId}},
                                 {.gitEventId = entry.eventId, .operationId = entry.operationId});
}
}
