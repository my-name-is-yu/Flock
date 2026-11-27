#pragma once

#include "flock/types.hpp"
#include "flock/json.hpp"

#include <optional>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>
namespace flockv0 {
struct ConversationInvitationEntry {
    std::string environmentId, threadId, title, inviterBotId, inviterName, note;
    Id depth;
};

struct ConversationWakeEntry {
    std::string environmentId, threadId, title, reason;
    Id sourceMessageId;
};

struct ConversationBotMessageEntry {
    std::string fromBotId, fromName;
    Id depth;
};

struct ConversationMessageEntry {
    std::variant<std::monostate, ConversationInvitationEntry, ConversationWakeEntry, ConversationBotMessageEntry> detail;
};

enum class ConversationReplyRoute { reply, error, bot_paused, session_access, session_left, channel_access, access };
enum class ConversationReplyStatus { done, working, stopping, needs_you };

struct ConversationReplyEntry {
    Id replyToMessageId;
    std::string text;
    ConversationReplyStatus status = ConversationReplyStatus::done;
    std::optional<ConversationReplyRoute> route;
    std::optional<Id> taskQuestionMessageId;
    Json actions, suggestions, artifacts;
};

Json encodeConversationInvitation(const ConversationInvitationEntry&);
Json encodeConversationWake(const ConversationWakeEntry&);
Json encodeConversationBotMessage(const ConversationBotMessageEntry&);
ConversationMessageEntry decodeConversationMessage(Id, const Json&);
ConversationReplyEntry decodeConversationReply(Id, const Json&);
}
