#include "storage.hpp"
#include "conversation_storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {
constexpr std::size_t attachmentLimit = 4 * 1024 * 1024;
constexpr std::size_t attachmentCount = 4;
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string required(const Json& data, std::string_view key, std::size_t max) {
    auto value = field(data,key);
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
        } else encoded += static_cast<char>(ch);
    }
    return encoded;
}

ChannelOrigin originFrom(const Json& data) {
    return {field(data,"channelKind") == "slack" ? ChannelKind::slack : ChannelKind::imessage,
            field(data,"workspaceId"),field(data,"threadRef"),field(data,"externalMessageId")};
}

std::string limitation(const ChannelCapabilities& caps, std::string_view operation, const Json& payload) {
    if (operation == "reply" && !caps.supportsReplies) return "This channel does not support threaded replies.";
    if (operation == "channel_edit_message" && !caps.supportsEdit) return "This channel does not support editing messages.";
    if (operation == "channel_react" && !caps.supportsReact) return "This channel does not support reactions.";
    if (operation == "publish_artifact" && !caps.supportsHtmlArtifacts) return "This channel does not support HTML artifacts.";
    if (operation == "channel_send_attachment") {
        const auto mime = field(payload,"mimeType");
        if (std::ranges::find(caps.allowedAttachmentTypes,mime) == caps.allowedAttachmentTypes.end())
            return "This channel does not accept that attachment type.";
        if (payload.value("size",std::size_t{}) > std::min(caps.maxAttachmentBytes,attachmentLimit))
            return "That attachment exceeds this channel's size limit.";
    }
    return {};
}

bool matchesMime(std::string_view mime, std::string_view bytes) {
    if (mime == "image/png") return bytes.starts_with("\x89PNG\r\n\x1a\n");
    if (mime == "image/jpeg") return bytes.starts_with("\xff\xd8\xff");
    if (mime == "image/webp") return bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8,4) == "WEBP";
    if (mime == "image/gif") return bytes.starts_with("GIF87a") || bytes.starts_with("GIF89a");
    if (mime == "text/plain") {
        if (bytes.find('\0') != bytes.npos) return false;
        try { (void)Json(bytes).dump(); }
        catch (const Json::type_error&) { return false; }
    }
    return true;
}

std::size_t decodedSize(std::string_view input) {
    if (input.empty() || input.size() % 4 != 0) throw Conflict("Invalid attachment encoding");
    return input.size() / 4 * 3 - input.ends_with('=') - input.ends_with("==");
}
}

std::string encodeAttachment(std::string_view bytes) {
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i+1 < bytes.size() ? static_cast<unsigned char>(bytes[i+1]) : 0;
        const unsigned c = i+2 < bytes.size() ? static_cast<unsigned char>(bytes[i+2]) : 0;
        result += alphabet[a >> 2];
        result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i+1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i+2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return result;
}

std::string decodeAttachment(std::string_view input, std::size_t maxBytes) {
    const auto size = decodedSize(input);
    if (!size || size > maxBytes) throw Conflict("Invalid attachment size");
    std::string result;
    result.reserve(size);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        unsigned value = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const auto digit = alphabet.find(input[i+j]);
            if (digit == alphabet.npos && !(input[i+j] == '=' && i+j >= input.size()-(input.size()/4*3-size)))
                throw Conflict("Invalid attachment encoding");
            value = (value << 6) | (digit == alphabet.npos ? 0u : static_cast<unsigned>(digit));
        }
        for (int shift : {16,8,0})
            if (result.size() < size) result += static_cast<char>((value >> shift) & 255);
    }
    if (encodeAttachment(result) != input) throw Conflict("Invalid attachment encoding");
    return result;
}

std::string_view channelName(ChannelKind kind) {
    return kind == ChannelKind::slack ? "slack" : "imessage";
}

Json ChannelOrigin::json() const {
    return {{"channelKind",channelName(kind)},{"workspaceId",workspaceId},{"threadRef",threadRef},
            {"externalMessageId",externalMessageId}};
}

std::string ChannelOrigin::key() const {
    return "channel:" + std::string(channelName(kind)) + ":" + segment(workspaceId) + ":" +
           segment(threadRef) + ":" + segment(externalMessageId);
}

Scope channelScope(const ChannelOrigin& origin, std::string_view operation) {
    return {"service",Json::array({channelName(origin.kind),origin.workspaceId}).dump(),std::string(operation)};
}

ChannelConnector::ChannelConnector(ChannelKind kind, ChannelCapabilities caps, AuthenticateChannel auth,
                                   std::shared_ptr<ChannelTransport> transport)
    : kind_(kind), capabilities_(std::move(caps)), authenticate_(std::move(auth)), transport_(std::move(transport)) {
    if (!authenticate_ || !transport_) throw Conflict("A channel needs authentication and transport");
}

std::optional<ChannelSender> ChannelConnector::authenticate(std::string_view credential, std::string_view workspace,
                                                             std::string_view sender) const {
    return authenticate_(credential,workspace,sender);
}

ChannelDelivery ChannelConnector::send(std::string_view key, const ChannelOrigin& origin,
                                       std::string_view operation, const Json& payload) const {
    if (origin.kind != kind_ || !limitation(capabilities_,operation,payload).empty())
        return {ChannelDelivery::State::rejected,{}};
    return transport_->send(key,origin,operation,payload);
}

ChannelDelivery ChannelConnector::reconcile(std::string_view key, const ChannelOrigin& origin) const {
    return transport_->reconcile(key,origin);
}

SlackConnector::SlackConnector(AuthenticateChannel auth, std::shared_ptr<ChannelTransport> transport,
                               ChannelCapabilities caps)
    : ChannelConnector(ChannelKind::slack,std::move(caps),std::move(auth),std::move(transport)) {}

IMessageConnector::IMessageConnector(AuthenticateChannel auth, std::shared_ptr<ChannelTransport> transport,
                                     ChannelCapabilities caps)
    : ChannelConnector(ChannelKind::imessage,std::move(caps),std::move(auth),std::move(transport)) {}

void Store::Impl::migrateChannels() {
    exec(R"SQL(CREATE TABLE IF NOT EXISTS channel_messages(
      message_id INTEGER PRIMARY KEY REFERENCES messages(id),
      event_key TEXT NOT NULL UNIQUE, origin TEXT NOT NULL, person_id TEXT NOT NULL))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS channel_attachments(
      id INTEGER PRIMARY KEY, message_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      position INTEGER NOT NULL, name TEXT NOT NULL, mime_type TEXT NOT NULL, bytes BLOB NOT NULL,
      UNIQUE(message_id,position)))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS channel_deliveries(
      id TEXT PRIMARY KEY, source_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      operation TEXT NOT NULL, payload TEXT NOT NULL,
      state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','unknown','accepted','rejected','unsupported')),
      external_id TEXT NOT NULL DEFAULT '', next_attempt INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL DEFAULT ''))SQL");
    exec("CREATE INDEX IF NOT EXISTS channel_delivery_due ON channel_deliveries(next_attempt) WHERE state IN ('pending','unknown')");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS channel_notices(
      source_id INTEGER NOT NULL REFERENCES channel_messages(message_id), reason TEXT NOT NULL,
      PRIMARY KEY(source_id,reason)))SQL");
    exec(R"SQL(CREATE TABLE IF NOT EXISTS channel_artifacts(
      id TEXT PRIMARY KEY, source_id INTEGER NOT NULL REFERENCES channel_messages(message_id),
      title TEXT NOT NULL, html TEXT NOT NULL))SQL");
}

void Store::connectChannel(std::shared_ptr<ChannelConnector> connector) {
    if (!connector) throw Conflict("Channel connector is required");
    std::lock_guard lock(p_->mutex);
    p_->channels[connector->kind()] = std::move(connector);
}

void Store::artifactBaseUrl(std::string url) {
    if (!url.empty() && (!url.starts_with("https://") || url.size() <= 8 ||
        url.find_first_of(" \r\n\t?#\\") != url.npos || url.find('\0') != url.npos))
        throw Conflict("Artifact base URL must use HTTPS");
    while (url.ends_with('/')) url.pop_back();
    std::lock_guard lock(p_->mutex);
    p_->artifactOrigin = std::move(url);
}

bool Store::Impl::permitsChannel(std::string_view bot, const ChannelOrigin& origin,
                                std::string_view person, std::string_view operation) const {
    auto access = Access(const_cast<Impl&>(*this));
    return access.permitsPerson({std::string(person),false},bot,Role::decide) &&
           access.permits(bot,channelScope(origin,operation),true);
}

Json Store::receiveChannel(ChannelKind kind, std::string_view credential, const Json& event,
                           const std::function<Intake(std::string_view,std::string_view)>& routing) {
    std::shared_ptr<ChannelConnector> connector;
    {
        std::lock_guard lock(p_->mutex);
        const auto found = p_->channels.find(kind);
        if (found == p_->channels.end()) throw ChannelAccessDenied("Channel is not connected");
        connector = found->second;
    }
    ChannelOrigin origin{kind,required(event,"workspaceId",128),required(event,"threadRef",200),
                         required(event,"externalMessageId",128)};
    const auto sender = connector->authenticate(credential,origin.workspaceId,required(event,"senderId",200));
    if (!sender || !p_->permitsChannel(sender->botId,origin,sender->personId,"use"))
        throw ChannelAccessDenied("Channel sender is not authorized");
    VerifiedChannelMessage message{origin,sender->personId,{}};
    const auto files = event.value("attachments",Json::array());
    if (!files.is_array() || files.size() > attachmentCount) throw Conflict("Too many attachments");
    const auto& caps = connector->capabilities();
    if (!files.empty() && !p_->permitsChannel(sender->botId,origin,sender->personId,"read"))
        throw ChannelAccessDenied("Channel attachment access is not granted");
    std::size_t total = 0;
    for (const auto& file : files) {
        const auto name = required(file,"name",255);
        const auto mime = required(file,"mimeType",128);
        const auto encoded = required(file,"contentBase64",attachmentLimit * 4 / 3 + 4);
        const auto size = decodedSize(encoded);
        const auto reason = limitation(caps,"channel_send_attachment",{{"mimeType",mime},{"size",size}});
        if (!reason.empty()) throw Conflict(reason);
        total += size;
        if (total > attachmentLimit) throw Conflict("Attachments exceed the total size limit");
        auto bytes = decodeAttachment(encoded,caps.maxAttachmentBytes);
        if (!matchesMime(mime,bytes)) throw Conflict("Attachment content does not match its MIME type");
        message.attachments.push_back({name,mime,std::move(bytes)});
    }
    auto text = field(event,"text");
    if (!conversation_storage::hasText(text) && !files.empty()) text = "Photo or file attached.";
    const auto intake = routing(sender->botId,text);
    if (intake.botId != sender->botId) throw ChannelAccessDenied("Channel route does not match the linked Bot");
    return receive(intake,origin.key(),text,0,&message);
}

void Store::Impl::saveChannelMessage(Id id, const VerifiedChannelMessage& message, bool replay) {
    auto botRow = query("SELECT bot_id FROM messages WHERE id=?",{id});
    if (!botRow.next()) throw Conflict("Channel message is missing");
    const auto bot = botRow.string(0);
    if (!permitsChannel(bot,message.origin,message.personId,"use") ||
        (!message.attachments.empty() && !permitsChannel(bot,message.origin,message.personId,"read")))
        throw ChannelAccessDenied("Channel access is no longer granted");
    const auto scrub = redactor(bot);
    scrub.requirePublic({message.origin.json(),message.personId});
    if (replay) {
        auto saved = query("SELECT 1 FROM channel_messages WHERE message_id=? AND event_key=? AND person_id=?",
                           {id,message.origin.key(),message.personId});
        if (!saved.next()) throw Conflict("Channel event already belongs to another sender");
        auto files = query("SELECT name,mime_type,bytes FROM channel_attachments WHERE message_id=? ORDER BY position",{id});
        for (const auto& file : message.attachments) {
            if (!files.next() || files.string(0) != file.name || files.string(1) != file.mimeType ||
                files.string(2) != file.bytes) throw Conflict("Channel event already has different attachments");
        }
        if (files.next()) throw Conflict("Channel event already has different attachments");
        return;
    }
    if (query("SELECT 1 FROM channel_messages WHERE event_key=?",{message.origin.key()}).next())
        throw Conflict("Channel event already belongs to another Bot");
    exec("INSERT INTO channel_messages VALUES(?,?,?,?)",{id,message.origin.key(),message.origin.json(),message.personId});
    Id position = 0;
    for (const auto& file : message.attachments) {
        scrub.requirePublic({file.name,file.mimeType,file.bytes});
        exec("INSERT INTO channel_attachments(message_id,position,name,mime_type,bytes) VALUES(?,?,?,?,CAST(? AS BLOB))",
             {id,position++,file.name,file.mimeType,file.bytes});
    }
}

Json Store::Impl::channelContext(Id id, std::string_view bot) const {
    auto row = query("SELECT c.origin,c.person_id FROM channel_messages c JOIN messages m ON m.id=c.message_id"
                     " WHERE c.message_id=? AND m.bot_id=?",{id,bot});
    if (!row.next()) return Json::object();
    const auto origin = row.json(0);
    Json context{{"channel",origin}};
    if (permitsChannel(bot,originFrom(origin),row.string(1),"read")) {
        auto files = query("SELECT id,name,mime_type,length(bytes) FROM channel_attachments WHERE message_id=? ORDER BY position",{id});
        auto& attachments = context["attachments"] = Json::array();
        while (files.next()) attachments.push_back({{"id",files.number(0)},{"name",files.string(1)},
            {"mimeType",files.string(2)},{"size",files.number(3)}});
    }
    if (!permitsChannel(bot,originFrom(origin),row.string(1),"use")) return context;
    auto sent = query(R"SQL(
SELECT d.external_id,json_extract(d.payload,'$.text') FROM channel_deliveries d
JOIN channel_messages c ON c.message_id=d.source_id JOIN messages m ON m.id=c.message_id
WHERE m.bot_id=? AND c.person_id=? AND d.state='accepted' AND d.operation='reply' AND d.external_id!=''
AND json_extract(c.origin,'$.channelKind')=? AND json_extract(c.origin,'$.workspaceId')=?
AND json_extract(c.origin,'$.threadRef')=? ORDER BY d.rowid DESC LIMIT 10
)SQL",{bot,row.string(1),origin.at("channelKind"),origin.at("workspaceId"),origin.at("threadRef")});
    auto& messages = context["channel"]["sentMessages"] = Json::array();
    while (sent.next()) messages.push_back({{"externalMessageId",sent.string(0)},{"text",bounded(sent.string(1),500)}});
    const auto connector = channels.find(originFrom(origin).kind);
    if (connector != channels.end()) {
        const auto& caps = connector->second->capabilities();
        context["channel"]["capabilities"] = {{"supportsReplies",caps.supportsReplies},{"supportsEdit",caps.supportsEdit},
            {"supportsReact",caps.supportsReact},{"supportsHtmlArtifacts",caps.supportsHtmlArtifacts},
            {"allowedAttachmentTypes",caps.allowedAttachmentTypes},{"maxAttachmentBytes",std::min(caps.maxAttachmentBytes,attachmentLimit)}};
    }
    return context;
}

Json Store::Impl::channelImages(Id id, std::string_view bot) const {
    const auto context = channelContext(id,bot);
    Json images = Json::array();
    for (const auto& ref : context.value("attachments",Json::array())) {
        const auto type = field(ref,"mimeType");
        if (type != "image/png" && type != "image/jpeg" && type != "image/webp" && type != "image/gif") continue;
        auto file = query("SELECT bytes FROM channel_attachments WHERE id=?",{ref.at("id")});
        if (file.next()) images.push_back("data:" + type + ";base64," + encodeAttachment(file.string(0)));
    }
    return images;
}

std::optional<ChannelResource> Store::attachment(const Person& person, Id id) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query(R"SQL(
SELECT a.name,a.mime_type,a.bytes,m.bot_id,c.origin,c.person_id
FROM channel_attachments a JOIN channel_messages c ON c.message_id=a.message_id JOIN messages m ON m.id=c.message_id
WHERE a.id=?)SQL",{id});
    if (!row.next() || !Access(*p_).permitsPerson(person,row.string(3),Role::view) ||
        !p_->permitsChannel(row.string(3),originFrom(row.json(4)),row.string(5),"read")) return std::nullopt;
    return ChannelResource{row.string(0),row.string(1),row.string(2)};
}

std::optional<ChannelResource> Store::artifact(std::string_view id) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query("SELECT title,html FROM channel_artifacts WHERE id=?",{id});
    if (!row.next()) return std::nullopt;
    return ChannelResource{row.string(0),"text/html; charset=utf-8",row.string(1)};
}

void Store::Impl::queueChannelReply(Id source, Id reply, const Json& data) {
    exec(R"SQL(
INSERT INTO channel_deliveries(id,source_id,operation,payload)
SELECT ?,message_id,'reply',? FROM channel_messages WHERE message_id=?)SQL",
         {"reply-" + std::to_string(reply) + "-" + actionId(),
          Json{{"text",data.at("text")},{"suggestions",data.value("suggestions",Json::array())}},source});
}

void Store::Impl::channelLimitation(Id source, std::string_view bot, std::string_view reason, bool supportsReplies) {
    exec("INSERT OR IGNORE INTO channel_notices VALUES(?,?)",{source,reason});
    if (!sqlite3_changes(db)) return;
    std::string notice(reason);
    if (supportsReplies) notice += " I can post a normal follow-up in this conversation if you'd like.";
    exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_reply','{}',?)",{unixTime(),bot});
    const auto reply = last();
    const Json data{{"messageId",reply},{"replyToMessageId",source},{"text",notice},{"status","done"}};
    exec("UPDATE journal SET data=? WHERE id=?",{data,reply});
    if (supportsReplies) queueChannelReply(source,reply,data);
}

Json Store::Impl::channelTool(Id message, std::string_view bot, const Decision& decision) {
    const auto& tool = decision.tool;
    if (tool != "channel_edit_message" && tool != "channel_react" && tool != "channel_send_attachment" &&
        tool != "publish_artifact" && tool != "read_channel_attachment") return nullptr;
    auto source = query("SELECT origin,person_id FROM channel_messages WHERE message_id=?",{message});
    if (!source.next()) throw Conflict("This tool requires a channel conversation");
    const auto origin = originFrom(source.json(0));
    const auto person = source.string(1);
    const auto found = channels.find(origin.kind);
    if (found == channels.end()) throw Conflict("Channel is disconnected");
    const auto& caps = found->second->capabilities();
    if (!permitsChannel(bot,origin,person,tool == "read_channel_attachment" ? "read" : "use"))
        throw ChannelAccessDenied("Channel access is no longer granted");

    const auto& args = decision.args;
    const auto attachment = [&]() {
        if (!permitsChannel(bot,origin,person,"read"))
            throw ChannelAccessDenied("Channel attachment access is no longer granted");
        auto rows = query(R"SQL(
SELECT a.name,a.mime_type,a.bytes FROM channel_attachments a
JOIN channel_messages c ON c.message_id=a.message_id JOIN messages m ON m.id=c.message_id
WHERE a.id=? AND m.bot_id=? AND json_extract(c.origin,'$.channelKind')=?
AND json_extract(c.origin,'$.workspaceId')=? AND json_extract(c.origin,'$.threadRef')=? AND c.person_id=?
)SQL",{conversation_storage::integer(args,"attachmentId"),bot,channelName(origin.kind),origin.workspaceId,origin.threadRef,person});
        if (!rows.next()) throw Conflict("Attachment is not in this conversation");
        return ChannelResource{rows.string(0),rows.string(1),rows.string(2)};
    };
    if (tool == "read_channel_attachment") {
        const auto file = attachment();
        if (file.mimeType != "text/plain") throw Conflict("Image attachments are provided directly in the model input");
        return {{"ok",true},{"name",file.name},{"text",bounded(file.bytes,16000)},{"truncated",file.bytes.size() > 16000}};
    }

    Json payload = Json::object();
    std::string bytes;
    if (tool == "channel_send_attachment") {
        if (args.contains("attachmentId")) {
            auto file = attachment();
            bytes = std::move(file.bytes);
            payload = {{"name",file.name},{"mimeType",file.mimeType},{"size",bytes.size()},
                       {"attachmentId",args.at("attachmentId")}};
        } else {
            const auto mime = required(args,"mimeType",128);
            const auto encoded = field(args,"contentBase64");
            payload = {{"name",required(args,"name",255)},{"mimeType",mime},{"size",decodedSize(encoded)}};
        }
    }
    if (auto reason = limitation(caps,tool,payload); !reason.empty()) {
        channelLimitation(message,bot,reason,caps.supportsReplies);
        return {{"ok",false},{"unsupported",true},{"reason",reason},{"noticeQueued",caps.supportsReplies}};
    }

    if (tool == "channel_edit_message") {
        const auto target = required(args,"externalMessageId",128);
        if (!query(R"SQL(
SELECT 1 FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id
JOIN messages m ON m.id=c.message_id WHERE m.bot_id=? AND d.operation IN ('reply','channel_send_attachment','publish_artifact')
AND d.state='accepted' AND d.external_id=? AND json_extract(c.origin,'$.channelKind')=?
AND json_extract(c.origin,'$.workspaceId')=? AND json_extract(c.origin,'$.threadRef')=? AND c.person_id=?
)SQL",{bot,target,channelName(origin.kind),origin.workspaceId,origin.threadRef,person}).next())
            throw Conflict("The target must be a message this Bot sent in this channel thread");
        payload = {{"externalMessageId",target},{"text",required(args,"text",8000)}};
    } else if (tool == "channel_react") {
        payload = {{"externalMessageId",origin.externalMessageId},{"reaction",required(args,"reaction",80)}};
    } else if (tool == "channel_send_attachment") {
        if (bytes.empty()) bytes = decodeAttachment(field(args,"contentBase64"),std::min(caps.maxAttachmentBytes,attachmentLimit));
        if (!matchesMime(field(payload,"mimeType"),bytes)) throw Conflict("Attachment content does not match its MIME type");
        if (!payload.contains("attachmentId")) {
            redactor(bot).requirePublic(bytes);
            payload["contentBase64"] = encodeAttachment(bytes);
        }
    } else if (tool == "publish_artifact") {
        const auto title = required(args,"title",255);
        const auto html = required(args,"html",256 * 1024);
        redactor(bot).requirePublic({title,html});
        const auto id = actionId();
        exec("INSERT INTO channel_artifacts VALUES(?,?,?,?)",{id,message,title,html});
        payload = {{"title",title},{"url",artifactOrigin + "/api/artifacts/" + id}};
    }
    redactor(bot).requirePublic(payload);
    const auto key = actionId();
    exec("INSERT INTO channel_deliveries(id,source_id,operation,payload) VALUES(?,?,?,?)",{key,message,tool,payload});
    Json result{{"ok",true},{"delivery","queued"},{"deliveryId",key}};
    if (tool == "publish_artifact") {
        result["url"] = payload.at("url");
        result["artifact"] = payload;
    }
    return result;
}

Json Store::Impl::channelToolStatus(Json result) const {
    const auto key = field(result,"deliveryId");
    if (key.empty()) return result;
    auto row = query("SELECT state,external_id,reason FROM channel_deliveries WHERE id=?",{key});
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

void Store::Impl::deliverChannels() {
    std::unique_lock deliveryLock(deliveryMutex,std::try_to_lock);
    if (!deliveryLock.owns_lock()) return;
    for (int i = 0; i < 20; ++i) {
        std::shared_ptr<ChannelConnector> connector;
        ChannelOrigin origin{};
        std::string key, operation, state, bot;
        Json payload;
        {
            std::lock_guard lock(mutex);
            Transaction tx(db);
            auto row = query(R"SQL(
SELECT d.id,d.operation,d.payload,d.state,c.origin,c.person_id,m.bot_id,d.source_id
FROM channel_deliveries d JOIN channel_messages c ON c.message_id=d.source_id JOIN messages m ON m.id=c.message_id
WHERE d.state IN ('pending','unknown') AND next_attempt<=? ORDER BY d.rowid LIMIT 1
)SQL",{unixTime()});
            if (!row.next()) return;
            key = row.string(0);
            operation = row.string(1);
            payload = row.json(2);
            state = row.string(3);
            origin = originFrom(row.json(4));
            bot = row.string(6);
            const auto found = channels.find(origin.kind);
            if (found != channels.end()) connector = found->second;
            std::string blocked;
            if (!connector) {
                exec("UPDATE channel_deliveries SET next_attempt=? WHERE id=?",{unixTime()+5,key});
                tx.commit();
                continue;
            }
            const auto reason = state == "pending" ? limitation(connector->capabilities(),operation,payload) : std::string{};
            if (!permitsChannel(bot,origin,row.string(5),"use")) blocked = "rejected";
            else if (!reason.empty()) {
                blocked = "unsupported";
                if (operation != "reply") channelLimitation(row.number(7),bot,reason,connector->capabilities().supportsReplies);
            }
            if (blocked.empty() && state == "pending" && payload.contains("attachmentId")) {
                auto file = query("SELECT bytes FROM channel_attachments WHERE id=?",{payload.at("attachmentId")});
                if (!permitsChannel(bot,origin,row.string(5),"read") || !file.next()) blocked = "rejected";
                else payload["contentBase64"] = encodeAttachment(file.string(0));
            }
            if (!blocked.empty()) {
                exec("UPDATE channel_deliveries SET state=?,reason=? WHERE id=?",{blocked,reason,key});
                tx.commit();
                continue;
            }
            exec("UPDATE channel_deliveries SET state='unknown',next_attempt=? WHERE id=?",{unixTime()+5,key});
            tx.commit();
        }
        ChannelDelivery outcome;
        try {
            outcome = state == "pending" ? connector->send(key,origin,operation,payload)
                                          : connector->reconcile(key,origin);
        } catch (const std::exception&) {}
        if (outcome.state == ChannelDelivery::State::unknown) continue;
        std::lock_guard lock(mutex);
        const auto scrub = redactor(bot);
        const auto external = scrub.text(bounded(outcome.externalMessageId,128));
        const auto next = outcome.state == ChannelDelivery::State::accepted ? "accepted" :
                          outcome.state == ChannelDelivery::State::notSent ? "pending" : "rejected";
        exec("UPDATE channel_deliveries SET state=?,external_id=? WHERE id=? AND state='unknown'",
             {next,external,key});
    }
}
}
