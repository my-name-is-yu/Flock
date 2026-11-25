#include "image_headers.hpp"
#include "flock/message_attachments.hpp"

#include <bitset>

namespace flockv0::message_attachments {
namespace {
constexpr unsigned char jpegTemporaryMarker = 0x01, jpegBaselineFrame = 0xc0;
constexpr unsigned char jpegFirstRestart = 0xd0, jpegStartOfScan = 0xda;

std::uint32_t bigEndian(std::string_view bytes) {
    std::uint32_t value = 0;
    for (unsigned char byte : bytes) value = (value << 8) | byte;
    return value;
}

std::optional<ImageSize> boundedImageSize(std::uint32_t width, std::uint32_t height) {
    if (!width || !height || width > maxImageEdge || height > maxImageEdge || width > maxImagePixels / height) return std::nullopt;
    return ImageSize{width, height};
}

std::uint32_t pngCrc(std::string_view bytes) {
    constexpr std::uint32_t polynomial = 0xedb88320;
    std::uint32_t crc = 0xffffffff;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? polynomial : 0);
    }
    return ~crc;
}

bool validPngDepth(char colorType, char depth) {
    switch (colorType) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    default: return false;
    }
}

bool isJpegFrame(unsigned char marker) {
    constexpr unsigned char huffmanTable = 0xc4, extension = 0xc8, arithmeticTable = 0xcc, lastFrame = 0xcf;
    return marker >= jpegBaselineFrame && marker <= lastFrame && marker != huffmanTable && marker != extension && marker != arithmeticTable;
}

std::optional<ImageSize> jpegFrameSize(unsigned char marker, std::string_view frame) {
    constexpr std::size_t headerBytes = 6, componentBytes = 3;
    if (frame.size() < headerBytes) return std::nullopt;
    const bool progressive = marker == 0xc2 || marker == 0xc6 || marker == 0xca || marker == 0xce;
    const bool lossless = marker == 0xc3 || marker == 0xc7 || marker == 0xcb || marker == 0xcf;
    const auto components = static_cast<unsigned char>(frame[5]);
    if (!components || (progressive && components > 4) || frame.size() != headerBytes + componentBytes * components) return std::nullopt;
    const auto precision = static_cast<unsigned char>(frame[0]);
    const bool validPrecision =
        lossless ? (precision >= 2 && precision <= 16) : (precision == 8 || (precision == 12 && marker != jpegBaselineFrame));
    if (!validPrecision) return std::nullopt;

    const auto size = boundedImageSize(bigEndian(frame.substr(3, 2)), bigEndian(frame.substr(1, 2)));
    if (!size) return std::nullopt;
    std::bitset<256> componentIds;
    for (std::size_t offset = headerBytes; offset < frame.size(); offset += componentBytes) {
        const auto id = static_cast<unsigned char>(frame[offset]);
        const auto sampling = static_cast<unsigned char>(frame[offset + 1]);
        const auto horizontal = sampling >> 4, vertical = sampling & 0xf;
        const auto quantizationTable = static_cast<unsigned char>(frame[offset + 2]);
        if (componentIds[id] || horizontal < 1 || horizontal > 4 || vertical < 1 || vertical > 4 || quantizationTable > (lossless ? 0 : 3))
            return std::nullopt;
        componentIds.set(id);
    }
    return size;
}
}

std::optional<ImageSize> pngHeaderSize(std::string_view bytes) {
    if (!bytes.starts_with(pngSignature)) return std::nullopt;
    bytes.remove_prefix(pngSignature.size());
    constexpr std::size_t ihdrLength = 13, lengthBytes = 4, typeBytes = 4, crcBytes = 4;
    if (bytes.size() < lengthBytes + typeBytes + ihdrLength + crcBytes || bigEndian(bytes.substr(0, lengthBytes)) != ihdrLength ||
        bytes.substr(lengthBytes, typeBytes) != "IHDR")
        return std::nullopt;
    const auto chunk = bytes.substr(lengthBytes, typeBytes + ihdrLength);
    const auto header = chunk.substr(typeBytes);
    const auto bitDepth = header[8], colorType = header[9];
    const auto compression = header[10], filter = header[11];
    const auto interlace = static_cast<unsigned char>(header[12]);
    if (pngCrc(chunk) != bigEndian(bytes.substr(lengthBytes + chunk.size(), crcBytes)) || !validPngDepth(colorType, bitDepth) ||
        compression != 0 || filter != 0 || interlace > 1)
        return std::nullopt;
    return boundedImageSize(bigEndian(header.substr(0, 4)), bigEndian(header.substr(4, 4)));
}

std::optional<ImageSize> jpegHeaderSize(std::string_view bytes) {
    if (!bytes.starts_with(jpegSignature)) return std::nullopt;
    constexpr std::size_t startOfImageBytes = 2, lengthBytes = 2;
    bytes.remove_prefix(startOfImageBytes);
    while (bytes.starts_with('\xff')) {
        const auto markerOffset = bytes.find_first_not_of('\xff');
        if (markerOffset == bytes.npos) return std::nullopt;
        const auto marker = static_cast<unsigned char>(bytes[markerOffset]);
        bytes.remove_prefix(markerOffset + 1);
        if (marker == jpegTemporaryMarker) continue;
        if (marker < jpegBaselineFrame || (marker >= jpegFirstRestart && marker <= jpegStartOfScan) || bytes.size() < lengthBytes)
            return std::nullopt;
        const auto length = bigEndian(bytes.substr(0, lengthBytes));
        if (length < lengthBytes || length > bytes.size()) return std::nullopt;
        if (isJpegFrame(marker)) return jpegFrameSize(marker, bytes.substr(lengthBytes, length - lengthBytes));
        bytes.remove_prefix(length);
    }
    return std::nullopt;
}
}
