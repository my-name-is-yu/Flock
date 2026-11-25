#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace flockv0::connector_credentials {

inline constexpr std::size_t tokenBytes = 32;
inline constexpr std::string_view digits = "0123456789abcdef";

inline std::string hex(const std::array<unsigned char, tokenBytes>& bytes) {
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

inline std::string digest(std::string_view value) {
    std::array<unsigned char, 32> bytes{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1 || size != bytes.size())
        throw std::runtime_error("Could not hash connection data");
    return hex(bytes);
}

inline std::string bearer(std::string_view prefix) {
    std::array<unsigned char, tokenBytes> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw std::runtime_error("Could not generate connection token");
    return std::string(prefix) + hex(bytes);
}

inline bool validBearer(std::string_view token, std::string_view prefix) {
    return token.starts_with(prefix) && token.size() == prefix.size() + 2 * tokenBytes &&
           token.substr(prefix.size()).find_first_not_of(digits) == token.npos;
}

inline std::string redact(std::string value, std::string_view prefix, std::string_view replacement) {
    const auto length = prefix.size() + 2 * tokenBytes;
    std::size_t at = 0;
    while ((at = value.find(prefix, at)) != value.npos) {
        if (validBearer(std::string_view(value).substr(at, length), prefix)) {
            value.replace(at, length, replacement);
            at += replacement.size();
        } else
            at += prefix.size();
    }
    return value;
}

}
