#include "flock/percent_encoding.hpp"

namespace flockv0 {
std::string percentEncode(std::string_view value, std::string_view keep) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || keep.find(c) != keep.npos)
            encoded += static_cast<char>(c);
        else {
            encoded += '%';
            encoded += digits[c >> 4];
            encoded += digits[c & 15];
        }
    }
    return encoded;
}
}
