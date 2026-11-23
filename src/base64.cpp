#include "flock/base64.hpp"

#include <array>
#include <stdexcept>

namespace flockv0 {
namespace {
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr auto digits = [] {
    std::array<int,256> table;
    table.fill(-1);
    for (int i = 0; i < static_cast<int>(alphabet.size()); ++i)
        table[static_cast<unsigned char>(alphabet[i])] = i;
    return table;
}();

int digit(unsigned char ch) { return digits[ch]; }
}

std::optional<std::size_t> base64DecodedSize(std::string_view input) {
    if (input.empty() || input.size() % 4 != 0) return std::nullopt;
    return input.size() / 4 * 3 - input.ends_with('=') - input.ends_with("==");
}

std::string encodeBase64(std::string_view bytes) {
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

std::string decodeBase64(std::string_view input, std::size_t maxBytes) {
    const auto size = base64DecodedSize(input);
    if (!size) throw std::invalid_argument("Invalid Base64 length");
    if (*size > maxBytes) throw std::length_error("Decoded Base64 exceeds the byte limit");

    const auto padding = input.size() / 4 * 3 - *size;
    std::string result;
    result.reserve(*size);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const auto blockPadding = i + 4 == input.size() ? padding : 0;
        const int a = digit(input[i]);
        const int b = digit(input[i + 1]);
        const int c = blockPadding == 2 ? 0 : digit(input[i + 2]);
        const int d = blockPadding ? 0 : digit(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) throw std::invalid_argument("Invalid Base64 character or padding");
        if ((blockPadding == 2 && (b & 15)) || (blockPadding == 1 && (c & 3)))
            throw std::invalid_argument("Nonzero unused Base64 padding bits");

        result += static_cast<char>((a << 2) | (b >> 4));
        if (blockPadding < 2) result += static_cast<char>(((b & 15) << 4) | (c >> 2));
        if (blockPadding == 0) result += static_cast<char>(((c & 3) << 6) | d);
    }
    return result;
}
}
