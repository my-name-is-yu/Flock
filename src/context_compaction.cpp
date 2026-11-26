#include "flock/conversations.hpp"
#include "conversation_context_storage.hpp"

#include "attachment_storage.hpp"
#include "conversation_storage.hpp"
#include "database.hpp"
#include "secret_storage.hpp"
#include "flock/limits.hpp"
#include "journal_conversation.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/secrets.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace flockv0 {
using conversation_storage::identifier;
using limits::conversation_context::maxExpandedBytes;

namespace {
constexpr std::size_t maxSummaryBytes = 4'000;

std::string boundedJsonText(std::string_view text, std::size_t limit, std::size_t bytes) {
    auto prefix = bounded(text, std::min(limit, bytes));
    if (Json(prefix).dump().size() - 2 <= bytes) return prefix;

    std::size_t low = 0, high = prefix.size() - 1;
    while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        if (Json(bounded(text, middle)).dump().size() - 2 <= bytes)
            low = middle;
        else
            high = middle - 1;
    }
    return bounded(text, low);
}

unsigned char asciiLower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

}

std::size_t ConversationContextStorage::boundExpandedContext(Json& turn) {
    std::size_t expandedBytes = 0;
    for (auto step = turn.rbegin(); step != turn.rend(); ++step) {
        auto& result = step->at("result");
        if (step->at("tool") != "expand_context" || !result.contains("messages")) continue;
        if (auto revision = result.find("_redactionRevision"); revision != result.end()) {
            (*step)["_redactionRevision"] = std::move(*revision);
            result.erase(revision);
        }
        const auto bytes = result.dump().size();
        if (expandedBytes + bytes > maxExpandedBytes) {
            result.erase("messages");
            result["evidenceOmitted"] = true;
        } else
            expandedBytes += bytes;
    }
    return expandedBytes;
}

ConversationContextStorage::ConversationContextStorage(Database& database, const SecretStorage& secretStorage,
                                                       const AttachmentStorage& attachments)
    : database_(database), secretStorage_(secretStorage), attachments_(attachments) {}

Json ConversationContextStorage::compactions(std::string_view bot, Id before, unsigned limit, Id beforeMessage) const {
    const auto scrub = secretStorage_.redactor(bot);
    auto rows = database_.query(
        "SELECT id,start_message_id,end_message_id,summary,original_count,created_at,created_by "
        "FROM conversation_compactions WHERE bot_id=? AND (?=0 OR id<?) AND (?=0 OR end_message_id<?) ORDER BY id DESC LIMIT ?",
        {bot, before, before, beforeMessage, beforeMessage, std::min(limit, 100u)});
    Json result = Json::array();
    while (rows.next())
        result.push_back({{"compactionId", rows.number(0)},
                          {"startMessageId", rows.number(1)},
                          {"endMessageId", rows.number(2)},
                          {"summary", boundedJsonText(scrub.text(rows.string(3)), maxSummaryBytes, maxExpandedBytes - 2)},
                          {"originalCount", rows.number(4)},
                          {"createdAt", rows.number(5)},
                          {"createdBy", rows.string(6)}});
    return result;
}

Json Conversations::compactions(std::string_view bot, Id before, unsigned limit) const {
    std::lock_guard lock(database_.mutex);
    return context_.compactions(bot, before, limit);
}

Json ConversationContextStorage::conversationHistory(const conversation::Message& message, const Json& summaries,
                                                     std::size_t expandedBytes) const {
    const auto scrub = secretStorage_.redactor(message.bot);
    std::size_t remaining = limits::conversation_context::maxBytes - expandedBytes - limits::conversation_context::emptyHistoryBytes;
    Json history = Json::array();
    Id boundary = 0;
    Json summary;
    if (!summaries.empty()) {
        const auto& saved = summaries.front();
        boundary = saved.at("endMessageId");
        summary = {{"kind", "conversation_compaction"},
                   {"compactionId", saved.at("compactionId")},
                   {"startMessageId", saved.at("startMessageId")},
                   {"endMessageId", boundary},
                   {"summary", saved.at("summary")}};
        remaining -= summary.dump().size();
    }
    auto rows = database_.query(R"SQL(
SELECT entry.id,entry.task_id,entry.kind,entry.data,entry.text,entry.wait_id FROM compactable_messages entry
LEFT JOIN channel_messages source ON source.message_id=entry.source_message_id
WHERE entry.bot_id=? AND entry.id>?
  AND (entry.kind NOT IN ('conversation_message','conversation_reply') OR entry.source_message_id<?)
  AND (?='' OR (source.scope_key=? AND entry.kind!='user_answer'))
ORDER BY entry.id DESC LIMIT 24
)SQL",
                                {message.bot, boundary, message.id, message.channelScope, message.channelScope});
    while (rows.next()) {
        const auto kind = rows.string(2);
        const auto id = rows.number(0);
        Json entry = {{"messageId", id}, {"kind", kind}, {"text", ""}};
        const bool isReply = kind == "conversation_reply";
        bool system = false;
        if (kind == "conversation_message") {
            const auto decoded = decodeConversationMessage(id, rows.json(3));
            system = !std::holds_alternative<std::monostate>(decoded.detail);
            if (const auto* wake = std::get_if<ConversationWakeEntry>(&decoded.detail))
                entry["sessionWake"] = scrub.record(encodeConversationWake(*wake));
            else if (const auto* invite = std::get_if<ConversationInvitationEntry>(&decoded.detail))
                entry["sessionInvitation"] = scrub.record(encodeConversationInvitation(*invite));
            if (message.channelScope.empty()) {
                auto media = attachments_.attachmentViews(id);
                if (!media.empty()) entry["attachments"] = std::move(media);
            }
        }
        const bool user = !system && (kind == "conversation_message" || kind == "user_answer");
        entry["role"] = system ? "system" : user ? "user" : isReply ? "bot" : "task";
        if (rows.number(1)) entry["taskId"] = rows.number(1);
        if (isReply) {
            const auto reply = decodeConversationReply(id, rows.json(3));
            if (!reply.actions.is_null()) entry["actions"] = scrub.record(reply.actions);
            if (!reply.suggestions.is_null()) entry["suggestions"] = scrub.record(reply.suggestions);
        } else if (!rows.null(5))
            entry["waitId"] = rows.number(5);
        const std::size_t limit = user ? 4000 : isReply ? 1500 : 700;
        const auto bytes = entry.dump().size() + (history.empty() && summary.is_null() ? 0 : 1);
        if (bytes > remaining) break;
        entry["text"] = boundedJsonText(scrub.text(rows.string(4)), limit, remaining - bytes);
        remaining -= bytes + (entry.at("text").dump().size() - 2);
        history.push_back(std::move(entry));
    }
    if (!summary.is_null()) history.push_back(std::move(summary));
    std::reverse(history.begin(), history.end());
    return history;
}

Json ConversationContextStorage::contextTool(std::string_view tool, const Json& args, const conversation::Message& message, int step) {
    const auto& bot = message.bot;
    const auto latest = compactions(bot, 0, 1);
    const Id boundary = latest.empty() ? 0 : latest.front().at("endMessageId").get<Id>();
    if (tool == "compact_conversation") {
        const auto end = identifier(args, "endMessageId", true);
        const auto summary = secretStorage_.redactor(bot).text(conversation_storage::text(args, "summary", maxSummaryBytes, true));
        if (summary.size() > maxSummaryBytes || Json(summary).dump().size() > maxExpandedBytes)
            throw Conflict("Escaped summary exceeds the context budget");
        if (end <= boundary || end >= message.id)
            throw Conflict("Use a boundary after the previous summary and before the current message");
        auto range = database_.query("SELECT min(id),max(id),count(*) FROM compactable_messages WHERE bot_id=? AND id<=?", {bot, end});
        range.next();
        if (range.number(1) != end) throw Conflict("Unknown conversation boundary");
        database_.exec("INSERT INTO conversation_compactions(bot_id,start_message_id,end_message_id,summary,original_count,"
                       "created_at,created_by) VALUES(?,?,?,?,?,?,?)",
                       {bot, range.number(0), end, summary, range.number(2), unixTime(), bot});
        return {{"ok", true}, {"compaction", compactions(bot, 0, 1).front()}};
    }

    Id end = std::min(boundary, message.id - 1);
    if (args.contains("compactionId")) {
        auto row = database_.query("SELECT end_message_id FROM conversation_compactions WHERE bot_id=? AND id=?",
                                   {bot, identifier(args, "compactionId", true)});
        if (!row.next()) throw Conflict("Unknown compaction");
        end = std::min(end, row.number(0));
    }
    if (args.contains("endMessageId")) end = std::min(end, identifier(args, "endMessageId", true));
    Json revision;
    const auto scrub = secretStorage_.redactor(bot, &revision);
    Id start = args.contains("startMessageId") ? identifier(args, "startMessageId", true) : 1;
    Id offset = 0;
    Id queryMessage = message.id, queryStep = step;
    auto needle = conversation_storage::text(args, "query", 512, false);
    if (args.contains("cursor")) {
        auto saved = database_.query(R"SQL(
SELECT c.message_id,c.byte_offset,c.redaction_revision,c.end_message_id,c.query_message_id,c.query_step,
       coalesce(json_extract(s.decision,'$[0].args.query'),'') AS search_query
FROM conversation_expansion_cursors c
JOIN conversation_steps s ON s.message_id=c.query_message_id AND s.step=c.query_step
WHERE c.bot_id=? AND c.id=?
)SQL",
                                     {bot, identifier(args, "cursor", true)});
        if (!saved.next() || saved.json(2) != revision)
            throw Conflict("Expansion cursor is stale or unknown. Read again from the start of the message.");
        auto originalNeedle = saved.string(6);
        if (!conversation_storage::hasText(originalNeedle)) originalNeedle.clear();
        if ((args.contains("startMessageId") && start != saved.number(0)) || (args.contains("query") && needle != originalNeedle) ||
            end < saved.number(3))
            throw Conflict("Cursor does not match the requested range or search");
        start = saved.number(0);
        offset = saved.number(1);
        end = saved.number(3);
        queryMessage = saved.number(4);
        queryStep = saved.number(5);
        needle = std::move(originalNeedle);
    }
    if (end < start) throw Conflict("No compacted messages in this range");
    auto rows = database_.query("SELECT id,kind,text FROM compactable_messages WHERE bot_id=? AND id BETWEEN ? AND ? ORDER BY id",
                                {bot, start, end});
    Json result = {{"ok", true}, {"messages", Json::array()}};
    constexpr std::size_t chunkBytes = 2000, cursorBytes = 128;
    const std::size_t limit = args.contains("limit") ? std::min<Id>(identifier(args, "limit", true), 8) : 8;
    const auto continueAt = [&](Id id, Id next) {
        auto saved = database_.query(R"SQL(
INSERT INTO conversation_expansion_cursors(bot_id,message_id,byte_offset,redaction_revision,end_message_id,query_message_id,query_step)
VALUES(?,?,?,?,?,?,?)
ON CONFLICT(bot_id,message_id,byte_offset,redaction_revision,end_message_id,query_message_id,query_step)
DO UPDATE SET byte_offset=excluded.byte_offset RETURNING id
)SQL",
                                     {bot, id, next, revision, end, queryMessage, queryStep});
        saved.next();
        result["nextCursor"] = saved.number(0);
    };
    std::size_t used = result.dump().size() + cursorBytes;
    while (rows.next()) {
        const auto body = scrub.text(rows.string(2));
        if (offset && (rows.number(0) != start || static_cast<std::size_t>(offset) >= body.size() ||
                       (static_cast<unsigned char>(body[offset]) & 0xc0) == 0x80))
            throw Conflict("Invalid UTF-8 byte offset");
        if (!needle.empty() &&
            std::search(body.begin(), body.end(), needle.begin(), needle.end(),
                        [](unsigned char lhs, unsigned char rhs) { return asciiLower(lhs) == asciiLower(rhs); }) == body.end()) {
            if (offset) throw Conflict("Cursor message does not match this search");
            continue;
        }
        if (result["messages"].size() >= limit || maxExpandedBytes - used < cursorBytes) {
            continueAt(rows.number(0), 0);
            break;
        }
        Json entry = {{"messageId", rows.number(0)}, {"kind", rows.string(1)}, {"text", ""}};
        const auto available = maxExpandedBytes - used - entry.dump().size() - 1;
        const auto tail = std::string_view(body).substr(offset);
        auto chunk = bounded(tail, chunkBytes);
        if (Json(chunk).dump().size() - 2 > available) chunk = bounded(tail, available / 6);
        entry["text"] = chunk;
        used += entry.dump().size() + 1;
        result["messages"].push_back(std::move(entry));
        const auto next = offset + static_cast<Id>(chunk.size());
        if (static_cast<std::size_t>(next) < body.size()) {
            continueAt(rows.number(0), next);
            break;
        }
        offset = 0;
    }
    result["_redactionRevision"] = std::move(revision);
    return result;
}
}
