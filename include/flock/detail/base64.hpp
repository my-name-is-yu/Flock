#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0::base64 {
inline constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

enum class TrailingBits { zero, ignore };

// Return the size implied by the encoded length and trailing padding.
inline std::size_t decodedSize(std::string_view input) {
    if (input.empty() || input.size() % 4 != 0) return 0;
    return input.size() / 4 * 3 - input.ends_with('=') - input.ends_with("==");
}

inline std::optional<std::string> decode(std::string_view input, TrailingBits trailingBits) {
    const auto size = decodedSize(input);
    if (!size) return std::nullopt;
    const auto padding = input.size() / 4 * 3 - size;
    std::string bytes;
    bytes.reserve(size);
    unsigned value = 0, bits = 0;
    for (char ch : input.substr(0,input.size() - padding)) {
        const auto digit = alphabet.find(ch);
        if (digit == alphabet.npos) return std::nullopt;
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += static_cast<char>((value >> bits) & 255);
        }
    }
    if (trailingBits == TrailingBits::zero && (value & ((1u << bits) - 1))) return std::nullopt;
    return bytes;
}
}
