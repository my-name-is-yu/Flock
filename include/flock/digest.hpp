#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace flockv0 {
std::string encodeHex(std::span<const unsigned char> bytes);
std::array<unsigned char, 32> sha256(std::string_view bytes);
std::string sha256Hex(std::string_view bytes);
}
