#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
// Checks the length and accounts for trailing padding without decoding the content.
std::optional<std::size_t> base64DecodedSize(std::string_view input);
std::string encodeBase64(std::string_view bytes);

// Throws std::invalid_argument for invalid encoding and std::length_error for an over-limit payload.
std::string decodeBase64(std::string_view input, std::size_t maxBytes);
}
