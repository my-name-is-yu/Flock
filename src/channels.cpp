#include "storage.hpp"
#include "web_identity_state.hpp"
#include "secret_storage.hpp"
#include "journal_storage.hpp"
#include "json_fields.hpp"
#include "flock/policy.hpp"
#include "flock/channels.hpp"
#include "database.hpp"
#include "channel_storage.hpp"
#include "conversation_storage.hpp"
#include "journal_conversation.hpp"
#include "flock/base64.hpp"
#include "flock/markdown.hpp"
#include "flock/types.hpp"
#include "flock/intake.hpp"
#include "flock/person.hpp"
#include "flock/store.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/decision.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace flockv0 {
namespace {
constexpr std::size_t attachmentLimit = 4 * 1024 * 1024;
constexpr std::size_t attachmentCount = 4;
constexpr std::size_t artifactLimit = 256 * 1024;

std::string required(const Json& data, std::string_view key, std::size_t max) {
    auto value = field(data, key);
    if (!conversation_storage::hasText(value) || value.size() > max || value.find('\0') != value.npos)
        throw Conflict("Invalid " + std::string(key));
    return value;
}

std::string segment(std::string_view value) {
    std::string encoded;
    for (unsigned char ch : value) {
        if (ch == ':' || ch == '%' || ch < 32 || ch == 127) {
            constexpr std::string_view hex = "0123456789ABCDEF";
            encoded += '%';
            encoded += hex[ch >> 4];
            encoded += hex[ch & 15];
        } else
            encoded += static_cast<char>(ch);
    }
    return encoded;
}

ChannelOrigin originFrom(const Json& data) {
    const auto name = field(data, "channelKind");
    if (name != "slack" && name != "imessage") throw Conflict("Invalid stored channel kind");
    return {name == "slack" ? ChannelKind::slack : ChannelKind::imessage, field(data, "workspaceId"), field(data, "threadRef"),
            field(data, "externalMessageId")};
}

std::string channelScopeKey(const VerifiedChannelMessage& message) {
    const auto& origin = message.origin;
    return Json::array({channelName(origin.kind), origin.workspaceId, origin.threadRef, message.senderId, message.personId}).dump();
}

std::string limitation(const ChannelCapabilities& caps, std::string_view operation, bool canPublish) {
    if (operation == "reply" && !caps.supportsReplies) return "This channel does not support threaded replies.";
    if (operation == "channel_edit_message" && !caps.supportsEdit) return "This channel does not support editing messages.";
    if (operation == "channel_react" && !caps.supportsReact) return "This channel does not support reactions.";
    if (operation == "publish_artifact" && !caps.supportsHtmlArtifacts) return "This channel does not support HTML artifacts.";
    if (operation == "publish_artifact" && !canPublish) return "No public HTML preview URL is configured for this channel.";
    return {};
}

std::string attachmentLimitation(const ChannelCapabilities& caps, std::string_view mime, std::size_t size) {
    if (std::ranges::find(caps.allowedAttachmentTypes, mime) == caps.allowedAttachmentTypes.end())
        return "This channel does not accept that attachment type.";
    if (size > std::min(caps.maxAttachmentBytes, attachmentLimit)) return "That attachment exceeds this channel's size limit.";
    return {};
}

bool matchesMime(std::string_view mime, std::string_view bytes) {
    if (mime == "image/png") return bytes.starts_with("\x89PNG\r\n\x1a\n");
    if (mime == "image/jpeg") return bytes.starts_with("\xff\xd8\xff");
    if (mime == "image/webp") return bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP";
    if (mime != "text/plain" || bytes.find('\0') != bytes.npos) return false;
    try {
        (void)Json(bytes).dump();
    } catch (const Json::type_error&) {
        return false;
    }
    return true;
}

bool canReadAttachment(const ChannelCapabilities& caps, std::string_view mime, std::string_view bytes) {
    return !bytes.empty() && attachmentLimitation(caps, mime, bytes.size()).empty() && matchesMime(mime, bytes);
}

std::size_t decodedSize(std::string_view input) {
    const auto size = base64DecodedSize(input);
    if (!size) throw Conflict("Invalid attachment encoding");
    return *size;
}
}

std::string encodeAttachment(std::string_view bytes) {
    return encodeBase64(bytes);
}

std::string decodeAttachment(std::string_view input, std::size_t maxBytes) {
    try {
        return decodeBase64(input, maxBytes);
    } catch (const std::length_error&) {
        throw Conflict("Invalid attachment size");
    } catch (const std::invalid_argument&) {
        throw Conflict("Invalid attachment encoding");
    }
}

std::string_view channelName(ChannelKind kind) {
    return kind == ChannelKind::slack ? "slack" : "imessage";
}

Json ChannelOrigin::json() const {
    return {{"channelKind", channelName(kind)},
            {"workspaceId", workspaceId},
            {"threadRef", threadRef},
            {"externalMessageId", externalMessageId}};
}

std::string ChannelOrigin::key() const {
    return "channel:" + std::string(channelName(kind)) + ":" + segment(workspaceId) + ":" + segment(threadRef) + ":" +
           segment(externalMessageId);
}

std::string channelPolicyTarget(const ChannelOrigin& origin) {
    return Json::array({channelName(origin.kind), origin.workspaceId}).dump();
}

ChannelCapabilities channelCapabilities(ChannelKind kind) {
    return {true, kind == ChannelKind::slack, true, true, {"image/png", "image/jpeg", "image/webp", "text/plain"}, attachmentLimit};
}

void ChannelStorage::connectChannel(ChannelConnector connector) {
    if (!connector.authenticates || !connector.resolveSender || !connector.transport)
        throw Conflict("A channel needs authentication, an identity directory, and transport");
    std::lock_guard lock(database.mutex);
    connectors.insert_or_assign(connector.kind, std::move(connector));
}

std::string normalizeArtifactBaseUrl(std::string url) {
    if (!url.empty()) {
        if (!url.starts_with("https://") || url.size() <= 8) throw Conflict("Artifact base URL must be an HTTPS origin");
        if (url.ends_with('/')) url.pop_back();
        const auto authority = std::string_view(url).substr(8);
        const auto portSeparator = authority.find(':');
        const auto host = authority.substr(0, portSeparator);
        if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != host.npos ||
            host.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == host.npos)
            throw Conflict("Artifact base URL must be an HTTPS origin");
        if (portSeparator != authority.npos) {
            const auto port = authority.substr(portSeparator + 1);
            unsigned value = 0;
            const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || value == 0 || value > 65535)
                throw Conflict("Invalid artifact origin port");
        }
    }
    return url;
}

void ChannelStorage::artifactBaseUrl(std::string url) {
    auto origin = normalizeArtifactBaseUrl(std::move(url));
    std::lock_guard lock(database.mutex);
    artifactOrigin = std::move(origin);
}

bool ChannelStorage::permitsChannel(std::string_view bot, const ChannelOrigin& origin, std::string_view sender, std::string_view person,
                                    std::string_view operation) const {
    const auto connector = connectors.find(origin.kind);
    if (connector == connectors.end()) return false;
    const auto linked = connector->second.resolveSender(origin.workspaceId, sender);
    if (!linked || linked->botId != bot || linked->personId != person) return false;
    if (webIdentity.resolve && permissions.requiresIdentity() && !webIdentity.resolve(person)) return false;
    return permissions.allows(Person{std::string(person), false}, Action::bot_talk, bot) &&
           permissions.allows(bot, Action::channel_use, channelPolicyTarget(origin)) &&
           (operation == "use" ||
            !permissions.forbidden(bot, operation == "read" ? Action::channel_read : Action::channel_post, channelPolicyTarget(origin)));
}

Json ChannelStorage::receiveChannel(Store& store, ChannelKind kind, std::string_view credential, const Json& event,
                                    const std::function<Intake(std::string_view, std::string_view)>& routing) {
    std::lock_guard lock(database.mutex);
    const auto found = connectors.find(kind);
    if (found == connectors.end()) throw ChannelAccessDenied("Channel is not connected");
    const auto& connector = found->second;
    ChannelOrigin origin{kind, required(event, "workspaceId", 128), required(event, "threadRef", 200),
                         required(event, "externalMessageId", 128)};
    const auto senderId = required(event, "senderId", 200);
    if (!connector.authenticates(credential, origin.workspaceId)) throw ChannelAccessDenied("Channel bridge is not authorized");
    const auto sender = connector.resolveSender(origin.workspaceId, senderId);
    if (!sender || !permitsChannel(sender->botId, origin, senderId, sender->personId, "use"))
        throw ChannelAccessDenied("Channel sender is not authorized");
    VerifiedChannelMessage message{origin, senderId, sender->personId, {}};
    Id replyTo = 0;
    if (event.contains("replyToExternalMessageId")) {
        auto target = database.query(R"SQL(
SELECT reply_id FROM channel_posts
WHERE bot_id=? AND scope_key=? AND external_id=? AND reply_id IS NOT NULL LIMIT 2
)SQL",
                                     {sender->botId, channelScopeKey(message), required(event, "replyToExternalMessageId", 128)});
        if (!target.next()) throw Conflict("The channel reply target is not available");
        replyTo = target.number(0);
        if (target.next()) throw Conflict("The channel reply target is ambiguous");
    }
    const auto files = event.value("attachments", Json::array());
    if (!files.is_array() || files.size() > attachmentCount) throw Conflict("Too many attachments");
    const auto& caps = connector.capabilities;
    if (!files.empty() && !permitsChannel(sender->botId, origin, senderId, sender->personId, "read"))
        throw ChannelAccessDenied("Channel attachment read denied");
    std::size_t total = 0;
    for (const auto& file : files) {
        const auto name = required(file, "name", 255);
        const auto mime = required(file, "mimeType", 128);
        const auto encoded = required(file, "contentBase64", attachmentLimit * 4 / 3 + 4);
        const auto size = decodedSize(encoded);
        const auto reason = attachmentLimitation(caps, mime, size);
        if (!reason.empty()) throw Conflict(reason);
        total += size;
        if (total > attachmentLimit) throw Conflict("Attachments exceed the total size limit");
        auto bytes = decodeAttachment(encoded, caps.maxAttachmentBytes);
        if (!matchesMime(mime, bytes)) throw Conflict("Attachment content does not match its MIME type");
        message.attachments.push_back({name, mime, std::move(bytes)});
    }
    auto text = field(event, "text");
    if (!conversation_storage::hasText(text) && !files.empty()) text = "Photo or file attached.";
    return store.receive({sender->personId, false}, sender->botId, origin.key(), text, replyTo, conversation::PrincipalSource::channel,
                         &message, [&] { return routing(sender->botId, text); });
}

void ChannelStorage::saveChannelMessage(Id id, std::string_view bot, const VerifiedChannelMessage& message, bool replay) {
    if (!permitsChannel(bot, message.origin, message.senderId, message.personId, message.attachments.empty() ? "use" : "read"))
        throw ChannelAccessDenied("Channel access is no longer authorized");
    const auto scrub = secretStorage.redactor(bot);
    scrub.requirePublic({message.origin.json(), message.senderId, message.personId});
    if (replay) {
        auto saved = database.query("SELECT 1 FROM channel_messages WHERE message_id=? AND sender_id=? AND person_id=?",
                                    {id, message.senderId, message.personId});
        if (!saved.next()) throw Conflict("Channel event already belongs to another sender");
        auto files = database.query("SELECT name,mime_type,bytes FROM channel_attachments WHERE message_id=? ORDER BY position", {id});
        for (const auto& file : message.attachments) {
            if (!files.next() || files.string(0) != file.name || files.string(1) != file.mimeType || files.string(2) != file.bytes)
                throw Conflict("Channel event already has different attachments");
        }
        if (files.next()) throw Conflict("Channel event already has different attachments");
        return;
    }
    if (database.query("SELECT 1 FROM channel_messages WHERE event_key=?", {message.origin.key()}).next())
        throw Conflict("Channel event already belongs to another Bot");
    database.exec("INSERT INTO channel_messages VALUES(?,?,?,?,?,?)",
                  {id, message.origin.key(), channelScopeKey(message), message.origin.json(), message.senderId, message.personId});
    Id position = 0;
    for (const auto& file : message.attachments) {
        scrub.requirePublic({file.name, file.mimeType, file.bytes});
        database.exec("INSERT INTO channel_attachments(message_id,position,name,mime_type,bytes) VALUES(?,?,?,?,CAST(? AS BLOB))",
                      {id, position++, file.name, file.mimeType, file.bytes});
    }
}

bool ChannelStorage::channelMessageAuthorized(Id id, std::string_view bot) const {
    auto row = database.query("SELECT origin,sender_id,person_id FROM channel_messages WHERE message_id=?", {id});
    return !row.next() || permitsChannel(bot, originFrom(row.json(0)), row.string(1), row.string(2), "use");
}

Json ChannelStorage::channelContext(Id id, std::string_view bot, std::vector<ModelImage>* images, std::string_view viewer) const {
    auto row =
        database.query("SELECT c.origin,c.sender_id,c.person_id,c.scope_key FROM channel_messages c JOIN messages m ON m.id=c.message_id"
                       " WHERE c.message_id=? AND m.bot_id=?",
                       {id, bot});
    if (!row.next()) return Json::object();
    const auto origin = originFrom(row.json(0));
    const auto sender = row.string(1), person = row.string(2);
    Json context{{"channel", origin.json()}};
    if (!viewer.empty()) context["channel"]["fromViewer"] = viewer == person;
    if (!permitsChannel(bot, origin, sender, person, "use")) return context;
    const auto& caps = connectors.at(origin.kind).capabilities;
    if (permitsChannel(bot, origin, sender, person, "read")) {
        const bool includeImages = images && permissions.allows(bot, Action::channel_read, channelPolicyTarget(origin));
        const int modelContext = images ? 1 : 0;
        auto files = database.query(R"SQL(
SELECT a.id,a.name,a.mime_type,length(a.bytes),a.message_id,CASE WHEN a.message_id=? AND ? THEN a.bytes END
FROM channel_attachments a JOIN channel_messages c ON c.message_id=a.message_id JOIN messages m ON m.id=c.message_id
WHERE c.scope_key=? AND m.bot_id=? AND a.message_id<=? AND (? OR a.message_id=?)
ORDER BY a.message_id DESC,a.position LIMIT 24
)SQL",
                                    {id, static_cast<int>(includeImages), row.string(3), bot, id, modelContext, id});
        while (files.next()) {
            const auto type = files.string(2);
            const auto size = files.number(3);
            if (size <= 0 || !attachmentLimitation(caps, type, static_cast<std::size_t>(size)).empty()) continue;
            const bool current = files.number(4) == id;
            context[current ? "attachments" : "earlierAttachments"].push_back(
                {{"id", files.number(0)}, {"name", files.string(1)}, {"mimeType", type}, {"size", size}, {"messageId", files.number(4)}});
            if (current && includeImages && type.starts_with("image/")) {
                const auto bytes = files.string(5);
                if (matchesMime(type, bytes)) images->push_back({id, {}, type, encodeAttachment(bytes)});
            }
        }
    }
    if (!images) return context;
    auto sent = database.query(R"SQL(
WITH edits AS (
  SELECT d.rowid AS sequence,d.payload
  FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id
  JOIN messages m ON m.id=c.message_id WHERE m.bot_id=? AND c.scope_key=? AND d.state='accepted'
  AND d.operation='channel_edit_message'
)
SELECT post.external_id,coalesce((
  SELECT json_extract(edit.payload,'$.text') FROM edits edit WHERE edit.sequence>post.sequence
  AND json_extract(edit.payload,'$.externalMessageId')=post.external_id ORDER BY edit.sequence DESC LIMIT 1
),json_extract(post.payload,'$.text'),json_extract(post.payload,'$.title'),json_extract(post.payload,'$.name'))
FROM channel_posts post WHERE post.bot_id=? AND post.scope_key=? ORDER BY post.sequence DESC LIMIT 10
)SQL",
                               {bot, row.string(3), bot, row.string(3)});
    auto& messages = context["channel"]["sentMessages"] = Json::array();
    while (sent.next()) messages.push_back({{"externalMessageId", sent.string(0)}, {"text", bounded(sent.string(1), 500)}});
    if (images) {
        auto pending = database.query(R"SQL(
SELECT DISTINCT p.action FROM policy_approvals p JOIN channel_messages c ON c.message_id=p.anchor
WHERE p.status='pending' AND c.scope_key=? AND p.bot=? ORDER BY p.action
)SQL",
                                      {row.string(3), bot});
        while (pending.next())
            context["channel"]["pendingToolResults"].push_back(
                {{"ok", false}, {"policyWaiting", true}, {"action", pending.string(0)}, {"message", approvalError(ApprovalGate::waiting)}});
    }
    context["channel"]["capabilities"] = {{"supportsReplies", caps.supportsReplies},
                                          {"supportsEdit", caps.supportsEdit},
                                          {"supportsReact", caps.supportsReact},
                                          {"supportsHtmlArtifacts", caps.supportsHtmlArtifacts && !artifactOrigin.empty()},
                                          {"allowedAttachmentTypes", caps.allowedAttachmentTypes},
                                          {"maxAttachmentBytes", std::min(caps.maxAttachmentBytes, attachmentLimit)}};
    return context;
}

std::optional<ChannelResource> ChannelStorage::channelAttachment(Id source, Id attachment, std::string_view bot) const {
    auto row = database.query(R"SQL(
SELECT a.name,a.mime_type,a.bytes,original.origin,original.sender_id,original.person_id
FROM channel_attachments a JOIN channel_messages original ON original.message_id=a.message_id
JOIN messages m ON m.id=a.message_id JOIN channel_messages current ON current.message_id=?
WHERE a.id=? AND m.bot_id=? AND original.scope_key=current.scope_key AND original.message_id<=current.message_id
)SQL",
                              {source, attachment, bot});
    if (!row.next()) return std::nullopt;
    const auto origin = originFrom(row.json(3));
    if (!permitsChannel(bot, origin, row.string(4), row.string(5), "read")) return std::nullopt;
    return ChannelResource{row.string(0), row.string(1), row.string(2)};
}

std::optional<ChannelResource> ChannelStorage::attachment(const Person& person, Id id) const {
    std::lock_guard lock(database.mutex);
    auto row = database.query(R"SQL(
SELECT a.name,a.mime_type,a.bytes,m.bot_id,c.origin,c.sender_id,c.person_id
FROM channel_attachments a JOIN channel_messages c ON c.message_id=a.message_id JOIN messages m ON m.id=c.message_id
WHERE a.id=?)SQL",
                              {id});
    if (!row.next()) return std::nullopt;
    const auto bot = row.string(3);
    const auto origin = originFrom(row.json(4));
    if (!permissions.allows(person, Action::bot_view, bot) || !permitsChannel(bot, origin, row.string(5), row.string(6), "read"))
        return std::nullopt;
    ChannelResource file{row.string(0), row.string(1), row.string(2)};
    if (!canReadAttachment(connectors.at(origin.kind).capabilities, file.mimeType, file.bytes)) return std::nullopt;
    return file;
}

std::optional<ChannelResource> ChannelStorage::artifact(std::string_view id) const {
    std::lock_guard lock(database.mutex);
    auto row = database.query(R"SQL(
SELECT a.title,a.html,c.origin,c.sender_id,c.person_id,m.bot_id
FROM channel_artifacts a JOIN channel_deliveries d ON d.id=a.id
JOIN channel_messages c ON c.message_id=d.source_id JOIN messages m ON m.id=c.message_id
WHERE a.id=? AND length(CAST(a.html AS BLOB))<=? AND d.state IN ('unknown','accepted')
)SQL",
                              {id, artifactLimit});
    if (!row.next()) return std::nullopt;
    const auto origin = originFrom(row.json(2));
    if (!permitsChannel(row.string(5), origin, row.string(3), row.string(4), "use") ||
        !connectors.at(origin.kind).capabilities.supportsHtmlArtifacts)
        return std::nullopt;
    return ChannelResource{row.string(0), "text/html; charset=utf-8", row.string(1)};
}

void ChannelStorage::queueChannelReply(Id source, Id reply, std::string_view text, const Json& suggestions) {
    database.exec(R"SQL(
INSERT INTO channel_deliveries(id,source_id,reply_id,operation,payload)
SELECT ?,c.message_id,?,'reply',? FROM channel_messages c JOIN messages m ON m.id=c.message_id
WHERE c.message_id=? AND NOT EXISTS (SELECT 1 FROM bot_pauses p WHERE p.bot=m.bot_id)
ON CONFLICT(reply_id) DO NOTHING)SQL",
                  {"reply-" + std::to_string(reply) + "-" + actionId(), reply, Json{{"text", text}, {"suggestions", suggestions}}, source});
}

void ChannelStorage::queueChannelTaskUpdate(Id task, Id entry, std::string_view text, std::span<const std::string> suggestions) {
    if (text.empty()) return;
    auto source = database.query("SELECT message_id,t.bot_id FROM channel_tasks c JOIN tasks t ON t.id=c.task_id WHERE t.id=?", {task});
    if (!source.next()) return;
    const auto scrub = secretStorage.redactor(source.string(1));
    queueChannelReply(source.number(0), entry, scrub.text(bounded(text, 8000)), scrub.payload(Json(suggestions)));
}

void ChannelStorage::channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies) {
    channelNotice(source, bot, reason, supportsReplies, " I can post a normal follow-up in this conversation if you'd like.");
}

void ChannelStorage::channelApprovalNotice(Id source) {
    auto row = database.query("SELECT m.bot_id,c.origin FROM channel_messages c JOIN messages m ON m.id=c.message_id"
                              " WHERE c.message_id=?",
                              {source});
    if (!row.next()) return;
    const auto origin = originFrom(row.json(1));
    const auto connector = connectors.find(origin.kind);
    const auto bot = row.string(0);
    if (connector == connectors.end() || !connector->second.capabilities.supportsReplies ||
        permissions.check(bot, Resource{Action::channel_post, channelPolicyTarget(origin), true}) != PolicyResult::allow)
        return;
    channelNotice(source, bot, approvalError(ApprovalGate::waiting), true);
}

void ChannelStorage::channelNotice(Id source, std::string_view bot, std::string_view reason, bool supportsReplies,
                                   std::string_view followUp) {
    database.exec("INSERT OR IGNORE INTO channel_notices VALUES(?,?)", {source, reason});
    if (!sqlite3_changes(database.db)) return;
    std::string notice(reason);
    if (supportsReplies) notice += followUp;
    const auto reply = journalStorage.journalConversationReply(std::nullopt, bot, {.replyToMessageId = source, .text = notice});
    if (supportsReplies) queueChannelReply(source, reply, notice, Json::array());
}

std::optional<Resource> ChannelStorage::channelToolPolicy(Id message, std::string_view tool) {
    if (tool != "reply" && tool != "read_channel_attachment" && tool != "channel_edit_message" && tool != "channel_react" &&
        tool != "channel_send_attachment" && tool != "publish_artifact")
        return {};
    auto source = database.query("SELECT origin FROM channel_messages WHERE message_id=?", {message});
    if (!source.next()) return {};
    return Resource{tool == "read_channel_attachment" ? Action::channel_read : Action::channel_post,
                    channelPolicyTarget(originFrom(source.json(0))),
                    // Acting inside the conversation that asked is part of answering it; publishing is not.
                    tool == "reply" || tool == "channel_react" || tool == "channel_edit_message" || tool == "channel_send_attachment"};
}

Json ChannelStorage::channelTool(Id message, std::string_view bot, const Decision& decision) {
    const auto& tool = decision.tool;
    if (tool != "channel_edit_message" && tool != "channel_react" && tool != "channel_send_attachment" && tool != "publish_artifact" &&
        tool != "read_channel_attachment")
        return nullptr;
    auto source = database.query("SELECT origin,sender_id,person_id,scope_key FROM channel_messages WHERE message_id=?", {message});
    if (!source.next()) throw Conflict("This tool requires a channel conversation");
    const auto origin = originFrom(source.json(0));
    const auto sender = source.string(1), person = source.string(2);
    if (!permitsChannel(bot, origin, sender, person, tool == "read_channel_attachment" ? "read" : "use"))
        throw ChannelAccessDenied("Channel access is no longer authorized");
    const auto& caps = connectors.at(origin.kind).capabilities;

    const auto& args = decision.args;
    const auto attachment = [&]() {
        auto file = channelAttachment(message, conversation_storage::integer(args, "attachmentId"), bot);
        if (!file) throw Conflict("Attachment is unavailable in this conversation");
        return std::move(*file);
    };
    if (tool == "read_channel_attachment") {
        const auto file = attachment();
        if (!canReadAttachment(caps, file.mimeType, file.bytes)) throw Conflict("Attachment is outside current channel limits");
        return {{"ok", true}, {"readAttachmentId", args.at("attachmentId")}, {"channelKind", channelName(origin.kind)}};
    }

    Json payload = Json::object();
    std::string bytes;
    auto reason = limitation(caps, tool, !artifactOrigin.empty());
    if (tool == "channel_send_attachment") {
        if (args.contains("attachmentId")) {
            if (args.contains("name") || args.contains("mimeType") || args.contains("contentBase64"))
                throw Conflict("Use an attachment ID or file data");
            auto file = attachment();
            bytes = std::move(file.bytes);
            payload = {{"name", file.name}, {"mimeType", file.mimeType}, {"size", bytes.size()}, {"attachmentId", args.at("attachmentId")}};
        } else {
            const auto mime = required(args, "mimeType", 128);
            const auto encoded = field(args, "contentBase64");
            payload = {{"name", required(args, "name", 255)}, {"mimeType", mime}, {"size", decodedSize(encoded)}};
        }
        reason = attachmentLimitation(caps, field(payload, "mimeType"), payload.at("size"));
    }
    if (!reason.empty()) {
        channelLimitation(message, bot, reason, caps.supportsReplies);
        return {{"ok", false}, {"unsupported", true}, {"reason", reason}, {"noticeQueued", caps.supportsReplies}};
    }

    const auto key = actionId();
    std::string html;
    if (tool == "channel_edit_message") {
        const auto target = required(args, "externalMessageId", 128);
        if (!database.query("SELECT 1 FROM channel_posts WHERE bot_id=? AND scope_key=? AND external_id=?", {bot, source.string(3), target})
                 .next())
            throw Conflict("The target must be a message this Bot sent in this channel thread");
        if (database
                .query(R"SQL(
SELECT 1 FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id
JOIN messages m ON m.id=c.message_id WHERE m.bot_id=? AND c.scope_key=? AND d.operation='channel_edit_message'
AND d.state IN ('pending','unknown') AND json_extract(d.payload,'$.externalMessageId')=?
)SQL",
                       {bot, source.string(3), target})
                .next())
            throw Conflict("An edit to this message is awaiting delivery confirmation");
        payload = {{"externalMessageId", target}, {"text", required(args, "text", 8000)}};
    } else if (tool == "channel_react") {
        payload = {{"externalMessageId", origin.externalMessageId}, {"reaction", required(args, "reaction", 80)}};
    } else if (tool == "channel_send_attachment") {
        if (bytes.empty()) bytes = decodeAttachment(field(args, "contentBase64"), std::min(caps.maxAttachmentBytes, attachmentLimit));
        if (!matchesMime(field(payload, "mimeType"), bytes)) throw Conflict("Attachment content does not match its MIME type");
        if (!payload.contains("attachmentId")) {
            secretStorage.redactor(bot).requirePublic(bytes);
            payload["contentBase64"] = encodeAttachment(bytes);
        }
    } else if (tool == "publish_artifact") {
        const auto title = required(args, "title", 255);
        html = required(args, "html", artifactLimit);
        secretStorage.redactor(bot).requirePublic({title, html});
        payload = {{"title", title}, {"url", artifactOrigin + "/api/artifacts/" + key}};
    }
    secretStorage.redactor(bot).requirePublic(payload);
    database.exec("INSERT INTO channel_deliveries(id,source_id,operation,payload) VALUES(?,?,?,?)", {key, message, tool, payload});
    Json result{{"ok", true}, {"delivery", "queued"}, {"deliveryId", key}};
    if (tool == "publish_artifact") {
        database.exec("INSERT INTO channel_artifacts VALUES(?,?,?)", {key, payload.at("title"), html});
        result["url"] = payload.at("url");
        result["artifact"] = {{"title", payload.at("title")}, {"id", key}};
    }
    return result;
}

Json ChannelStorage::channelToolStatus(Id source, std::string_view bot, Json result, std::vector<ModelImage>* images) const {
    if (result.contains("readAttachmentId")) {
        const auto file = channelAttachment(source, result.at("readAttachmentId"), bot);
        const auto kind = field(result, "channelKind") == "slack" ? ChannelKind::slack : ChannelKind::imessage;
        if (!file || !canReadAttachment(connectors.at(kind).capabilities, file->mimeType, file->bytes))
            return {{"ok", false}, {"error", "Channel attachment is no longer available"}};
        if (file->mimeType == "text/plain")
            return {{"ok", true}, {"name", file->name}, {"text", bounded(file->bytes, 16000)}, {"truncated", file->bytes.size() > 16000}};
        if (images) {
            ModelImage image{source, {}, file->mimeType, encodeAttachment(file->bytes)};
            if (std::ranges::none_of(*images, [&](const auto& existing) {
                    return existing.mimeType == image.mimeType && existing.dataBase64 == image.dataBase64;
                })) {
                std::size_t used = 0;
                for (const auto& existing : *images) used += existing.dataBase64.size() + existing.mimeType.size() + 13;
                if (used + image.dataBase64.size() + image.mimeType.size() + 13 > (attachmentLimit + 2) / 3 * 4 + attachmentCount * 32)
                    return {{"ok", false}, {"error", "The total image limit for this turn is reached"}};
                images->push_back(std::move(image));
            }
        }
        return {{"ok", true}, {"name", file->name}, {"imageIncluded", true}};
    }
    const auto key = field(result, "deliveryId");
    if (key.empty()) return result;
    auto row = database.query("SELECT state,external_id,reason FROM channel_deliveries WHERE id=?", {key});
    if (!row.next()) return result;
    const auto state = row.string(0);
    if (state != "pending") result["delivery"] = state;
    if (state == "rejected" || state == "unsupported") result["ok"] = false;
    if (state == "unsupported") {
        result["unsupported"] = true;
        result["reason"] = row.string(2);
        result.erase("delivery");
    }
    if (!row.string(1).empty()) result["externalMessageId"] = row.string(1);
    return result;
}

bool ChannelStorage::hasConnectors() const {
    std::lock_guard lock(database.mutex);
    return !connectors.empty();
}

bool ChannelStorage::channelDeliveryDue() const {
    std::lock_guard lock(database.mutex);
    return !connectors.empty() && database
                                      .query("SELECT 1 FROM channel_deliveries WHERE state IN ('pending','unknown')"
                                             " AND next_attempt<=? LIMIT 1",
                                             {unixTime()})
                                      .next();
}

void ChannelStorage::dropPausedBotDeliveries(std::string_view bot) {
    database.exec(R"SQL(
UPDATE channel_deliveries SET state='rejected',reason=? WHERE state IN ('pending','unknown')
AND source_id IN (SELECT id FROM messages WHERE bot_id=?)
)SQL",
                  {botPausedMessage, bot});
}

void ChannelStorage::deliverChannels(std::stop_token shutdown) {
    std::unique_lock deliveryLock(deliveryMutex, std::try_to_lock);
    if (!deliveryLock.owns_lock()) return;
    for (int i = 0; i < 20 && !shutdown.stop_requested(); ++i) {
        const SessionDispatchFileLock dispatch(path, SessionDispatchFileLock::Mode::shared);
        std::shared_ptr<ChannelTransport> transport;
        ChannelOrigin origin{};
        std::string key, operation, state, bot;
        Json payload;
        {
            std::lock_guard lock(database.mutex);
            Transaction tx(database.db);
            if (shutdown.stop_requested()) return;
            auto row = database.query(R"SQL(
SELECT d.id,d.operation,d.payload,d.state,c.origin,c.sender_id,c.person_id,m.bot_id,d.source_id,j.task_id
FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id JOIN messages m ON m.id=c.message_id
LEFT JOIN journal j ON j.id=d.reply_id
WHERE d.state IN ('pending','unknown') AND next_attempt<=? ORDER BY d.rowid LIMIT 1
)SQL",
                                      {unixTime()});
            if (!row.next()) return;
            key = row.string(0);
            operation = row.string(1);
            payload = row.json(2);
            state = row.string(3);
            origin = originFrom(row.json(4));
            bot = row.string(7);
            const auto found = connectors.find(origin.kind);
            const auto sender = row.string(5), person = row.string(6);
            if (found == connectors.end() || (state == "unknown" && !permitsChannel(bot, origin, sender, person, "post"))) {
                database.exec("UPDATE channel_deliveries SET next_attempt=? WHERE id=?", {unixTime() + 5, key});
                tx.commit();
                continue;
            }
            transport = found->second.transport;
            const auto& caps = found->second.capabilities;
            std::string blocked, reason;
            if (state == "pending") {
                if (!permitsChannel(bot, origin, sender, person, "post"))
                    blocked = "rejected";
                else if (operation == "channel_send_attachment") {
                    reason = attachmentLimitation(caps, field(payload, "mimeType"), payload.at("size"));
                    if (reason.empty() && payload.contains("attachmentId")) {
                        const auto file = channelAttachment(row.number(8), payload.at("attachmentId"), bot);
                        if (!file || !canReadAttachment(caps, file->mimeType, file->bytes))
                            blocked = "rejected";
                        else
                            payload["contentBase64"] = encodeAttachment(file->bytes);
                    }
                } else
                    reason = limitation(caps, operation, !artifactOrigin.empty());
                if (!reason.empty()) {
                    blocked = "unsupported";
                    if (operation != "reply") channelLimitation(row.number(8), bot, reason, caps.supportsReplies);
                }
            }
            if (blocked.empty() && state == "pending" && row.number(9)) {
                const auto source = row.number(8);
                const auto approval = "channel-task-update/" + key;
                const auto target = channelPolicyTarget(origin);
                const auto used = database.query("SELECT 1 FROM policy_approvals WHERE operation=? AND status='used'", {approval}).next();
                if (!used) {
                    const auto requester = policyApprovals.policyOrigin(remaining.loadTurnMessage(source));
                    const auto gate =
                        policyApprovals.approveAction({bot, requester.scope, requester.requester, requester.anchor, 0, 0, 0, 0, true},
                                                      {Action::channel_post, target, true}, approval, payload, policyApprovals.now());
                    if (gate == ApprovalGate::waiting) {
                        database.exec("UPDATE channel_deliveries SET next_attempt=? WHERE id=?", {unixTime() + 5, key});
                        tx.commit();
                        continue;
                    }
                    if (gate != ApprovalGate::allow) {
                        blocked = "rejected";
                        reason = approvalError(gate);
                    }
                }
            }
            if (!blocked.empty()) {
                database.exec("UPDATE channel_deliveries SET state=?,reason=? WHERE id=?", {blocked, reason, key});
                tx.commit();
                continue;
            }
            database.exec("UPDATE channel_deliveries SET state='unknown',next_attempt=? WHERE id=?", {unixTime() + 5, key});
            tx.commit();
        }
        ChannelDelivery outcome;
        try {
            if (state == "pending") {
                auto deliveryPayload = payload;
                if (operation == "reply" && deliveryPayload.contains("text") && deliveryPayload["text"].is_string()) {
                    const auto source = deliveryPayload["text"].get<std::string>();
                    if (origin.kind == ChannelKind::slack) {
                        const auto slackReply = markdown::toSlack(source);
                        deliveryPayload["text"] = slackReply.text;
                        deliveryPayload["deliveryMetadata"] = {
                            {"mrkdwn", slackReply.mrkdwn}, {"parse", "none"}, {"link_names", false}, {"disableAutoMentions", true}};
                    } else if (origin.kind == ChannelKind::imessage) {
                        deliveryPayload["text"] = markdown::toPlain(source);
                    }
                }
                outcome = transport->send(key, origin, operation, deliveryPayload);
            } else
                outcome = transport->reconcile(key, origin);
        } catch (const std::exception&) {
        }
        if (outcome.state == ChannelDelivery::State::unknown) continue;
        std::lock_guard lock(database.mutex);
        auto& external = outcome.externalMessageId;
        if (external.size() > 128 || external.find('\0') != external.npos || !conversation_storage::hasText(external) ||
            secretStorage.redactor(bot).text(external) != external)
            external.clear();
        const auto next = outcome.state == ChannelDelivery::State::accepted  ? "accepted"
                          : outcome.state == ChannelDelivery::State::notSent ? "pending"
                                                                             : "rejected";
        database.exec("UPDATE channel_deliveries SET state=?,external_id=? WHERE id=? AND state='unknown'", {next, external, key});
    }
}
void Store::connectChannel(ChannelConnector connector) {
    p_->channelStorage.connectChannel(std::move(connector));
}

void Store::artifactBaseUrl(std::string url) {
    p_->channelStorage.artifactBaseUrl(std::move(url));
}

Json Store::receiveChannel(ChannelKind kind, std::string_view credential, const Json& event, const IntakeResolver& routing) {
    return p_->channelStorage.receiveChannel(*this, kind, credential, event, routing);
}

std::optional<ChannelResource> Store::attachment(const Person& person, Id id) const {
    return p_->channelStorage.attachment(person, id);
}

std::optional<ChannelResource> Store::artifact(std::string_view id) const {
    return p_->channelStorage.artifact(id);
}
}
