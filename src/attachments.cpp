#include "attachment_storage.hpp"
#include "database.hpp"
#include "conversation_storage.hpp"
#include "workspace_internal.hpp"
#include "flock/channels.hpp"
#include "flock/conversations.hpp"
#include "flock/limits.hpp"
#include "image_headers.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/secrets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
using namespace message_attachments;
using limits::messages::maxAttachments;
using limits::messages::maxImageBytes;
using limits::messages::maxFileBytes;

std::uint32_t littleEndian(std::string_view data, std::size_t offset, unsigned size) {
    std::uint32_t value = 0;
    while (size) value = (value << 8) | static_cast<unsigned char>(data[offset + --size]);
    return value;
}

void describeWav(std::string_view data, Json& result) {
    std::uint32_t bytesPerSecond = 0, samples = 0;
    for (std::size_t pos = 12; pos + 8 <= data.size();) {
        const auto chunk = data.substr(pos, 4);
        const auto size = littleEndian(data, pos + 4, 4);
        pos += 8;
        if (size > data.size() - pos) throw Conflict("Invalid WAV chunk size");
        if (chunk == "fmt " && size >= 16) {
            result["channels"] = littleEndian(data, pos + 2, 2);
            result["sampleRateHz"] = littleEndian(data, pos + 4, 4);
            bytesPerSecond = littleEndian(data, pos + 8, 4);
            result["bitsPerSample"] = littleEndian(data, pos + 14, 2);
        }
        if (chunk == "data") samples = size;
        pos += size + size % 2;
    }
    if (bytesPerSecond) result["durationMs"] = 1000ull * samples / bytesPerSecond;
}

Json metadata(const Json& value) {
    if (!value.is_object() || value.size() != 3) throw Conflict("Invalid attachment");
    const auto name = toolText(value, "name", 1020);
    const auto mime = toolText(value, "mimeType", 400);
    if (name.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
        mime.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw Conflict("Attachment name or MIME type is invalid");
    const bool isImage = limits::messages::isAttachmentImage(mime);
    const auto encoded = value.find("dataBase64");
    if (encoded == value.end() || !encoded->is_string()) throw Conflict("Invalid attachment base64 data");
    std::string bytes;
    try {
        bytes = decodeAttachment(encoded->get_ref<const std::string&>(), isImage ? maxImageBytes : maxFileBytes);
    } catch (const Conflict& error) {
        throw Conflict(name + ": " + error.what());
    }
    const std::string_view data(bytes);
    Json result{{"name", name}, {"mimeType", mime}, {"sizeBytes", data.size()}};
    const bool png = mime == "image/png" && data.starts_with(pngSignature);
    const bool jpeg = mime == "image/jpeg" && data.starts_with(jpegSignature);
    const bool wav = mime == "audio/wav" && data.size() >= 44 && data.starts_with("RIFF") && data.substr(8, 4) == "WAVE";
    const bool mp3 = mime == "audio/mpeg" && data.size() >= 4 &&
                     (data.starts_with("ID3") || (static_cast<unsigned char>(data[0]) == 0xff && (data[1] & 0xe0) == 0xe0));
    const bool gif = data.starts_with("GIF87a") || data.starts_with("GIF89a");
    const bool webp = data.size() >= 12 && data.starts_with("RIFF") && data.substr(8, 4) == "WEBP";
    if (isImage && !png && !jpeg && !(mime == "image/gif" && gif) && !(mime == "image/webp" && webp))
        throw Conflict(name + ": image data does not match its MIME type");
    if (wav) describeWav(data, result);
    result["kind"] = isImage ? "image" : (wav || mp3 ? "audio" : "file");
    return result;
}

AttachmentFile readFile(const Json& payload, const SecretRedactor& scrub, std::string_view label) {
    auto name = field(payload, "name");
    auto mime = field(payload, "mimeType");
    auto bytes = decodeAttachment(payload.at("dataBase64").get_ref<const std::string&>(),
                                  limits::messages::isAttachmentImage(mime) ? maxImageBytes : maxFileBytes);
    if (scrub.containsProtectedText(name) || scrub.containsProtectedText(mime) || scrub.containsProtectedText(bytes))
        throw Conflict(scrub.containsProtectedText(name) ? std::string(label) + " has a protected filename" :
                                                           name + ": contains protected information");
    return {std::move(name), std::move(mime), std::move(bytes)};
}
}

void AttachmentStorage::saveAttachments(Id message, const Json& attachments) {
    if (!attachments.is_array()) throw InvalidAttachment("Attachments must be a list of files");
    if (attachments.size() > maxAttachments)
        throw InvalidAttachment("Attach up to eight files per message");
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const auto& attachment = attachments[i];
        const auto id = std::to_string(i + 1);
        Json view;
        try {
            view = metadata(attachment);
        } catch (const Conflict& error) {
            throw InvalidAttachment(error.what());
        }
        view["attachmentId"] = id;
        db_.exec("INSERT INTO message_attachments VALUES(?,?,?,?)", {message, id, view, attachment});
    }
}

Json AttachmentStorage::attachmentViews(Id message) const {
    auto rows = db_.query("SELECT attachment_id FROM message_attachments WHERE message_id=? ORDER BY attachment_id", {message});
    auto result = Json::array();
    while (rows.next()) result.push_back({{"attachmentId", rows.string(0)}});
    return result;
}

Statement AttachmentStorage::scopedAttachment(std::string_view bot, Id throughMessage, const Json& args) const {
    return db_.query("SELECT a.metadata,a.payload FROM message_attachments a JOIN messages m ON m.id=a.message_id "
                     "WHERE m.bot_id=? AND a.message_id<=? AND a.message_id=? AND a.attachment_id=?",
                     {bot, throughMessage, toolId(args, "messageId"), toolText(args, "attachmentId", 128)});
}

bool AttachmentStorage::hasAttachment(std::string_view bot, Id throughMessage, const Json& args) const {
    return scopedAttachment(bot, throughMessage, args).next();
}

std::vector<TaskAttachmentRef> AttachmentStorage::selectForTask(const conversation::Message& message, const Json& args) const {
    using conversation_storage::ToolError;
    const auto selected = args.find("attachments");
    if (selected == args.end()) return {};
    if (!selected->is_array() || selected->size() > maxAttachments)
        throw ToolError("Select up to eight attachments per T3 instruction");
    if (selected->empty()) return {};
    if (message.botOrigin || (message.principalSource != conversation::PrincipalSource::web &&
                              message.principalSource != conversation::PrincipalSource::trusted))
        throw ToolError("Only a person's Flock Web request can share Flock Web attachments with a task");
    std::vector<TaskAttachmentRef> refs;
    for (const auto& item : *selected) {
        if (!item.is_object() || item.size() != 2) throw ToolError("An attachment needs its messageId and attachmentId");
        TaskAttachmentRef ref{toolId(item, "messageId"), toolText(item, "attachmentId", 128)};
        auto source = db_.query("SELECT a.metadata FROM message_attachments a JOIN messages m ON m.id=a.message_id "
                       "WHERE m.bot_id=? AND m.person_id=? AND a.message_id<=? AND a.message_id=? AND a.attachment_id=?",
                       {message.bot, message.admittedActor.id, message.id, ref.messageId, ref.attachmentId});
        if (!source.next())
            throw ToolError("Attachment " + ref.attachmentId + " on message " + std::to_string(ref.messageId) +
                            " is unavailable or you do not have permission to share it");
        if (std::ranges::find(refs, ref) != refs.end())
            throw ToolError(field(source.json(0), "name") + " was selected twice");
        refs.push_back(std::move(ref));
    }
    return refs;
}

std::vector<AttachmentFile> AttachmentStorage::taskAttachments(
    std::string_view bot, const std::vector<TaskAttachmentRef>& refs, const SecretRedactor& scrub) const {
    std::vector<AttachmentFile> files;
    for (const auto& ref : refs) {
        auto row = scopedAttachment(bot, ref.messageId, {{"messageId", ref.messageId}, {"attachmentId", ref.attachmentId}});
        const auto label = "Attachment " + ref.attachmentId + " on message " + std::to_string(ref.messageId);
        if (!row.next()) throw Conflict(label + " is no longer available");
        files.push_back(readFile(row.json(1), scrub, label));
    }
    return files;
}

std::optional<AttachmentFile> Conversations::attachment(const Person& viewer, std::string_view bot, Id message, Id attachment) const {
    Json payload;
    SecretRedactor redactor({});
    {
        std::lock_guard lock(database_.mutex);
        const Policy policy{store_};
        if (viewer.id.empty() || !policy.allows(viewer, Action::bot_view, bot)) return std::nullopt;
        auto row = database_.query("SELECT a.payload FROM message_attachments a JOIN messages m ON m.id=a.message_id "
                                   "WHERE m.bot_id=? AND m.person_id=? AND a.message_id=? AND a.attachment_id=?",
                                   {bot, viewer.id, message, std::to_string(attachment)});
        if (!row.next()) return std::nullopt;
        payload = row.json(0);
        redactor = policy.secretRedactor(bot);
    }
    try {
        return readFile(payload, redactor, "Attachment");
    } catch (const Conflict&) {
        return std::nullopt;
    }
}

Json AttachmentStorage::inspectAttachment(std::string_view bot, Id throughMessage, const Json& args) {
    Json view, attachment;
    {
        std::lock_guard lock(db_.mutex);
        auto row = scopedAttachment(bot, throughMessage, args);
        if (!row.next()) throw Conflict("Attachment is unavailable in this conversation");
        view = row.json(0);
        const auto mime = field(view, "mimeType");
        if (mime != "image/png" && mime != "image/jpeg") return view;
        attachment = row.json(1);
    }
    const auto bytes = decodeAttachment(attachment.at("dataBase64").get_ref<const std::string&>(), maxImageBytes);
    const auto size = view.at("mimeType") == "image/png" ? pngHeaderSize(bytes) : jpegHeaderSize(bytes);
    if (!size) throw Conflict("Attachment image is corrupt or exceeds the pixel limit");
    view.update({{"width", size->width}, {"height", size->height}});
    return view;
}

std::vector<ModelImage> AttachmentStorage::inspectedImages(std::string_view bot, Id message, const Json& steps) const {
    std::vector<ModelImage> images;
    std::set<std::pair<Id, std::string>> seen;
    for (auto it = steps.rbegin(); it != steps.rend() && images.size() < maxAttachments; ++it) {
        const auto& entry = *it;
        if (entry.at("tool") != "inspect_attachment" || !entry.at("result").at("ok").get<bool>() ||
            entry.at("result").at("data").value("kind", "") != "image")
            continue;
        const auto& args = entry.at("args");
        const auto id = toolId(args, "messageId");
        const auto attachmentId = toolText(args, "attachmentId", 128);
        if (!seen.emplace(id, attachmentId).second) continue;
        auto row = scopedAttachment(bot, message, args);
        if (!row.next()) continue;
        const auto attachment = row.json(1);
        const auto mime = field(attachment, "mimeType");
        if (mime != "image/png" && mime != "image/jpeg" && mime != "image/webp") continue;
        const auto& base64 = attachment.at("dataBase64").get_ref<const std::string&>();
        images.push_back({id, attachmentId, attachment.at("mimeType"), base64});
    }
    return images;
}
}
