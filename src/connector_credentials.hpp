#pragma once

#include "flock/digest.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/rand.h>

namespace flockv0::connector_credentials {

inline constexpr std::size_t tokenBytes = 32;

inline std::string bearer(std::string_view prefix) {
    std::array<unsigned char, tokenBytes> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) throw std::runtime_error("Could not generate connection token");
    return std::string(prefix) + encodeHex(bytes);
}

inline bool validBearer(std::string_view token, std::string_view prefix) {
    return token.starts_with(prefix) && token.size() == prefix.size() + 2 * tokenBytes &&
           token.substr(prefix.size()).find_first_not_of("0123456789abcdef") == token.npos;
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
