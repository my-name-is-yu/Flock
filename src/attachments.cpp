#include "storage.hpp"
#include "flock/message_attachments.hpp"

#include <csetjmp>
#include <jpeglib.h>
#include <png.h>
#include <set>

namespace flockv0 {
namespace {
using namespace message_attachments;

using ImageSize = std::pair<unsigned, unsigned>;

bool supportedImageSize(unsigned width, unsigned height) {
    return width && height && width <= maxImageEdge && height <= maxImageEdge && width <= maxImagePixels / height;
}

struct PngImage {
    png_image image{};
    PngImage() { image.version = PNG_IMAGE_VERSION; }
    ~PngImage() { png_image_free(&image); }
};

ImageSize pngSize(std::string_view bytes) {
    PngImage reader;
    auto& image = reader.image;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()) || !supportedImageSize(image.width, image.height)) return {};
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) || image.warning_or_error) return {};
    return {image.width, image.height};
}

struct JpegErrors {
    jpeg_error_mgr manager{};
    std::jmp_buf jump;
};

void failJpeg(j_common_ptr codec) {
    std::longjmp(reinterpret_cast<JpegErrors*>(codec->err)->jump, 1);
}

void checkJpegMessage(j_common_ptr codec, int level) {
    if (level < 0) failJpeg(codec);
}

struct JpegImage {
    jpeg_decompress_struct codec{};
    JpegErrors errors;
    JpegImage() {
        codec.err = jpeg_std_error(&errors.manager);
        errors.manager.error_exit = failJpeg;
        errors.manager.emit_message = checkJpegMessage;
    }
    ~JpegImage() { jpeg_destroy_decompress(&codec); }
};

ImageSize jpegSize(std::string_view bytes) {
    const auto reader = std::make_unique<JpegImage>();
    auto& codec = reader->codec;
    if (setjmp(reader->errors.jump)) return {};
    jpeg_create_decompress(&codec);
    jpeg_mem_src(&codec, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    if (jpeg_read_header(&codec, TRUE) != JPEG_HEADER_OK || !supportedImageSize(codec.image_width, codec.image_height) ||
        !jpeg_start_decompress(&codec))
        return {};
    const auto row =
        codec.mem->alloc_sarray(reinterpret_cast<j_common_ptr>(&codec), JPOOL_IMAGE, codec.output_width * codec.output_components, 1);
    while (codec.output_scanline < codec.output_height)
        if (jpeg_read_scanlines(&codec, row, 1) != 1) return {};
    if (!jpeg_finish_decompress(&codec)) return {};
    return {codec.image_width, codec.image_height};
}

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
    const auto name = toolText(value, "name", 128);
    const auto mime = toolText(value, "mimeType", 64);
    const bool isImage = mime == "image/png" || mime == "image/jpeg";
    const auto encoded = value.find("dataBase64");
    if (encoded == value.end() || !encoded->is_string()) throw Conflict("Invalid attachment base64 data");
    const auto bytes = decodeAttachment(encoded->get_ref<const std::string&>(), isImage ? maxImageBytes : maxAudioBytes);
    const std::string_view data(bytes);
    Json result{{"name", name}, {"mimeType", mime}, {"sizeBytes", data.size()}};
    const bool png = mime == "image/png" && data.starts_with("\x89PNG\r\n\x1a\n");
    const bool jpeg = mime == "image/jpeg" && data.starts_with("\xff\xd8\xff");
    const bool wav = mime == "audio/wav" && data.size() >= 44 && data.starts_with("RIFF") && data.substr(8, 4) == "WAVE";
    const bool mp3 = mime == "audio/mpeg" && data.size() >= 4 &&
                     (data.starts_with("ID3") || (static_cast<unsigned char>(data[0]) == 0xff && (data[1] & 0xe0) == 0xe0));
    if (!png && !jpeg && !wav && !mp3) throw Conflict("Attachment must contain PNG, JPEG, WAV, or MP3 data matching its MIME type");
    if (wav) describeWav(data, result);
    result["kind"] = isImage ? "image" : "audio";
    return result;
}
}

void Store::Impl::saveAttachments(Id message, const Json& attachments) {
    if (!attachments.is_array() || attachments.size() > maxFiles) throw Conflict("Attachments exceed the message limit");
    for (std::size_t i = 0; i < attachments.size(); ++i) {
        const auto& attachment = attachments[i];
        const auto id = std::to_string(i + 1);
        auto view = metadata(attachment);
        view["attachmentId"] = id;
        exec("INSERT INTO message_attachments VALUES(?,?,?,?)", {message, id, view, attachment});
    }
}

Json Store::Impl::attachmentViews(Id message) const {
    auto rows = query("SELECT attachment_id FROM message_attachments WHERE message_id=? ORDER BY attachment_id", {message});
    auto result = Json::array();
    while (rows.next()) result.push_back({{"attachmentId", rows.string(0)}});
    return result;
}

Statement Store::Impl::scopedAttachment(std::string_view bot, Id throughMessage, const Json& args) const {
    return query("SELECT a.metadata,a.payload FROM message_attachments a JOIN messages m ON m.id=a.message_id "
                 "WHERE m.bot_id=? AND a.message_id<=? AND a.message_id=? AND a.attachment_id=?",
                 {bot, throughMessage, toolId(args, "messageId"), toolText(args, "attachmentId", 128)});
}

Json Store::Impl::inspectAttachment(std::string_view bot, Id throughMessage, const Json& args) {
    Json view, attachment;
    {
        std::lock_guard lock(mutex);
        auto row = scopedAttachment(bot, throughMessage, args);
        if (!row.next()) throw Conflict("Attachment is unavailable in this conversation");
        view = row.json(0);
        if (view.at("kind") != "image") return view;
        attachment = row.json(1);
    }
    const auto bytes = decodeAttachment(attachment.at("dataBase64").get_ref<const std::string&>(), maxImageBytes);
    const auto [width, height] = view.at("mimeType") == "image/png" ? pngSize(bytes) : jpegSize(bytes);
    if (width == 0 || height == 0) throw Conflict("Attachment image is corrupt or exceeds the pixel limit");
    view.update({{"width", width}, {"height", height}});
    return view;
}

std::vector<ModelImage> Store::Impl::inspectedImages(std::string_view bot, Id message, const Json& steps) const {
    std::vector<ModelImage> images;
    std::set<std::pair<Id, std::string>> seen;
    for (auto it = steps.rbegin(); it != steps.rend() && images.size() < maxFiles; ++it) {
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
        const auto& base64 = attachment.at("dataBase64").get_ref<const std::string&>();
        images.push_back({id, attachmentId, attachment.at("mimeType"), base64});
    }
    return images;
}
}
