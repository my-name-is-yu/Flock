#pragma once

#include "flock/types.hpp"
#include "flock/json.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace flockv0 {
struct ForwardedPeerMessage {
    std::string text;
    Seconds time;
};

struct PeerResultReference {
    std::string environmentId, threadId, runId, itemId, link, status;
};

struct BotMessageEntry {
    std::string exchangeId, messageId, fromBotId, toBotId, messageType, requestKind, text;
    std::optional<std::string> replyTo;
    std::optional<ForwardedPeerMessage> forwarded;
    std::optional<std::string> status;
    std::vector<PeerResultReference> evidence;
};

struct SessionExcerptEntry {
    std::string environmentId, threadId, runId, itemId, text, nextCursor;
    std::size_t nextOffset;
    bool more, itemTruncated;
};

struct PeerEvidenceEntry {
    std::string exchangeId, messageId;
    SessionExcerptEntry excerpt;
};

struct GitPrFollowupEntry {
    std::string_view operationId, eventId;
    Id messageId;
};

Json encodeForwardedPeerMessage(const ForwardedPeerMessage&);
Json encodeBotMessage(const BotMessageEntry&);
BotMessageEntry decodeBotMessage(Id, const Json&);
Json encodeSessionExcerpt(const SessionExcerptEntry&);
Json encodePeerEvidence(const PeerEvidenceEntry&);
PeerEvidenceEntry decodePeerEvidence(Id, const Json&);
}
