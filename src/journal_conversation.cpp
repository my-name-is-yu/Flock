#include "database.hpp"
#include "journal_storage.hpp"
#include "journal_conversation.hpp"
#include "journal.hpp"
#include "record_decode.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
using namespace std::literals;

constexpr std::array replyRoutes{
    std::pair{ConversationReplyRoute::reply, "reply"sv},
    std::pair{ConversationReplyRoute::error, "error"sv},
    std::pair{ConversationReplyRoute::bot_paused, "bot_paused"sv},
    std::pair{ConversationReplyRoute::session_access, "session_access"sv},
    std::pair{ConversationReplyRoute::session_left, "session_left"sv},
    std::pair{ConversationReplyRoute::channel_access, "channel_access"sv},
    std::pair{ConversationReplyRoute::access, "access"sv},
};

constexpr std::array replyStatuses{
    std::pair{ConversationReplyStatus::done, "done"sv},
    std::pair{ConversationReplyStatus::working, "working"sv},
    std::pair{ConversationReplyStatus::stopping, "stopping"sv},
    std::pair{ConversationReplyStatus::needs_you, "needs_you"sv},
};

ConversationInvitationEntry readInvitation(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "environmentId"),
            row.get<std::string>(data, "threadId"),
            row.get<std::string>(data, "title"),
            row.get<std::string>(data, "inviterBotId"),
            row.get<std::string>(data, "inviterName"),
            row.get<std::string>(data, "note"),
            row.get<Id>(data, "depth")};
}

ConversationBotMessageEntry readBotMessage(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "fromBotId"), row.get<std::string>(data, "fromName"), row.get<Id>(data, "depth")};
}

ConversationWakeEntry readWake(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "environmentId"), row.get<std::string>(data, "threadId"),
            row.get<std::string>(data, "title"),         row.get<std::string>(data, "reason"),
            row.get<Id>(data, "sourceMessageId"),        row.optional<Id>(data, "taskId").value_or(0)};
}
}

Json encodeConversationInvitation(const ConversationInvitationEntry& entry) {
    return {
        {"environmentId", entry.environmentId}, {"threadId", entry.threadId}, {"title", entry.title}, {"inviterBotId", entry.inviterBotId},
        {"inviterName", entry.inviterName},     {"note", entry.note},         {"depth", entry.depth}};
}

Json encodeConversationWake(const ConversationWakeEntry& entry) {
    Json data{{"environmentId", entry.environmentId},
              {"threadId", entry.threadId},
              {"title", entry.title},
              {"reason", entry.reason},
              {"sourceMessageId", entry.sourceMessageId}};
    if (entry.taskId) data["taskId"] = entry.taskId;
    return data;
}

Json encodeConversationBotMessage(const ConversationBotMessageEntry& entry) {
    return {{"fromBotId", entry.fromBotId}, {"fromName", entry.fromName}, {"depth", entry.depth}};
}

ConversationMessageEntry decodeConversationMessage(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [](const Json& payload, const RecordDecoder& row) {
        const auto invite = row.find(payload, "sessionInvitation");
        const auto wake = row.find(payload, "sessionWake");
        const auto bot = row.find(payload, "botMessage");
        if ((invite != payload.end()) + (wake != payload.end()) + (bot != payload.end()) > 1)
            row.fail("sessionWake", "a conversation message has at most one origin");
        if (invite != payload.end()) return ConversationMessageEntry{row.nested("sessionInvitation").decodeClosed(*invite, readInvitation)};
        if (wake != payload.end()) return ConversationMessageEntry{row.nested("sessionWake").decodeClosed(*wake, readWake)};
        if (bot != payload.end()) return ConversationMessageEntry{row.nested("botMessage").decodeClosed(*bot, readBotMessage)};
        return ConversationMessageEntry{};
    });
}

ConversationReplyEntry decodeConversationReply(Id id, const Json& data) {
    const auto rowId = std::to_string(id);
    return RecordDecoder("journal", rowId).decodeClosed(data, [id](const Json& payload, const RecordDecoder& row) {
        if (row.get<Id>(payload, "messageId") != id) row.fail("messageId", "does not match the journal ID");
        const auto status = enumValue(replyStatuses, row.get<std::string>(payload, "status"));
        if (!status) row.fail("status", "unknown conversation reply status");
        ConversationReplyEntry entry{
            .replyToMessageId = row.get<Id>(payload, "replyToMessageId"), .text = row.get<std::string>(payload, "text"), .status = *status};
        if (const auto route = row.optional<std::string>(payload, "route")) {
            entry.route = enumValue(replyRoutes, *route);
            if (!entry.route) row.fail("route", "unknown conversation reply route");
        }
        entry.taskQuestionMessageId = row.optional<Id>(payload, "taskQuestionMessageId");
        if (const auto actions = row.find(payload, "actions"); actions != payload.end()) entry.actions = row.array(*actions, "actions");
        if (const auto suggestions = row.optional<std::vector<std::string>>(payload, "suggestions")) entry.suggestions = *suggestions;
        if (const auto artifacts = row.find(payload, "artifacts"); artifacts != payload.end())
            entry.artifacts = row.array(*artifacts, "artifacts");
        return entry;
    });
}

Id JournalStorage::journalConversationMessage(std::string_view bot, const ConversationMessageEntry& entry) {
    auto data = Json::object();
    if (const auto* invite = std::get_if<ConversationInvitationEntry>(&entry.detail))
        data["sessionInvitation"] = encodeConversationInvitation(*invite);
    else if (const auto* wake = std::get_if<ConversationWakeEntry>(&entry.detail))
        data["sessionWake"] = encodeConversationWake(*wake);
    else if (const auto* bot = std::get_if<ConversationBotMessageEntry>(&entry.detail))
        data["botMessage"] = encodeConversationBotMessage(*bot);
    return database.writeJournal(std::nullopt, JournalKind::conversation_message, data, {}, bot);
}

Id JournalStorage::journalConversationReply(std::optional<Id> task, std::string_view bot, const ConversationReplyEntry& entry) {
    const auto status = enumName(replyStatuses, entry.status);
    Json data{{"replyToMessageId", entry.replyToMessageId}, {"text", entry.text}, {"status", status}};
    std::optional<std::string_view> route;
    if (entry.route) data["route"] = route.emplace(enumName(replyRoutes, *entry.route));
    if (entry.taskQuestionMessageId) data["taskQuestionMessageId"] = *entry.taskQuestionMessageId;
    if (!entry.actions.is_null()) data["actions"] = entry.actions;
    if (!entry.suggestions.is_null()) data["suggestions"] = entry.suggestions;
    if (!entry.artifacts.is_null()) data["artifacts"] = entry.artifacts;
    const auto reply = database.writeJournal(task, JournalKind::conversation_reply, data,
                                             {.replyToMessageId = entry.replyToMessageId,
                                              .taskQuestionMessageId = entry.taskQuestionMessageId,
                                              .conversationRoute = route,
                                              .conversationStatus = status},
                                             bot);
    database.exec("UPDATE journal SET data=json_set(data,'$.messageId',id) WHERE id=?", {reply});
    return reply;
}
}
