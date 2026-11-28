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
    // Set when the wake reports on a task the Bot started from this conversation.
    Id taskId = 0;
};

// The person's message a Bot passed on word for word, as it read when it was sent.
struct ConversationForwardedMessage {
    Id messageId;
    std::string text;
    Id time;
};

struct ConversationBotMessageEntry {
    std::string fromBotId, fromName;
    Id depth;
    std::optional<ConversationForwardedMessage> forwarded;
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
