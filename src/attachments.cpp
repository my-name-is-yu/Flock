#include "storage.hpp"

#include <array>
#include <set>

namespace flockv0 {
namespace {
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string decodeAttachment(std::string_view base64) {
    if (base64.empty() || base64.size() > 88000 || base64.size() % 4 != 0)
        throw Conflict("Attachment exceeds the size limit or has invalid base64 data");
    std::string bytes;
    bytes.reserve(base64.size()/4*3);
    for (std::size_t pos = 0; pos < base64.size(); pos += 4) {
        const auto block = base64.substr(pos,4);
        std::array<unsigned,4> sextets{};
        unsigned padding = 0;
        for (unsigned i = 0; i < block.size(); ++i) {
            if (block[i] == '=' && i >= 2 && pos+4 == base64.size()) { ++padding; continue; }
            const auto value = alphabet.find(block[i]);
            if (value == alphabet.npos || padding) throw Conflict("Invalid attachment base64 data");
            sextets[i] = static_cast<unsigned>(value);
        }
        if ((padding == 2 && (sextets[1] & 15)) || (padding == 1 && (sextets[2] & 3)))
            throw Conflict("Invalid attachment base64 data");
        bytes += static_cast<char>((sextets[0] << 2) | (sextets[1] >> 4));
        if (padding < 2) bytes += static_cast<char>((sextets[1] << 4) | (sextets[2] >> 2));
        if (padding == 0) bytes += static_cast<char>((sextets[2] << 6) | sextets[3]);
    }
    if (bytes.size() > 65536) throw Conflict("Attachment exceeds the size limit");
    return bytes;
}

std::uint32_t bigEndian(std::string_view data, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = offset; i < offset+4; ++i) value = (value << 8) | static_cast<unsigned char>(data[i]);
    return value;
}

std::uint32_t littleEndian(std::string_view data, std::size_t offset, unsigned size) {
    std::uint32_t value = 0;
    while (size) value = (value << 8) | static_cast<unsigned char>(data[offset + --size]);
    return value;
}

void describeWav(std::string_view data, Json& result) {
    std::uint32_t bytesPerSecond = 0, samples = 0;
    for (std::size_t pos = 12; pos + 8 <= data.size();) {
        const auto chunk = data.substr(pos,4);
        const auto size = littleEndian(data,pos+4,4);
        pos += 8;
        if (size > data.size()-pos) throw Conflict("Invalid WAV chunk size");
        if (chunk == "fmt " && size >= 16) {
            result["channels"] = littleEndian(data,pos+2,2);
            result["sampleRateHz"] = littleEndian(data,pos+4,4);
            bytesPerSecond = littleEndian(data,pos+8,4);
            result["bitsPerSample"] = littleEndian(data,pos+14,2);
        }
        if (chunk == "data") samples = size;
        pos += size + size%2;
    }
    if (bytesPerSecond) result["durationMs"] = 1000ull*samples/bytesPerSecond;
}

Json metadata(const Json& value, const SecretRedactor& scrub) {
    if (!value.is_object() || value.size() != 3) throw Conflict("Invalid attachment");
    const auto name = toolText(value,"name",128);
    const auto mime = toolText(value,"mimeType",64);
    const auto bytes = decodeAttachment(toolText(value,"dataBase64",88000));
    if (scrub.text(bytes) != bytes) throw Conflict("Protected secret values cannot be stored in attachments");
    const std::string_view data(bytes);
    Json result{{"name",name},{"mimeType",mime},{"sizeBytes",data.size()}};
    if (mime == "image/png" && data.size() >= 33 && data.starts_with("\x89PNG\r\n\x1a\n") &&
        data.substr(12,4) == "IHDR" && bigEndian(data,8) == 13) {
        const auto width = bigEndian(data,16);
        const auto height = bigEndian(data,20);
        if (!width || !height || width > 32768 || height > 32768) throw Conflict("Invalid PNG dimensions");
        result.update({{"width",width},{"height",height}});
    } else {
        const bool jpeg = mime == "image/jpeg" && data.size() >= 4 && data.starts_with("\xff\xd8\xff");
        const bool wav = mime == "audio/wav" && data.size() >= 44 && data.starts_with("RIFF") && data.substr(8,4) == "WAVE";
        const bool mp3 = mime == "audio/mpeg" && data.size() >= 4 &&
            (data.starts_with("ID3") || (static_cast<unsigned char>(data[0]) == 0xff && (data[1] & 0xe0) == 0xe0));
        if (!jpeg && !wav && !mp3)
            throw Conflict("Attachment must contain PNG, JPEG, WAV, or MP3 data matching its MIME type");
        if (wav) describeWav(data,result);
    }
    result["kind"] = mime.starts_with("image/") ? "image" : "audio";
    return result;
}
}

void Store::Impl::migrateAttachments() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS message_attachments(
 message_id INTEGER NOT NULL REFERENCES messages(id), attachment_id TEXT NOT NULL,
 metadata TEXT NOT NULL, payload TEXT NOT NULL, PRIMARY KEY(message_id,attachment_id));
)SQL");
}

void Store::Impl::saveAttachments(Id message, const Json& attachments) {
    if (!attachments.is_array() || attachments.size() > 4 || attachments.dump().size() > 98000)
        throw Conflict("Attachments exceed the message limit");
    if (attachments.empty()) return;
    const auto scrub = sessionRedactor();
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const auto& attachment = attachments[i];
        const auto id = std::to_string(i+1);
        scrub.requirePublic(attachment);
        auto view = metadata(attachment,scrub);
        view["attachmentId"] = id;
        exec("INSERT INTO message_attachments VALUES(?,?,?,?)",{message,id,view,attachment});
    }
}

Json Store::Impl::attachmentViews(Id message) const {
    auto rows = query("SELECT attachment_id FROM message_attachments WHERE message_id=? ORDER BY attachment_id",{message});
    auto result = Json::array();
    while (rows.next()) result.push_back({{"attachmentId",rows.string(0)}});
    return result;
}

Statement Store::Impl::scopedAttachment(std::string_view bot, Id throughMessage, const Json& args) const {
    return query("SELECT a.metadata,a.payload FROM message_attachments a JOIN messages m ON m.id=a.message_id "
                 "WHERE m.bot_id=? AND a.message_id<=? AND a.message_id=? AND a.attachment_id=?",
                 {bot,throughMessage,toolId(args,"messageId"),toolText(args,"attachmentId",128)});
}

std::vector<ModelImage> Store::Impl::inspectedImages(std::string_view bot, Id message, const Json& steps) const {
    std::vector<ModelImage> images;
    std::set<std::pair<Id,std::string>> seen;
    for (auto it = steps.rbegin(); it != steps.rend() && images.size() < 4; ++it) {
        const auto& entry = *it;
        if (entry.at("tool") != "inspect_attachment" || !entry.at("result").at("ok").get<bool>() ||
            entry.at("result").at("data").value("kind","") != "image") continue;
        const auto& args = entry.at("args");
        const auto id = toolId(args,"messageId");
        const auto attachmentId = toolText(args,"attachmentId",128);
        if (!seen.emplace(id,attachmentId).second) continue;
        auto row = scopedAttachment(bot,message,args);
        if (!row.next()) continue;
        const auto attachment = row.json(1);
        const auto& base64 = attachment.at("dataBase64").get_ref<const std::string&>();
        const auto bytes = decodeAttachment(base64);
        if (sessionRedactor().text(bytes) == bytes)
            images.push_back({id,attachmentId,attachment.at("mimeType"),base64});
    }
    return images;
}
}
