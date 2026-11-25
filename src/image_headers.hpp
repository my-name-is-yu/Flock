#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace flockv0::message_attachments {
inline constexpr std::string_view pngSignature = "\x89PNG\r\n\x1a\n";
inline constexpr std::string_view jpegSignature = "\xff\xd8\xff";

struct ImageSize {
    std::uint32_t width, height;
};

std::optional<ImageSize> pngHeaderSize(std::string_view bytes);
std::optional<ImageSize> jpegHeaderSize(std::string_view bytes);
}
