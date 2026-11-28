#include "flock/digest.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace flockv0 {
std::string encodeHex(std::span<const unsigned char> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

std::array<unsigned char, 32> sha256(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    unsigned size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 || size != digest.size())
        throw std::runtime_error("Could not compute a SHA-256 digest");
    return digest;
}

std::string sha256Hex(std::string_view bytes) {
    return encodeHex(sha256(bytes));
}
}
